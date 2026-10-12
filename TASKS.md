# TASKS.md

A complete backlog for fleshing this engine out into something capable of re-creating a game like The Legend of Zelda: A Link to the Past. Items are grouped by category; the intent is to be exhaustive, not prescriptive about order. Baseline features that already exist are checked off.

## Foundation & Platform

- [x] Virtual SNES-resolution rendering (224px vertical, widescreen-tolerant, point filtering)
- [x] Native (macOS/clang++) build script
- [x] Web (Emscripten) build with `emscripten_set_main_loop`
- [x] Console-port sanity build (Dreamcast) kept compiling via `build-dreamcast.sh` (KallistiOS + raylib4Consoles in Docker; Gamecube/N64 were evaluated and dropped — their raylib ports ship without the audio module, so they'd be missing-feature targets)
- [ ] Fixed-timestep or delta-time audit so gameplay speed is resolution/framerate independent
- [x] Deterministic RNG (seeded, saveable) for drops and enemy behavior (seeded xorshift in `engine.h`; save integration pending the save system)
- [x] Input remapping layer beyond the hardcoded `ACTION_KEY` (action, sword, item, dash, pause bindings; a map button pending the map screen)

## Player Controller

- [x] 8-direction movement with normalized diagonal speed
- [x] Axis-separated tile collision (wall sliding)
- [x] Map-bounds clamping
- [x] Directional facing + stand/move animations
- [x] Per-room footstep sfx accumulator using real distance
- [x] Player state machine (idle, walk, attack, hurt, fall) so actions can override/cancel cleanly (dash/swim/lift/carry/throw/die states still to add)
- [x] Knockback on taking damage with hurt animation and directional recoil
- [x] Invincibility frames with sprite flicker
- [x] Falling into pits (hole tiles): shrink/drop animation, 1/2-heart damage, respawn at last safe tile
- [x] Deep water: flinch, splash animation, respawn at edge (bounce-back without flippers in; drowning/flippers pending the swimming system)
- [ ] Swimming (after acquiring flippers): swim stroke movement, can only exit water at banks
- [x] Shallow water wading with splashing steps
- [x] Ice tiles: sliding/momentum movement with reduced friction
- [x] Ledge hopping: jumping down predefined ledge edges with a hop animation and landing dust (`"ledge"` tile metadata; vault-over collider tiles, one-way)
- [x] Conveyor belt tiles pushing the player (`"push": [dx, dy]` tile metadata, 4-direction placeholder tiles)
- [x] Wind/current tiles pushing the player (for water flow) (same push mechanism; test current channel in room 01 feeding the pond)
- [x] Pegasus boots: charge dash, breaking pots/plants, crashing into walls (stun), dashing over gaps, damage on contact (dash + wall-crash stun + grass trample + contact damage; pots/gap-jumping pending those systems)
- [x] Carrying state: lifting pots/bushes/rocks over head, walking with them, throwing them (rocks pending gloves)
- [ ] Grabbing and pulling objects (statues, lever blocks) with a pull animation
- [x] Death sequence: spin collapse, hearts drain, game-over screen, respawn flow

## Combat

- [x] Sword slash attack in all 4 facings: swing arc animation, hitbox lifetime, cancels walking
- [x] Spin attack: hold to charge, release for full spin; weapon-shake visual feedback while charging
- [x] Sword beam: fire a projectile at full hearts that travels facing direction and dissipates at screen edge
- [x] Passive shield: blocks projectiles from the faced direction while walking
- [ ] Active shield (holding button): raises shield, blocks wider arcs, movement slows (passive walk block + deflection is in; the raise pose/slow pending)
- [x] Shield knock angle deflection for blocked projectiles
- [ ] Hammer: ground pound with quake circle, breaks rocks/stakes, flips enemies
- [x] Enemy contact damage to player (hearts/quarters-of-hearts), knockback, i-frames
- [x] Player attack resolution: single-hit-per-swing against an enemy, hitstun, knockback, death poof
- [x] Enemy death effects: cloud poof (explosion for bomb-vulnerable enemies pending bombs)
- [x] Hit-pause / screen shake feedback for impactful hits (screen shake; hit-pause pending)
- [x] Grass cutting: slash removes grass tiles, spawns drop + rustle sfx
- [ ] Bush cutting (only after they've been made liftable-or-cuttable per tile data)

## Items & Inventory

- [x] Inventory data model: item list, equipment grid (A/B assignment like LttP), quantities (bombs, arrows, rupees) (`Inventory` struct + baked `ITEMS`; sword fixed + one assignable item slot — full A/B grid pending equipment variety)
- [x] Item selection screen (pause menu) with grid cursor and A/B slot assignment (pause = item subscreen: cursor + equip + quantities/magic readouts)
- [x] Bow + arrows: consumes arrows (or rupees at max), projectile with tile/enemy collision
- [ ] Silver arrows upgrade
- [x] Bombs: place, fuse timer, explosion hitbox (damages enemies, blasts walls/cracked floors), pickup-throw variant (place + fuse + blast + bombable tiles in; pickup-throw pending)
- [x] Boomerang: outward arc and return, stuns enemies, grabs items, cannot be reused while airborne
- [ ] Magical boomerang upgrade
- [ ] Hookshot: extends, pulls Link, retrieves items, stuns enemies, latches to hook targets
- [x] Lantern: light radius in darkness, ignites torches, consumes magic (passive radius + slow drain + auto-lighting adjacent torches in)
- [x] Fire rod / Ice rod: projectile magic with tile interactions (light torches, freeze enemies, melt things) (bolts burn/shatter/freezes + torch lighting in; tile melting pending tiles that need it)
- [ ] Bombos / Ether / Quake medallions: screen-wide effects with visual weather effects
- [ ] Cane of Somaria: create blocks on red switches, block-based puzzles
- [ ] Cane of Byrna: invincibility aura consuming magic
- [ ] Magic cape: invisible/intangible while draining magic
- [ ] Magic powder: sprinkle on enemies/objects (transform effects, unlock shrine)
- [ ] Shovel: dig spots with buried items
- [ ] Flute: bird summon fast-travel points
- [ ] Bottles: fill with potions/fairies/bugs, use contents
- [ ] Bug-catching net: catches bees/fairies, agitates enemies
- [x] Magic meter: capacity, consumption per item, refill potions/fountains (meter + HUD + drop refills + rod consumption in; potions/fountains pending)
- [x] Heart containers/pieces: pickup, health max increase, chest fanfare (container pickup + max-raise in; pieces + chest grant pending)
- [x] Rupee wallets and rupee counting with loss-on-damage (wallet cap + counting in; LttP doesn't drop rupees on hit — intentionally omitted)
- [x] Item-get sequence: hold-up pose, fanfare, description dialogue, inventory update

## Enemies & AI

- [x] Enemy framework: spawnable entity configs in resources (sprite sheets, stats, drops, sounds) baked by the generator
- [x] Enemy lifecycle: idle/notice/attack/hurt/death states, room-local spawning
- [x] Room spawn sets: enemies defined per room in json, spawned on room entry (editor placement UI pending)
- [x] Room-clear state (enemies defeated until room reload, like dungeon rooms)
- [x] Common grunt AI archetypes: soldier (patrol + charge), octorok (shoot + walk) implemented data-driven; keese/slime/stalfos/etc. pending
- [x] Enemy variants: blue/red (speed, hp, projectile) via config reuse (soldier vs soldier_blue)
- [x] Enemy projectiles (rocks, spears, fireballs — rocks via octorok) with tile/player collision
- [x] Projectiles blocked by shield depending on facing (octorok rocks deflect off the passive shield)
- [x] Stun mechanic (boomerang), grab-and-throw some stunned enemies (throw pending)
- [ ] Element interactions: fire (burn), ice (freeze/thaw), bombs (explode), powder (transform) (fire/ice/bombs in via rods + bombs item; powder pending)
- [ ] Ambush/spawn-from-object enemies (wall hide, pots popping out)
- [ ] Zora-in-water, wall-crawling and diving enemy types (depth quirks)
- [x] Enemy drop table: hearts, rupees (1/5) with weighted rolls (fairies, magic, bombs/arrows + LttP persistent drop cycling pending)
- [ ] Placed-item entities: hearts in grass, buried rupees, rupee-crab decoys

## Bosses

- [x] Boss framework: multi-phase scripted enemies, boss health, arena door locks (phased-charge bosses, HUD health bar, whole-room seal while alive, permanent defeat)
- [ ] Dungeon boss archetypes (Helmasaur, Armos Knights, Lanmolas, Moldorm, Agahnim, etc. behavior patterns: patterned charges, tail-weak-point, reflectable magic, phase transitions) (one archetype in: the phased-charge knight)
- [ ] Boss intro / defeat sequences: heart container drop, dungeon-complete fanfare, story flags (defeat sequence + heart container + fanfare in; intro + story flags pending)
- [ ] Mini-bosses and mid-dungeon arena fights

## Rooms, World & Transitions

- [x] Rooms with decor + collider layers (both rendered, colliders block) — pure `.json`, edited in the web editor (no Tiled)
- [x] Per-room music with no-restart logic
- [x] Camera centered on hero and clamped to map bounds
- [x] One-shot encounter memory (process-lifetime) with `repeat` bypass
- [x] Room interconnection data model (map graph): doors, edges, stairs, warps defined in the editor (`"exits"` per-edge links baked as room indices; door/stair/warp entities pending)
- [x] Edge-scroll transitions: hero exits screen edge, camera + hero glide into adjacent room (the LttP signature) (scroll-in glide with unclamped camera follow; both-rooms seam scroll pending)
- [x] Fade transitions for warp/door tiles and house entrances (fade-in from black on non-scroll loads; warp/door tiles pending)
- [x] Door entities: open, closed (opens from either side), locked (small key), big-key, bombable, treasure-chest-triggered, shutter doors that close for fights (all kinds but treasure-chest-triggered in; doors pair-sync across rooms and persist in the save)
- [ ] Staircases / pit-landing transitions between vertically-stacked room layers
- [ ] Overworld <-> dungeon room treatment (persistent enemies vs. respawn-on-reentry)
- [x] Upper/decor-above layer: tiles drawn over the hero (tree tops, archways) (`"upper"` room layer, optional; test canopy strip in room 01)
- [x] Depth sorting: hero renders behind bottom-half of tall obstacles (bushes, signs, statues) (baseline y-sort of hero/enemies/standing objects)
- [x] Room-entry point resolution (entering from north vs. south places hero at the right door) (entry edge + preserved perpendicular coordinate + nearest-free-tile search)
- [ ] Large-room scrolling within a single room larger than one screen
- [x] Dark rooms: lighting masks, lantern radius, torch-lighting reveals (lightmap: ambient gloom + additive radial lights, multiplied blend; lantern + lit torches reveal)
- [ ] Weather/ambience overlays (rain, lightning flashes, fog)
- [ ] Light/Dark world: mirror transform, parallel world data pairing, mirror-warps
- [ ] Dynamic tiles: bombable walls, cracked floors with holes, burned-open entrances, permanently destructible in save state (bombable walls/floors blast open at runtime; permanence pending the save system)
- [ ] Water current routing (river pushing hero toward specific edges)

## Objects & Interactive Entities

- [x] Entity/object framework: non-character entities with collision and interaction, defined in room data, visible in editor (room-data objects solid via a Room solidity hook; editor placement UI pending)
- [x] Chests: normal, locked, big; open animation, item pop-out, hold-up sequence, permanently-open flag (lid-open frame + rise + hold-up + permanent-open registry in; save-schema permanence pending)
- [x] Small/big keys per dungeon inventory, consumed on unlock, persistent door state (consumed on chest unlock in; per-dungeon counts + door state pending dungeons/save)
- [x] Push blocks: pushable one tile, sink into floor when on a switch; session vs. permanent states (sustain-push + slide + sink; session-only until the save system)
- [ ] Statues: pullable, pushable onto floor switches (pushable via the block mechanic; pulling pending)
- [x] Floor switches: hold-weight type, latch type, linked to doors/barriers (barriers in; doors pending)
- [ ] Crystal switch: toggles blue/red barrier blocks dungeon-wide
- [ ] Blue/red barrier blocks (raised/lowered state, damage on wrong approach)
- [x] Torch posts and lanterns: ignite with fire items, timers, light contribution (fire-rod lighting + barrier link groups in; timers/light radius pending dark rooms)
- [x] Pots: liftable, throwable (smash + drops), swap out for enemies occasionally (pot-swap enemies pending)
- [x] Bushes: liftable, sometimes hide enemies/rupees (liftable + throw-smash in; hidden enemies/rupees pending)
- [ ] Rocks: hammer/smashable, big rocks need gloves
- [x] Signs: readable via action button, includes directional orientation (readable via dialogue; directional orientation art pending)
- [ ] Levers/valves: pull to open gates, drain water, move platforms
- [ ] Moving platforms: rail-based, water-surface, carry the hero (and thrown objects)
- [ ] Spinner/vane tiles, pressure-plate timed puzzles
- [ ] Warp tiles, warp pads, telepathic story tiles
- [ ] Fairy fountains (heal), sand hourglass / timed challenge rooms
- [x] Range-exchange system for thrown/liftable objects (physics arc, tile collision on land, splash in water)
- [ ] Rupee-like pickups: heart, rupee, fairy, magic, keys, bombs/arrows, with magnet-ish pull from boomerang/hookshot

## NPCs, Dialogue & Scripting

- [x] LttP-style typewriter dialogue: nine-patch border, word wrap, ~30 cps, skip/advance via action key
- [x] Per-letter and per-line completion sfx
- [x] Movement freeze during dialogue
- [x] Action scripting in dialogue files (the future feature NOTES calls out): commands for setting flags, giving items, playing sfx/music, warping, camera control, branch on story state (flags/sfx/music/warp/labels/branches in; give-items + camera pending the item/cutscene systems)
- [x] Dialogue choice branches (yes/no selections with a cursor)
- [x] Conditional dialogue lines based on world flags/inventory (world flags in; inventory conditions pending the item system)
- [x] NPC entities: idle, wander, path-following, facing hero when talked to (idle + wander + talk-facing in; path-following pending)
- [x] NPC schedules and position by story state (spawn conditions on world flags — presence by story state in; schedules pending)
- [ ] Shop system: rupee exchanges, item purchase, robbing and consequences
- [x] Chest/held-item dialogue coupling (item descriptions on pickup)
- [ ] Cutscene framework: scripted camera pans, character blocking, music stingers, letterbox or forced-walk segments
- [ ] Endgame/credits sequence

## UI, HUD & Menus

- [x] HUD: hearts row, rupee counter (magic meter, keys/bombs/arrows equipped-item display pending) (magic gauge + equipped item/count in; keys pending the chest system)
- [x] Pause menu (LttP map screen): dungeon map, floor selector, items grid, equipment grid, save/quit (items grid + map tab + save-and-quit in; dungeon floors/equipment grid pending their systems)
- [ ] Dungeon map item: minimap of visited rooms + compass chest/boss markers (visited-room minimap on the map tab in; compass markers pending)
- [x] Overworld map subscreen (the map tab draws the visited-room world graph)
- [x] Title screen, file select (3 slots), name entry, copy/erase (title + 3-slot select with summaries in; name entry/copy/erase pending)
- [x] Game-over and continue flow (collapse -> GAME OVER -> action continues in the room at full hearts)
- [x] Low-health beeping and flashing hearts
- [x] Dialogue with visual prompt (arrow indicator on multi-line, waiting state)
- [x] Generic toasts / notifications (item-get, learned hints) (toast banner system in, wired to autosaves; item-get uses the dialogue flow)

## Save System

- [x] Persistent save state: hero location, health, magic, inventory, rupees, dungeon keys, world flags (chests opened, doors unlocked, walls blasted, bushes pulled, enemies-forever-dead, story progress) (versioned binary blob; chests/doors/flags/encounters/inventory/rng in — walls/bushes/enemies-forever-dead pending those systems)
- [x] 3 save slots, selectable from the title screen
- [x] Save points (church, ponds) / save-and-quit (autosave on transitions/warps/chests + save-and-quit via the pause subscreen in; dedicated save points pending)
- [ ] Room state persistence distinct from encounter one-shot memory (bake-ids into the save schema) (encounter one-shots persist; room-local blocks/switches reset pending the schema work)
- [ ] Editor tooling awareness of flags: naming, listing, renaming safeties (flags bake as `WORLD_FLAG_NAMES`; editor UI pending)

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

- [x] Depth/y-sort pass for entities and hero vs. tall tiles (entity baseline sort; tall tiles pending tile metadata)
- [x] Sweep/arc visual for sword slash with directional variants (4-direction arc sheet)
- [ ] Sprite effects: flash on hit (tint), freeze frames, dust/leaves/grass-chunk particles, splashes (enemy hit flash in; particles pending)
- [x] Bomb and explosion animation entities (fuse-blink bomb + expanding explosion rings + white flash)
- [ ] Screen-space effects: shake, flash, fade in/out helper (shake + death fade in; flash/fade helpers pending)
- [ ] LttP-style palette: master palette with swap-on-room-change fade for screen transitions
- [ ] Night palette or ambient tint states
- [x] Reflections in shallow water, tree-blob shadows under entities (blob shadows under hero/enemies; reflections pending)
- [x] Thrown-object arc shadow rendering

## Editor & Tooling

- [x] Rooms (default config, encounters, tile painting), Dialogue, Characters, binary asset upload, Build button
- [ ] Tile property editor: solid type, step-sfx type, cuttable, burnable, bombable, pit, water depth, ledge direction, current direction, damage, light-block, switch metadata
- [x] Upper layer painting + layer visibility toggles (decor/colliders/upper selectable + toggleable)
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
- [x] Generator-side validation: frame index bounds, missing-dialogue references, orphan rooms, tile id sanity, unique encounter ids (anim frames, layer tile ids, orphan rooms, all name refs warn; encounter ids unique by construction)
- [ ] Baked asset indexing for fast lookup by name (rooms by name/stem, strings, etc.)
- [x] Deterministic generator output (no churn on rebuild) (verified by the soak test's double-bake hash)
- [ ] Generated-code size management as data scales (web build memory budget)
- [x] Editor saves JSON in a canonical, generator-compatible format (save round-trips the full file: unknown keys like enemies/objects are preserved)

## Testing & Verification

- [x] Engine-side assertions for data misuse (out-of-range indices, bad ids) instead of silent clamping in release (asserts on bake-guaranteed indices: room loads, tile metadata; data-level guards + bake warnings stay graceful)
- [ ] Test maps exercising each new system (ledge course, ice rink, water flows, switch gauntlet)
- [x] Smoke checklist per feature: native + web builds, controller + keyboard, editor round-trip (documented in NOTES.md Testing; practiced each round)
- [x] Save/load soak test: all flags survive a round trip (headless --soak: byte-level round trip of every persisted field, rng masked as load-advancing; wired into scripts/soak_test.py)
