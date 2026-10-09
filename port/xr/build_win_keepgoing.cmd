@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cmake --build D:\n64work\halo2\build\xr -j 3 -- -k 0
echo BUILD_EXIT=%errorlevel%
