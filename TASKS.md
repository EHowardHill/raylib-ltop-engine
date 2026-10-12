# TASKS.md

A complete backlog for fleshing this engine out into something capable of re-creating a game like The Legend of Zelda: A Link to the Past. Items are grouped by category; the intent is to be exhaustive, not prescriptive about order. Baseline features that already exist are checked off.

## Foundation & Platform

- [x] Virtual SNES-resolution rendering (224px vertical, widescreen-tolerant, point filtering)
- [x] Native (macOS/clang++) build script
- [x] Web (Emscripten) build with `emscripten_set_main_loop`
- [ ] Console-port sanity builds (PSP/Dreamcast/other raylib platforms) kept compiling via CI or a makefile target
- [ ] Fixed-timestep or delta-time audit so gameplay speed is resolution/framerate independent
- [ ] Deterministic RNG (seeded, saveable) for drops and enemy behavior
- [ ] Input remapping layer beyond the hardcoded `ACTION_KEY` (multiple action buttons: sword, item, dash, map, pause)
- [ ] Touch controls for the web build (virtual dpad + buttons)

## Player Controller

- [x] 8-direction movement with normalized diagonal speed
- [x] Axis-separated tile collision (wall sliding)
- [x] Map-bounds clamping
- [x] Directional facing + stand/move animations
- [x] Per-room footstep sfx accumulator using real distance
- [ ] Player state machine (idle, walk, attack, dash, swim, lift, carry, throw, hurt, fall, drown, die) so actions can override/cancel cleanly
- [ ] Knockback on taking damage with hurt animation and directional recoil
- [ ] Invincibility frames with sprite flicker
- [ ] Falling into pits (hole tiles): shrink/drop animation, 1/2-heart damage, respawn at last safe tile
- [ ] Deep water: flinch, splash animation, respawn at edge (or drown without flippers)
- [ ] Swimming (after acquiring flippers): swim stroke movement, can only exit water at banks
- [ ] Shallow water wading with splashing steps
- [ ] Ice tiles: sliding/momentum movement with reduced friction
- [ ] Ledge hopping: jumping down predefined ledge edges with a hop animation and landing dust
- [ ] Conveyor belt tiles pushing the player
- [ ] Wind/current tiles pushing the player (for water flow)
- [ ] Pegasus boots: charge dash, breaking pots/plants, crashing into walls (stun), dashing over gaps, damage on contact
- [ ] Carrying state: lifting pots/bushes/rocks over head, walking with them, throwing them
- [ ] Grabbing and pulling objects (statues, lever blocks) with a pull animation
- [ ] Death sequence: spin collapse, hearts drain, game-over screen, respawn flow

## Combat

