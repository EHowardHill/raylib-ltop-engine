# Purpose

This is an **open engine**, based on Raylib, for building games in the style of The Legend of Zelda: A Link to the Past. The engine and the game are two separate artifacts:

- The **engine** lives in `include`, `src`, `scripts`, and `editor`. It defines the *vocabulary* — components, conditions, actions, tile rules, interaction sets — and the runtime that interprets them. Engine code must stay agnostic to any particular game: no game concept (a specific item, enemy archetype, object, flag) belongs in engine headers or sources.
- The **game** is the `resources/` folder. A game is *entirely* defined by the contents of its folder: data (json/png/audio), and game-local C++ behaviors in `resources/code/`. **A new resources folder = a new game.** Nothing outside a game's folder should be required to build and play it.

The bundled LttP sample is the first game built on the engine, not the engine's identity. Long-term goal: Solarus-level openness (anyone can build a complete quest), achieved with composition + bake-time codegen + C++ instead of an embedded scripting language.

Your job is to maintain the code within `include`, `src`, `scripts`, and `editor` as *engine* code, and the contents of `resources/` as *game* content. The folders `include_auto` and `src_auto` are not meant to be edited by anything but `generate_assets.py`.

## Design pillars

1. **Bake, don't parse.** The binary never imports or parses .json. `scripts/generate_assets.py` reads the game folder and emits .h/.cpp that the build compiles in. The binary loads only images/audio from `assets/` at runtime. This rule extends to game code: `resources/code/` is *compiled* by the build scripts, never read or parsed at runtime.
2. **Compose, don't hard-code.** JSON describes "what and where"; the engine interprets a fixed vocabulary of components, conditions, and actions to define "how". A new item, enemy variant, or puzzle should be achievable through data alone wherever reasonable. Behavior belongs in engine switch-branch *interpreters over vocabulary terms*, not in per-type branches keyed to game-specific IDs.
3. **C++ is the scripting language.** When the vocabulary genuinely can't express something, the game writes C++ into `resources/code/`, which gets compiled into the binary by the build scripts. No embedded VM (no Lua, Wren, AngelScript). C++ game behaviors get the same debugger, profiler, and console targets as the engine.
4. **Dreamcast is a hard constraint.** Every feature must compile and run under KallistiOS: C++11 subset, no reliance on exceptions or RTTI, plain structs, fixed-size arrays, careful with allocation. Anything that would only work with a scripting VM or dynamic loading is off the table.
5. **Determinism.** All randomness goes through the seeded `Rng` (`engine.h`) — data behaviors and `resources/code/` included. Never call raw random. Always use `frameDt()` instead of raw `GetFrameTime()`.
6. **No control flow in JSON — a hard line.** Condition lists, action lists, trigger tables: yes. Loops, variables, arithmetic, computed state: no. The moment a data-defined behavior needs real logic, it *graduates* to `resources/code/` by design, not by failure. Vocabulary creep (JSON that wishes it were a language) is the biggest long-term architecture risk; resist it.

## The behavior layer model

Every behavior in a game resolves through these layers, in order. Higher layers exist so lower ones stay rare:

1. **Stats/params** — hp, speed, damage, periods, ranges (already data).
2. **Components** — named, parameterized building blocks composed in JSON: movers (wander/patrol/chase/hover/path), attacks (contact/projectile/charge), reactions (stunnable, burnable, freezable, shieldable), on-death (drops, splits). Enemy "archetypes" are presets: named compositions of these.
3. **Trigger vocabulary** — the shared condition/action grammar (see "Trigger vocabulary" below), consumed by dialogue (today), and by items, enemies, objects, doors, and room encounters (migration below).
4. **C++ behaviors** (`resources/code/`) — the sanctioned escape hatch, for what layers 1–3 can't express.
5. **Interpreted bytecode** — only if a concrete need survives layers 1–4, and probably nothing will. If it ever happens: the generator compiles scripts to bytecode at bake and the runtime stays a small deterministic interpreter (a generalization of the existing dialogue interpreter — see "Runtime scripting policy"). Never per-frame execution; event-driven only.

## The game project (resources/)

A game folder has this shape (the bundled sample uses these exact paths):

- `rooms/`, `enemies/`, `characters/`, `dialogue/`, `tiles/`, `objects/`, `pickups/`, `items/`, `audio/`, `music/`, `ui/`, `effects/` — data, as documented in the sections below.
- `code/` — game-local C++ behavior units, compiled into the binary (see "Custom behaviors"). Code snippets are *encouraged* to live in the game folder: if a behavior can't be expressed in data, its C++ belongs here, next to the content that uses it.

**ID ownership.** The generator is the single authority for game-defined IDs. Item IDs, object type IDs, door kinds, chest kinds, grant kinds, behavior preset names, world flags, encounter one-shot slots — all are assigned per game from its data and emitted into `include_auto/assets.h` as `ITEM_*`/`OBJECT_*`/`CHEST_*`/`GRANT_*`/`DOOR_*`/`BARRIER_*` constants. Constants for these must not live in `include/engine.h`; the engine header owns only vocabulary (interactions, use-actions, elements, effects, flags). The generator validates every cross-reference at bake (unknown names, out-of-range ids) and freezes IDs so save files survive content edits.

**Stable IDs and saves.** Saves are per-game: the save blob carries the game's slug (`resources/game.json`, generator-validated, ≤ 15 chars) alongside the magic/version, so a save from one game is never loaded by another. Bumping content that changes stable IDs is a save-schema version bump, treated exactly like the existing `SAVE_MAGIC`/version discipline.

**Multiple games.** Builds and the editor point at one game folder at a time — default `./resources`. A new game is created by making a new folder of this shape; no engine changes required. The generator synthesizes placeholder art/audio for anything missing, so a new game folder is playable (ugly) from day one.

**Budgets.** The engine uses fixed-size entity pools (16 enemies, 32 pickups, etc. — the console-friendly rule). The generator validates that a game's rooms fit the platform budgets and warns at bake instead of letting the engine silently overflow.

## Build

`build-mac.sh` builds the native binary (Mac, clang++). `build-web.sh` builds a hostable web build (Emscripten). `build-dreamcast.sh` builds a Dreamcast disc image (KallistiOS + raylib4Consoles); it cross-compiles inside Docker, so a working Docker daemon (OrbStack) is the only host requirement. The first run builds the toolchain image (`ltop/dreamcast`, from `scripts/console/dreamcast.Dockerfile`; pull + raylib build, cached afterwards), and produces `build/dreamcast/ltop.elf` (dcload-able) plus `build/dreamcast/ltop.cdi` (self-boot disc, run in an emulator or burn to CD-R). The assets are packed into the romdisk; writing save files on hardware still needs a console path strategy (/ram or VMU) — `scripts/console/dreamcast_shim.c` currently backs raylib's `FileExists` with KOS `fs_open`.

Gamecube and N64 were evaluated and rejected as targets: their raylib ports (raylib4Consoles) ship without the audio module because miniaudio has no backend on that hardware, and a target that can't carry the engine's full feature set isn't worth a sanity build. Dreamcast's port has real audio — its bundled miniaudio talks to the KOS AICA driver directly, and the build links `-lpthread` (KOS pthreads addon) for it. Don't reintroduce those platforms (or new ones) without full feature parity.

Because the console build compiles against the raylib port's own `raylib.h` (not the vendored one), `include/engine.h` includes `<raylib.h>` with angle brackets; the include order in each build script picks the header that matches the library being linked. Keep the engine's raylib calls compatible across header versions (no APIs newer than the oldest raylib the console ports track).

Both build scripts run the generator against the active game folder (default `resources/`; the scripts accept a game path/env so another game folder can be built without moving anything), which also mirrors the runtime files into `assets/` (excluding `.json`, which is baked into the binary). The binary loads **only** from `assets/` at runtime; the game folder is the editable source of truth and is never read by the game. Build scripts additionally compile every `resources/code/*.cpp` into the binary alongside the engine sources — game code is first-class at build time and identical on every target.

Some fields may be optional — be sensitive to not breaking if these fields are missing.

## Global

