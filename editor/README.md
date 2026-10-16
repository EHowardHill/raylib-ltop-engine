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
- `src-tauri/src/main.rs` — the backend. Commands: `scan`, `save`, `create`, `delete`, `upload`, `rename_asset`, `read_resource`, `build`, `build_web`, `stop_web`, `build_dreamcast`, `play`, `stop_game`, `run_generator`, `headless_check`, `set_project`, `new_game`, `save_slots`, `delete_save`, `list_flags`, `check_code`. JSON files are written with 4-space indentation to match what's already committed in `resources/`.
- The backend only reads/writes files under the open project's `resources/`, and only runs the build scripts (`build-mac.sh` on Mac, `build-web.sh` elsewhere; the web export and the Dreamcast docker build are surfaced from the Build tab with their logs).

## Project folder

The editor edits `resources/` of an **engine repository checkout** (this repo). It finds the project root by:

1. `LTOP_EDITOR_PROJECT` env var (if it contains a `resources/` dir),
2. the last folder chosen in the app (persisted in `~/Library/Application Support/com.ltop.editor/config.json`),
3. walking up from the executable (so dev builds inside `editor/src-tauri/target/` find the repo automatically),
4. walking up from the current directory.

If none match, the app opens with a folder picker; `Change…` in the nav switches projects.

## Tools

- **File** — project-level meta actions: open/switch game, new game (a starter folder), game identity (title/slug/intro), save-slot tools, and one-click Build & Play (the launched dev build is tracked: a new launch replaces it, Quit stops it).
- **Rooms** — a visual tile map editor. Both layers render at once (colliders get a red tint so solids are obvious); toggle per-layer visibility and pick the active layer for painting. Tile tools: paint (B), erase (E), fill (G), rect (R), pick (I) — right-drag erases. Marker tools: select/drag (V), spawn (S), place instant (N) / interactive (M) encounters. Entity tools place objects (O), enemies (K), NPCs (P), and pickups (U). With an NPC or platform selected, its waypoint rail draws on the map: click open ground to extend, drag the circles to shape, right-click to remove. Zoom, grid toggle, room resize, doors/exits/mirror/pit editing, and editing of `default` (hero, spawn, music, walk sfx) and encounter properties (dialogue, repeat, if/unless guards). A leftover Tiled `.tmx` is flagged and can be deleted from the UI.
- **Dialogue** — create/delete dialogue files, edit their lines: structured line cards over the whole scripting vocabulary (flags, gives, warps, choices, cutscene cues including image-scene picks with thumbnails), a branch-tree view, and a credits/`next` header.
- **Enemies** — property forms (stats, behaviors, components, drops, frame lists), sheet upload/replace + rename, and a one-click "Test in gauntlet" sandbox (build + boot the gauntlet room with the enemy spawned beside the hero).
- **Items** — use-action forms over the full vocabulary, plus a subscreen preview rendering the pause grid exactly as the engine draws it.
- **Tiles** — tileset manager + per-tile behavior editor; sheet upload/replace in place and rename (repointing rooms).
- **Object Types** — create/edit/delete the `types.json` compositions over the engine's interaction vocabulary.
- **Characters** — graphical animation frame picker per direction with live preview at the engine's frame rates; sheet upload/replace + rename.
- **Code** — in-window editing of `resources/code/*.cpp`, compile checks with inline errors, and the Enemy API reference rendered alongside.
- **Flags** — the baked world-flag registry (via the generator's `--flags` tooling mode) with provenance and save-safety guidance.
- **Music & Files** — upload/replace into any `resources/` directory (music, audio, ui, tiles, characters, enemies, pickups, effects, objects, fonts, scenes), the placeholder-asset audit (the artist's worklist) with per-row imports, and rename-in-place that rewrites every JSON reference to the renamed asset.
- **Build** — the platform build with its log, web export (build, host `build/web/`, open the browser), the Dreamcast docker build, and a clickable runbook of the smoke checks (generator warnings, both builds, the `--soak` save round-trip).