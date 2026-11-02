# Macro Studio

Record your clicks and play them back frame by frame, with a clean modern menu.

Press **P** in the game to open or close the menu.

## Features

- **Record**: saves every press and release on the exact physics frame, for both players and the platformer left/right buttons.
- **Play**: replays a macro by pressing the buttons for you on the right frames.
- **Library**: browse, load and delete your saved macros without leaving the game.
- **Auto-save**: a finished recording is saved by itself when you complete the level or leave it.
- **Practice mode fix**: when you respawn on a checkpoint, inputs recorded after that checkpoint are dropped.
- **Speed control**: slow the game down to 0.05x to record hard sections, or speed it up to 2x.
- **Status overlay**: a small REC / PLAY indicator with the current frame.
- **Saved settings**: your options are remembered between sessions.

## How to use it

1. Open a level and press **P**.
2. Click **Record**. The level restarts and recording begins from frame 0.
3. Play the level. Finishing it saves the macro for you.
4. Open the **Library** tab, pick your macro and click **Load**.
5. Click **Play** on the **Macro** tab and watch it run.

## Good to know

- Macros are saved as `.gdr2` files, so other bots that support the format can open them.
- The game's physics rate should match the macro's FPS (240 by default), or the frames will not line up.
- Macros only replay correctly on the level they were recorded on.
- Dying while recording starts a fresh attempt. Only your last attempt is kept.
