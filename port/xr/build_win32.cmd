@echo off
rem 32-bit (x86) build of the same sources: first step of P2 (identity-mapped 32-bit model).
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64_x86 >nul
cmake -S D:\n64work\openhalo2\port\xr -B D:\n64work\halo2\build\xr32 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl
if errorlevel 1 (echo CONFIGURE_FAILED & exit /b 1)
cmake --build D:\n64work\halo2\build\xr32 -j 3 -- -k 0
echo BUILD_EXIT=%errorlevel%
