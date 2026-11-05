# Macro Studio

Record your clicks, play them back frame by frame, and render them to a video, all from one clean menu.

Press **P** in the game to open or close the menu.

## Features

- **Record**: saves every press and release on the exact physics frame, for both players and the platformer left/right buttons.
- **Play**: replays a macro by pressing the buttons for you on the right frames.
- **Render**: plays a macro at a fixed frame rate and encodes it to an `.mp4` video with FFmpeg, including the level music.
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

## Rendering a video

Rendering needs **FFmpeg** (free). It is not included with the mod.

1. Download a Windows build of FFmpeg (for example from gyan.dev) and put `ffmpeg.exe` in the mod's folder. The **Open mod folder** button on the **Render** tab opens it.
2. On the **Render** tab, click **Detect / test FFmpeg**. It should say FFmpeg was found.
3. Load a macro and open the level it was recorded on.
4. Choose a resolution, frame rate, encoder and bitrate, then click **Start render**.
5. When the level ends, the video is saved in the renders folder. Press **P** at any time to stop early.

Render options:

- **Resolution**: match your window, or 720p, 1080p, 1440p and 4K.
- **Frame rate**: 30, 60, 120 or 240 FPS.
- **Encoder**: H.264 or H.265 on the CPU, or the faster NVIDIA, AMD and Intel hardware encoders.
- **Bitrate** and **Preset** for quality and file size.
- **Audio**: the level's song is added automatically, with volume, song offset and a custom audio file option.
- **End delay**: how many seconds to keep after the level is completed.
- **Hide menu while rendering**: keeps the menu out of the video.

## Good to know

- Macros are saved as `.gdr2` files, so other bots that support the format can open them.
- The game's physics rate should match the macro's FPS (240 by default), or the frames will not line up.
- Macros only replay correctly on the level they were recorded on.
- Dying while recording starts a fresh attempt. Only your last attempt is kept.
- Rendering is Windows only. It captures the game window, so use a larger window for a sharper video, and do not resize it during a render.
- Set the game's music volume to 0 while rendering. The video's audio comes from the song file.
