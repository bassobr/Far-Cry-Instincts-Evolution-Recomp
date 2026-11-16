# Changelog

## 0.1.0 (2026-11-16)

The first public release of Far Cry Instincts Evolution Recomp. It is a fork of
[Far Cry Instincts Recomp](https://github.com/bassobr/Far-Cry-Instincts-Recomp) and runs on its engine as of 0.9.1,
with the fixes made since: the recompiler, the Xbox kernel, the Direct3D 8 and DirectSound HLE, the renderer and all
of its PC features. You need your own copy of the Xbox game *Far Cry Instincts Evolution* (USA/Europe, title ID
`55530060`); see the README.

- **What works:** the game boots through the logos and the title screen, creates and loads profiles, and plays
  the tutorial and the story from the menus. All nine story levels (`20_TUTORIAL` to `30_MOUNTAIN`) were played for
  a few minutes each at 120 fps, with every level change from one to the next and back to the main menu; dying and
  loading the last checkpoint, saving and resuming a checkpoint, quitting to the main menu from the pause menu and
  the trailer after it. Saves go to `data\hdd\` next to the executable, with a backup of the last good save.
- **Evolution's own hooks:** Evolution's code is laid out differently from the first game's, so every
  game-specific hook was found again in Evolution's `default.xbe` and checked in the running game: widescreen (Hor+)
  and field of view, direct mouse look, the menu videos at their own frame rate, the trailer after "Quit to main
  menu", level of detail and view distance, sun shadow range and cascades, the map editor notice, the button icons
  for the keyboard, PlayStation and Switch, the binoculars and scope masks on widescreen, and the experimental
  widescreen HUD.
- **Frame rates up to 120 fps:** all of the first game's fixes for game logic that depended on the frame rate are
  in place and were measured in Evolution's levels at 30 and 120 fps: boats and the jet ski, buoyancy, currents and
  the view sway while swimming, fall damage, drowning, wheeled vehicles, steps and landings, AI paths. See the
  README's chapter "Frame rate and game logic".
- **The AI runs at the console's rate:** at 60 or 120 fps the AI truck in the opening of `21_DUNEA` turned into the
  bends about 2 m early, could get stuck on the rocks for good and left the player on its gun with no way on. The
  AI's chains of goals and signals counted frames; the AI system now updates 30 times per second of game time, as
  on the Xbox, and the truck got through in 5 of 5 runs at 120 fps. `FCI_FRAME_AI_UPDATE=1` brings back the update
  in every frame.
- **Split screen** with 2, 3 and 4 players draws every view in colour. Evolution draws the glare of each view but
  the first into a part of the frame buffer that starts further down or to the right, and those views came out
  black and white or pale.
- **Controllers on a thread of their own (optional, experimental):** with `FCI_PAD_THREAD=1` the controllers are
  read and their rumble is sent on a thread of their own, so a slow controller driver can't hold up a frame;
  `FCI_PAD_POLL_MS` sets how often that thread reads. By default the controllers are read on the game's threads,
  as before: on the Steam Deck the game slowed down more and more after a few minutes with the thread, and the
  cause is still open.
- **Shadow cascades** (`shadow_cascades = on`, off by default) draw the near and the far shadow map in every frame,
  so the shadows of moving objects are no longer a frame old.
- **Languages:** the Evolution disc has English, French, German, Italian and Spanish text and voices; `language`
  picks one.
- **Keyboard:** two movement keys at once move diagonally as fast as straight (5.15 m/s on foot, 2.5 m/s swimming),
  like a fully pushed stick. `FCI_KEY_DIAGONAL_FULL=1` brings back the faster diagonal.
- **Exit Game** in the F10 menu, as in Far Cry Instincts Recomp 0.9: quits to the desktop after a confirmation and
  lets a save in progress finish first.
- **Steam Deck:** detected automatically, with its own defaults: `msaa = 2`, `shadow_scale = 2`, `ao = gtao`,
  `frame_queue = 2`, the console's detail distances. On a Steam Deck OLED the tutorial runs at 90.0 fps with an
  average 1 % low of 87.8 fps. The first start compiles the game code in about 5 minutes.
- **ROG Ally X:** detected automatically, with `msaa = 2` and the PC defaults otherwise. On a ROG Xbox Ally X at
  1920x1080 and 120 Hz the tutorial runs at 119.1 fps with a 1 % low of 106.7 fps.
- The game code is optimised with a PGO profile made from Evolution's own levels (`config/game_code.profdata`:
  function names and counters only, no code).
- **Known issues:**
  - In `23_DUNEC` the stream pushes a figure standing in the creek bed about 40 % faster at 120 fps than at 30 fps.
  - The AI truck in the opening of `21_DUNEA` turns into the bends up to about 2 m earlier at 120 fps than at 30 fps,
    but gets through reliably.
