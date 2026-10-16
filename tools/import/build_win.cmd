@echo off
rem Builds bin\h2import.exe with MSVC. Run from any shell; PATH must not put
rem Git's link.exe ahead of MSVC's (vcvars prepends MSVC, which is enough).
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo VCVARS_FAILED & exit /b 1)
if not exist "%~dp0bin" mkdir "%~dp0bin"
cl /nologo /O2 /W4 /std:c11 /Fo"%~dp0bin\\" /Fe"%~dp0bin\h2import.exe" "%~dp0h2import.c" "%~dp0h2import_main.c"
echo BUILD_EXIT=%errorlevel%
