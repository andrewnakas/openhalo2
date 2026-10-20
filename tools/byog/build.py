"""Generate halo2_recomp.exe on the player's machine from their own disc.

Run by OpenHalo2.exe (tools/byog/launcher.c) on first start, or by hand:

    python\\python.exe tools\\byog\\build.py            (from the release folder)

1. lift.py turns game\\default.xbe into C in build\\gen and checks every file
   against kit\\gen_manifest.json (the code this release was tested with);
2. clang-cl compiles it with the flags of the tested build;
3. lld-link links it with the prebuilt Open Halo 2 runtime in kit\\ into
   halo2_recomp.exe.

Compiling needs Microsoft's C runtime and Windows SDK libraries. They are not
part of this download. An installed Visual Studio or Build Tools (with the C++
workload) is used when present; otherwise xwin fetches them from Microsoft into
toolchain\\msvc after the player accepts Microsoft's licence.

Everything goes to build\\build.log as well. Exit 0 = halo2_recomp.exe written.
"""
import json
import os
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
KIT = os.path.join(ROOT, "kit")
BUILD = os.path.join(ROOT, "build")
GEN = os.path.join(BUILD, "gen")
OBJ = os.path.join(BUILD, "obj")
TC = os.path.join(ROOT, "toolchain")
LOG = None
MS_LICENCE = "https://go.microsoft.com/fwlink/?LinkId=2086102"


def say(msg):
    print(msg, flush=True)
    if LOG:
        LOG.write(msg + "\n")
        LOG.flush()


def fail(msg):
    say("")
    say("BUILD FAILED: " + msg)
    say(f"Full log: {os.path.join(BUILD, 'build.log')}")
    sys.exit(1)


# ---- toolchain -------------------------------------------------------------

