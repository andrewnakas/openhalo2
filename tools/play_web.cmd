@echo off
rem Open Halo 2, clean-room web build, for the owner to play.
rem Starts the local server on port 8092 (if not already up) and opens Edge with its
rem own profile (D:\n64work\halo2\build\web\owner_profile, separate from the test profile).
rem First launch copies the 3.9 GB clean game folder into that profile (several minutes,
rem progress shows on the page); later launches only check sizes.
netstat -ano | findstr /R /C:":8092 .*LISTENING" >nul || (
  start "halo2 server" /min python D:\n64work\halo2-cleanroom\ports\wasm\serve.py D:\n64work\halo2\build\web\site_clean 8092
  timeout /t 2 >nul
)
start "" "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe" --user-data-dir=D:\n64work\halo2\build\web\owner_profile --no-first-run --new-window "http://localhost:8092/index.html?import=1"
