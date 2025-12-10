/*=============================================================================
convert_utf8.cpp
UTF-8 Batch Conversion Helper Functions

Functions for converting HSP source files (*.as, *.hsp) to UTF-8 encoding
Uses Windows API directly instead of Footy2 for simpler and more robust conversion.
=============================================================================*/

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <vector>

// Encoding detection results
enum EncodingType {
	ENC_UNKNOWN = 0,
	ENC_UTF8,
	ENC_UTF8_BOM,
	ENC_UTF16_LE_BOM,
	ENC_UTF16_BE_BOM,
	ENC_SHIFT_JIS,  // or other ANSI codepage
};

// File list structure
struct FileListItem {
	char path[_MAX_PATH];
	int encoding;
};

static std::vector<FileListItem> g_fileList;
static int g_convertedCount = 0;
static int g_skippedCount = 0;
static int g_errorCount = 0;

// Check if file extension is .as or .hsp
static int IsHspFile(const char* filename) {
	const char* ext = strrchr(filename, '.');
	if (!ext) return 0;
	return (_stricmp(ext, ".as") == 0 || _stricmp(ext, ".hsp") == 0);
}

// Read entire file into buffer
static char* ReadFileToBuffer(const char* filePath, DWORD* outSize) {
	HANDLE hFile = CreateFileA(filePath, GENERIC_READ, FILE_SHARE_READ, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) return NULL;

	DWORD fileSize = GetFileSize(hFile, NULL);
	if (fileSize == INVALID_FILE_SIZE || fileSize == 0) {
		CloseHandle(hFile);
		*outSize = 0;
		return NULL;
	}

	char* buffer = new char[fileSize + 1];
	if (!buffer) {
		CloseHandle(hFile);
		return NULL;
	}

	DWORD bytesRead;
	if (!ReadFile(hFile, buffer, fileSize, &bytesRead, NULL)) {
		CloseHandle(hFile);
		delete[] buffer;
		return NULL;
	}

	CloseHandle(hFile);
	buffer[fileSize] = '\0';
	*outSize = fileSize;
	return buffer;
}

// Write buffer to file
static bool WriteBufferToFile(const char* filePath, const char* buffer, DWORD size) {
	HANDLE hFile = CreateFileA(filePath, GENERIC_WRITE, 0, NULL,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) return false;

	DWORD bytesWritten;
	BOOL result = WriteFile(hFile, buffer, size, &bytesWritten, NULL);
	CloseHandle(hFile);
	return result && (bytesWritten == size);
}

// Check if buffer is valid UTF-8
static bool IsValidUTF8(const unsigned char* data, DWORD size) {
	DWORD i = 0;
	int utf8CharCount = 0;  // Count of multi-byte UTF-8 sequences found

	while (i < size) {
		unsigned char c = data[i];

		if (c < 0x80) {
			// ASCII character
			i++;
		} else if ((c & 0xE0) == 0xC0) {
			// 2-byte sequence
			if (i + 1 >= size) return false;
			if ((data[i + 1] & 0xC0) != 0x80) return false;
			// Check for overlong encoding
			if (c < 0xC2) return false;
			utf8CharCount++;
			i += 2;
		} else if ((c & 0xF0) == 0xE0) {
			// 3-byte sequence
			if (i + 2 >= size) return false;
			if ((data[i + 1] & 0xC0) != 0x80) return false;
			if ((data[i + 2] & 0xC0) != 0x80) return false;
			// Check for overlong and surrogate
			unsigned int codepoint = ((c & 0x0F) << 12) | ((data[i + 1] & 0x3F) << 6) | (data[i + 2] & 0x3F);
			if (codepoint < 0x800 || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) return false;
			utf8CharCount++;
			i += 3;
		} else if ((c & 0xF8) == 0xF0) {
			// 4-byte sequence
			if (i + 3 >= size) return false;
			if ((data[i + 1] & 0xC0) != 0x80) return false;
			if ((data[i + 2] & 0xC0) != 0x80) return false;
			if ((data[i + 3] & 0xC0) != 0x80) return false;
			// Check for valid range
			unsigned int codepoint = ((c & 0x07) << 18) | ((data[i + 1] & 0x3F) << 12) |
				((data[i + 2] & 0x3F) << 6) | (data[i + 3] & 0x3F);
			if (codepoint < 0x10000 || codepoint > 0x10FFFF) return false;
			utf8CharCount++;
			i += 4;
		} else {
			// Invalid UTF-8 start byte
			return false;
		}
	}

	// If we found multi-byte sequences, it's likely UTF-8
	// If it's all ASCII, we still consider it valid UTF-8
	return true;
}

