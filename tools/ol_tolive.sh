#!/bin/bash
# ol_tolive.sh <outdir>: from the main menu of a copy whose profile is already
# bound to an Open Live account (it signs in when the profile is chosen),
# opens Xbox Live: highlight row 1, A (the profile screen), A (the Xbox Live
# menu). Saves live.png. When the copy is on the "Sign in to Xbox Live" list
# instead (the profile did not sign in by itself), it picks the first account
# there, which leads to the same profile screen.
O=${1:?outdir}
T=/d/n64work/openhalo2/tools
if bash $T/ol_mainrow.sh "$O" 1; then
  bash $T/ol_keys.sh "$O" prof a:300@8 > /dev/null
else
  bash $T/ol_keys.sh "$O" signin down:150@3 a:300@15 > /dev/null
fi
bash $T/ol_keys.sh "$O" live a:300@10
