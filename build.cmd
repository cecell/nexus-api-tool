@echo off
rem Configure, build and test with MSVC 14.44 + Ninja + vcpkg. Run from any shell:
rem   cmd /c C:\projects\nexus-api-tool\build.cmd [Debug|Release]
setlocal
set CFG=%~1
if "%CFG%"=="" set CFG=Release
rem vcvars replaces VCPKG_ROOT with Visual Studio's own copy, so keep the real one.
set VCPKG=%VCPKG_ROOT%
if "%VCPKG%"=="" set VCPKG=C:\vcpkg
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" -vcvars_ver=14.44 >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=%CFG% "-DCMAKE_TOOLCHAIN_FILE=%VCPKG%\scripts\buildsystems\vcpkg.cmake" || exit /b 1
cmake --build build || exit /b 1
ctest --test-dir build --output-on-failure || exit /b 1
