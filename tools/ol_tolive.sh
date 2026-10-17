#!/bin/bash
# ol_tolive.sh <outdir>: from the main menu of a copy whose profile is already
# bound to an Open Live account (it signs in when the profile is chosen),
# opens Xbox Live: highlight row 1, A (the profile screen), A (the Xbox Live
# menu). Saves live.png.
O=${1:?outdir}
T=/d/n64work/openhalo2/tools
bash $T/ol_mainrow.sh "$O" 1 || exit 1
bash $T/ol_keys.sh "$O" prof a:300@8 > /dev/null
bash $T/ol_keys.sh "$O" live a:300@10