def msvc_env():
    """INCLUDE/LIB of an installed Visual Studio / Build Tools with the C++ tools, or None."""
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                           "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if not os.path.exists(vswhere):
        return None
    try:
        path = subprocess.run([vswhere, "-latest", "-products", "*", "-requires",
                               "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                               "-property", "installationPath"],
                              capture_output=True, text=True).stdout.strip().splitlines()
    except OSError:
        return None
    if not path:
        return None
    vcvars = os.path.join(path[0], "VC", "Auxiliary", "Build", "vcvars64.bat")
    if not os.path.exists(vcvars):
        return None
    out = subprocess.run(f'cmd /s /c ""{vcvars}" >nul && set"', capture_output=True,
                         text=True, shell=True).stdout
    env = dict(l.split("=", 1) for l in out.splitlines() if "=" in l)
    env = {k.upper(): v for k, v in env.items()}
    if not env.get("INCLUDE") or not env.get("LIB"):
        return None
    say(f"[build] using Microsoft libraries from {path[0]}")
    return {"INCLUDE": env["INCLUDE"], "LIB": env["LIB"]}


def xwin_env():
    """Microsoft's CRT + SDK fetched with xwin into toolchain\\msvc (asks for the licence once)."""
    root = os.path.join(TC, "msvc")
    stamp = os.path.join(root, "ok")
    if not os.path.exists(stamp):
        say("")
        say("Compiling needs Microsoft's C runtime and Windows SDK libraries (about 85 MB")
        say("download, from Microsoft). They are covered by Microsoft's licence:")
        say("    " + MS_LICENCE)
        say("(Players who have Visual Studio or its Build Tools installed are not asked.)")
        ans = input("Do you accept that licence and want to download them now? (yes/no) ").strip().lower()
        if ans not in ("y", "yes"):
            fail("the Microsoft libraries are needed to build; nothing was downloaded")
        LOG.write(f"licence accepted by the player: {ans}\n")
        xwin = os.path.join(TC, "xwin.exe")
        cache = os.path.join(TC, "xwin-cache")
        say("[build] downloading Microsoft CRT + SDK with xwin ...")
        p = subprocess.run([xwin, "--accept-license", "--log-level", "warn", "--http-retry", "3",
                            "--cache-dir", cache, "splat", "--use-winsysroot-style",
                            "--preserve-ms-arch-notation", "--disable-symlinks", "--output", root],
                           stdout=LOG, stderr=subprocess.STDOUT)
        if p.returncode != 0:
            fail("xwin could not download the Microsoft libraries (network?)")
        shutil.rmtree(cache, ignore_errors=True)
        open(stamp, "w").write("ok\n")
    return {"WINSYSROOT": root}


def toolchain():
    llvm = os.path.join(TC, "llvm", "bin")
    cc, ld = os.path.join(llvm, "clang-cl.exe"), os.path.join(llvm, "lld-link.exe")
    for t in (cc, ld):
        if not os.path.exists(t):
            fail(f"missing {t}")
    # OPENHALO2_USE_XWIN=1 tests the no-Visual-Studio path on a machine that has it.
    env = (None if os.environ.get("OPENHALO2_USE_XWIN") == "1" else msvc_env()) or xwin_env()
    return cc, ld, env


# ---- steps -----------------------------------------------------------------

def lift():
    # A retry after a failed compile or link keeps code that is already lifted and checked.
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import lift as liftmod
    with open(os.path.join(KIT, "gen_manifest.json")) as f:
        want = json.load(f)
    if os.path.isdir(GEN) and liftmod.manifest_of(GEN) == want:
        say(f"[build] lifted code in {GEN} already matches the tested build")
        return
    py = sys.executable
    args = [py, "-I", os.path.join(ROOT, "tools", "byog", "lift.py"),
            "--xbe", os.path.join(ROOT, "game", "default.xbe"),
            "--toolkit", os.path.join(ROOT, "toolkit"),
            "--h2lift", os.path.join(ROOT, "h2lift"),
            "--seeds", os.path.join(KIT, "seed_decomp_functions.json"),
            "--manifest", os.path.join(KIT, "gen_manifest.json"),
            "--out", GEN, "--work", os.path.join(BUILD, "lift_work")]
    p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    for line in p.stdout:
        say(line.rstrip())
    if p.wait() != 0:
        fail("lifting the game code did not finish")


def sysroot_args(env):
    if "WINSYSROOT" in env:
        return [f"/winsysroot{env['WINSYSROOT']}"], [f"/winsysroot:{env['WINSYSROOT']}"]
    return [], []


def compile_all(cc, env, cfg):
    os.makedirs(OBJ, exist_ok=True)
    srcs = sorted(f for f in os.listdir(GEN) if f.endswith(".c"))
    cflags, _ = sysroot_args(env)
    base = ([cc, "/nologo", "/c", "/w"] + cfg["flags"] + ["/D" + d for d in cfg["defines"]]
            + ["/I" + GEN] + ["/I" + os.path.join(KIT, i) for i in cfg["include"]] + cflags)
    penv = dict(os.environ)
    penv.update({k: v for k, v in env.items() if k != "WINSYSROOT"})
    jobs = cfg.get("jobs") or jobs_for_ram()
    say(f"[build] compiling {len(srcs)} files, {jobs} at a time (this is the long part) ...")
    t0 = time.time()
    done = [0]

    def one(src):
        obj = os.path.join(OBJ, src[:-2] + ".obj")
        p = subprocess.run(base + [os.path.join(GEN, src), "/Fo" + obj],
                           capture_output=True, text=True, env=penv)
        done[0] += 1
        if p.returncode != 0:
            return src, p.stdout + p.stderr
        say(f"[build]   {done[0]}/{len(srcs)} {src} ({time.time() - t0:.0f} s)")
        return src, None

    with ThreadPoolExecutor(jobs) as ex:
        errs = [(s, e) for s, e in ex.map(one, srcs) if e]
    for s, e in errs:
        LOG.write(f"--- {s}\n{e}\n")
    if errs:
        fail(f"{len(errs)} files did not compile (first: {errs[0][0]})")
    say(f"[build] compiled in {time.time() - t0:.0f} s")
    return [os.path.join(OBJ, s[:-2] + ".obj") for s in srcs]


def jobs_for_ram():
    """One compile per ~2 GB of free memory, at most one per core."""
    try:
        import ctypes

        class MS(ctypes.Structure):
            _fields_ = [("len", ctypes.c_ulong), ("load", ctypes.c_ulong),
                        ("total", ctypes.c_ulonglong), ("avail", ctypes.c_ulonglong),
                        ("tp", ctypes.c_ulonglong), ("ap", ctypes.c_ulonglong),
                        ("tv", ctypes.c_ulonglong), ("av", ctypes.c_ulonglong),
                        ("ae", ctypes.c_ulonglong)]
        m = MS()
        m.len = ctypes.sizeof(MS)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
        by_ram = int(m.avail // (2 << 30))
    except Exception:
        by_ram = 2
    return max(1, min(os.cpu_count() or 2, by_ram))


def link(ld, env, cfg, objs):
    out = os.path.join(ROOT, "halo2_recomp.exe")
    tmp = out + ".new"
    _, lflags = sysroot_args(env)
    rsp = os.path.join(BUILD, "link.rsp")
    with open(rsp, "w") as f:
        for a in (["/nologo", "/OUT:" + tmp, "/MAP:" + os.path.join(BUILD, "halo2_recomp.map")]
                  + cfg["flags"] + lflags
                  + [os.path.join(KIT, o) for o in cfg["objects"]] + objs
                  + [os.path.join(KIT, l) for l in cfg["kit_libs"]] + cfg["system_libs"]):
            f.write(f'"{a}"\n')
    penv = dict(os.environ)
    penv.update({k: v for k, v in env.items() if k != "WINSYSROOT"})
    say("[build] linking ...")
    p = subprocess.run([ld, "@" + rsp], capture_output=True, text=True, env=penv)
    LOG.write(p.stdout + p.stderr)
    if p.returncode != 0:
        fail("the link did not finish:\n" + (p.stdout + p.stderr)[-1500:])
    os.replace(tmp, out)
    return out


def main():
    global LOG
    os.makedirs(BUILD, exist_ok=True)
    LOG = open(os.path.join(BUILD, "build.log"), "a", encoding="utf-8")
    LOG.write(f"\n==== {time.ctime()} ====\n")
    say("Open Halo 2: building the game program from your own copy of Halo 2.")
    say("This happens once and takes about 10-30 minutes. You can leave it running.")
    say("")
    with open(os.path.join(KIT, "kit.json")) as f:
        kit = json.load(f)
    if not os.path.exists(os.path.join(ROOT, "game", "default.xbe")):
        fail("no game\\default.xbe: start OpenHalo2.exe to import your disc first")
    t0 = time.time()
    cc, ld, env = toolchain()
    lift()
    objs = compile_all(cc, env, kit["compile"])
    exe = link(ld, env, kit["link"], objs)
    with open(os.path.join(BUILD, "stamp"), "w") as f:
        f.write(kit["version"] + "\n")
    shutil.rmtree(OBJ, ignore_errors=True)
    say(f"[build] done in {(time.time() - t0) / 60:.0f} min: {exe}")


if __name__ == "__main__":
    main()
