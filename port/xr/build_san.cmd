@echo off
rem The sanitizer build: every call checked for an unbalanced stack or a
rem clobbered ebx/esi/edi (recomp_types.h, RECOMP_SANITIZE). Own tree in
rem build\xr_san; the exe is copied into build\xr as halo2_san.exe so it runs
rem on that folder's game and save. Run it with RECOMP_SANITIZE=1 to make the
rem first violation stop the run.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if not exist D:\n64work\halo2\build\xr_san\build.ninja (
  cmake -S D:\n64work\openhalo2\port\xr -B D:\n64work\halo2\build\xr_san -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl "-DCMAKE_C_FLAGS=/DRECOMP_SANITIZE"
  if errorlevel 1 (echo CONFIGURE_FAILED & exit /b 1)
)
cmake --build D:\n64work\halo2\build\xr_san -j %1 -- -k 0
set BE=%errorlevel%
if %BE%==0 (
  copy /y D:\n64work\halo2\build\xr_san\halo2_recomp.exe D:\n64work\halo2\build\xr\halo2_san.exe >nul
  copy /y D:\n64work\halo2\build\xr_san\halo2_recomp.exe D:\n64work\halo2\build\xr_clean\halo2_san.exe >nul
)
echo BUILD_EXIT=%BE%