- [ ] Sword slash attack in all 4 facings: swing arc animation, hitbox lifetime, cancels walking
- [ ] Spin attack: hold to charge, release for full spin; weapon-shake visual feedback while charging
- [ ] Sword beam: fire a projectile at full hearts that travels facing direction and dissipates at screen edge
- [ ] Passive shield: blocks projectiles from the faced direction while walking
- [ ] Active shield (holding button): raises shield, blocks wider arcs, movement slows
- [ ] Shield knock angle deflection for blocked projectiles
- [ ] Hammer: ground pound with quake circle, breaks rocks/stakes, flips enemies
- [ ] Enemy contact damage to player (hearts/quarters-of-hearts), knockback, i-frames
- [ ] Player attack resolution: single-hit-per-swing against an enemy, hitstun, knockback, death poof
- [ ] Enemy death effects: cloud poof, explosion for bomb-vulnerable enemies
- [ ] Hit-pause / screen shake feedback for impactful hits
- [ ] Grass cutting: slash removes grass tiles, spawns drop + rustle sfx
- [ ] Bush cutting (only after they've been made liftable-or-cuttable per tile data)

## Items & Inventory

- [ ] Inventory data model: item list, equipment grid (A/B assignment like LttP), quantities (bombs, arrows, rupees)
- [ ] Item selection screen (pause menu) with grid cursor and A/B slot assignment
- [ ] Bow + arrows: consumes arrows (or rupees at max), projectile with tile/enemy collision
- [ ] Silver arrows upgrade
- [ ] Bombs: place, fuse timer, explosion hitbox (damages enemies, blasts walls/ cracked floors), pickup-throw variant
- [ ] Boomerang: outward arc and return, stuns enemies, grabs items, cannot be reused while airborne
- [ ] Magical boomerang upgrade
- [ ] Hookshot: extends, pulls Link, retrieves items, stuns enemies, latches to hook targets
- [ ] Lantern: light radius in darkness, ignites torches, consumes magic
- [ ] Fire rod / Ice rod: projectile magic with tile interactions (light torches, freeze enemies, melt things)
- [ ] Bombos / Ether / Quake medallions: screen-wide effects with visual weather effects
- [ ] Cane of Somaria: create blocks on red switches, block-based puzzles
- [ ] Cane of Byrna: invincibility aura consuming magic
- [ ] Magic cape: invisible/intangible while draining magic
- [ ] Magic powder: sprinkle on enemies/objects (transform effects, unlock shrine)
- [ ] Shovel: dig spots with buried items
- [ ] Flute: bird summon fast-travel points
- [ ] Bottles: fill with potions/fairies/bugs, use contents
- [ ] Bug-catching net: catches bees/fairies, agitates enemies
- [ ] Magic meter: capacity, consumption per item, refill potions/fountains
- [ ] Heart containers/pieces: pickup, health max increase, chest fanfare
- [ ] Rupee wallets and rupee counting with loss-on-damage
- [ ] Item-get sequence: hold-up pose, fanfare, description dialogue, inventory update

## Enemies & AI

- [ ] Enemy framework: spawnable entity configs in resources (sprite sheets, stats, drops, sounds) baked by the generator
- [ ] Enemy lifecycle: idle/notice/attack/hurt/death states, room-local spawning
- [ ] Room spawn sets: enemies defined per room in the editor, spawned on room entry
- [ ] Room-clear state (enemies defeated until room reload, like dungeon rooms)
- [ ] Common grunt AI archetypes: soldier (patrol + charge), octorok (shoot + walk), keese (erratic flight), slime/blob (hop, split), stalfos (bounce), rat, vulture, popo, etc. — a data-driven behavior library
- [ ] Enemy variants: blue/red (speed, hp, projectile) via config reuse
- [ ] Enemy projectiles (rocks, spears, fireballs) with tile/player collision
- [ ] Projectiles blocked by shield depending on facing
- [ ] Stun mechanic (boomerang/hookshot), grab-and-throw some stunned enemies
- [ ] Element interactions: fire (burn), ice (freeze/thaw), bombs (explode), powder (transform)
- [ ] Ambush/spawn-from-object enemies (wall hide, pots popping out)
- [ ] Zora-in-water, wall-crawling and diving enemy types (depth quirks)
- [ ] Enemy drop table: hearts, rupees (1/5), fairies, magic, bombs/arrows — with the LttP weighted, semi-persistent drop cycling
- [ ] Placed-item entities: hearts in grass, buried rupees, rupee-crab decoys

## Bosses

- [ ] Boss framework: multi-phase scripted enemies, boss health, arena door locks
- [ ] Dungeon boss archetypes (Helmasaur, Armos Knights, Lanmolas, Moldorm, Agahnim, etc. behavior patterns: patterned charges, tail-weak-point, reflectable magic, phase transitions)
- [ ] Boss intro / defeat sequences: heart container drop, dungeon-complete fanfare, story flags
- [ ] Mini-bosses and mid-dungeon arena fights

## Rooms, World & Transitions

- [x] Rooms with decor + collider layers (both rendered, colliders block) — pure `.json`, edited in the web editor (no Tiled)
- [x] Per-room music with no-restart logic
- [x] Camera centered on hero and clamped to map bounds
- [x] One-shot encounter memory (process-lifetime) with `repeat` bypass
- [ ] Room interconnection data model (map graph): doors, edges, stairs, warps defined in the editor
- [ ] Edge-scroll transitions: hero exits screen edge, camera + hero glide into adjacent room (the LttP signature)
- [ ] Fade transitions for warp/door tiles and house entrances
- [ ] Door entities: open, closed (opens from either side), locked (small key), big-key, bombable, treasure-chest-triggered, shutter doors that close for fights
- [ ] Staircases / pit-landing transitions between vertically-stacked room layers
- [ ] Overworld <-> dungeon room treatment (persistent enemies vs. respawn-on-reentry)
- [ ] Upper/decor-above layer: tiles drawn over the hero (tree tops, archways)
- [ ] Depth sorting: hero renders behind bottom-half of tall obstacles (bushes, signs, statues)
- [ ] Room-entry point resolution (entering from north vs. south places hero at the right door)
- [ ] Large-room scrolling within a single room larger than one screen
- [ ] Dark rooms: lighting masks, lantern radius, torch-lighting reveals
- [ ] Weather/ambience overlays (rain, lightning flashes, fog)
- [ ] Light/Dark world: mirror transform, parallel world data pairing, mirror-warps
- [ ] Dynamic tiles: bombable walls, cracked floors with holes, burned-open entrances, permanently destructible in save state
- [ ] Water current routing (river pushing hero toward specific edges)

## Objects & Interactive Entities

- [ ] Entity/object framework: non-character entities with collision and interaction, defined in room data, visible in editor
- [ ] Chests: normal, locked, big; open animation, item pop-out, hold-up sequence, permanently-open flag
- [ ] Small/big keys per dungeon inventory, consumed on unlock, persistent door state
- [ ] Push blocks: pushable one tile, sink into floor when on a switch; session vs. permanent states
- [ ] Statues: pullable, pushable onto floor switches
- [ ] Floor switches: hold-weight type, latch type, linked to doors/barriers
- [ ] Crystal switch: toggles blue/red barrier blocks dungeon-wide
- [ ] Blue/red barrier blocks (raised/lowered state, damage on wrong approach)
- [ ] Torch posts and lanterns: ignite with fire items, timers, light contribution
- [ ] Pots: liftable, throwable (smash + drops), swap out for enemies occasionally
- [ ] Bushes: liftable, sometimes hide enemies/rupees
- [ ] Rocks: hammer/smashable, big rocks need gloves
- [ ] Signs: readable via action button, includes directional orientation
- [ ] Levers/valves: pull to open gates, drain water, move platforms
- [ ] Moving platforms: rail-based, water-surface, carry the hero (and thrown objects)
- [ ] Spinner/vane tiles, pressure-plate timed puzzles
- [ ] Warp tiles, warp pads, telepathic story tiles
- [ ] Fairy fountains (heal), sand hourglass / timed challenge rooms
- [ ] Range-exchange system for thrown/liftable objects (physics arc, tile collision on land, splash in water)
- [ ] Rupee-like pickups: heart, rupee, fairy, magic, keys, bombs/arrows, with magnet-ish pull from boomerang/hookshot

## NPCs, Dialogue & Scripting

- [x] LttP-style typewriter dialogue: nine-patch border, word wrap, ~30 cps, skip/advance via action key
- [x] Per-letter and per-line completion sfx
- [x] Movement freeze during dialogue
- [ ] Action scripting in dialogue files (the future feature NOTES calls out): commands for setting flags, giving items, playing sfx/music, warping, camera control, branch on story state
- [ ] Dialogue choice branches (yes/no selections with a cursor)
- [ ] Conditional dialogue lines based on world flags/inventory
- [ ] NPC entities: idle, wander, path-following, facing hero when talked to
- [ ] NPC schedules and position by story state
- [ ] Shop system: rupee exchanges, item purchase, robbing and consequences
- [ ] Chest/held-item dialogue coupling (item descriptions on pickup)
- [ ] Cutscene framework: scripted camera pans, character blocking, music stingers, letterbox or forced-walk segments
- [ ] Endgame/credits sequence

## UI, HUD & Menus

- [ ] HUD: hearts row, magic meter, rupee counter, keys/bombs/arrows equipped-item display, all drawn in virtual pixels over the world
- [ ] Pause menu (LttP map screen): dungeon map, floor selector, items grid, equipment grid, save/quit
- [ ] Dungeon map item: minimap of visited rooms + compass chest/boss markers
- [ ] Overworld map subscreen
- [ ] Title screen, file select (3 slots), name entry, copy/erase
- [ ] Game-over and continue flow
- [ ] Low-health beeping and flashing hearts
- [ ] Dialogue with visual prompt (arrow indicator on multi-line, waiting state)
- [ ] Generic toasts / notifications (item-get, learned hints)

## Save System

- [ ] Persistent save state: hero location, health, magic, inventory, rupees, dungeon keys, world flags (chests opened, doors unlocked, walls blasted, bushes pulled, enemies-forever-dead, story progress)
- [ ] 3 save slots, selectable from the title screen
- [ ] Save points (church, ponds) / save-and-quit
- [ ] Room state persistence distinct from encounter one-shot memory (bake-ids into the save schema)
- [ ] Editor tooling awareness of flags: naming, listing, renaming safeties

## Audio

- [x] Music streaming, no-restart when same file
- [x] Walk sfx per room, letter sfx in dialogue
- [ ] Sfx bank convention (hurt, slash, dash, explosion, rupee, secrets, fanfares)
- [ ] Ambient world sfx (birds, rain) and layered ambience
- [ ] Fanfare handling: temporarily duck/pause bgm for item-get, resume properly
- [ ] LttP "secret discovered" jingle convention
- [ ] Dynamic music layers / crossfade for danger states if desired
- [ ] Web build: memory-conscious audio loading strategy (lazy streaming, prioritized bake order)

## Rendering & Presentation

- [ ] Depth/y-sort pass for entities and hero vs. tall tiles
- [ ] Sweep/arc visual for sword slash with directional variants
- [ ] Sprite effects: flash on hit (tint), freeze frames, dust/leaves/grass-chunk particles, splashes
- [ ] Bomb and explosion animation entities
- [ ] Screen-space effects: shake, flash, fade in/out helper
- [ ] LttP-style palette: master palette with swap-on-room-change fade for screen transitions
- [ ] Night palette or ambient tint states
- [ ] Reflections in shallow water, tree-blob shadows under entities
- [ ] Thrown-object arc shadow rendering

## Editor & Tooling

- [x] Rooms (default config, encounters, tile painting), Dialogue, Characters, binary asset upload, Build button
- [ ] Tile property editor: solid type, step-sfx type, cuttable, burnable, bombable, pit, water depth, ledge direction, current direction, damage, light-block, switch metadata
- [ ] Upper layer painting + layer visibility toggles
- [ ] Object/entity placement mode (chests, blocks, NPCs, enemies, torches, platforms, signs, warps) with drag/properties panel
- [ ] Door and room-link editing (draw links between room edges, edit warp targets, stair pairs)
- [ ] Room graph preview / minimap of the world connections
- [ ] Enemy definitions editor (sprite sheets, stats, drops, behavior parameters)
- [ ] Item/inventory definitions editor
- [ ] World flags list view with names
- [ ] Dialogue editor: branch/choice tree view, script command insertion
- [ ] Test-from-here: launch the build with a specific room + hero position
- [ ] Build button parity on non-darwin (build-web or platform-detect)

## Data Pipeline & Generator

- [x] `.json` room/tileset data baked into `include_auto`/`src_auto`, binary-only runtime loads from `assets/`
- [x] Optional-field handling with defaults (spawn, music, walk_sfx, dialogue, repeat)
- [ ] Bake pipeline for new data families: enemies, items, objects, room links, tile metadata, flags, save schema
- [ ] Generator-side validation: frame index bounds, missing-dialogue references, orphan rooms, tile id sanity, unique encounter ids
- [ ] Baked asset indexing for fast lookup by name (rooms by name/stem, strings, etc.)
- [ ] Deterministic generator output (no churn on rebuild)
- [ ] Generated-code size management as data scales (web build memory budget)
- [ ] Editor saves JSON in a canonical, generator-compatible format

## Testing & Verification

- [ ] Engine-side assertions for data misuse (out-of-range indices, bad ids) instead of silent clamping in release
- [ ] Test maps exercising each new system (ledge course, ice rink, water flows, switch gauntlet)
- [ ] Smoke checklist per feature: native + web builds, controller + keyboard, editor round-trip
- [ ] Save/load soak test: all flags survive a round trip
