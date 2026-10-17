@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d %1
cl /nologo /O2 /Ob2 /DNDEBUG /DWIN32 /D_WINDOWS -DNOMINMAX -DWIN32_LEAN_AND_MEAN -std:c11 /bigobj /W0 /LD /I"%~dp0." /I"D:\n64work\openhalo2\port\xr\src\recomp\gen" /I"D:\n64work\openhalo2\port\xr\src\recomp" /I"D:\n64work\openhalo2\port\xr\src" /I"D:\n64work\halo2\third_party\xboxrecomp\include" lifted_dll.c /Felifted.dll > build.log 2>&1
echo BUILD_EXIT=%ERRORLEVEL%
