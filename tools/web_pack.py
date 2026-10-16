"""Pack the clean web build into folders a static host can take.

usage: python web_pack.py <out dir> [--name halo2-cleanroom] [--maps all|a,b,c]
                          [--part-mb 95] [--shard-mb 900]
                          [--site D:/n64work/halo2/build/web/site_clean]

Writes <out>/<name>/ (the page: index.html, halo2.js/.wasm, scripts, coi-sw.js,
manifest.json) and, when the game files do not fit in --shard-mb, further folders
<out>/<name>-data1, -data2, ... Each folder stays under --shard-mb (GitHub Pages:
1 GB a site) and each file under --part-mb (GitHub: 100 MB a file): bigger game
files are cut into pieces which the page joins again while it copies them into the
browser's private file system (port/web/dev.html, importTree). The data folders are
meant to be published as sibling project sites of the same account, which makes
them the same origin as the page ("../<name>-data1/...").

--maps picks multiplayer/campaign maps by name (without .map); mainmenu, shared and
single_player_shared always go in. The save tree is taken from <site>/save: files
over 4 MB (disk images, map caches) are listed as blank at their size, the title
formats them on first boot; small ones are copied.

Only ever point this at the CLEAN tree, and run the taint scan on <out> before
anything is uploaded. Nothing here publishes.
"""
import argparse
import json
import os
import shutil
import sys

ALWAYS = {"mainmenu", "shared", "single_player_shared"}
PAGE = ["halo2.js", "halo2.wasm", "input.js", "audio.js", "net.js", "h2net.js"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--name", default="halo2-cleanroom")
    ap.add_argument("--maps", default="all")
    ap.add_argument("--part-mb", type=int, default=95)
    ap.add_argument("--shard-mb", type=int, default=900)
    ap.add_argument("--site", default="D:/n64work/halo2/build/web/site_clean")
    ap.add_argument("--coi", default="D:/n64work/halo2-cleanroom/ports/wasm/coi-sw.js")
    ap.add_argument("--dry", action="store_true", help="print the layout, write nothing")
    a = ap.parse_args()
    part, shard = a.part_mb << 20, a.shard_mb << 20
    want = None if a.maps == "all" else ALWAYS | set(a.maps.split(","))

    # what goes in: (path in the private file system, source file, size, blank)
    items, dirs = [], []
    for top in ("game", "save"):
        for d, sub, names in os.walk(os.path.join(a.site, top)):
            rel = os.path.relpath(d, a.site).replace(os.sep, "/")
            dirs.append(rel)
            for n in sorted(names):
                src = os.path.join(d, n); size = os.path.getsize(src)
                if top == "game":
                    if n.endswith((".json", ".bik")):
                        continue
                    if n.endswith(".map") and want is not None and n[:-4] not in want:
                        continue
                items.append((rel + "/" + n, src, size, top == "save" and size > 4 << 20))

    # the page folder first, then data folders as each fills up
    folders = [a.name]; used = [sum(os.path.getsize(os.path.join(a.site, f)) for f in PAGE)]
    files = []; plan = []   # plan: (folder index, relative file, source, offset, length)
    for path, src, size, blank in sorted(items, key=lambda i: (i[3], i[2])):
        if blank:
            files.append([path, size, None, 1]); continue
        pieces = []; off = 0; n = 0
        while off < size or (size == 0 and n == 0):
            ln = min(part, size - off)
            if used[-1] + ln > shard:
                folders.append(f"{a.name}-data{len(folders)}"); used.append(0)
            k = len(folders) - 1
            rel = path if size <= part else f"{path}.{n:02d}"
            used[k] += ln; plan.append((k, rel, src, off, ln))
            pieces.append(("" if k == 0 else f"../{folders[k]}/") + rel)
            off += ln; n += 1
            if size == 0:
                break
        files.append([path, size, pieces if (len(pieces) > 1 or not pieces[0] == path) else None, 0])

    for k, f in enumerate(folders):
        print(f"{f}: {used[k] / 2**20:.0f} MB, {sum(1 for p in plan if p[0] == k)} files")
    print(f"total {sum(used) / 2**30:.2f} GB in {len(folders)} folders; "
          f"{sum(1 for f in files if f[3])} blank save files; {sum(1 for f in files if f[2] and len(f[2]) > 1)} files in pieces")
    if a.dry:
        return
    for k, f in enumerate(folders):
        os.makedirs(os.path.join(a.out, f), exist_ok=True)
        open(os.path.join(a.out, f, ".nojekyll"), "w").close()
    page = os.path.join(a.out, a.name)
    for f in PAGE:
        shutil.copy(os.path.join(a.site, f), os.path.join(page, f))
    shutil.copy(os.path.join(a.site, "index.html"), os.path.join(page, "index.html"))
    shutil.copy(a.coi, os.path.join(page, "coi-sw.js"))
    json.dump({"v": 2, "files": files, "dirs": dirs}, open(os.path.join(page, "manifest.json"), "w"))
    for k, rel, src, off, ln in plan:
        dst = os.path.join(a.out, folders[k], rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if os.path.exists(dst) and os.path.getsize(dst) == ln and os.path.getmtime(dst) >= os.path.getmtime(src):
            continue
        with open(src, "rb") as i, open(dst, "wb") as o:
            i.seek(off); left = ln
            while left:
                b = i.read(min(left, 4 << 20)); o.write(b); left -= len(b)
    print("written to", a.out)


if __name__ == "__main__":
    sys.exit(main())
