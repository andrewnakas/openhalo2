# Open Halo 2

Halo 2 (original Xbox, 2004) running natively on a PC. The game's own program code was translated
instruction by instruction into C (a *static recompilation*) and is compiled into this program, beside
a model of the Xbox hardware it talks to.

**You need your own copy of the game.** This download contains no game files: no maps, no sounds,
no textures, no movies. On first start you point it at your Halo 2 disc image and it copies the
files it needs into a `game` folder next to the program.

## Getting started (Windows 10/11, 64-bit)

1. Unzip anywhere you can write to (not `Program Files`). About 5 GB of free space is needed for the
   game files and 1.5 GB for the save/cache folder.
2. Run `halo2_recomp.exe`.
3. When it asks, choose your disc image: a Redump `.iso` or an XISO, or the `default.xbe` of a copy
   you already extracted. The import takes a few minutes and checks that it is the original retail
   disc (the one version this build runs).
4. The first start after that copies maps into the emulated hard disk; it can take several minutes.
   Later starts are quick.

Needs a GPU with OpenGL 3.3. No other installs (the program carries its own C runtime).

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

The recompiled program code in this executable is derived from the retail game and is provided so
that owners of the game can play it on current hardware. See `LICENSE` and `NOTICE` for the licences
of the original parts of this project and the third-party code it uses.
