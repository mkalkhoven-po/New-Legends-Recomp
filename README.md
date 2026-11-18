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
- Your own **New Legends (USA)** disc for the original Xbox, as a disc image (`.iso` or
  `.xiso`, made from your disc) or as a folder with all files copied from it. A PC drive
  cannot read Xbox discs, so you need one of these.
- About 3.5 GB of free disk space.
- An Xbox controller (or any XInput controller), or a keyboard and mouse.

## Installing

1. Unpack the release zip into a folder of your own, for example `C:\Games\New Legends Recomp`.
2. Start `newlegends.exe`.
3. The first time, a setup window opens. Click **Choose disc image** and pick your `.iso` /
   `.xiso`, or **Choose folder with the game files**. The program checks that it is the right
   version of the game and copies the files into a `game` folder next to itself. It can also
   make a shortcut on the desktop and in the Start menu (both ticked).
4. The game starts. From then on, use the shortcut or `newlegends.exe`.

After setup the disc image is no longer needed. If the game files are ever damaged or the
wrong version, the setup window opens again and says what is wrong. To set them up again or
make the shortcuts later, start `newlegends.exe --setup`. Without the window:
`newlegends.exe --install "path\to\New Legends.iso"`.

## Controls

A controller works as on the Xbox. The Black and White buttons are on the right and left
bumpers.

You can also play with keyboard and mouse, without setting anything up. Both work at the
same time as a controller, so you can switch whenever you like.

| Action | Keyboard and mouse | Controller |
|---|---|---|
| Move | W A S D | Left stick |
| Look around | Mouse | Right stick |
| Attack with left hand | Left mouse button | X |
| Attack with right hand | Right mouse button | B |
| Jump | Space | A |
| Parry | Shift | Y |
| Use | E | White |
| Next weapon | Q | Black |
| Change weapon | Mouse wheel, ← → | D-pad left / right |
| Aim (hold) | Ctrl, mouse back button | Left trigger |
| Camera behind you | Middle mouse button, R | Right trigger |
| Inventory | Tab | Back |
| Menu | Escape, Enter | Start |

Jump + left attack is a dash attack; parry + right attack is a super attack. In menus,
the arrow keys choose, Space or Enter confirms and Backspace or the right mouse button
goes back.

While you play, the game window keeps the mouse pointer. **Alt+Tab** gives it back; click
in the window to play on. **Alt+Enter** or **F11** switches between fullscreen and a window.

## Settings

The first start creates `newlegends.ini` next to the program. Open it in a text editor to:

- start in fullscreen, or set the window size;
- choose the picture shape: the original 4:3 with black bars, or stretched to fill the screen;
- choose smooth or sharp scaling;
- set the mouse speed, or turn the up/down direction of the mouse or the right stick around;
- change any key or mouse button (the file lists the names).

Delete the file to get the defaults back.

## Where things are stored

Everything stays in the program folder:

- `game` — the game files from your disc (if setup replaced an old `game` folder, that one
  is kept as `game.old`; you can delete it);
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