// Detect encoding of file buffer
static EncodingType DetectEncoding(const unsigned char* data, DWORD size) {
	if (size == 0) return ENC_UTF8;

	// Check for BOM
	if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
		return ENC_UTF8_BOM;
	}
	if (size >= 2 && data[0] == 0xFF && data[1] == 0xFE) {
		return ENC_UTF16_LE_BOM;
	}
	if (size >= 2 && data[0] == 0xFE && data[1] == 0xFF) {
		return ENC_UTF16_BE_BOM;
	}

	// Check if valid UTF-8
	if (IsValidUTF8(data, size)) {
		// Check if it contains any multi-byte UTF-8 sequences
		// to distinguish from pure ASCII or Shift-JIS
		bool hasMultiByte = false;
		for (DWORD i = 0; i < size; i++) {
			if (data[i] >= 0x80) {
				hasMultiByte = true;
				break;
			}
		}

		if (!hasMultiByte) {
			// Pure ASCII - treat as UTF-8
			return ENC_UTF8;
		}

		// Has multi-byte sequences and is valid UTF-8
		return ENC_UTF8;
	}

	// Default to Shift-JIS (system default codepage for Japanese Windows)
	return ENC_SHIFT_JIS;
}

// Enumerate HSP files recursively
static void EnumerateHspFilesRecursive(const char* dirPath) {
	char searchPath[_MAX_PATH];
	WIN32_FIND_DATA fd;
	HANDLE hFind;

	// Search for all files
	sprintf(searchPath, "%s\\*", dirPath);
	hFind = FindFirstFileA(searchPath, &fd);

	if (hFind == INVALID_HANDLE_VALUE) return;

	do {
		// Skip . and ..
		if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
			continue;

		char fullPath[_MAX_PATH];
		sprintf(fullPath, "%s\\%s", dirPath, fd.cFileName);

		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			// Recursive search
			EnumerateHspFilesRecursive(fullPath);
		} else {
			// Check if HSP file
			if (IsHspFile(fd.cFileName)) {
				FileListItem item;
				strcpy(item.path, fullPath);
				item.encoding = ENC_UNKNOWN;
				g_fileList.push_back(item);
			}
		}
	} while (FindNextFileA(hFind, &fd));

	FindClose(hFind);
}

