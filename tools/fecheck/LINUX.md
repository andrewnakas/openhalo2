# fecheck and the rigs on Linux

fecheck on Linux runs the same VS2022 x86 `cl.exe` (14.44.35207, SDK 10.0.26100.0) as on Windows,
under Wine, so its objects and verdicts are the Windows ones. The rigs run `h2fe.exe` under Wine.
Windows behaviour is unchanged; everything below is chosen by `sys.platform` / `uname`.

## One-time setup

1. Wine (tested with Wine 10.0, wow64) and Python 3.12 with `unicorn==2.1.4 capstone numpy pillow`
   (the Windows versions; `uv venv -p 3.12` works without root).
2. The compiler: copy a VS2022 BuildTools install from a Windows machine (yours, under the licence you
   accepted there) into one folder, `$FECHECK_MSVC` (default: `msvc/` beside the openhalo2 checkout):

   | From (Windows) | To |
   |---|---|
   | `C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207` | `VC/Tools/MSVC/14.44.35207` |
   | `C:/Program Files (x86)/Windows Kits/10/{Include,Lib,bin}/10.0.26100.0` | `Kits/10/{Include,Lib,bin}/10.0.26100.0` |
   | `C:/Windows/SysWOW64/dbghelp.dll` | `dbghelp.dll` (or `$FECHECK_DBGHELP`) |

   Only this VS2022 toolchain: never MSVC 7.1, the XDK or the RXDK.
3. The paths: `FECHECK_ROOT` (the folder holding `halo2/`; on Linux the E: mount), and optionally
   `FECHECK_DECOMP` and `FECHECK_WORK`. A WORK on a Linux file system is much faster than one on the
   exFAT USB disk; copy `entry_maps.csv`, `extra_symbols.csv` and `callee_maps/` into it (and the
   `rcache_v6/` entries you need). The compiler-made caches (`struct_sizes.json`,
   `storage_bases.json`) are rebuilt.

```sh
export FECHECK_ROOT=/media/$USER/E FECHECK_WORK=~/fecheck-work WINEPREFIX=~/fecheck-wine
python fecheck.py 59e50 --tag _lx
```

Once per new `runtime/` (a fresh clone or worktree), build every object and rebuild the symbol table, or
the users of inferred data (`g_network_configuration`, `async_globals`, ...) are unbuildable:

```sh
python dbuild.py --all --jobs 2 && python symmap.py
```

`wine_cl.py` stands in for `cl.exe` (it turns `/home/...` arguments into `Z:\home\...`, which cl would
otherwise read as options); `undec.exe` (built on first use from `undec.c`) runs Microsoft's dbghelp
`UnDecorateSymbolName` for `coff.undecorate`.

## Rigs

```sh
ln -s "/media/$USER/TOSHIBA EXT" ~/E      # RIG_E must have no spaces
export RIG_E=~/E RIG_PY=~/venv/bin/python RIG_WINEPREFIX=~/h2-rigs-wine
FE_SNAP_ROOT=$RIG_E/halo2/snaps/link3 bash rigs_link.sh $RIG_E/halo2/reports/link3 link2_slayer
```

`rigproc.sh` holds the process and path operations: the game is started with `wine` (the PID survives
the exec chain), killed by PID with its children only, and `userbusy` also counts GTA V and Steam
games (`RIG_OWNER_GAMES`) as the owner playing. The game runs headless (the rig's watchdog / pad /
frame-dump variables select test mode); its renderer is OpenGL 3.3, which Wine passes to the host.
