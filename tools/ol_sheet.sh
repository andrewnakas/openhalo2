#!/bin/bash
# ol_sheet.sh <outdir> <label> [copies...]: shoots each running copy
# (<outdir>/<copy>, default a b c) and saves them side by side as
# <outdir>/<label>.png
O=${1:?outdir}; L=${2:?label}; shift 2
C=("$@"); [ ${#C[@]} -eq 0 ] && C=(a b c)
for k in "${C[@]}"; do bash /d/n64work/openhalo2/tools/shot.sh "$O/$k" "$L" > /dev/null; done
W=$(cygpath -m "$O")
python - "$W" "$L" "${C[@]}" <<'EOF'
import sys
from PIL import Image
o, l, cs = sys.argv[1], sys.argv[2], sys.argv[3:]
s = Image.new('RGB', (640 * len(cs), 480))
for i, k in enumerate(cs):
    try:
        s.paste(Image.open(f'{o}/{k}/{l}.png').convert('RGB').resize((640, 480)), (640 * i, 0))
    except OSError:
        pass
s.save(f'{o}/{l}.png')
EOF
echo "$O/$L.png"
