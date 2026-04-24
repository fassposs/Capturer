@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set PATH=C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64;%PATH%
D:\Programs\cmake-4.3.1-windows-x86_64\bin\cmake.exe --build D:\workspace\vmshareroom\c++_project\Capturer\build --config Release -j4
echo BUILD_EXIT_CODE=%ERRORLEVEL%
