# New Legends Recomp

A native Windows version of **New Legends**, the 2002 action game for the original Xbox.
The game's own program code was translated to PC code ahead of time (a *static
recompilation*). A small runtime stands in for the Xbox: its system software, graphics chip
(through Vulkan), sound and controllers.

This is an unofficial fan project. It contains **no game data**: you need your own copy of
the game (the US release).

> **Preview.** Every level loads and runs in automated tests. Nobody has played the whole
> game through on it yet. Expect rough edges, and please report what you find.

## What you need

- Windows 10 (version 1803 or later) or Windows 11, 64-bit.
- A graphics card with Vulkan 1.3 support and an up-to-date driver (most cards from 2016 on).
- The files from your **New Legends (USA)** disc for the original Xbox.
- An Xbox controller (or any XInput controller) is recommended; the keyboard works too.

## Installing

1. Unpack the release zip into a folder of your own, for example `C:\Games\New Legends Recomp`.
2. Copy **all** files from your New Legends disc into the `game` folder in it, so that
   `game\default.xbe` and the level files (`entry.lin`, `01aFlare.lin`, …) are there.
   - A PC drive cannot read Xbox discs. If you have a disc image (`.iso`) of your own disc,
     extract it with an XISO tool such as *extract-xiso*. With Python 3 installed, the included
     script works too: `python extract_iso.py extract "New Legends.iso" game`
3. Start `newlegends.exe`.

At startup the program checks that the game files are the right version. If they are
missing or different, it tells you what is wrong.

## Controls

A controller works as on the Xbox. The Black and White buttons are on the right and left
bumpers.

The keyboard works as a controller too, also when a controller is plugged in:

| Controller | Key | Controller | Key |
|---|---|---|---|
| Left stick | W A S D | Right stick | I J K L |
| D-pad | Arrow keys | Start / Back | Enter / Backspace |
| A / B / X / Y | Space / E / Q / F | Black / White | C / Z |
| Left / right trigger | Tab / R | Left / right stick click | V / B |

**Alt+Enter** or **F11** switches between fullscreen and a window.

## Settings

The first start creates `newlegends.ini` next to the program. Open it in a text editor to:

- start in fullscreen, or set the window size;
- choose the picture shape: the original 4:3 with black bars, or stretched to fill the screen;
- choose smooth or sharp scaling;
- change any keyboard key (the file lists the key names).

Delete the file to get the defaults back.

## Where things are stored

Everything stays in the program folder:

- `hdd` — the Xbox hard disk: your saved games and the game's own cache;
- `cache` — compiled shaders, so places you have been load without stutter next time;
- `newlegends.ini` — your settings.

## Known issues

- **The first visit to a place can stutter briefly** while its shaders are built. The next
  time they come from the `cache` folder.
- **No true widescreen.** The game only renders in 4:3; *Stretch* in the settings fills a
  wide screen but makes the picture wider.
- **In the mission "Return to the Soo Kingdom" the compass in the corner can show as a
  garbled square.** The rest of the mission is fine.
- A console window opens next to the game. It shows what the program is doing; if something
  goes wrong, the last lines there help with a bug report.

## Reporting a problem

Please include: what you did, what you saw, the last 30 lines of the console window, and
your graphics card and driver version.

## Legal

New Legends is © its respective owners. This project is not affiliated with or endorsed by
them. No game data is distributed; you must own the game.

The project's own source code is under the MIT License (`LICENSE`). That license does not
cover the game or the recompiled game code built into `newlegends.exe`. Licenses of the
open-source parts are listed in `THIRD_PARTY_NOTICES.md`.
