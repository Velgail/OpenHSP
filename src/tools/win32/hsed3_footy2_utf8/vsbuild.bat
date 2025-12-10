REM Batch build script for Visual Studio 2017/2019 (UTF-8 version)
echo off
MSBuild hsed_vc2017.sln -t:Rebuild -p:Configuration=Release;Platform="win32"
MSBuild hsed_vc2017.sln -t:Rebuild -p:Configuration=release_en;Platform="win32"
copy /B /Y Release\hsed3u8.exe ..\..\..\hsp3\Release
copy /B /Y release_en\hsed3u8.exe ..\..\..\hsp3\Release\hsed3u8_en.exe
