# LttP Resource Editor

A Tauri (Rust backend + React/Vite frontend) desktop app for configuring `resources/` and building the game. This is the only map editor — no Tiled or other external tools are needed.

## Run (development)

    cd editor
    npm install
    npm run tauri:dev      # vite dev server + native window, live frontend reload

## Ship it

    npm run tauri:build    # bundles editor/src-tauri/target/release/bundle/… (.app/.dmg on macOS)

## How it fits together

- `src/main.jsx` — the whole UI (React, no component framework), compiled by Vite. Talks to the backend only through `src/api.js`.
- `src/api.js` — maps the editor's HTTP-shaped API onto Tauri `invoke` commands, and loads `resources/` images as data URLs for the webview.
- `src-tauri/src/main.rs` — the backend. Commands: `scan`, `save`, `create`, `delete`, `upload`, `read_resource`, `build`, `set_project`. JSON files are written with 4-space indentation to match what's already committed in `resources/`.
- The backend only reads/writes files under the open project's `resources/`, and only runs `./build-mac.sh` (Mac only) for its Build button.

## Project folder

The editor edits `resources/` of an **engine repository checkout** (this repo). It finds the project root by:

1. `LTOP_EDITOR_PROJECT` env var (if it contains a `resources/` dir),
2. the last folder chosen in the app (persisted in `~/Library/Application Support/com.ltop.editor/config.json`),
3. walking up from the executable (so dev builds inside `editor/src-tauri/target/` find the repo automatically),
4. walking up from the current directory.

If none match, the app opens with a folder picker; `Change…` in the nav switches projects.

## Tools

- **Rooms** — a visual tile map editor. Both layers render at once (colliders get a red tint so solids are obvious); toggle per-layer visibility and pick the active layer for painting. Tile tools: paint (B), erase (E), fill (G), rect (R), pick (I) — right-drag erases. Marker tools: select/drag (V), spawn (S), place instant (N) / interactive (M) encounters. Zoom, grid toggle, room resize, and editing of `default` (hero, spawn, music, walk sfx) and encounter properties (dialogue, repeat). A leftover Tiled `.tmx` is flagged and can be deleted from the UI.
- **Dialogue** — create/delete dialogue files, edit their lines.
- **Characters** — graphical animation editor: pick `stand`/`move` frames per direction by clicking tiles on the sprite sheet (orange badges show each tile's position in the sequence), drag-free reordering via the insert-point slot, right-click a sequence slot to remove it, and a live preview that plays at the engine's exact frame rates (stand 2 fps, move 12 fps). Width and raw frame-index lists remain editable as text.
- **Music & Files** — upload/delete binary assets (music, audio, ui, tiles, characters). Tiles accept `.png` only.
- **Build** — runs `./build-mac.sh` from the repo root and shows its output (5-minute timeout).