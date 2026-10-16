@echo off
rem Open Halo 2, SANITIZER build, on the clean-room maps with the owner's save.
rem Plays like the normal build but slower; it stops at the first engine fault
rem (unbalanced call, runaway memory fill, call through a null pointer) instead
rem of carrying on, and writes where it happened to halo2_recomp.log.
rem Play to the Cairo Station door (or wherever it used to crash) until it stops.
cd /d D:\n64work\halo2\build\xr_clean
set RECOMP_SANITIZE=1
rem 0x334BAD: XHV voice talker null call on a headset-removal DPC (separate USB bug)
set RECOMP_SAN_ICALL_OK=0x334BAD
start "" halo2_san.exe
