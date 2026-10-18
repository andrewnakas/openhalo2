# Open Halo 2

Looking to play halo 2? 
https://github.com/andrewnakas/openhalo2/releases

Please support the halo 2 decompolation project as that's the project that we really want to get behind, and i made this project primarily as a helper vehicle to get us to halo 2 full decompilation, so please if you have tokens burning though your claude wallet please lend a hand, if we decompile it that's what unlocked a lot of potential with mods and better porting abilities.  

I'm just scratching the surface but i think this can help us get the project decompiled faster?

https://github.com/kirklandsig/halo2-decompiled


Halo 2 (original Xbox, 2004) running natively on a PC. The game's own program code is translated
instruction by instruction into C (a *static recompilation*) and compiled into a Windows program,
beside a model of the Xbox hardware it talks to.

**You need your own copy of the game, and the recompilation is made from it.** This download
contains nothing from Halo 2: no maps, sounds, textures or movies, and none of the game's program
code. On first start you point it at your disc image; it copies the game files into a `game`
folder, then translates *your* copy of the game's program into C and compiles it on your PC.

## Getting started (Windows 10/11, 64-bit)

1. Unzip anywhere you can write to (not `Program Files`). About 10 GB of free space is needed: the
   game files, the build and the save/cache folder.
2. Run `OpenHalo2.exe`.
3. When it asks, choose your disc image: a Redump `.iso` or an XISO, or the `default.xbe` of a copy
   you already extracted. The import takes a few minutes and checks that it is the original retail
   disc (the one version this build runs).
4. It then builds the game program (`halo2_recomp.exe`) from your copy, once, in a window that shows
   the progress: about 10-30 minutes on a typical PC. The lifted code is checked against the code
   this release was tested with before it is compiled.
   - The compiler (LLVM clang) and the lifter come with the download. Compiling also needs
     Microsoft's C runtime and Windows SDK libraries. If Visual Studio or its free Build Tools (C++
     workload) are installed they are used; if not, the build asks you to accept Microsoft's
     licence and downloads them from Microsoft (about 85 MB) with [xwin](https://github.com/Jake-Shadle/xwin).
5. The game starts. The first start copies maps into the emulated hard disk; it can take several
   minutes. Later starts are quick (always start `OpenHalo2.exe`).

Needs a GPU with OpenGL 3.3 and an internet connection for the one Microsoft download (if Visual
Studio is not installed). The finished program needs no other installs.

### Building it yourself instead

Everything in the download is made from this repository; nothing from the game is in it.
`port/xr/build_kit.cmd` builds the runtime kit (no game code), `tools/byog/build_launcher.cmd` the
launcher, and `tools/make_release.sh` packs them with the lifter and compiler. To work on the code,
`tools/relift.sh` lifts your own `default.xbe` into `port/xr/src/recomp/gen/` and
`port/xr/build_win.cmd` builds the full program with Visual Studio 2022.

## Controls

Controller (XInput): as on the console. Left bumper = white, right bumper = black.

Keyboard and mouse: W A S D move (Shift walks), mouse looks (click into the window first; F1 or
Alt+Tab releases the pointer), left mouse fire, right mouse grenade, middle mouse or Z zoom, Space jump
(A), F or Backspace (B), E or R reload/action (X), Tab switch weapon (Y), Q (white), G (black),
Ctrl or C crouch, Enter or Esc Start, F2 Back, arrow keys d-pad. F11 or Alt+Enter: fullscreen.
F12: screenshot.

## Settings: `halo2.ini`

Create `halo2.ini` next to the program:

```ini
fullscreen = 1       ; start fullscreen
mouse_sens = 1.0     ; mouse look speed
mouse_deadzone = 0.2
volume_db = 0        ; master trim in dB, e.g. -6
log_cap_mb = 64      ; halo2_recomp.log is cut back past this size (0 = never)
```

## Online play (free, peer to peer)

There are no game servers. Players connect directly to each other; a small free relay only
introduces them. Add one of these to `halo2.ini`:

**System link over the internet.** Everyone who uses the same room name sees each other's games
under Multiplayer > System Link, as if on one LAN (up to 10 players in a room):

```ini
room = my-friends-room
```

**Open Live.** The game's own Xbox Live menus (sign in, friends, invites, parties, matchmaking),
run between players instead of on Microsoft's servers:

```ini
open_live = 1
gamertag = YourName
```

Stats and leaderboards are not kept.

## Saves and logs

`save\` next to the program is the emulated Xbox hard disk (profiles, game saves, map cache).
`halo2_recomp.log` describes the last run; if the game ever stops with an error message, that file
says why. Please attach it to bug reports.

## Known issues

- The picture is the console's 640x480, scaled.
- Sound effects that the console runs on its audio DSP (reverb in large rooms) are not produced yet.
- Closing the window waits up to 5 seconds for a save in progress to finish, then quits.
- Windows only for now; macOS, Linux and browser builds are planned.

## Legal

Halo 2 is a trademark of Microsoft. This project is not affiliated with or endorsed by Microsoft,
Bungie or 343 Industries. It ships no game data, and it runs only with a copy of the game you own.

Open Halo 2 does not distribute the game's program code in any form. The recompilation is made on
your PC from your own copy of the game and stays there. Open Halo 2's own code, tools and
documentation are public domain (CC0 1.0, see `LICENSE`):
decompilation projects and anyone else may reuse any of it freely. `NOTICE` lists the third-party
code it uses and its licences.
