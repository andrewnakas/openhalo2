@echo off
rem Builds h2net.lib + h2net_test.exe (MSVC, /MT) into D:\n64work\halo2\build\net.
rem Needs libdatachannel:x64-windows-static installed in D:\n64work\vcpkg.
rem Git's link.exe must not shadow MSVC's: drop Git's usr\bin from PATH.
set "PATH=%PATH:C:\Program Files\Git\usr\bin;=%"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cmake -S D:\n64work\openhalo2\net -B D:\n64work\halo2\build\net -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ^
  -DCMAKE_TOOLCHAIN_FILE=D:/n64work/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static -DVCPKG_MANIFEST_MODE=OFF
if errorlevel 1 (echo CONFIGURE_FAILED & exit /b 1)
cmake --build D:\n64work\halo2\build\net -j 3
echo BUILD_EXIT=%errorlevel%
