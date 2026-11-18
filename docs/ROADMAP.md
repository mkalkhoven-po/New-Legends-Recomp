# Roadmap

Kept here until the project has a GitHub repository; then these become issues.

## Before the 0.9.0 release

- [ ] Playthrough and listening test by a person (in progress).
- Playtest 1 (Martin, 2026-11-16): saving and loading work.
- [x] Voices are drowned out in gameplay: footsteps and weapons are much louder, and in fights
      voices drop out completely (a talking head moves its mouth, but nothing is heard).
      Fixed: 3D effects came out about 8 dB above speech and music (+3 dB pan law in front and
      no HRTF loss). Now effects, speech and music average -16, -15 and -14 dB. To retest in
      a fight with a talking head.
- [x] Frame rate keeps fluctuating during play. Fixed: the GPU thread spent 15-20 ms a frame
      generating and hashing shader text and hashing every texture per draw, so the game
      dropped between 60 and 30 fps. Shaders are now cached by the registers they read and
      textures are checked once per frame: 3 ms a frame, a steady 60 fps in 01aFlare and
      13Water (`NL_TRACE_FPS`, `NL_PROFILE`).
- [x] Water looks good but shows visible square tiles on its surface. Fixed: the ripple
      simulation renders into 128x128 targets with a quad starting at 0.53125 px (Xbox D3D's
      offset); the first row and column were never written, leaving flat (0,0,0) normals on the
      border of the tiled normal map. Vertices are now truncated to 1/16 px as on the NV2A.
- [x] Aiming in third person is inverted (stick up looks down). The game has no option for it
      and the left stick is correct, so this is the original behaviour. Now flipped by default;
      `InvertAim=1` in `newlegends.ini` restores the Xbox behaviour.
- [x] Sound in videos is louder than in gameplay. The video player (Bink) sets the headroom to
      0 itself, so videos are about 6 dB above music on the Xbox too; menus now match gameplay.
      A video volume setting is a nice-to-have for later (see below).
- [ ] **Mouse and keyboard play out of the box** (built; a person still has to try it)
  - [x] Mouse look (right stick) with a sensible default sensitivity, mouse buttons for attack/use.
        Raw mouse input goes to the camera as its own JoyU/JoyV events each frame (viewport
        input event 0x16DEF0, after UpdateInput), so it skips the right stick filter (0x16DBD0:
        30% dead zone, top speed about 180 degrees a second). Playtest 2 (Martin, 2026-11-18):
        everything works, but turning was far too slow through the stick; now 4000 counts/s
        turns 360 degrees a second, checked with `NL_FAKE_MOUSE` in frame dumps.
  - [x] Keyboard defaults that follow the game's own controller layout (`user.ini`, config 0):
        mouse buttons attack, Space jump, Shift parry, E use, Q / wheel weapons, Ctrl aim,
        Tab inventory, Escape menu.
  - [x] Controller and mouse/keyboard work at the same time, switching freely.
  - [x] Settings for sensitivity and inverting the vertical axis (`[Mouse]` in `newlegends.ini`).
  - [ ] Try it in play: mouse feel and speed, the wheel, buttons in menus (Backspace / right
        button to go back), Alt+Tab and clicking back in.
- [x] **Easy installation** (built into the program, chosen 2026-11-18)
  - [x] Point the program at a disc image (`.iso` / `.xiso`, trimmed or full) or at a folder with
        the game files; it checks default.xbe's SHA-256 and entry.lin before copying, copies via
        `game.partial` and keeps an old `game` folder as `game.old` (runtime/src/setup.cpp).
        Tested with `--install`: from the xiso and from a folder, identical to the original files
        (443 files), plus a damaged file, an empty folder, the wrong version, missing levels.
  - [x] A setup window on the first start (or with `--setup`) instead of an error message:
        choose a disc image or folder, progress bar, stop button.
  - [x] Desktop shortcut and Start menu entry (ticked by default).
  - [x] The release zip no longer has an empty `game` folder or the Python ISO script.
  - [x] Click through the window: file picker, progress, shortcuts, the game starting after
        (Martin, 2026-11-18, from the release zip: all works, extracting is quick).
- [ ] Draft release on GitHub; Martin publishes and makes the repository public.

## Later

- [ ] **Graphics options** (in `newlegends.ini`, later perhaps an in-game menu)
  - [ ] Anisotropic filtering: floors and walls at a steep angle stay sharp. Small job; the
        Vulkan device already enables `samplerAnisotropy`.
  - [ ] Higher internal render resolution (2x, 3x, 4x): the biggest visual gain over the
        upscaled 640x480. Render targets, viewport, clip and the 1/16 px vertex snapping must
        scale along; effects that render into small textures (water ripple simulation,
        reflections, shadows) must keep their own size and still line up.
  - [ ] Anti-aliasing (MSAA), mainly useful at the native resolution.
  - [ ] Brightness / gamma.
- [ ] Video volume setting (nice-to-have): videos play about 6 dB louder than gameplay, as on
      the Xbox. A `VideoVolume` option could scale voices that the video player (Bink) creates
      with headroom 0.
- True widescreen (needs a change to the engine's field of view and picture width; the game
  ignores the Xbox widescreen setting).
- Compass in "Return to the Soo Kingdom" shows garbage (source pointer of the texture is stale
  at upload; root cause in the engine's loader not found).