// Convert single file to UTF-8
static int ConvertFileToUTF8(const char* filePath) {
	// Read file
	DWORD fileSize = 0;
	char* buffer = ReadFileToBuffer(filePath, &fileSize);
	if (!buffer) {
		if (fileSize == 0) return 0;  // Empty file, skip
		return -1;  // Read error
	}

	// Detect encoding
	EncodingType encoding = DetectEncoding((unsigned char*)buffer, fileSize);

	// If already UTF-8 (with or without BOM), skip
	if (encoding == ENC_UTF8 || encoding == ENC_UTF8_BOM) {
		delete[] buffer;
		return 0;  // Already UTF-8
	}

	// Determine source codepage and data offset
	int sourceCodepage;
	const char* sourceData = buffer;
	DWORD sourceSize = fileSize;

	if (encoding == ENC_UTF16_LE_BOM) {
		// Convert from UTF-16 LE
		sourceData = buffer + 2;  // Skip BOM
		sourceSize = fileSize - 2;

		// First get required size
		int utf8Size = WideCharToMultiByte(CP_UTF8, 0, (wchar_t*)sourceData, sourceSize / 2,
			NULL, 0, NULL, NULL);
		if (utf8Size <= 0) {
			delete[] buffer;
			return -1;
		}

		char* utf8Buffer = new char[utf8Size + 1];
		WideCharToMultiByte(CP_UTF8, 0, (wchar_t*)sourceData, sourceSize / 2,
			utf8Buffer, utf8Size, NULL, NULL);

		// Create backup
		char bakPath[_MAX_PATH];
		sprintf(bakPath, "%s.bak", filePath);
		CopyFileA(filePath, bakPath, FALSE);

		// Write UTF-8
		bool success = WriteBufferToFile(filePath, utf8Buffer, utf8Size);
		delete[] utf8Buffer;
		delete[] buffer;

		if (!success) {
			CopyFileA(bakPath, filePath, FALSE);
			DeleteFileA(bakPath);
			return -2;
		}
		return 1;
	} else if (encoding == ENC_UTF16_BE_BOM) {
		// Convert from UTF-16 BE - swap bytes first
		sourceData = buffer + 2;  // Skip BOM
		sourceSize = fileSize - 2;

		wchar_t* swapped = new wchar_t[sourceSize / 2 + 1];
		for (DWORD i = 0; i < sourceSize / 2; i++) {
			swapped[i] = (((unsigned char)sourceData[i * 2]) << 8) |
				((unsigned char)sourceData[i * 2 + 1]);
		}

		int utf8Size = WideCharToMultiByte(CP_UTF8, 0, swapped, sourceSize / 2,
			NULL, 0, NULL, NULL);
		if (utf8Size <= 0) {
			delete[] swapped;
			delete[] buffer;
			return -1;
		}

		char* utf8Buffer = new char[utf8Size + 1];
		WideCharToMultiByte(CP_UTF8, 0, swapped, sourceSize / 2,
			utf8Buffer, utf8Size, NULL, NULL);
		delete[] swapped;

		char bakPath[_MAX_PATH];
		sprintf(bakPath, "%s.bak", filePath);
		CopyFileA(filePath, bakPath, FALSE);

		bool success = WriteBufferToFile(filePath, utf8Buffer, utf8Size);
		delete[] utf8Buffer;
		delete[] buffer;

		if (!success) {
			CopyFileA(bakPath, filePath, FALSE);
			DeleteFileA(bakPath);
			return -2;
		}
		return 1;
	}

	// Assume Shift-JIS (CP932) for other cases
	sourceCodepage = 932;  // Shift-JIS

	// Convert to wide char first
	int wideSize = MultiByteToWideChar(sourceCodepage, 0, buffer, fileSize, NULL, 0);
	if (wideSize <= 0) {
		delete[] buffer;
		return -1;
	}

	wchar_t* wideBuffer = new wchar_t[wideSize + 1];
	MultiByteToWideChar(sourceCodepage, 0, buffer, fileSize, wideBuffer, wideSize);

	// Convert wide to UTF-8
	int utf8Size = WideCharToMultiByte(CP_UTF8, 0, wideBuffer, wideSize, NULL, 0, NULL, NULL);
	if (utf8Size <= 0) {
		delete[] wideBuffer;
		delete[] buffer;
		return -1;
	}

	char* utf8Buffer = new char[utf8Size + 1];
	WideCharToMultiByte(CP_UTF8, 0, wideBuffer, wideSize, utf8Buffer, utf8Size, NULL, NULL);
	delete[] wideBuffer;

	// Create backup
	char bakPath[_MAX_PATH];
	sprintf(bakPath, "%s.bak", filePath);
	CopyFileA(filePath, bakPath, FALSE);

	// Write UTF-8
	bool success = WriteBufferToFile(filePath, utf8Buffer, utf8Size);
	delete[] utf8Buffer;
	delete[] buffer;

	if (!success) {
		CopyFileA(bakPath, filePath, FALSE);
		DeleteFileA(bakPath);
		return -2;
	}

	return 1;  // Converted
}

// Convert folder to UTF-8 (entry point)
int ConvertFolderToUTF8(const char* dirPath, char* resultMessage, int msgBufSize) {
	g_fileList.clear();
	g_convertedCount = 0;
	g_skippedCount = 0;
	g_errorCount = 0;

	// Enumerate files
	EnumerateHspFilesRecursive(dirPath);

	if (g_fileList.size() == 0) {
		sprintf(resultMessage, "No HSP files (*.as, *.hsp) found in:\n%s", dirPath);
		return 0;
	}

	// Convert each file
	for (size_t i = 0; i < g_fileList.size(); i++) {
		int result = ConvertFileToUTF8(g_fileList[i].path);

		if (result == 1) {
			g_convertedCount++;
		} else if (result == 0) {
			g_skippedCount++;
		} else {
			g_errorCount++;
		}
	}

	// Build result message
	sprintf(resultMessage,
		"UTF-8変換完了\n\n"
		"対象: %s\n\n"
		"総ファイル数: %d\n"
		"変換済み: %d\n"
		"スキップ (既にUTF-8): %d\n"
		"エラー: %d\n\n"
		"変換されたファイルのバックアップファイル (*.bak) が作成されました。",
		dirPath,
		(int)g_fileList.size(),
		g_convertedCount,
		g_skippedCount,
		g_errorCount
	);

	return 1;
}
