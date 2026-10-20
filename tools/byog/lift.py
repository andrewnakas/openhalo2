"""Lift the player's own default.xbe into C (the static recompilation).

The release carries no code from the game. This script turns the player's
copy of default.xbe into the lifted C that the build step compiles, using the
xboxrecomp toolkit and the h2lift passes, exactly as tools/relift.sh does for
development:

    xbe_parser --json -> disasm (seeded) -> func_id -> abi_analysis -> recomp
    -> recomp_types.h from the toolkit template -> mmio, tiled, xo_hle passes

The result is checked file by file against gen_manifest.json (SHA-256 of
every file this release was tested with). A mismatch means a different
default.xbe or toolkit and the build stops: code that differs from the tested
build is not run.

usage: python lift.py --xbe game/default.xbe --toolkit toolkit --h2lift h2lift
                      --seeds seed_decomp_functions.json --out build/gen
                      [--manifest gen_manifest.json] [--write-manifest]
Prints one line per step and a summary.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time


def run(step, args, cwd):
    t = time.time()
    print(f"[lift] {step} ...", flush=True)
    p = subprocess.run([sys.executable] + args, cwd=cwd,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if p.returncode != 0:
        print(p.stdout[-4000:])
        raise SystemExit(f"[lift] {step} failed (exit {p.returncode})")
    print(f"[lift] {step} done in {time.time() - t:.0f} s", flush=True)
    return p.stdout


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def manifest_of(gen):
    return {n: sha256(os.path.join(gen, n)) for n in sorted(os.listdir(gen))
            if os.path.isfile(os.path.join(gen, n))}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--xbe", required=True)
    ap.add_argument("--toolkit", required=True)
    ap.add_argument("--h2lift", required=True)
    ap.add_argument("--seeds", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--work", help="toolkit work dir (default: <out>/../lift_work)")
    ap.add_argument("--manifest")
    ap.add_argument("--write-manifest", action="store_true")
    a = ap.parse_args()

    xbe = os.path.abspath(a.xbe)
    tk = os.path.abspath(a.toolkit)
    out = os.path.abspath(a.out)
    work = os.path.abspath(a.work or os.path.join(os.path.dirname(out), "lift_work"))
    seeds = os.path.abspath(a.seeds)
    h2lift = os.path.abspath(a.h2lift)
    for p in (xbe, tk, seeds, h2lift):
        if not os.path.exists(p):
            raise SystemExit(f"[lift] missing {p}")

    # The toolkit finds <stem>_analysis.json beside the XBE: work on a copy so
    # nothing is written into the player's game folder.
    os.makedirs(work, exist_ok=True)
    wxbe = os.path.join(work, "default.xbe")
    if not os.path.exists(wxbe) or sha256(wxbe) != sha256(xbe):
        shutil.copyfile(xbe, wxbe)
    os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
    t0 = time.time()
    run("parse XBE", ["-m", "tools.xbe_parser", wxbe, "--json",
                      os.path.join(work, "default_analysis.json")], tk)
    run("disassemble", ["-m", "tools.disasm", wxbe, "--force", "--seed-functions", seeds], tk)
    run("identify functions", ["-m", "tools.func_id", wxbe], tk)
    run("ABI analysis", ["-m", "tools.abi_analysis", wxbe], tk)

    tmp = out + ".tmp"
    # Guard: only ever clear a directory this script names, never an empty or root path.
    if os.path.basename(tmp) != os.path.basename(out) + ".tmp" or len(tmp) < 8:
        raise SystemExit(f"[lift] refusing odd output path {tmp}")
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    run("translate to C", ["-m", "tools.recomp", wxbe, "--all", "--split", "250",
                           "--game-name", "halo2", "--gen-dir", tmp], tk)
    shutil.copyfile(os.path.join(tk, "templates", "runtime", "recomp_types.h"),
                    os.path.join(tmp, "recomp_types.h"))
    for p in ("mmio_pass.py", "tiled_pass.py", "xo_hle_pass.py"):
        run(p, [os.path.join(h2lift, p), tmp], tk)

    got = manifest_of(tmp)
    if a.write_manifest:
        with open(a.manifest, "w") as f:
            json.dump(got, f, indent=0, sort_keys=True)
        print(f"[lift] wrote manifest of {len(got)} files to {a.manifest}")
    elif a.manifest:
        with open(a.manifest) as f:
            want = json.load(f)
        bad = sorted(n for n in set(want) | set(got) if want.get(n) != got.get(n))
        if bad:
            print(f"[lift] {len(bad)} files differ from the tested build: {', '.join(bad[:8])}")
            raise SystemExit("[lift] the lifted code does not match this release; "
                             "is game/default.xbe the original retail disc?")
        print(f"[lift] all {len(got)} files match the tested build")

    if os.path.isdir(out):
        if os.path.basename(out) != os.path.basename(a.out.rstrip("/\\")) or len(out) < 4:
            raise SystemExit(f"[lift] refusing odd output path {out}")
        shutil.rmtree(out)
    os.replace(tmp, out)
    print(f"[lift] {len(got)} files in {out} ({time.time() - t0:.0f} s)")


if __name__ == "__main__":
    main()
