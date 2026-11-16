# New Legends Recomp 0.1.0 (preview)

The first test release: the whole game runs natively on Windows. You need the files from
your own New Legends (USA) disc; see the README.

## What is in it

- All 35 levels load and run: graphics through Vulkan, sound through Windows audio,
  controller and keyboard input, saving to the `hdd` folder.
- Fullscreen (Alt+Enter or F11) and windowed mode; the original 4:3 picture or stretched.
- Settings file `newlegends.ini`, including keyboard keys.
- Shaders are kept in the `cache` folder, so a place only stutters on the first visit.
- A check at startup that the game files are present and are the US version.

## Known issues

- Nobody has played the whole game through on it yet; levels were tested with automated
  runs (random input and teleporting through each level). Bosses were not tested.
- The compass in "Return to the Soo Kingdom" can show as a garbled square.
- No true widescreen (the game renders 4:3).
- A console window opens next to the game.
