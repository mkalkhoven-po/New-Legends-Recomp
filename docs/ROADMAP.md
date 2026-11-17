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
- [ ] Sound in videos is louder than in gameplay. The video player (Bink) sets the headroom to
      0 itself, so videos are about 6 dB above music on the Xbox too; menus now match gameplay.
      Decide whether to add a video volume setting.
- [ ] **Mouse and keyboard play out of the box**
  - [ ] Mouse look (right stick) with a sensible default sensitivity, mouse buttons for attack/use.
  - [ ] Keyboard defaults that suit play on a PC, so nobody has to change keys.
  - [ ] Controller and mouse/keyboard work at the same time, switching freely.
  - [ ] Settings for sensitivity and inverting the vertical axis.
- [ ] **Easy installation**
  - [ ] Point the program at a disc image (`.iso` / `.xiso`) or at a folder with the game files;
        it checks the version and copies or extracts what it needs by itself.
  - [ ] A clear first-run screen instead of a message about a missing folder.
  - [ ] Desktop shortcut (and Start menu entry) created on install.
- [ ] Draft release on GitHub; Martin publishes and makes the repository public.

## Later

- True widescreen (needs a change to the engine's field of view and picture width; the game
  ignores the Xbox widescreen setting).
- Compass in "Return to the Soo Kingdom" shows garbage (source pointer of the texture is stale
  at upload; root cause in the engine's loader not found).