- All tiles and characters are expected to be 32x32 pixels in size.
- Because of this, X and Y references are almost exclusively within this idea (e.g. 1x1 = 32x32).

The system keeps a virtual vertical resolution of "224", regardless of the actual resolution.
- It's only locked vertically so it can support widescreen.
- It is meant to simulate the height of a SNES console.

This engine is written in Raylib, so it has many potential supported console outputs — but the supported build targets are exactly: Mac (`build-mac.sh`), Web (`build-web.sh`), and Dreamcast (`build-dreamcast.sh`). Do not write code that would prevent the Dreamcast build from compiling; don't add other console targets unless their raylib port has full feature parity (audio included).
- The web build uses `-DPLATFORM_WEB` and `emscripten_set_main_loop` (see `src/main.cpp`); keep the desktop loop path inside `#else` guards (the Dreamcast uses that desktop path).

Frame timing: every gameplay system scales by the real frame delta, so gameplay speed is framerate independent. Always use `frameDt()` (`engine.h`) instead of raw `GetFrameTime()` — it clamps the delta to 1/20s so pathological one-frame gaps (window drags, alt-tabs, debugger pauses) can't teleport entities across the map.

Engine code style is deliberately plain — C++11, plain structs, fixed-size arrays, no exceptions, no RTTI, no template metaprogramging — because the Dreamcast toolchain is a limited gcc and the console budget is real. `resources/code/` follows the same rules.

## Controls

- Desktop: arrow keys move, Z is the action button, X is the sword (tap = swing; hold to charge a spin attack), C is the item (boomerang), V is the dash (pegasus boots), Left Shift raises the shield (hold), Enter is pause.
- Gamepad: left stick / dpad move, face-down button (A on Xbox, B on SNES mappings) is the action button, face-left is the sword, face-up is the item, right trigger is dash, left trigger raises the shield, middle-right is pause.
- Logical buttons are defined as `InputBinding`s in `include/engine.h` (`BINDING_ACTION`, `BINDING_SWORD`, `BINDING_ITEM`, `BINDING_DASH`, `BINDING_SHIELD`, `BINDING_PAUSE`); every input check goes through `bindingPressed`/`bindingHeld` so remapping only touches that table. Bindings are engine-level (input is not game content), but a game may want remappable schemes later — keep the table as the only seam.

## Trigger vocabulary (shared conditions & actions)

The engine's condition/action grammar, shared by every scripted surface. It exists today as the dialogue mini-language (`src/dialogue.cpp`); the migration generalizes it into one vocabulary consumed by dialogue (today) and by items, enemies, objects, doors, chests, and room encounters (next). One grammar, many hosts:

