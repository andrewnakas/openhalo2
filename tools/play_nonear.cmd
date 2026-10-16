@echo off
rem Open Halo 2 with the near-plane clip at SET_CLIP_MIN switched off
rem (A/B for objects still cut close to the camera). Same exe and save as
rem a plain double-click of build\xr_clean\halo2_recomp.exe.
cd /d D:\n64work\halo2\build\xr_clean
set RECOMP_GPU_NEAR=0
start "" halo2_recomp.exe
