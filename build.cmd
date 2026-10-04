@echo off
rem Configure and build the 32-bit host with MSVC + Ninja. The compiler itself is
rem the 64-bit-hosted one (amd64_x86): the 32-bit cl.exe runs out of heap on a
rem 400-function chunk of the full lift (C1002 in pass 2).
rem Needs Visual Studio 2022 (any edition, or the Build Tools) with the C++ x86 tools.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VS=%%i"
if not defined VS (echo Visual Studio 2022 not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" amd64_x86 >nul || exit /b 1
if not defined BUILD_DIR set "BUILD_DIR=build"
if not defined BUILD_TYPE set "BUILD_TYPE=RelWithDebInfo"
if not exist %BUILD_DIR%\build.ninja cmake -S . -B %BUILD_DIR% -G Ninja -DCMAKE_BUILD_TYPE=%BUILD_TYPE% %CMAKE_ARGS% || exit /b 1
cmake --build %BUILD_DIR% %*
