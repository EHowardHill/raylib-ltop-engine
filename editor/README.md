# LttP Resource Editor

A minimalist React web app for configuring `resources/` and building the game.

## Run

    cd editor
    npm start        # or: node server.js

Then open http://localhost:5173

- Zero npm dependencies (Node stdlib server); React + Babel are loaded from unpkg, so the browser needs internet access.
- The server only reads/writes files under `resources/` and only runs `./build-mac.sh` (Mac only).

## Tools

- **Rooms** — edit `default` (hero, spawn, music, walk sfx), encounters (instant/interactive, location, dialogue, repeat), and paint the `decor`/`colliders` tile layers with a palette built from the tileset. Spawn/encounter markers are drawn on the map.
- **Dialogue** — create/delete dialogue files, edit their lines.
- **Characters** — width, stand/move frame lists per direction, sprite sheet preview.
- **Music & Files** — upload/delete binary assets (music, audio, ui, tiles, characters).
- **Build** — runs `./build-mac.sh` from the repo root and shows its output.
