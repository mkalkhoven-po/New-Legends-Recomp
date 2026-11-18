# New Legends Recomp 0.9.0 (preview)

The first test release: the whole game runs natively on Windows. You need the files from
your own New Legends (USA) disc; see the README.

## What is in it

- All 35 levels load and run: graphics through Vulkan, sound through Windows audio,
  controller, keyboard and mouse input, saving to the `hdd` folder.
- Easy setup: on the first start, choose your disc image (`.iso` / `.xiso`) or a folder with
  the game files. The program checks the version, copies the files and can make a desktop
  shortcut and a Start menu entry.
- Keyboard and mouse play without setting anything up, next to a controller.
- Fullscreen (Alt+Enter or F11) and windowed mode; the original 4:3 picture or stretched.
- Settings file `newlegends.ini`: keys, mouse speed, inverted looking.
- Shaders are kept in the `cache` folder, so a place only stutters on the first visit.

## Known issues

- Nobody has played the whole game through on it yet; levels were tested with automated
  runs (random input and teleporting through each level). Bosses were not tested.
- The compass in "Return to the Soo Kingdom" can show as a garbled square.
- No true widescreen (the game renders 4:3).
- A console window opens next to the game.
