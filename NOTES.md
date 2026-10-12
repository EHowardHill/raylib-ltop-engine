# Purpose

This is an engine, based on Raylib, that is designed to mimic the functionality of The Legend of Zelda: A Link to the Past.

Your job is to maintain the code within `include`, `src`, `scripts`, and `editor`. Modify the code within `resources` if nescessary. The folders `include_auto` and `src_auto` are not meant to be edited by anything but `generate_assets.py`.

## Build

`build-mac.sh` builds the native binary (Mac, clang++). `build-web.sh` builds a hostable web build (Emscripten). `build-dreamcast.sh` builds a Dreamcast disc image (KallistiOS + raylib4Consoles); it cross-compiles inside Docker, so a working Docker daemon (OrbStack) is the only host requirement. The first run builds the toolchain image (`ltop/dreamcast`, from `scripts/console/dreamcast.Dockerfile`; pull + raylib build, cached afterwards), and produces `build/dreamcast/ltop.elf` (dcload-able) plus `build/dreamcast/ltop.cdi` (self-boot disc, run in an emulator or burn to CD-R). The assets are packed into the romdisk; writing save files on hardware still needs a console path strategy (/ram or VMU) — `scripts/console/dreamcast_shim.c` currently backs raylib's `FileExists` with KOS `fs_open`.

Gamecube and N64 were evaluated and rejected as targets: their raylib ports (raylib4Consoles) ship without the audio module because miniaudio has no backend on that hardware, and a target that can't carry the engine's full feature set isn't worth a sanity build. Dreamcast's port has real audio — its bundled miniaudio talks to the KOS AICA driver directly, and the build links `-lpthread` (KOS pthreads addon) for it. Don't reintroduce those platforms (or new ones) without full feature parity.

Because the console build compiles against the raylib port's own `raylib.h` (not the vendored one), `include/engine.h` includes `<raylib.h>` with angle brackets; the include order in each build script picks the header that matches the library being linked. Keep the engine's raylib calls compatible across header versions (no APIs newer than the oldest raylib the console ports track).

At no point should the Raylib binary import or parse .json. The purpose of `scripts/generate_assets.py` is to generate .h and .cpp files that correspond to the content of the various .json files found in the source code.

Some fields may be optional - be sensitive to not breaking if these fields are missing.

Both build scripts run the generator, which also mirrors the runtime files from `resources/` into `assets/` (excluding `.json`, which is baked into the binary). The binary loads **only** from `assets/` at runtime; `resources/` is the editable source of truth and is never read by the game.

## Global

- All tiles and characters are expected to be 32x32 pixels in size.
- Because of this, X and Y references ire almost exclusively within this idea (e.g. 1x1 = 32x32).

The system keeps a virtual vertical resolution of "224", regardless of the actual resolution.
- It's only locked vertically so it can support widescreen.
- It is meant to simulate the height of a SNES console.

This game is written in Raylib, so it has many potential supported console outputs — but the supported build targets are exactly: Mac (`build-mac.sh`), Web (`build-web.sh`), and Dreamcast (`build-dreamcast.sh`). Do not write code that would prevent the Dreamcast build from compiling; don't add other console targets unless their raylib port has full feature parity (audio included).
- The web build uses `-DPLATFORM_WEB` and `emscripten_set_main_loop` (see `src/main.cpp`); keep the desktop loop path inside `#else` guards (the Dreamcast uses that desktop path).

## Controls

- Desktop: arrow keys move, Z is the action button, X is the sword (tap = swing; hold to charge a spin attack), C is the item (boomerang), V is the dash (pegasus boots), Enter is pause.
- Gamepad: left stick / dpad move, face-down button (A on Xbox, B on SNES mappings) is the action button, face-left is the sword, face-up is the item, right trigger is dash, middle-right is pause.
- Logical buttons are defined as `InputBinding`s in `include/engine.h` (`BINDING_ACTION`, `BINDING_SWORD`, `BINDING_ITEM`, `BINDING_DASH`, `BINDING_PAUSE`); every input check goes through `bindingPressed`/`bindingHeld` so remapping only touches that table.

## Dialogue

This engine must support Link to the Past-style dialogue. It appears like this:
- There is a border around text. This is a bitmap, stored in `resources/ui/border.png`. The generator creates it if missing (a 24x24 nine-patch, 8px patches).
- There is no speaker icon or speaker label.

