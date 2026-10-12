# Purpose

This is an engine, based on Raylib, that is designed to mimic the functionality of The Legend of Zelda: A Link to the Past.

Your job is to maintain the code within `include`, `src`, `scripts`, and `editor`. Modify the code within `resources` if nescessary. The folders `include_auto` and `src_auto` are not meant to be edited by anything but `generate_assets.py`.

## Build

`build-mac.sh` builds the native binary (Mac, clang++). `build-web.sh` builds a hostable web build (Emscripten).

At no point should the Raylib binary import or parse .json. The purpose of `scripts/generate_assets.py` is to generate .h and .cpp files that correspond to the content of the various .json files found in the source code.

Some fields may be optional - be sensitive to not breaking if these fields are missing.

Both build scripts run the generator, which also mirrors the runtime files from `resources/` into `assets/` (excluding `.json`, `.tmx`, `.tsx`, which are baked into the binary). The binary loads **only** from `assets/` at runtime; `resources/` is the editable source of truth and is never read by the game.

## Global

- All tiles and characters are expected to be 32x32 pixels in size.
- Because of this, X and Y references ire almost exclusively within this idea (e.g. 1x1 = 32x32).

The system keeps a virtual vertical resolution of "224", regardless of the actual resolution.
- It's only locked vertically so it can support widescreen.
- It is meant to simulate the height of a SNES console.

This game is written in Raylib, which means it has many potential supported console outputs. We are developing exclusively on Mac right now, but do not write code that would prevent it from compiling on a system like PSP or Dreamcast.
- The web build uses `-DPLATFORM_WEB` and `emscripten_set_main_loop` (see `src/main.cpp`); keep the desktop loop path inside `#else` guards.

## Controls

- Desktop: arrow keys move, Z is the action button.
- Gamepad: left stick / dpad move, face-down button (A on Xbox, B on SNES mappings) is the action button.
- The action key constant lives in `include/engine.h` (`ACTION_KEY`).

## Dialogue

This engine must support Link to the Past-style dialogue. It appears like this:
- There is a border around text. This is a bitmap, stored in `resources/ui/border.png`. The generator creates it if missing (a 24x24 nine-patch, 8px patches).
- There is no speaker icon or speaker label.
- In the future, there will be action scripting. Avoid this for now.

Dialogue is stored in `resources/dialogue` as .json files.

The .json files for now have no action, only individual text.
- "lines" is a list containing the lines.
- each line has "text" which contains the text.

Behavior:
- Text types out at ~30 characters/second with word wrap.
- Action button skips the current line's typing, or advances to the next line; after the last line the box closes.
- Movement is frozen while dialogue is active.

Sound:
- Sfx for letters appearing on the screen is `resources/audio/message.wav`.
- Sfx for the line being fully drawn is `resources/audio/message_finish.wav`.

## Rooms

Rooms are based on Tiled maps.
- Rooms are stored in `resources/rooms`, as a .json and a .tmx file (same stem; a room without its .tmx is skipped at build time).
- All base tiles are stored in resources/tiles, as a .png and a .tsx file.

Each tiled map has the following layers:
- "colliders": these are tiles that characters cannot cross over. They are rendered just like decor.
- "decor": these are background tiles that characters can walk underneath.

Within the .json file:
- we have "default"

Within "default":
- "hero": Which character is the player character.
- "spawn": If there is no spawn location, place the hero character at this X and Y location. (Tile coordinates.)
- "music": Play the music from `resources/music/` + the filename provided. Do not restart if the same file is already playing.
- "walk_sfx": When the hero is walking, use the sound cue from `resources/audio/` + the filename provided.

Within "encounters":
- These are the actions that are cued depending on hero location.
- "instant": these are ones that cue the moment the hero enters the location.
- "interactive": these are ones that wait for the hero to hit the "action" button.

Both "instant" and "interactive" have the following attributes:
- "location": the X and Y coordinate (tile coordinates; compares against the tile under the hero's feet).
- "dialogue" (optional): the dialogue that is cued.
- "repeat": if true, the encounter can fire multiple times. If false or missing, it is one-shot; one-shot state lives in engine-global memory for the life of the process (survives room reloads).

## Characters

Character sprites are in resources/characters. They are PNG files, accompanied by .json configuration files.

.json configuration files have the following attributes:

- width: (how many tiles wide the image is).
- stand and move: which animation tiles correspond to standing and moving.

Both "stand" and "move" have the following directions: down, right, left, and up.
- Frame lists are indices into the sheet, counting left-to-right, top-to-bottom.
- The generator derives total frame count from the PNG dimensions; the engine clamps out-of-range indices.

## Hero

The hero moves with the controls described above. Right now, we don't support sword or actions. Just movement.
- Diagonal movement is normalized to the same overall speed, and the walk sfx accumulator uses real distance so stepping never doubles up.
- The hero cannot exit the map (position is clamped so the whole 32x32 sprite stays inside).

The virtual "camera" follows the hero around. However, the camera never leaves the parameter of the map. Because of this, the camera tries to stay centered on the character, unless the character is close enough to the edge of the map.

## Editor

`editor/` contains a web application (React + a zero-dependency Node server) for editing `resources/` and running the build. Run with `node editor/server.js` (or `cd editor && npm start`), then open http://localhost:5173. It reads/writes only `resources/` and runs `./build-mac.sh` (Mac only) for its Build button. See `editor/README.md`.
