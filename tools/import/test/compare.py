#!/usr/bin/env python3
"""TEST-ONLY: check an h2import output against the source folder byte-for-byte.

usage: compare.py SRC_GAME OUT_GAME [GLOB ...]
Expected set = SRC files under default.xbe, maps/, media/, bink/, TDATA/, UDATA/
(case-insensitive top level), optionally narrowed to GLOBs. Reports missing,
extra and differing files; exit 0 only if identical.
"""
import filecmp
import fnmatch
import os
import sys

KEEP_DIRS = {"maps", "media", "bink", "tdata", "udata"}


def files(root):
    out = {}
    for d, _, fs in os.walk(root):
        for f in fs:
            full = os.path.join(d, f)
            out[os.path.relpath(full, root).replace(os.sep, "/")] = full
    return out


def main():
    src, out, globs = sys.argv[1], sys.argv[2], sys.argv[3:]
    s, o = files(src), files(out)
    exp = {}
    for rel, full in s.items():
        top = rel.split("/")[0].lower()
        if not (rel.lower() == "default.xbe" or ("/" in rel and top in KEEP_DIRS)):
            continue
        if globs and not any(fnmatch.fnmatch(rel, g) for g in globs):
            continue
        exp[rel] = full
    missing = sorted(set(exp) - set(o))
    extra = sorted(set(o) - set(exp))
    diff = [r for r in sorted(set(exp) & set(o)) if not filecmp.cmp(exp[r], o[r], shallow=False)]
    nbytes = sum(os.path.getsize(exp[r]) for r in exp)
    print(f"expected {len(exp)} files {nbytes} bytes | missing {len(missing)} extra {len(extra)} "
          f"differ {len(diff)}")
    for tag, lst in (("missing", missing), ("extra", extra), ("differ", diff)):
        for r in lst[:5]:
            print(f"  {tag}: {r}")
    return 0 if not (missing or extra or diff) else 1


if __name__ == "__main__":
    sys.exit(main())
