"""Write the dev page's import list: every file under site/game and site/save
(path relative to the site, size), and the directories. The analysis JSON
beside the XBE is skipped; Bink movies are skipped unless WEBBINK is set.
usage: python web_manifest.py <site dir>"""
import json
import os
import sys

site = sys.argv[1]
files, dirs = [], []
for top in ("game", "save"):
    for d, sub, names in os.walk(os.path.join(site, top)):
        rel = os.path.relpath(d, site).replace(os.sep, "/")
        dirs.append(rel)
        for n in names:
            if top == "game" and (n.endswith(".json") or (n.endswith(".bik") and not os.environ.get("WEBBINK"))):
                continue
            files.append([rel + "/" + n, os.path.getsize(os.path.join(d, n))])
json.dump(files, open(os.path.join(site, "manifest.json"), "w"))
json.dump(dirs, open(os.path.join(site, "manifest_dirs.json"), "w"))
print(f"manifest: {len(files)} files, {sum(f[1] for f in files) / 2**30:.2f} GB, {len(dirs)} dirs")
