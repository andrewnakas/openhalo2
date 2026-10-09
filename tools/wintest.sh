#!/bin/bash
# wintest.sh <outdir> [extra env assignments...]
# One short windowed session (about 70 s, the window is closed at the end):
# boots with the game window, then from outside the process presses keys,
# captures the mouse, moves it, resizes the window, toggles fullscreen, and
# captures the window at each step. Sound runs but no device is opened.
# Results: <outdir>/w_*.png (window captures), fb_title.png (the title's own
# frame at the same moment as w_title.png), win.log ([REPORT] lines show the
# controller report each input produced).
O=$1; shift
mkdir -p "$O"; rm -f "$O/dumpflag"
T=/d/n64work/openhalo2/tools
cd /d/n64work/halo2/build/xr
env "$@" RECOMP_WINDOW=1 RECOMP_APU_MUTE=1 RECOMP_INPUT_DIAG=2 RECOMP_PB_REPORT_MS=5000 \
    RECOMP_FB_DUMP_FLIPS=$O/dumpflag RECOMP_FB_DUMP=$O/fb ./halo2_recomp.exe > "$O/win.log" 2>&1 &
sleep ${BOOTWAIT:-24}
cd $T
python wintool.py info
python wintool.py cap "$O/w_title.png" | tail -1
touch "$O/dumpflag"; sleep 0.5; rm -f "$O/dumpflag"; sleep 0.3
f=$(ls -t "$O"/fb*.bmp 2>/dev/null | sed -n 2p)
[ -n "$f" ] && python -c "import sys;from PIL import Image;Image.open(sys.argv[1]).convert('RGB').save(sys.argv[2])" "$f" "$O/fb_title.png"
find "$O" -maxdepth 1 -name 'fb*.bmp' -delete
python wintool.py focus
echo "MARK keys" >> "$O/win.log"
for k in w a s d space f e tab q g c z up down left right f2; do python wintool.py key $k 150 > /dev/null; sleep 0.12; done
echo "MARK click-capture" >> "$O/win.log"
python wintool.py click; sleep 0.3
echo "MARK mouse-right" >> "$O/win.log"
python wintool.py mouse 600 0 400; sleep 0.4
echo "MARK mouse-up" >> "$O/win.log"
python wintool.py mouse 0 -300 300; sleep 0.4
echo "MARK lbutton" >> "$O/win.log"
python wintool.py lbutton 200; sleep 0.2
python wintool.py key f1 > /dev/null; sleep 0.2
echo "MARK after-f1-mouse" >> "$O/win.log"
python wintool.py mouse 300 0 200; sleep 0.3
echo "MARK enter" >> "$O/win.log"
python wintool.py key enter 300; sleep 3.5
python wintool.py cap "$O/w_after_start.png" | tail -1
python wintool.py size 1100 520
python wintool.py cap "$O/w_wide.png" | tail -1
python wintool.py size 500 800
python wintool.py cap "$O/w_tall.png" | tail -1
python wintool.py key altenter 100; sleep 1
python wintool.py info
python wintool.py cap "$O/w_full.png" | tail -1
python wintool.py key f11 100; sleep 1
python wintool.py info
grep -a "fps (" "$O/win.log" | tail -2
python wintool.py close; sleep 2
echo "still running: $(tasklist | grep -c halo2_recomp)"