Dialogue is stored in `resources/dialogue` as .json files.

The .json files:
- "lines" is a list containing the lines.
- each line is either a plain text string or an object whose "text" contains the text, plus optional scripting fields (see below).

Action scripting, per line (all fields optional):
- "label": names the line as a `goto` target (within the same file).
- "if" / "unless": world flag name; the line is only shown when the flag matches. Skipped lines execute nothing.
- "set" / "clear": world flag names (list); applied when the line is SHOWN.
- "sfx": audio file from `resources/audio/`, played when the line is shown (lazy-cached).
- "music": music file from `resources/music/` switched when the line is shown; "" stops the bgm.
- "goto": label to jump to when the line completes (action press after typing).
- "warp": {"room": "<room file or stem>", "tile": [x, y]} — queued when the line completes; the game warps once the dialogue closes (room loads at the tile, fade in).
- "choice": a list of options (text plus optional set/clear/sfx/goto). After the line types out, the action press opens the list; up/down moves the cursor, action selects. The chosen option applies its set/clear/sfx and goes to its goto (or advances).

World flags: flag names are collected by the generator across every dialogue (first-use order) and baked as `WORLD_FLAG_NAMES` / `WORLD_FLAG_COUNT`; the store is the global `gWorldFlags[]` array (process-lifetime until the save system lands). Flags are referenced by name in dialogue files; the generator resolves them to indices with warnings on typos.

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
- "bombable": true — cracked wall/floor: bombs blast it open, leaving "bomb_result" (0 = blown-open hole). Bombable walls live in the colliders layer; both layers are runtime-mutable copies now (blasts open passages; grass cutting changes decor).
- The generator bakes this into `TILE_META`/`TILE_META_COUNT` (`include_auto/assets.h`); `Room::tileMetaAt` combines the decor + collider layers at a tile, `Room::cutTileAt` cuts and `Room::bombTileAt` blasts the runtime copies.

Within the .json file:
- we have "default"

Within "default":
- "hero": Which character is the player character.
- "spawn": If there is no spawn location, place the hero character at this X and Y location. (Tile coordinates.)
- "music": Play the music from `resources/music/` + the filename provided. Do not restart if the same file is already playing.
  - Caution: music loads synchronously through raylib's tracker parser. A corrupt module file hangs the game before the first frame (no window, no error). Test new music files by running the game before committing them.
- "walk_sfx": When the hero is walking, use the sound cue from `resources/audio/` + the filename provided.

