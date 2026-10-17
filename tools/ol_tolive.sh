#!/bin/bash
# ol_tolive.sh <outdir>: drives a copy to the Xbox Live menu from wherever it is
# (title, sign-in list, main menu, profile screen), reading its screen
# (tools/ol_drive.py). Saves live.png.
O=${1:?outdir}
T=/d/n64work/openhalo2/tools
python $T/ol_drive.py tolive "$(dirname "$O")" "$(basename "$O")"
bash $T/shot.sh "$O" live
