@echo off
rem Runtime-only kit (no lifted code) for player-side builds: see tools/byog.
rem usage: build_kit.cmd [build dir]   (default D:\n64work\halo2\build\kit)
setlocal
set B=%~1
if "%B%"=="" set B=D:\n64work\halo2\build\kit
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cmake -S %~dp0. -B %B% -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DOPENHALO2_KIT=ON
if errorlevel 1 (echo CONFIGURE_FAILED & exit /b 1)
cmake --build %B% -j 3
echo BUILD_EXIT=%errorlevel%