Also at the top level:
- "doors" (optional): `{"north"|"south"|"east"|"west": {"kind": <kind>}}` — doors gate the map-graph edges (baked as DOOR_* kinds). While a boss lives in the room, every edge stays shut regardless of kind (arena lock). Kinds: "open" (cosmetic archway), "closed" (pushes open from either side, `door open.wav`), "locked" (consumes a small key, `use key.wav`), "big" (consumes the big key), "bombable" (a bomb blast near the wall midpoint blows it open, `bombable wall.wav`), "shutter" (open only while no enemies live in the room — the classic fight lock). Opening a door syncs the reciprocal door in the linked room (it's the same door from both sides), and opened doors persist in the save. Doors render at their wall midpoint: a dark archway when open, a slab (keyhole plate / crack marks / shutter slats) when closed; pressing into a closed-but-openable door opens it with a cue first, then the next press scrolls through.
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
  - "behavior": "" / "soldier" (patrols, charges when the hero is within "sight" px, stuns on wall impact) / "octorok" (walks, pauses, spits a rock roughly every "shoot_period" seconds).
  - "sight": charge radius for soldiers (px).
  - "shoot_period": seconds between rock volleys for octoroks.

Behavior ids are generator-side constants (`BEHAVIORS` in `generate_assets.py`, `BEHAVIOR_*` in `engine.h`) — keep them in sync when adding archetypes (keese, slime, stalfos...).

Runtime (`src/enemy.cpp`): lifecycle modes idle/walk/pause/charge/stun/frozen/hurt; sword hits are resolved single-hit-per-swing via the hero's swing id; death spawns a cloud poof plus a drop roll. Defeated enemies stay dead until the room reloads.

Arena bosses: `"behavior": "boss"` (or `"boss": true`) in an enemy's stats. Bosses draw at 1.5x with a bigger hitbox, charge in phases (faster + shorter recovery as their health drops), and while one lives its room seals — every edge behaves shutter-locked until the fight is won. The HUD shows a boss health bar during the fight. Defeat plays the big sequence (`boss dies.wav`, triple explosion, screen shake), releases the doors, and a heart container rises through the item-get flow; bosses stay dead across reloads and the save (defeat is banked immediately).

## Objects

Interactive world objects (pots, bushes) are defined per room and baked like enemy spawns. Type names are generator-side constants (`OBJECT_TYPES` in `scripts/generate_assets.py`, `OBJECT_*` in `engine.h`); sprite frames are single 32x32 images in `resources/objects/` (placeholders are synthesized when missing).

Room .json:
- "objects": a list of `{"type": <name>, "location": [x, y]}` entries (tile coordinates). The generator bakes `ObjectSpawn` sets; the engine spawns the room's set on entry, so smashed pots are back on the next visit (LttP behavior).
- Types: "pot", "bush" (liftable/throwable), "block" (pushable), "switch" (floor plate), "barrier" (linked gate), "sign" (readable).
- Switch extras: "mode" — "hold" (active while weight is on the plate) or "latch" (stays pressed once triggered); "link" — link group id shared with the barriers it controls.
- Barrier extra: "link" — the group id of the switches that lower it.
- Sign extra: "dialogue" — the dialogue file it shows.
- Chest extras: "kind" — "normal", "locked" (consumes a small key), or "big" (consumes the big key); "grant" — {"kind": "item"|"rupees"|"arrows"|"bombs"|"magic"|"heart_container"|"key"|"big_key", "item": <item name> (for "item"), "amount": N}. The grant kinds bake to `GRANT_*` (engine.h).

Runtime (`src/main.cpp`):
- Ground-state objects are solid: `Room::setSolidExtras` hooks their footprints into `solidBox`, so the hero, enemies, thrown objects, and projectiles all collide with them like tile colliders. Floor switches are the exception: they're walkable plates.
- Lift: the action button next to a liftable object (pot/bush) starts `HERO_LIFT` (brief hoist pause, `lift.wav`); the object glides from its tile to over the hero's head and the hero enters carrying mode.
- Carrying: the object rides overhead; sword, spin, dash, and the boomerang are locked out. Getting hurt, falling, splashing, or dying smashes the carried object (poof + drop roll).
- Throw: the action button hurls the object along the facing (`throw.wav`): a gravity arc with a ground shadow. It smashes on the first enemy hit (damage + knockback, poof + drop), on walls, or on touchdown (`break.wav`, poof + drop roll) — landing in water just splashes (`item in water.wav`, gone without a drop).
- Push blocks: walking face-first into a block for ~0.4s slides it one tile (`move object.wav`) if the destination is clear. Sliding onto a latch switch sinks the block into the floor and presses the switch forever.
- Switches: `button.wav` on press. Hold plates are active while the hero or a resting block is on them; latch plates stay active once triggered. All states are room-local (reset on reload; permanent states pending the save system).
- Barriers: raised (solid) until a switch in their link group is active, then they sink into the floor (`door open.wav`/`door close.wav`) and stop blocking.
- Signs: solid; the action button in front of one opens its dialogue.
- Torches: solid posts with a "link" group id. Fire bolts light them on contact (`fire.wav`); once lit they burn with a flickering flame forever (this visit) and count like latched switches in their link group — barriers tied to that group lower while any linked torch burns.
- Chests: solid, two-frame sheet (closed/lid open). The action button opens one: locked chests rattle (`error.wav`) without a small key, big chests without the big key; otherwise the key is consumed (`chest open.wav`) and the item-get sequence plays — the grant rises from the chest to over the hero's head with sparkle rays and a fanfare (`item get 2.wav`), then the description dialogue shows ("You got the BOW!"-style; the world is frozen until it closes). Grants apply to the inventory (`give`/quantities/heart container +1 heart/small key/big key). Opened lids stay open across room reloads (process-lifetime registry keyed by room+slot; a real save schema comes with the save system). Key counts show in the HUD and on the item subscreen.

## NPCs

Townsfolk are character-driven (they use `CharacterConfig` sheets — 4-column stand/move animation frames, same shape as the hero) and live in each room's `"npcs"` list:

- `{"character": "<name>", "dialogue": "<file>", "location": [x, y], "wander": true|false, "if": "<flag>", "unless": "<flag>"}` — "wander" picks idle vs stroll; "if"/"unless" gate the spawn on world flags (story-state presence, checked at room entry).
- Placeholder character sheets are synthesized for characters without art (name-hued cloak blob, 4 columns x 2 rows); the generator never overwrites real art.

Runtime (`src/main.cpp`):
- Idle NPCs turn to face the hero when nearby; wanderers stroll in random directions with pauses (blocked by walls, objects, and other NPCs), and face the hero when stopped nearby.
- NPCs are solid: the hero and enemies can't walk through them (their own movement excludes themselves).
- The action button near an NPC makes it face the hero, stand still, and open its dialogue; NPCs render in the depth/y-sort pass with blob shadows.

## Pickups & drops

- Pickup sprites: `resources/pickups/pickups.png`, 16x16 frames: 0 heart, 1 rupee (worth 1), 2 rupee (worth 5), 3 rock (octorok projectile, not collectible), 4 arrow, 5 bomb, 6 magic decanter, 7 heart container.
- Drops roll from a weighted table via the seeded deterministic `Rng` (`rollDrop` in `engine.h`): mostly nothing, then hearts / rupees / big rupees, occasionally an arrow, bomb, or magic. Currently spawned by grass cutting and enemy deaths.
- Hearts heal 2 halves (a full heart) and cap at max health; a full-health hero leaves hearts lying. Rupees add to the wallet (`inventory.rupeeMax` cap). Arrows/bombs add to their quivers (`arrowsMax`/`bombsMax`); full quivers leave pickups lying. Magic refills 2 half-units of the magic meter. A heart container raises the max by a full heart and fills the gauge (`item get 1.wav`).
- Sfx: `heart.wav`, `rupee.wav` on pickup; `grass destroyed.wav` on cutting; `enemy dies.wav` on death.

## Items & inventory

- The item list is generator-side (`ITEMS` in `scripts/generate_assets.py`, baked as `ItemConfig ITEMS[]`; indices are the `ITEM_*` constants in `engine.h`: boomerang, bow, bombs so far).
- `Inventory` (engine.h) holds rupees/wallet, arrows, bombs, magic (half-units), owned items, and the equipped item. Process-lifetime until the save system; the demo grants everything at boot (proper grants come with chests/item-get).
- The pause subscreen has two tabs (the sword button switches): ITEMS (grid cursor + equip + quantities/magic/keys readout) and MAP (visited rooms as connected boxes laid out from the exits graph; the current room glows with a hero dot; unvisited rooms stay hidden). Visited-room state persists in the save. Autosaves flash a "SAVED" toast (a generic top-center banner system, short-lived).
- The item button runs the equipped item: boomerang (arc + return), bow (arrow projectile; costs an arrow, or a rupee when the quiver is dry — LttP behavior; `error.wav` with neither; arrows knock enemies along their flight), bombs (placed at the feet, `lay bomb.wav`; fuse blinks, then `bomb explode.wav`: damage + knockback in a radius, blast bombable tiles open with `bombable wall.wav`, screen shake + expanding explosion rings), fire rod and ice rod (magic bolts costing 2 magic halves each, `fire rod.wav`/`ice rod.wav`):
  - Fire bolts burn enemies dead (`fire.wav` on torches), light torch posts as they pass, and shatter frozen enemies instantly.
  - Ice bolts freeze enemies solid (`ENEMY_FROZEN`: inert, blue-tinted ice block, thaws after ~4s; any sword/attack shatters a frozen enemy).
- The lantern is passive: owning it lights dark rooms around the hero (slow magic drain there; an unlit torch the hero stands next to catches fire automatically).
- HUD: rupee counter, equipped item + its count, and the bottom-left magic gauge. The pause button opens the item subscreen: grid cursor over the owned items (unowned cells blank), action equips, quantities + magic bar on the side.

## Hero

The hero moves with the controls described above, run through a state machine (`HeroState`): `idle`, `walk`, `attack` (sword swing), `spin` (charged spin attack), `dash` (pegasus boots), `crash` (dashed into a wall: stunned), `hurt` (knockback), `fall` (into pit tiles), `splash` (into deep water), `hop` (vaulted over a ledge), `lift` (hoisting a liftable object), and `dead`. A non-idle/walk state overrides input until it resolves, so attacks and reactions cancel movement cleanly.
- Diagonal movement is normalized to the same overall speed, and the walk sfx accumulator uses real distance so stepping never doubles up.
- The hero cannot exit the map (position is clamped so the whole 32x32 sprite stays inside).
- The sword (X / gamepad face-left) swings in all 4 facings with a 3-frame arc sheet (`assets/characters/link_sword.png`, placeholder generated with the build); `Hero::attackBox()` exposes the live hitbox for enemies to check. Walking is canceled for the swing's duration. Swings also cut `cuttable` tiles (grass) overlapped by the arc.
- Spin attack: holding the sword button charges (`spin attack charge.wav`, weapon-shake preview on the arc sheet); releasing after the threshold spins — a 360-degree hitbox (`spin attack.wav`, the arc whirls around the hero) that hits every enemy and cuts every tile around once per spin.
- Sword beam: a swing that *starts* at full hearts fires a beam projectile (`sword beam.wav`, `assets/effects/beam.png` 4-direction sheet) along the facing; beams strike enemies (one hit each, poof + drop on kill) and dissipate at the screen edge or on walls.
- Dash (V / gamepad right trigger, held with a direction): a fixed-direction burst that tramples grass, damages enemies it plows through (no damage taken while dashing), and crashes into walls (`ram.wav`) for a stunned stagger with no heart loss.
- Passive shield: while free to move (walk/idle), rocks arriving from the faced direction are knocked away at a deflected angle (`tink.wav`) instead of hitting.
- Boomerang (C / gamepad face-up): flies out in an arc, homes back to the hero, stuns enemies it touches (`small enemy hit.wav`), and fetches pickups along the way. Only one is airborne at a time.
- Damage (from tiles, enemy contact, or octorok rocks) knocks the hero back along a directional vector, grants ~1s of invincibility frames with sprite flicker, drains hearts by halves, and shakes the screen briefly.
- Pit tiles freeze the hero over the hole, shrink the sprite, drain half a heart, and respawn the hero on the last safe (non-pit) tile stood on.
- Water: shallow tiles are waded through at reduced speed with splashing steps (`walk water.wav`); deep water (until flippers exist) makes the hero flinch in with expanding splash rings (`fall in water.wav`) and hop back onto the last safe tile, taking no damage. Ice tiles trade direct movement for momentum: the held direction accelerates a velocity vector, releasing it coasts with weak friction, and walls stop the slide.
- Push tiles (`"push": [dx, dy]`, e.g. conveyor belts and water currents) drift the hero while standing on them, on top of input movement, without counting as footsteps; a current can sweep the hero into deep water or a pit.
- Ledge tiles (colliders with `"ledge": "<dir>"`) are one-way: pressing into them along their facing vaults the hero over with a two-tile hop — arc with a ground shadow, `land.wav` and dust rings on touchdown; from other sides they are plain walls.
- Carrying (pots, bushes): lifting is a brief hoist pause; while carrying, the object rides overhead, sword/dash/boomerang are locked out, and the action button throws it along the facing (see Objects above).
- Health is half-heart based (starts at 3 hearts); the HUD hearts row (`assets/ui/hearts.png`, placeholder full/half/empty frames) plus the rupee counter are drawn in virtual pixels over the world. At 1 heart left, hearts flash and a low-health beep plays (`low hp.wav`).
- Death: at 0 hearts the hero spin-collapses (`link dies.wav`), the screen fades, GAME OVER appears, and the action button reloads the room at full hearts (enemies respawn).

The virtual "camera" follows the hero around. However, the camera never leaves the parameter of the map. Because of this, the camera tries to stay centered on the character, unless the character is close enough to the edge of the map.

Draw order (inside the camera transform): room decor + colliders, then floor plates (switches, sunk objects), ground fx (pickups, poofs, projectiles, boomerang), then the depth/y-sort pass — the hero, living enemies, and standing objects back-to-front by baseline (the bottom of their collision boxes), each with a blob shadow at its feet (skipped while falling/splashing/dead) — then lifted/carried/thrown objects over everything, and the room's "upper" layer last.

Room transitions: pressing into a map boundary along a linked edge starts a scroll-in — the new room loads with the hero placed on the opposite edge (perpendicular coordinate preserved from the old room, nudged to the nearest free tile that isn't a pit or deep water), gliding in from one tile off-map over ~0.4s while the camera follows unclamped and the world stays frozen. Non-scroll loads (game start, death-continue, future warp tiles) fade in from black over ~0.3s instead.

## Placeholder assets

The generator synthesizes placeholder art/audio when missing, so the engine stays testable: `resources/ui/border.png` (dialogue nine-patch), `resources/ui/hearts.png` (full/half/empty heart gauge), `resources/characters/link_sword.png` (slash arc sheet: 4 columns down/right/left/up x 3 sweep frames, 64x64 each), `resources/enemies/*.png` (mob blob sheets: 4 columns down/right/left/up x 2 rows stand/move), `resources/pickups/pickups.png` (heart/rupee/rupee5/rock, 16x16 each), `resources/objects/*.png` (pot, bush, block, switch, barrier, sign — single 32x32 frames), and `resources/effects/` (`beam.png` 4-direction sword-beam bolts, `boomerang.png` 4 spin frames, 16x16 each). Drop real files at those paths to replace them; the generator never overwrites existing files.

## Save system

A versioned binary blob (`SaveData` in `src/main.cpp`, magic "LTOP", version 1), one file per slot (`SAVE_SLOT_COUNT` 3; the title screen will pick them — boot continues from slot 0). Desktop writes `save<slot>.dat` in the working directory; the web build writes into raylib's IDBFS-backed `/savefile/` directory so the browser persists it.

Persisted: hero location (room + tile), health + max, the full inventory (equipped item, owned items, rupees/wallet, arrows, bombs, magic, keys), all world flags, the opened-chest registry, the opened-door registry, the boss-defeated registry, visited-room marks (the map screen), one-shot encounter memory, and the world RNG state (drops stay deterministic across sessions). `WORLD_FLAG_COUNT`/`ENCOUNTER_TOTAL` are extern constants, so the schema uses fixed caps (flags 256, encounters 512, chests MAX_CHEST_SLOTS) with bounds-checked copies.

Auto-save (LttP style): on map-edge transitions, warps, chest opens, and the fresh start when a slot begins. Room-local state (push blocks, switches) resets on reload until it moves into the save schema.

Game flow: the title screen lists the three slots (with a hearts/rupees summary of what's inside); the action button starts an empty slot as a new game or continues a used one, resetting all process-global state first so slots never leak into each other. The pause subscreen's dash button is save-and-quit: it banks the save and returns to the title.

## Testing

Per-feature smoke checklist (practiced on every round):
- Generator run with zero warnings (`./build-mac.sh` prints them).
- Native build + web build compile clean (`-Wall -Wextra`).
- Headless boot of the binary: no `WARNING: IMAGE`/`ERROR` lines, no crashes.
- Keyboard + gamepad paths both handled for every new input (the `InputBinding` table).
- Editor round-trips: room and dialogue saves preserve keys the editor doesn't edit.

`scripts/soak_test.py` (exit 0 = healthy):
- Generator determinism: two bakes must produce byte-identical `include_auto`/`src_auto`.
- Save/load soak: `./main --soak` headlessly runs fresh game -> save -> reload -> re-save and requires every persisted byte to round-trip (the rng state is masked: loading a room spawns NPCs, which legitimately advances the world rng).

The generator validates data at bake time (warnings): room layer tile ids in tileset range (out-of-range cleared), animation frames inside their sheets, unknown references (dialogue/enemy/item/object/room/flag names), exits linking to real rooms, orphan rooms unreachable from room 0, and malformed optional fields. The engine asserts indices the generator guarantees (`loadRoom` room index, `tileMetaAt` tile ids).

## Editor

`editor/` contains the resource editor as a Tauri desktop app (React + Vite frontend, Rust backend) for editing `resources/` and running the build. Run with `cd editor && npm run tauri:dev` (or `npm run tauri:build` for a bundled app); it reads/writes only `resources/` and runs `./build-mac.sh` (Mac only) for its Build button. It locates the project via the `LTOP_EDITOR_PROJECT` env var, a persisted folder choice, or by walking up from the executable, and otherwise asks for the repo folder in the UI. See `editor/README.md`.

Room painting covers the `decor`, `colliders`, and `upper` layers (each selectable, each with a visibility toggle; upper paints last in the canvas so it previews over tiles). Saving a room spreads the file's parsed JSON first, so keys the editor doesn't edit (like `enemies` and `objects`) survive the round trip.
