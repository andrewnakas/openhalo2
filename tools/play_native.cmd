@echo off
rem Open Halo 2, native Windows build on the clean-room maps, for the owner to play.
rem Close the window to quit. A crash leaves its report in halo2_recomp.log next to the exe.
cd /d D:\n64work\halo2\build\xr_clean
start "" halo2_recomp.exe
