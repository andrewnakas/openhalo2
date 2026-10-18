@echo off
rem Builds bin\OpenHalo2.exe (tools/byog/launcher.c + the BYOG import) with MSVC,
rem static CRT. PATH must not put Git's link.exe ahead of MSVC's.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo VCVARS_FAILED & exit /b 1)
set R=%~dp0..\..
if not exist "%~dp0bin" mkdir "%~dp0bin"
cl /nologo /O2 /W3 /MT /std:c11 /D_CRT_SECURE_NO_WARNINGS /I"%R%\port\xr\src" /I"%R%\tools\import" /Fo"%~dp0bin\\" /Fe"%~dp0bin\OpenHalo2.exe" "%~dp0launcher.c" "%R%\port\xr\src\byog.c" "%R%\tools\import\h2import.c" /link /SUBSYSTEM:WINDOWS user32.lib comdlg32.lib
echo BUILD_EXIT=%errorlevel%
