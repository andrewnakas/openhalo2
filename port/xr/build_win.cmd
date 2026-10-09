@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cmake -S D:\n64work\openhalo2\port\xr -B D:\n64work\halo2\build\xr -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl
if errorlevel 1 (echo CONFIGURE_FAILED & exit /b 1)
cmake --build D:\n64work\halo2\build\xr -j 3
echo BUILD_EXIT=%errorlevel%