- **Conditions**: world flag set/clear, item owned/not owned, extensible per host (an item's use-action may condition on ammo; a door may condition on enemies-dead or a switch group).
- **Actions**: set/clear flags, give item/quantities, play sfx, switch music, goto/advance, warp, grant (chest payloads), and host-specific effects.

Semantics carried over from dialogue: conditions on a line/trigger that fail mean the trigger *executes nothing* (skip, don't half-apply); actions apply in listed order when the trigger fires; one-shot triggers remember firing (save-persisted where the host says so).

Hard line (restated because it governs this vocabulary): conditions and sequential action lists only. No loops, no variables, no arithmetic, no computed state in JSON. A trigger that needs logic becomes a `resources/code/` behavior.

## Dialogue

This engine must support Link to the Past-style dialogue. It appears like this:
- There is a border around text. This is a bitmap, stored in `resources/ui/border.png`. The generator creates it if missing (a 24x24 nine-patch, 8px patches).
- There is no speaker icon or speaker label.

Dialogue is stored in `resources/dialogue` as .json files, and is the first and most complete consumer of the trigger vocabulary.

The .json files:
- "lines" is a list containing the lines.
- each line is either a plain text string or an object whose "text" contains the text, plus optional scripting fields (see below).

Action scripting, per line (all fields optional):
- "label": names the line as a `goto` target (within the same file).
- "if" / "unless": world flag name; the line is only shown when the flag matches. Skipped lines execute nothing.
- "if_item" / "unless_item": item name (resolved by the generator to the game's item IDs); the line is only shown when the item is owned (or not). Same skip semantics as flag conditions.
- "give": item name; the item is granted to the inventory when the line is SHOWN (queued as a request the game drains: ownership + a "GOT <ITEM>" toast + `item get 1.wav`). Guard it with "unless_item" so an NPC only hands an item over once.
- "set" / "clear": world flag names (list); applied when the line is SHOWN.
- "sfx": audio file from `resources/audio/`, played when the line is shown (lazy-cached).
- "music": music file from `resources/music/` switched when the line is shown; "" stops the bgm.
- "goto": label to jump to when the line completes (action press after typing).
- "warp": {"room": "<room file or stem>", "tile": [x, y]} — queued when the line completes; the game warps once the dialogue closes (room loads at the tile, fade in).
- "choice": a list of options (text plus optional set/clear/sfx/goto, and the shop fields "cost"/"give"). After the line types out, the action press opens the list; up/down moves the cursor, action selects. The chosen option applies its set/clear/sfx and goes to its goto (or advances). With "cost": N, the pick checks the wallet first — an unaffordable option rattles with the error cue and nothing applies (the list stays up); an affordable pick deducts the rupees and then applies. With "give": <item>, the selection grants the item through the same drain as line gives (ownership + toast + `item get 1.wav`), which is how shop purchases hand goods over.

World flags: flag names are collected by the generator across every game file (first-use order) and baked per game as `WORLD_FLAG_NAMES` / `WORLD_FLAG_COUNT`; the store is the global `gWorldFlags[]` array. Flags are game-defined IDs owned by the generator; flag names like "crystal_red" are game content, not engine concepts. Flags are referenced by name in game files; the generator resolves them to indices with warnings on typos. Some flags are engine-registered rather than written by hand: boss rooms auto-register `boss_defeated_<room>` (set at the victory) and shrine objects register their "flag" extra — both usable by name anywhere flags are.

Behavior:
- Text types out at ~30 characters/second with word wrap.
- Action button skips the current line's typing, or advances to the next line; after the last line (or a warp) the box closes.
- A blinking continuation arrow sits in the box's bottom-right corner once a line is fully typed (the waiting state; hidden while a choice list is up).
- Movement is frozen while dialogue is active.

Sound:
- Sfx for letters appearing on the screen is `resources/audio/message.wav`.
- Sfx for the line being fully drawn is `resources/audio/message_finish.wav`.

## Rooms

Rooms are edited entirely in the web editor — no external map tool.
- Rooms are stored in `resources/rooms` as a single .json file per room.
- All base tiles are stored in `resources/tiles` as a single .png (a grid of 32x32 tiles; the tileset's columns and tile count are derived from the image dimensions at build time).

Each room .json contains:
- "width" and "height": the map size, in tiles.
- "decor": a flat array of width*height tile ids. These are background tiles that characters can walk underneath.
- "colliders": a flat array of width*height tile ids. These are tiles that characters cannot cross over. They are rendered just like decor (on top of it).
- "upper" (optional): a flat array of width*height tile ids for the decor-above layer (tree tops, archways). Rendered over the hero and entities, inside the camera transform, after everything that stands — so walking under a canopy tiles over the hero. Missing layer = all empty.
- "dark" (optional, top level): true renders the room in darkness — a lightmap (ambient gloom + additive radial lights) multiplied over the world each frame. Light sources: the hero's lantern (wide radius; ~1 magic half drained per 4 seconds while in dark rooms, shrinking to an ember when dry) and lit torches (warm flickering pools). The HUD stays lit. The gradient sprite is the generator placeholder `resources/effects/light.png` (opaque radial falloff).
- "weather" (optional, top level): `"rain"`, `"storm"`, or `"fog"` — a presentation layer over the world in screen space (under the HUD): rain streaks with a wind slant (storm = rain + fog-weighted air + lightning: `thunder.wav` + a white screen flash on a seeded-rng cadence). Pairs with the room ambient loop (e.g. rain weather + `rain.wav` ambience).
- "tint" (optional, top level): `[r, g, b]` (0-255) — an ambient tint multiplied over the world each frame (255 each = neutral/none). `"night": true` is a preset blue-dim tint. Composes with dark rooms (tint applies over the lightmap).
- Tile ids are 0-based frame indices into the tileset sheet, **plus 1**; 0 means empty.

Tile behavior metadata lives in `resources/tiles/tiles.json` (optional):
- "tiles" maps tile ids (as string keys, same 1-based ids rooms use) to properties.
- "pit": true — a hole; the hero falls in (shrink animation, half heart of damage, respawn on the last safe tile stood on).
- "damage": N — half-hearts lost on contact, with directional knockback and invincibility frames.
- "cuttable": true — sword swings cut the tile, revealing "cut_result" (a tile id, 0 = empty); cutting plays a rustle sfx and rolls a drop.
- "water": "deep" — deep water; without flippers the hero flinches in (splash rings, `fall in water.wav`) and hops back onto the last safe tile, no damage. "shallow" — wading: movement slowed, footsteps switch to `walk water.wav`.
- "ice": true — slippery: input accelerates a momentum vector, releasing the direction coasts with weak friction, walls stop the slide.
- "push": [dx, dy] — conveyor belt / water current / wind drift in px/s, applied to the hero while standing on the tile (stacks across the decor + collider layers, clamped). A current tile usually combines it with `"water": "shallow"`.
- "ledge": "down"|"right"|"left"|"up" — a one-way vault-over lip facing that direction. Ledge tiles belong in the colliders layer (they block movement); pressing into them along their facing makes the hero hop two tiles over the tile itself (arc + ground shadow + landing dust + `land.wav`). From every other side they simply block.
- "bombable": true — cracked wall/floor: bombs blast it open, leaving "bomb_result" (0 = blown-open hole). Bombable walls live in the colliders layer; both layers are runtime-mutable copies now (blasts open passages; grass cutting changes decor). Blasted tiles are recorded in a registry that persists in the save, so blown-open passages stay open across sessions.
- "heal": true — fairy-fountain water: standing on the tile slowly refills hearts and magic (`heart.wav` ticks + rising sparkles).
- "dig": true — soft soil: the shovel (an item composing use-kind "shovel") turns the tile over with a dirt burst and a drop roll; the room's `"buried"` list surfaces its items when their spot is dug (once per visit).
- The generator bakes this into `TILE_META`/`TILE_META_COUNT` (`include_auto/assets.h`); `Room::tileMetaAt` combines the decor + collider layers at a tile, `Room::cutTileAt` cuts and `Room::bombTileAt` blasts the runtime copies. Tile behavior flags are engine vocabulary — games compose them, the engine interprets them; new tile *behaviors* are engine features, new tile *content* is data.

Within the .json file:
- we have "default"

Within "default":
- "hero": Which character is the player character.
- "spawn": If there is no spawn location, place the hero character at this X and Y location. (Tile coordinates.)
- "music": Play the music from `resources/music/` + the filename provided. Do not restart if the same file is already playing.
  - Caution: music loads synchronously through raylib's tracker parser. A corrupt module file hangs the game before the first frame (no window, no error). Test new music files by running the game before committing them.
- "walk_sfx": When the hero is walking, use the sound cue from `resources/audio/` + the filename provided.
- "ambient" (optional): a looping ambience file from `resources/audio/` (birds, rain), streamed and played quietly under the bgm with the same no-restart convention (same loop keeps playing across reloads; a room without it fades the ambience out). The generator synthesizes placeholder `birds.wav`/`rain.wav` loops when missing.
- Jingle convention: fanfares (chest opens, boss defeats) and the secret sting (`secret.wav`, when a bomb blasts a passage open) duck the bgm — the music stream pauses while the jingle plays and resumes exactly where it left off once it finishes (`playJingle` in `src/main.cpp`).

Also at the top level:
- "doors" (optional): `{"north"|"south"|"east"|"west": {"kind": <kind>}}` — doors gate the map-graph edges. Door *kinds* are game-defined ids resolved by name at bake; each kind's *rule* is data baked into `DOOR_RULES` (a `DoorRule`: startsOpen / pushOpen / keyCost 0-1-2 / bombable / liveEnemiesDead), and the engine consults the table — there are no per-kind code branches. While a boss lives in the room, every edge stays shut regardless of kind (arena lock). Current kinds: "open" (always passable), "closed" (pushes open from either side, `door open.wav`), "locked" (consumes a small key, `use key.wav`), "big" (consumes the big key), "bombable" (a bomb blast near the wall midpoint blows it open, `bombable wall.wav`), "shutter" (open only while no enemies live in the room — the classic fight lock). Opening a door syncs the reciprocal door in the linked room (it's the same door from both sides), and opened doors persist in the save. Doors render at their wall midpoint: a dark archway when open, a slab (keyhole plate / crack marks / shutter slats) when closed; pressing into a closed-but-openable door opens it with a cue first, then the next press scrolls through.
- "exits" (optional): `{"north": "<room>", "south": ..., "east": ..., "west": ...}` — the map graph. Values are room file names (`"02.json"`) or stems (`"02"`), resolved by the generator to `ROOMS[]` indices baked on `RoomConfig` (`exitNorth/South/East/West`, -1 = walled). Walking off a linked edge glides the hero into the next room.
- "enemies": a list of `{"name": <enemy>, "location": [x, y]}` entries (tile coordinates). The generator bakes these into `EnemySpawn` sets; the engine spawns the room's set on entry. Defeated enemies stay dead until the room reloads.

Within "encounters":
- These are the actions that are cued depending on hero location.
- "instant": these are ones that cue the moment the hero enters the location.
- "interactive": these are ones that wait for the hero to hit the "action" button.

Both "instant" and "interactive" have the following attributes:
- "location": the X and Y coordinate (tile coordinates; compares against the tile under the hero's feet).
- "dialogue" (optional): the dialogue that is cued.
- "repeat": if true, the encounter can fire multiple times. If false or missing, it is one-shot; one-shot state lives in engine-global memory for the life of the process (survives room reloads).
- Encounters are the room-level host of the trigger vocabulary: their cue payload (dialogue, sfx, flags, warps) generalizes to the shared action list during the migration.

## Characters

Character sprites are in resources/characters. They are PNG files, accompanied by .json configuration files.

The web editor is the intended way to fill these in: it shows the sheet with a grid and frame indices, lets you click tiles to build each stand/move animation per direction, previews the animation at the engine's frame rates (stand 2 fps, move 12 fps), and keeps the raw index lists editable as text.

.json configuration files have the following attributes:

- width: (how many tiles wide the image is).
- stand and move: which animation tiles correspond to standing and moving.

Both "stand" and "move" have the following directions: down, right, left, and up.
- Frame lists are indices into the sheet, counting left-to-right, top-to-bottom.
- The generator derives total frame count from the PNG dimensions; the engine clamps out-of-range indices.

## Enemies

Enemy definitions live in `resources/enemies/<name>.json`, next to their sprite sheet (`<name>.png`, or an "image" override to reuse another sheet — how variants are made). The generator bakes them into `EnemyConfig`/`ENEMIES` tables; rooms reference enemies by name.

.json attributes:
- "width", "stand", "move": same shape as character configs (32x32 frames).
- "color": [r, g, b] only used to synthesize the placeholder sheet if the png is missing.
- "stats":
  - "hp": hits before death.
  - "speed": walk speed, px/s.
  - "damage": contact damage in half-hearts.
  - "behavior": "" / "soldier" / "octorok" / "boss" / "custom:<name>" — **preset names**, i.e. named compositions of engine components. "" is a plain wanderer. "soldier" = patrol mover + sight-radius charge + wall-impact stun. "octorok" = wander-with-pauses mover + timed projectile ("shoot_period"). "boss" = arena boss (below). "custom:<name>" delegates the AI to a registered game-code behavior (see "Custom behaviors").
  - "sight": charge radius for soldiers (px).
  - "shoot_period": seconds between rock volleys for octoroks.
  - "components" (optional, overrides the preset): {"charge": {"sight", "speed_scale", "stun_on_wall"}, "shoot": {"period"}, "mover": {"walk_time", "turn_chance"}, "phases": [{"hp_below", "speed_scale"}, ...], "boss": bool, "strike_immune": bool, "custom": "<name>"}. "strike_immune" makes direct hero strikes (sword/spin/dash/hammer) bounce off — only projectile hits land, which is how reflectable-magic fights work.
  - "drops" (optional): {"heart": 20, "rupee": 11, ...} — a per-enemy weighted drop table over pickup kinds; absent = the engine default roll.

The baked `EnemyConfig` carries component *parameters* (chargeSight, chargeSpeedScale, chargeStunOnWall, shootPeriod, walkSegment, turnInPlace, the boss phase table, customBehavior name, the drop table) — no archetype ids exist at runtime. `src/enemy.cpp` is the interpreter: charge behavior keys off `chargeSight > 0`, shooting off `shootPeriod > 0`, boss phase speed off the ascending-hpBelow phase table, and per-enemy drops roll from the baked table. A new enemy that fits existing components is pure data; genuinely novel AI is a `resources/code/` behavior.

Runtime (`src/enemy.cpp`): lifecycle modes idle/walk/pause/charge/stun/frozen/hurt; sword hits are resolved single-hit-per-swing via the hero's swing id; death spawns a cloud poof plus a drop roll. Defeated enemies stay dead until the room reloads. Sample enemies (resources/enemies/): soldiers (patrol + charge; blue variant via config reuse), octoroks (walk + timed rock spit), the phased-charge boss knight, the zora — an aquatic ambusher written as the sample `custom:` behavior (submerged lurk, surface, spit, dive) — and the wizard boss, the reflectable-magic archetype: `custom:wizard` teleport-volley choreography plus `"strike_immune": true`, so only projectiles the hero bats back (sword swings deflect incoming projectiles; deflected projectiles strike enemies) can land.

Arena bosses: `"behavior": "boss"` (or `"boss": true`) in an enemy's stats. Bosses draw at 1.5x with a bigger hitbox, charge in **phases** — a data table of `{hp_below, speed_scale}` thresholds baked from the preset (faster + shorter recovery as health drops; first threshold matched wins) — and while one lives its room seals — every edge behaves shutter-locked until the fight is won. The HUD shows a boss health bar during the fight. Defeat plays the big sequence (`boss dies.wav`, triple explosion, screen shake + white screen flash, the fanfare ducking the bgm), releases the doors, and a heart container rises through the item-get flow; bosses stay dead across reloads and the save (defeat is banked immediately). **Intro:** entering a room with a live boss holds the world ~1.5s behind a letterbox that names the boss (`boss hit.wav` sting). **Story flags:** every boss room auto-registers a `boss_defeated_<room>` world flag, set at the victory — dialogue branches on it by name, and it persists in the save like any flag. Boss *choreography* beyond parameter phases (teleport patterns, synced projectile ballets) is the canonical example of layer-4 content: a `resources/code/` behavior.

## Objects

Interactive world objects (pots, bushes) are defined per room and baked like enemy spawns. **Object types live in `resources/objects/types.json`**: each type has a name, a sprite image, and an `interactions` list composing the engine's interaction vocabulary (`OBJINT_*` in `engine.h`: solid, floor, liftable, cut, dash_smash, pushable, pullable, sinks, weight, switch, barrier, readable, chest, torch, warp, crystal, leaves, two_frame, hides, hammer_smash, heavy, lever, platform, hookable, powder). The engine branches on the baked interaction mask, never on type identity — a game-defined type behaves like the interactions it composes. The generator falls back to a baseline type set when the file is missing, so a fresh game folder is playable. Placeholders are synthesized when images are missing.

Room .json:
- "objects": a list of `{"type": <name>, "location": [x, y]}` entries (tile coordinates). Extras are consumed by interaction, not type name — a type composing "switch" takes "mode"/"link", one composing "chest" takes "kind"/"grant", and so on. The generator bakes `ObjectSpawn` sets (type index + aux payloads); the engine spawns the room's set on entry, so smashed pots are back on the next visit (LttP behavior).
- Sample types (see resources/objects/types.json): "pot" (liftable/throwable, dash- and hammer-smashable), "bush" (liftable/throwable; sword- and hammer-cuttable), "block" (pushable and pullable), "switch" (floor plate), "barrier" (linked gate), "sign" (readable), "chest", "torch", "warp" (step-on warp pad), "crystal" (crystal switch), "rock" (hammer-smashable), "big_rock" (heavy — liftable only with the gloves' `{"lift": true}` passive), "statue" (pushable/pullable, sinks onto latch switches like blocks), "lever" (grab-pull toggles its linked barriers), "platform" (moving platform on a tile-waypoint rail), "hook_post" (the hookshot latches it and reels the hero in), "somaria_block" (the Somaria's conjured puzzle block), "shrine" (magic powder awakens it — sets its story flag).
- Switch extras: "mode" — "hold" (active while weight is on the plate), "latch" (stays pressed once triggered), or "timed" (holds ~3 seconds after the weight leaves — dash-and-run puzzles); "link" — link group id shared with the barriers it controls.
- Barrier extras: "link" — the group id of the switches/torches that lower it; "color" — "blue" or "red" makes it a crystal barrier that follows the crystal-switch state instead of link groups (raised while the crystal is the other color).
- Sign extra: "dialogue" — the dialogue file it shows.
- Lever extra: "link" — the barrier link group a grab-pull toggles (pull flips the handle and the group follows like a switch group; a re-pull flips it back). The lever never travels — the hero does the pull stumble.
- Platform extra: "path" — tile waypoints (`[[x, y], ...]`, looped): the platform glides its rail at raft speed, carrying whoever stands on it. Riding a platform suppresses tile hazards — the platform is the ground underfoot, so a raft over deep water carries the hero without a swim. Platforms are floor-level (non-solid, drawn under entities).
- Pot extra: "hides" — enemy name; lifting the pot reveals an ambush (the enemy spawns from under it; shutter-style rooms stay sealed while it lives).
- Shrine extra: "flag" — the world flag name a magic-powder sprinkle sets when it awakens the shrine (resolved through the shared registry, like dialogue flags).
- Warp extra: "warp" — {"room": <room file/stem>, "tile": [x, y]}; the room resolves to a `ROOMS[]` index at bake.
- Chest extras: "kind" — "normal", "locked" (consumes a small key), or "big" (consumes the big key); "grant" — {"kind": "item"|"rupees"|"arrows"|"bombs"|"magic"|"heart_container"|"key"|"big_key"|"flag", "item": <item name> (for "item"), "flag": <flag name> (for "flag" — sets a story flag, e.g. unlocking an item upgrade), "amount": N}. Grant kinds are trigger-vocabulary actions (chests are another host of the same grammar).
- "pickups" (top level): a list of `{"kind": "heart"|"rupee"|"rupee5"|"arrow"|"bomb"|"magic", "location": [x, y]}` entries — items lying in the world, spawned fresh on every room entry like LttP field hearts.
- "buried" (top level): the same shape as "pickups", but invisible until the shovel turns the buried tile over — the item surfaces once per visit (dug spots stay empty until reload).

Runtime (`src/main.cpp`):
- Ground-state objects are solid: `Room::setSolidExtras` hooks their footprints into `solidBox`, so the hero, enemies, thrown objects, and projectiles all collide with them like tile colliders. Floor switches are the exception: they're walkable plates.
- Lift: the action button next to a liftable object (pot/bush) starts `HERO_LIFT` (brief hoist pause, `lift.wav`); the object glides from its tile to over the hero's head and the hero enters carrying mode. Pots with a "hides" enemy pop it out at the lift (`small enemy hit.wav` + poof).
- Carrying: the object rides overhead; sword, spin, dash, and the boomerang are locked out. Getting hurt, falling, splashing, or dying smashes the carried object (poof + drop roll).
- Throw: the action button hurls the object along the facing (`throw.wav`): a gravity arc with a ground shadow. It smashes on the first enemy hit (damage + knockback, poof + drop), on walls, or on touchdown (`break.wav`, poof + drop roll) — landing in water just splashes (`item in water.wav`, gone without a drop).
- Smashing sprays particles: pot shards, bush leaves, grass chunks (code-drawn chips with gravity, shade-jittered).
- Bushes are cuttable: a sword swing/spin overlapped by the arc smashes standing bushes like cut grass (rustle + leaves + drop), once per swing. A pegasus dash shatters pots and bushes it plows into (smashed pre-collision, so the dash never crashes into them).
- Push blocks: walking face-first into a block for ~0.4s slides it one tile (`move object.wav`) if the destination is clear. Sliding onto a latch switch sinks the block into the floor and presses the switch forever.
- Switches: `button.wav` on press. Hold plates are active while the hero or a resting block is on them; latch plates stay active once triggered. All states are room-local (reset on reload; permanent states pending the save system).
- Barriers: raised (solid) until a switch in their link group is active, then they sink into the floor (`door open.wav`/`door close.wav`) and stop blocking. Colored barriers (crystal barriers) ignore link groups: blue lowers while the crystal state is blue, red while it's red, and touching a raised one shocks the hero (half heart + knockback).
- Crystal switches: solid posts with a floating gem (blue = default state, red after a strike; `crystal.wav` + a sparkle burst + a brief blue screen shimmer on toggle). Any sword swing/spin/dash that reaches the crystal flips the state — which raised/lowered crystal barrier swaps follows. The state is a world flag ("crystal_red" — game content, not an engine constant), so it persists in the save like story progress.
- Signs: solid; the action button in front of one opens its dialogue.
- Torches: solid posts with a "link" group id. Fire bolts light them on contact (`fire.wav`); once lit they burn with a flickering flame forever (this visit) and count like latched switches in their link group — barriers tied to that group lower while any linked torch burns.
- Warp pads: walkable floor plates (a pulsing rune pad, placeholder `warp.png`). Stepping onto one plays `warp.wav`, loads the target room at the target tile (fade in), and banks the save ("SAVED" toast) — the same treatment dialogue warps get. A short cooldown keeps an arrival pad from re-triggering.
- Chests: solid, two-frame sheet (closed/lid open). The action button opens one: locked chests rattle (`error.wav`) without a small key, big chests without the big key; otherwise the key is consumed (`chest open.wav`) and the item-get sequence plays — the grant rises from the chest to over the hero's head with sparkle rays and a fanfare (`item get 2.wav`, ducking the bgm until it finishes), then the description dialogue shows ("You got the BOW!"-style; the world is frozen until it closes). Grants apply to the inventory (`give`/quantities/heart container +1 heart/small key/big key). Opened lids stay open across room reloads (process-lifetime registry keyed by room+slot; a real save schema comes with the save system). Key counts show in the HUD and on the item subscreen.

## NPCs

Townsfolk are character-driven (they use `CharacterConfig` sheets — 4-column stand/move animation frames, same shape as the hero) and live in each room's `"npcs"` list:

- `{"character": "<name>", "dialogue": "<file>", "location": [x, y], "wander": true|false, "path": [[x, y], ...], "if": "<flag>", "unless": "<flag>"}` — "wander" picks idle vs stroll; "path" is a list of tile waypoints the NPC walks in a loop (beats "wander"; blocked waypoints are skipped after a beat so a shove only stalls the stroll); "if"/"unless" gate the spawn on world flags (story-state presence, checked at room entry).
- Placeholder character sheets are synthesized for characters without art (name-hued cloak blob, 4 columns x 2 rows); the generator never overwrites real art.

Runtime (`src/main.cpp`):
- Idle NPCs turn to face the hero when nearby; wanderers stroll in random directions with pauses (blocked by walls, objects, and other NPCs), and face the hero when stopped nearby. Path-followers walk their waypoint loop at the NPC stroll speed, facing the dominant axis of travel, and face the hero when paused on a waypoint near them.
- NPCs are solid: the hero and enemies can't walk through them (their own movement excludes themselves).
- The action button near an NPC makes it face the hero, stand still, and open its dialogue; NPCs render in the depth/y-sort pass with blob shadows.

## Pickups & drops

- Pickup sprites: `resources/pickups/pickups.png`, 16x16 frames: 0 heart, 1 rupee (worth 1), 2 rupee (worth 5), 3 rock (octorok projectile, not collectable), 4 arrow, 5 bomb, 6 magic decanter, 7 heart container.
- Drops roll from a weighted table via the seeded deterministic `Rng` (`rollDrop` in `engine.h`): mostly nothing, then hearts / rupees / big rupees, occasionally an arrow, bomb, or magic. Currently spawned by grass cutting and enemy deaths.
- Hearts heal 2 halves (a full heart) and cap at max health; a full-health hero leaves hearts lying. Rupees add to the wallet (`inventory.rupeeMax` cap). Arrows/bombs add to their quivers (`arrowsMax`/`bombsMax`); full quivers leave pickups lying. Magic refills 2 half-units of the magic meter. A heart container raises the max by a full heart and fills the gauge (`item get 1.wav`).
- Sfx: `heart.wav`, `rupee.wav` on pickup; `grass destroyed.wav` on cutting; `enemy dies.wav` on death.
- Migration note: drop tables become enemy-content (per-enemy drop weights in the enemy json) rather than one global table.

## Items & inventory

Items are game-defined content: each item is a `.json` in `resources/items/` defining its subscreen label, its **use-action** (composed from the engine vocabulary: `kind` — boomerang/arrow/bomb/bolt/hammer/shovel/hookshot/medallion/byrna/cape/somaria/powder — plus `element` for bolts and medallions, a `cost` — magic half-units, or bombs/arrows ammo with the bow's rupee fallback — an `sfx` name, and `"spawn": <object type>` for block-conjuring use-actions), **passives** (`{"light": <radius>}` for the lantern, `{"swim": true}` for flippers, `{"lift": true}` for gloves, `{"compass": true}` for the dungeon compass), and an optional **upgrade** (`{"flag": <world flag>, "speed": N, "damage": N}`: while the story flag is set, the use-action runs with the upgraded stats — silver arrows fire faster and strike for 2 hits, the magical boomerang flies faster and reaches farther; the flag resolves through the shared world-flag registry, so chests can grant it with `{"kind": "flag"}`). `useEquippedItem()` in `src/main.cpp` is the interpreter over the baked `ItemConfig` — costs pay first (an unaffordable cost plays the error cue and cancels), then the spawn kind runs, then the use sfx plays; there is no per-item switch. Elements (fire/ice) and use-action kinds (ITEM_USE_*) are engine vocabulary — items compose them.

- `Inventory` (engine.h) holds rupees/wallet, arrows, bombs, magic (half-units), owned items, and the equipped item. Process-lifetime until the save system; the demo grants everything at boot (proper grants come with chests/item-get).
- The pause subscreen has two tabs (the sword button switches): ITEMS (grid cursor + equip + quantities/magic/keys readout) and MAP (visited rooms as connected boxes laid out from the exits graph; the current room glows with a hero dot; unvisited rooms stay hidden; with the compass passive owned, chest rooms carry a gold mark and boss rooms a red one — derived at bake from the room's spawns). Visited-room state persists in the save. Autosaves flash a "SAVED" toast (a generic top-center banner system, short-lived).
- Current items (all data, see resources/items/): boomerang (arc + return; stuns enemies it touches, `small enemy hit.wav`, fetches pickups; only one airborne at a time), bow (arrow projectile; costs an arrow, or a rupee when the quiver is dry — LttP behavior; `error.wav` with neither; arrows knock enemies along their flight), bombs (placed at the feet, `lay bomb.wav`; fuse blinks, then `bomb explode.wav`: damage + knockback in a radius, blast bombable tiles open with `bombable wall.wav` plus the `secret.wav` sting ducking the bgm when a passage opens, screen shake + a brief white screen flash + expanding explosion rings), fire rod and ice rod (magic bolts costing 2 magic halves each, `fire rod.wav`/`ice rod.wav`):
  - Fire bolts burn enemies dead (`fire.wav` on torches), light torch posts as they pass, and shatter frozen enemies instantly.
  - Ice bolts freeze enemies solid (`ENEMY_FROZEN`: inert, blue-tinted ice block, thaws after ~4s; any sword/attack shatters a frozen enemy).
- The lantern is passive data: owning the item that carries `{"light": N}` lights dark rooms around the hero (slow magic drain there; an unlit torch the hero stands next to catches fire automatically). No item id is hard-coded — the engine scans for the first owned light-carrying item. The flippers are the same shape: an item carrying `{"swim": true}` makes deep water swimmable, and the gloves' `{"lift": true}` lets the hero hoist heavy objects (the scans in `src/main.cpp` read `ItemConfig.hasSwim`/`hasLift`).
- Upgrades are flag-gated stats: `"upgrade": {"flag": "silver_arrows", "speed": 420, "damage": 2}` on the bow (arrows fly at 420 px/s and strike for 2 hits while `silver_arrows` is set) and `{"flag": "magic_boomerang", "speed": 280}` on the boomerang (faster, farther flight). The sample's silver-arrows chest (room 02) grants the flag with `{"kind": "flag", "flag": "silver_arrows"}`; enemy strikes take a damage count through `Enemy::tryHit`'s damage parameter.
- Hookshot (an item composing use-kind "hookshot"): the item button fires a chained claw along the facing — a dotted wire drawn to the head. It bounces off solids and out-of-wire range, stuns enemies (`small enemy hit.wav`) and collects pickups it passes; latching a `hookable` object (`hook_post` — solid + `OBJINT_HOOKABLE`) reels the hero to the tile in front of it as a collision-free glide, crossing pits and water (`HERO_HOOKSHOT`, `hookshot.wav` on throw and latch). One chain airborne at a time.
- Medallions (items composing use-kind "medallion"; the element decides the storm): Bombos (fire) burns every enemy dead in a firestorm — rising embers, an orange flash, a heavy shake; Ether (ice) freezes the screen — falling frost and a blue-white flash; Quake (no element) flips every enemy over stunned while the ground bucks. Each costs 8 magic halves and rings `spin energy.wav`; arena bosses keep their armor and ignore the storm.
- Cane of Byrna (use-kind "byrna", a toggle): protective sparks orbit the hero and incoming damage is ignored — and the aura stings whatever presses against the hero — while it burns a magic half every 2 seconds; an empty meter drops the aura. The sfx is the item's `useSfx` (`cane.wav`), played on every toggle press.
- Magic cape (use-kind "cape", a toggle): the hero blinks ghostly, incoming damage is blocked, and enemy AI chases a ghost far offscreen (combat math keeps the true position) while the cloak burns a magic half every 2 seconds; an empty meter drops the cloak.
- Cane of Somaria (use-kind "somaria", with `"spawn": "somaria_block"`): the item button conjures the linked block type on the clear tile in front of the hero — or shatters it where it stands. The block is solid, pushable, and sinks onto latch switches, so it carries puzzle weight (and it's room-local: gone when the room reloads).
- Magic powder (use-kind "powder"): the item button flicks a puff into the facing — enemies caught in it reel stunned in a sparkle shower, torch posts catch fire, and shrine objects (`OBJINT_POWDER`, aux2 = a world-flag index) awaken, setting their baked story flag with the `secret.wav` sting (it persists in the save, so dialogue can branch on the awakened shrine — e.g. `{"if": "shrine_awakened"}`).
- Hammer (an item composing use-kind "hammer"): the item button pounds in the facing — windup, then a quake (screen shake + `quake 1.wav` + an expanding ring; `hammer_smash` objects like rocks and pots shatter, enemies flip over stunned; grass and crystal switches respond like sword strikes).
- Shovel (an item composing use-kind "shovel"): the item button turns the tile ahead over — windup, then a stab with a dirt burst (`dig.wav`); soft soil (`"dig": true` tiles) rolls a drop, and the room's buried items surface at their spots (once per visit). A code-drawn shovel stands in for real art.
- Shop (dialogue choices with "cost"/"give"): priced picks check the wallet through the pay seam — unaffordable options rattle (`error.wav`, nothing applies); paid picks deduct the rupees and grant their "give" item through the give-drain. A shopkeeper NPC (room 01, dialogue `05.json`) prices a shovel and a heart refill.
- HUD: rupee counter, equipped item + its count (arrows for arrow-cost items, bombs for bomb-cost items), and the bottom-left magic gauge. The pause button opens the item subscreen: grid cursor over the owned items (unowned cells blank), action equips, quantities + magic bar on the side.

## Hero

The hero is engine-native (a game doesn't script the player character; it configures it — sprite, stats, and which items exist). The hero moves with the controls described above, run through a state machine (`HeroState`): `idle`, `walk`, `attack` (sword swing), `spin` (charged spin attack), `dash` (pegasus boots), `crash` (dashed into a wall: stunned), `hurt` (knockback), `fall` (into pit tiles), `splash` (into deep water without flippers), `swim` (in deep water with flippers), `hop` (vaulted over a ledge), `lift` (hoisting a liftable object), `grab` (holding a pullable object), `hammer` (pounding with the hammer item), `dig` (turning a tile over with the shovel item), `hookshot` (firing the chained hook; latching reels the hero), `sprinkle` (flicking magic powder), and `dead`. A non-idle/walk state overrides input until it resolves, so attacks and reactions cancel movement cleanly.
- Diagonal movement is normalized to the same overall speed, and the walk sfx accumulator uses real distance so stepping never doubles up.
- The hero cannot exit the map (position is clamped so the whole 32x32 sprite stays inside).
- The sword (X / gamepad face-left) swings in all 4 facings with a 3-frame arc sheet (`assets/characters/link_sword.png`, placeholder generated with the build); `Hero::attackBox()` exposes the live hitbox for enemies to check. Walking is canceled for the swing's duration. Swings also cut `cuttable` tiles (grass) overlapped by the arc.
- Spin attack: holding the sword button charges (`spin attack charge.wav`, weapon-shake preview on the arc sheet); releasing after the threshold spins — a 360-degree hitbox (`spin attack.wav`, the arc whirls around the hero) that hits every enemy and cuts every tile around once per spin.
- Hit-pause: strikes that connect (swing/spin/dash) freeze the whole world for a few frames — a light beat on a connect, heavier on the killing blow, heaviest on boss hits — layered on the screen shake for impact feel.
- Sword beam: a swing that *starts* at full hearts fires a beam projectile (`sword beam.wav`, `assets/effects/beam.png` 4-direction sheet) along the facing; beams strike enemies (one hit each, poof + drop on kill) and dissipate at the screen edge or on walls.
- Dash (V / gamepad right trigger, held with a direction): a fixed-direction burst that tramples grass, damages enemies it plows through (no damage taken while dashing), and crashes into walls (`ram.wav`) for a stunned stagger with no heart loss.
- Passive shield: while free to move (walk/idle), rocks arriving from the faced direction are knocked away at a deflected angle (`tink.wav`) instead of hitting.
- Active shield (Left Shift / gamepad left trigger, held): the hero raises its shield while free to move — a slow walk (~half speed), a code-drawn plate held toward the facing, and the blocked arc widens to everything except a projectile arriving from directly behind.
- Damage (from tiles, enemy contact, or octorok rocks) knocks the hero back along a directional vector, grants ~1s of invincibility frames with sprite flicker, drains hearts by halves, and shakes the screen briefly.
- Pit tiles freeze the hero over the hole, shrink the sprite, drain half a heart, and respawn the hero on the last safe (non-pit) tile stood on.
- Water: shallow tiles are waded through at reduced speed with splashing steps (`walk water.wav`); deep water without flippers makes the hero flinch in with expanding splash rings + droplet particles (`fall in water.wav`) and hop back onto the last safe tile, taking no damage. **Swimming** (any owned item carrying the `{"swim": true}` passive — the flippers, as data like the lantern's light): entering deep water starts `HERO_SWIM` — a stroke-glide (each direction press kicks a speed burst that decays with water drag; holding re-strokes once the glide slows), a float bob with ripple rings at the surface, and no sword/dash while afloat. Swimming persists while the tiles stay deep; the first non-deep tile reached is the bank — the hero stands up (wading/walking) there. Ice tiles trade direct movement for momentum: the held direction accelerates a velocity vector, releasing it coasts with weak friction, and walls stop the slide. "heal" tiles (fairy-fountain water) slowly refill hearts and magic while the hero stands in them.
- Push tiles (`"push": [dx, dy]`, e.g. conveyor belts and water currents) drift the hero while standing on them, on top of input movement, without counting as footsteps; a current can sweep the hero into deep water or a pit.
- Ledge tiles (colliders with `"ledge": "<dir>"`) are one-way: pressing into them along their facing vaults the hero over with a two-tile hop — arc with a ground shadow, `land.wav` and dust rings on touchdown; from other sides they are plain walls.
- Carrying (pots, bushes): lifting is a brief hoist pause; while carrying, the object rides overhead, sword/dash/boomerang are locked out, and the action button throws it along the facing (see Objects above).
- Grabbing and pulling (statues, lever blocks): the action button facing a `pullable` object grabs it (`HERO_GRAB` — the hero digs in facing the object); pressing straight away from it drags the pair one tile (the object slides into the tile the hero vacates, `move object.wav`; both paths are checked first). Letting go of the action button releases. Pulls use the same slide/settle path as pushes — a pulled block onto a latch switch sinks in like a pushed one. Heavy objects (big rocks) refuse a bare-handed lift with `error.wav`; any owned item carrying the `{"lift": true}` passive (the gloves, as data) hoists them.
- Moving platforms: standing on a `platform` object carries the hero along its rail, and riding suppresses tile hazards — a raft over deep water needs no flippers (the platform is the ground underfoot). The demo rail glides across room 02.
- Hammer (an item composing `ITEM_USE_HAMMER`, the item button): a windup, then the pound lands — screen shake + `quake 1.wav` + an expanding quake ring at the impact point (`HERO_HAMMER`, a code-drawn mallet placeholder). The pound box (a touch wider than the sword arc) shatters `hammer_smash` objects (rocks, pots, stakes), flips enemies it connects with over (stunned on their backs), and strikes grass/crystal switches like a sword swing would.
- Health is half-heart based (starts at 3 hearts); the HUD hearts row (`assets/ui/hearts.png`, placeholder full/half/empty frames) plus the rupee counter are drawn in virtual pixels over the world. At 1 heart left, hearts flash and a low-health beep plays (`low hp.wav`).
- Death: at 0 hearts the hero spin-collapses (`link dies.wav`), the screen fades, GAME OVER appears, and the action button reloads the room at full hearts (enemies respawn).

The virtual "camera" follows the hero around. However, the camera never leaves the parameter of the map. Because of this, the camera tries to stay centered on the character, unless the character is close enough to the edge of the map.

Draw order (inside the camera transform): room decor + colliders, then floor plates (switches, sunk objects), ground fx (pickups, poofs, projectiles, boomerang), then the depth/y-sort pass — the hero, living enemies, and standing objects back-to-front by baseline (the bottom of their collision boxes, each with a blob shadow at its feet (skipped while falling/splashing/swimming/dead)) — then lifted/carried/thrown objects over everything, and the room's "upper" layer last. Entities standing on shallow-water tiles draw a reflection: a flipped, squashed, water-tinted copy of their sprite below the feet (`Hero::drawReflection` / `Enemy::drawReflection`, NPCs inline), under the entity itself.

Room transitions: pressing into a map boundary along a linked edge starts a scroll-in — the new room loads with the hero placed on the opposite edge (perpendicular coordinate preserved from the old room, nudged to the nearest free tile that isn't a pit or deep water), gliding in from one tile off-map over ~0.4s while the camera follows unclamped and the world stays frozen. Non-scroll loads (game start, death-continue, future warp tiles) fade in from black over ~0.3s instead.

## Custom behaviors (resources/code/)

Game-local C++ — layer 4, the sanctioned escape hatch when composition can't express a behavior (multi-phase boss choreography, novel projectile physics, custom minigame logic). This seam is implemented and verified end to end:

- Location: `resources/code/*.cpp` in the game folder. One behavior (or a few related ones) per file; all three build scripts glob the folder and compile what's there into the binary, identically on Mac/Web/Dreamcast.
- The seam: game files reference a custom behavior by name (an enemy's `"behavior": "custom:bounce_octo"`); JSON stays data, C++ holds the logic. `include/gamecode.h` + `src/gamecode.cpp` are the bake-time-linked registry: each code unit calls `gamecodeRegisterEnemyBehavior(name, fn)` from a static initializer (plain C++11, no exceptions/RTTI, no dynamic loading, no runtime parsing). The registry is static storage, so cross-translation-unit init order is safe.
- Enemy behaviors: signature `void fn(Enemy&, const Room&, Vector2 heroCenter, Rng&, float dt)` — the behavior owns the whole AI in the AI states (idle/walk/pause/charge); the engine still handles hurt/stun/frozen recovery and hit resolution around it. The `Enemy` class exposes the movement/mode primitives for this (`setMode`, `slide`, `pickDirection`, `faceVector`, `setPosition`, `modeState`, `modeElapsed`, `aiTimer`, `setAiTimer`, `facingDirection`, `setFacing`, `config`, plus `queueShot` to fire a projectile through the room's spawner). An unregistered name degrades to the built-in component AI with a bake warning. The bundled sample ships one: `resources/code/zora.cpp` — the zora's lurk/surface/spit/dive cycle (referenced as `"behavior": "custom:zora"` in `resources/enemies/zora.json`; its stand frames read as the submerged ripple, its move frames as the surfaced body, so the dive cycle is pure art + this behavior).
- Validation: the generator can't read C++, so `resources/code/manifest.json` lists the behavior names the game code registers; the generator validates every `custom:` reference against it at bake and bakes the names (`CUSTOM_BEHAVIOR_NAMES`).
- Rules for game code: use `frameDt()`, the engine `Rng` (determinism), engine helpers and pools; no file I/O at runtime (romdisk/console constraint); no new global state that should be save-persisted without wiring it into the save schema.
- Editor support (see Editor): the editor scaffolds new behavior files with the registration boilerplate, adds the name to the manifest, compiles, and surfaces compiler errors in its UI. That scaffolding + error surfacing is the "scripting UX" — the editor is the place where C++ game code gets written.

## Runtime scripting policy

There is no embedded scripting language and there shouldn't be one. If a genuine, concrete need for *runtime* flexibility ever survives the composition and C++ layers, the path is: generalize the existing dialogue interpreter (`src/dialogue.cpp`) into a small bytecode VM that the generator compiles scripts into at bake time — not embedding Lua/Wren. Constraints on any such VM: deterministic (engine `Rng` only), no filesystem, no per-frame interpretation (event-driven triggers only — per-frame AI interpretation is off-limits on the SH-4), and it must compile clean on the Dreamcast target. This is a last resort, expected to remain unused.

## Placeholder assets

The generator synthesizes placeholder art/audio when missing, so a game stays testable: `resources/ui/border.png` (dialogue nine-patch), `resources/ui/hearts.png` (full/half/empty heart gauge), `resources/characters/link_sword.png` (slash arc sheet: 4 columns down/right/left/up x 3 sweep frames, 64x64 each), `resources/enemies/*.png` (mob blob sheets: 4 columns down/right/left/up x 2 rows stand/move), `resources/pickups/pickups.png` (heart/rupee/rupee5/rock, 16x16 each), `resources/objects/*.png` (pot, bush, block, switch, barrier, sign, torch, warp, crystal, chest, rock, big_rock, statue, lever, platform, hook_post, somaria_block, shrine — single 32x32 frames, chests two, the lever mirrors with its pulled state), `resources/audio/birds.wav`+`rain.wav` (ambient loop placeholders for room ambience), and `resources/effects/` (`beam.png` 4-direction sword-beam bolts, `boomerang.png` 4 spin frames, 16x16 each). Drop real files at those paths to replace them; the generator never overwrites existing files. This is what makes "a new game folder is playable from day one" true.

## Save system

A versioned binary blob (`SaveData` in `src/main.cpp`, magic "LTOP", version 7 — v7 widened the item slots from 16 to 20, so `owned[]` grew; older blobs fail the version check and read as absent), one file per slot (`SAVE_SLOT_COUNT` 3; the title screen will pick them — boot continues from slot 0). Desktop writes `save<slot>.dat` in the working directory; the web build writes into raylib's IDBFS-backed `/savefile/` directory so the browser persists it.

The save schema is **per-game**: the blob carries the game's identity (the `game[16]` slug field, stamped from `GAME_SLUG` on save and validated on load) so a slot from one game is never misread by another, and persisted game-defined state (items, flags, chests, doors, encounters) is keyed by the generator-assigned stable IDs of the game that wrote it.

Persisted: hero location (room + tile), health + max, the full inventory (equipped item, owned items, rupees/wallet, arrows, bombs, magic, keys), all world flags (story state + the crystal-switch state), the opened-chest registry, the opened-door registry, the boss-defeated registry, the blasted-tile registry (bombable passages stay open), visited-room marks (the map screen), one-shot encounter memory, and the world RNG state (drops stay deterministic across sessions). `WORLD_FLAG_COUNT`/`ENCOUNTER_TOTAL` are extern constants, so the schema uses fixed caps (flags 256, encounters 512, blasts 256, chests MAX_CHEST_SLOTS) with bounds-checked copies.

Auto-save (LttP style): on map-edge transitions, warps, chest opens, and the fresh start when a slot begins. Room-local state (push blocks, switches) resets on reload until it moves into the save schema.

Game flow: the title screen lists the three slots (with a hearts/rupees summary of what's inside); the action button starts an empty slot as a new game or continues a used one, resetting all process-global state first so slots never leak into each other. The pause subscreen's dash button is save-and-quit: it banks the save and returns to the title.

## Testing

Per-feature smoke checklist (practiced on every round):
- Generator run with zero warnings (`./build-mac.sh` prints them).
- Native build + web build compile clean (`-Wall -Wextra`); game code (`resources/code/`) compiles clean with the same flags. The Dreamcast docker build is the gate for anything touching game code or the registration seam.
- Headless boot of the binary: no `WARNING: IMAGE`/`ERROR` lines, no crashes.
- Keyboard + gamepad paths both handled for every new input (the `InputBinding` table).
- Editor round-trips: room and dialogue saves preserve keys the editor doesn't edit.

`scripts/soak_test.py` (exit 0 = healthy):
- Generator determinism: two bakes must produce byte-identical `include_auto`/`src_auto`.
- Save/load soak: `./main --soak` headlessly runs fresh game -> save -> reload -> re-save and requires every persisted byte to round-trip (the rng state is masked: loading a room spawns NPCs, which legitimately advances the world rng).

The generator validates data at bake time (warnings): room layer tile ids in tileset range (out-of-range cleared), animation frames inside their sheets, unknown references (dialogue/enemy/item/object/room/flag names), exits linking to real rooms, orphan rooms unreachable from room 0, malformed optional fields, spawn counts against the engine's fixed budgets, and (once it exists) `custom:` behavior references against the code manifest. The engine asserts indices the generator guarantees (`loadRoom` room index, `tileMetaAt` tile ids).

## Editor

`editor/` contains the game editor as a Tauri desktop app (React + Vite frontend, Rust backend) for editing a game folder and running the build. Run with `cd editor && npm run tauri:dev` (or `npm run tauri:build` for a bundled app); it reads/writes only the active game's `resources/` and runs `./build-mac.sh` (Mac only) for its Build button. It locates the project via the `LTOP_EDITOR_PROJECT` env var, a persisted folder choice, or by walking up from the executable, and otherwise asks for the folder in the UI. Because a game is a folder, the folder picker *is* the game picker: point the editor at a different game folder and it edits that game. See `editor/README.md`.

Room painting covers the `decor`, `colliders`, and `upper` layers (each selectable, each with a visibility toggle; upper paints last in the canvas so it previews over tiles). Saving a room spreads the file's parsed JSON first, so keys the editor doesn't edit (like `enemies` and `objects`) survive the round trip.

Editor growth direction (pairs with the composition architecture):
- **Forms over text**: composition surfaces (enemy components, item use-actions, trigger conditions/actions, object types) render as structured forms — dropdowns of vocabulary terms, parameter fields, and previews — not raw JSON textareas. Structured data is what makes the editor the primary authoring surface.
- **C++ behavior scaffolding**: a "new custom behavior" action creates the file in `resources/code/` with registration boilerplate, updates the manifest, runs the build, and shows compiler output (errors included) in the editor. Compile is the script's "run"; surface it honestly.
- **ID discipline**: the editor never invents IDs; it reads the vocabulary and existing names from the generator's bake output so forms offer only valid terms.

## Migration status

Steps from "LttP-shaped engine" to "open engine + game folder". Steps 1-6 are **landed**; each kept the build green and existing game data working. What remains is listed after.

1. **[done] Game ID extraction.** `ITEM_*`, `OBJECT_*`, `DOOR_*`, `CHEST_*`, `GRANT_*`, and `BARRIER_*` moved out of `include/engine.h`; the generator assigns them per game and emits them into `include_auto/assets.h` (engine.h owns only vocabulary: `OBJINT_*`, `ITEM_USE_*`, `GRANTEFFECT_*`, rule structs). The save carries the game slug (schema v6). Engine headers contain no LttP concepts.
2. **[done] Rule tables + shared vocabulary plumbing.** Door kinds, chest kinds, and grant kinds are interpreted through baked rule tables (`DOOR_RULES`, `CHEST_RULES`, `GRANT_RULES`) — `doorIsOpen`/`doorIsOpen`-adjacent logic, key consumption, push-open, bombable-edge blasts, doorway drawing, chest key costs, grant effects/pickup frames/description templates all consult the tables. The dialogue condition/action interpreter remains the trigger-vocabulary seed; the "sfx by name" action moved to a shared engine module (`sfx.h`) consumed by dialogue and item use-actions alike.
3. **[done] Items.** Per-game item jsons (`resources/items/*.json`) with use-actions (kind/element/cost/sfx) and passives (light radius); `useEquippedItem()` is now an interpreter — no per-item branches. The lantern's light is data (first owned item with a light radius); the HUD's equipped-count readout reads the use-action costs.
4. **[done] Enemies.** Component parameters baked into `EnemyConfig` (charge/shoot/mover params, boss phase table, drop table, custom behavior name); `BEHAVIOR_*` archetypes are gone and `src/enemy.cpp` interprets components — no `behavior ==` checks. Presets (soldier/octorok/boss) compose components at bake; explicit `components` in the json override them; boss phases are a data table; per-enemy drop tables roll at every enemy death site.
5. **[done] Objects.** `resources/objects/types.json` defines per-game types composing the `OBJINT_*` interaction vocabulary; the engine branches on the baked mask everywhere (solidity, lifting, cutting, dash-smashing, pushing, switches/barriers/torches/warps/crystals/chests/signs, floor vs standing draw passes, two-frame sheets). Aux packing at bake is interaction-driven, so a new type composing the same interactions consumes the same room-json extras.
6. **[done] Game code plumbing.** `resources/code/*.cpp` compiles in all three build scripts; `gamecode.h`/`gamecode.cpp` provide the static-init behavior registry; `manifest.json` validates `custom:` references at bake; the enemy custom-behavior seam is verified end to end (registered behavior + json reference + boot).

**Remaining** (editor-facing and extensions):

7. **Editor forms** — structured editors for the now-data vocabularies (object type compositions, item use-actions, enemy components/drops, door kinds), then the C++ behavior scaffolding + compile-error surfacing, then the game-folder picker polish. The existing editor keeps working unchanged in the meantime (room painting round-trips the new files like any other unedited keys).
8. **Extensions when needed** — game-defined door kind tables as content (new kinds with rule params), encounter payloads beyond dialogue refs, further component vocabulary (splits, path/hover movers, shield reactions) driven by real content needs, and per-type extras in types.json when a game actually needs them. Keep the hard line: conditions and action lists in data, logic in `resources/code/`.

Do not design vocabulary in a vacuum: new terms come from refactoring existing hard-coded branches into data, so the vocabulary stays shaped by real content rather than speculation.
