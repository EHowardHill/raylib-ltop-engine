# TASKS.md — everything the Editor needs to build a game, start to finish

A checklist of editor capabilities required to author a complete game from a
fresh folder to credits — without touching anything outside the editor, except
actual *pixel* work (GIMP) and *music/tracker* work (MilkyTracker). The editor
never edits pixels or notes: it **imports/replaces** the resulting files.

Status key: `[done]` exists today, `[todo]` missing, `[partial]` exists but
incomplete.

---

## 1. File tab (NEW — top priority)

Today the project-management actions ("New game", open/switch project, game
identity) are scattered across the Build tab, the sidebar footer, and a
fallback picker screen. That's messy. **Add a "File" tab, pinned first in the
nav bar, visually distinct (different background/accent color from the other
tabs)**, that owns every project-level meta action:

- [done] **File tab** — first position in the nav, distinct color/active style
  so it reads as "menu bar", not "workspace".
- [done] **New game** — create a starter game folder (game.json, starter
  tileset, hero character, first walled room), switch the editor to it.
- [done] **Open / switch game** — folder picker + a "recent games" list.
  (The folder-picker-as-game-picker logic exists; surface it here.)
- [done] **Game identity** — title + slug form, with the "changing the slug
  orphans saves" warning. (Form exists on the Build tab; move it here.)
- [done] **Save slot tools** — list the 3 slots (`save<slot>.dat`), show
  contents summary, delete a slot (the title screen can't do this).
- [done] **Build & Launch** — build for the host platform and run the game
  (full, from title). The Build tab keeps the detailed log/output view; the
  File tab gets a one-click "Make & Play".
- [done] **Quit / restart dev build** (optional, QoL) — the launched game is
  tracked: a new launch replaces the running session (restart), and a Quit
  button stops it.

---

## 2. World & rooms

- [done] Room list: create, rename, delete, resize rooms.
- [done] Tile painting on `decor`, `colliders`, `upper` layers with visibility
  toggles, grid, zoom.
- [done] Per-room tileset picker (sidebar sheet selection).
- [done] Entity placement on the canvas: objects, enemies, NPCs — place,
  drag, delete, type-driven property forms (chests, warps, stairs, switches,
  torches, levers, barriers, signs, pots w/ hidden enemy, platform rails).
- [done] **Encounter editing** — markers + property rows: location, dialogue
  pick, repeat, `if`/`unless` flag guards (selected-row form).
- [done] **Pickups / buried items** — the room canvas places, drags, and
  deletes pickups ("Place Pickup" tool, kind select + buried toggle), saved
  into the room's `pickups` / `buried` lists.
- [done] Doors & edge connections: per-edge door kind, exit target, mirror
  pair, pit-landing (room + tile).
- [done] World graph minimap (exits, mirrors, pits, warps, stairs) with
  click-to-jump.
- [done] Room ambience/music selection per room.
- [done] "Play from here" — build + boot into a room at a tile, saving
  suppressed.

## 3. Tiles

- [done] Tileset manager: create/delete sheets, upload/replace the sheet
  `.png` in place.
- [done] Per-tile behavior editor: pit/ice/heal/dig flags, water depth,
  ledge direction, cuttable + result, bombable + result, push current,
  contact damage.
- [done] **Tileset preview niceties** — hover magnifier/click-to-paint from
  the tileset into the room is functional; parity holds as sheets are
  replaced with real GIMP art (the scan reads sheet dimensions live from
  the PNG, so the palette and map re-derive on every replace).

## 4. Characters & NPCs

- [done] Character sheet roster: create/delete characters, upload/replace
  sprite sheet PNGs.
- [done] Stand/move animation frame picker per direction (grid + indices),
  animation preview at engine frame rates.
- [done] NPC placement in rooms with property forms (character, dialogue,
  wander, path waypoints, `if`/`unless` flag gates).
- [done] **NPC path waypoint editing on the room canvas** — the form takes
  `[[x,y],...]`; with the NPC (or platform) selected, its rail renders as a
  connected line of numbered circles — click open ground to extend, drag
  the circles to shape, right-click one to remove (Del removes the selected
  waypoint, Esc deselects).

## 5. Enemies

- [done] Enemy list: create (stub that bakes clean, placeholder art from
  color) / delete.
- [done] Property form: sheet + placeholder color, stats (hp/speed/damage),
  behavior presets + `custom:` behaviors from `code/manifest.json`, sight,
  shoot period, component checkboxes (boss / mini_boss / strike_immune /
  catchable) with mover knobs, drop tables, frame lists per direction.
- [done] Game-code behavior scaffolding entry point + manifest validation.
- [done] **Enemy test sandbox** — "Test in gauntlet" on the Enemies tab:
  build, then boot the gauntlet room with the enemy spawned beside the
  drop tile (the engine's `--enemy <name>` boot flag, a save-suppressed
  session); games without a gauntlet room fall back to the first room.

## 6. Objects

- [done] Object types render in the room placement UI from
  `resources/objects/types.json`; per-type property forms exist.
- [done] **Object type editor** — Object Types tab: create/edit/delete object
  *types* (name, sprite image, the `interactions` mask as checkboxes over the
  vocabulary) writing `types.json` with unknown-key preservation.
- [done] Type sprite upload/replace (objects dir is in the upload target
  list — keep it).

## 7. Items

- [done] Item list + forms: label, use-action kind over the full vocabulary,
  element, sfx, cost forms (magic / ammo + rupee fallback), Somaria spawn
  pick, passives (light/swim/lift/compass), flag-gated upgrades.
- [done] Item icon/sprite management — items reference sheets the engine
  reads; upload/replace stays in Files.
- [done] **Item subscreen layout preview** — render the pause grid as the
  engine will (owned cells, quantities, magic bar, the border nine-patch;
  values mirror the demo session start) so item authoring matches the
  player's view.

## 8. Dialogue & scripting

- [done] Structured line cards over the full vocabulary: text, labels,
  if/unless flags + items, give, set/clear, sfx, music (incl. stop-bgm),
  goto, warp, choices (cost/give shops), cutscene cues (wait beats, camera
  pan/return, letterbox, actor blocking).
- [done] Branch tree view (SVG control-flow map).
- [done] Credits + `next` chain header, insert-command buttons.
- [done] Save round-trips unknown keys and meaningful `""` values.

## 9. World flags

- [done] Flags tab: registry from `generator --flags`, provenance, filter,
  save-safety guidance.

## 10. Game code (`resources/code/`)

- [done] Code tab: file list + in-window editor, scaffolded registration
  boilerplate, manifest append, compile check w/ inline errors.
- [done] **Behavior reference/cheat-sheet panel** — the `Enemy` API
  (`setMode`, `slide`, `scratch(i)`, trail/weak-point vocabulary) rendered
  alongside the editor (a sticky column on the Code tab) so behaviors can
  be written without reading engine headers.

## 11. Asset import (the GIMP / MilkyTrader seam)

No pixel or note editing — import/replace only:

- [done] **Upload/replace any asset into `resources/`**: sprite sheets
  (characters, enemies, objects, tiles, pickups, effects), UI bitmaps
  (border, hearts), music (music dir), sfx (audio dir), fonts.
- [done] **Full directory coverage** — upload/replace anywhere in
  `resources/`: music, audio, ui, tiles, characters, enemies, pickups,
  effects, objects, fonts.
- [done] **Replace-in-place with rename** — upload `foo.png` over an existing
  name, or rename an existing asset and update its referencing JSON
  (character sheet name, enemy `image` override, sfx/music names in rooms
  and dialogue, item use sfx, `game.json`'s font, room tileset/hero/npc/enemy
  references, dialogue `image` cues) so replacing art from GIMP never breaks
  a bake. Character/enemy/tileset sheet renames carry their paired `.json`
  (definition / per-sheet metadata) with them.
- [done] **Asset audit** — a view listing every path the generator would
  synthesize a placeholder for (character/enemy/object sheets, UI bitmaps,
  pickups, effects, empty music), with an Import button per row. This is the
  artist's worklist.
- [done] Music/sfx **reference** picking everywhere (rooms, dialogue) is by
  name from the scan; keep it.

## 12. Build, test, ship

- [done] Build tab: host build, output log, errors surfaced.
- [done] Test-from-here sessions, save-suppressed.
- [done] **Web build export from the editor** — one button: run
  `build-web.sh`, then host `build/web/` on a local port and open it in the
  default browser (browsers refuse file:// wasm; a compile-only variant and
  a server stopper ride along). The editor is the whole toolchain; "ship"
  doesn't need a terminal.
- [done] **Dreamcast image build** (`build-dreamcast.sh`) as a Build action
  (Docker is a host requirement; the command + log are surfaced, with the
  long first-run toolchain build in mind).
- [done] **Runbook panel** — the per-feature smoke checklist from NOTES.md
  (generator zero warnings, both builds clean, headless boot/`--soak` clean,
  editor round-trips) as a visible checklist with one click per step.

---

## 13. Cutscenes (LttP-style)

Both flavors author as dialogue scripts, like every other script: **image
scenes** (a full-screen picture behind typewriter text — the intro/legend
beats) and **in-world scenes** (scripted pans, blocking, and beats over the
held overworld — the uncle/Zelda/maiden scenes). The in-world framework
exists (including actor facing/visibility and scripted fades); the image
flavor does not.

In-world vocabulary — engine (`CutsceneCamera`/`CutsceneMove`/letterbox/
scripted fade in `src/main.cpp`, drains in `DialogueBox`), generator
(`wait`/`camera`/`letterbox`/`move`/`face`/`show`/`hide`/`fade` baking), and
the Dialogue tab forms — all in place:

- [done] **Beat holds** — `"wait": <seconds>` auto-advances a line once the
  reveal completes (silent beats draw no box); the world freezes while any
  script is open, and a room change releases straggler pans/glides/bars.
- [done] **Camera** — `"camera": {"pan": [tileX, tileY], "time": <s>}`
  smoothstep glide + hold (clamped like the follow), `"camera": "hero"`
  returns to following.
- [done] **Letterbox** — `"letterbox": true|false`, eased bars over the
  world, under the dialogue; the boss intro reuses the same bars.
- [done] **Actor blocking** — `"move": {"actor": "hero"|<npc index>,
  "tile": [...], "time": <s>}` glides one actor to a tile, facing the stride.
- [done] **Music/sfx stingers** — `"music"` (incl. `""` stop-bgm) and
  `"sfx"` cue on the shown line, drained once per frame.
- [done] **`"credits"` roll + `"next"` chain** — black-screen centered
  auto-advancing roll; scripts chain into the next without dropping the box;
  a `"warp"` line still wins over the chain.
- [done] **Triggers** — instant (step-on) + interactive encounters,
  one-shot memory, `repeat`, `if`/`unless` flag guards (a failed guard never
  consumes the shot); boss rooms auto-register `boss_defeated_<room>`.

Image scenes (the missing flavor — none of this existed yet):

- [done] **`"image"` line cue** — `"image": "<file>"`: a `DialogueLine.image`
  field + a `takeImageRequest()` drain in `DialogueBox` (the pattern every
  cue uses); the game draws the picture full-bleed (cover-fit) over the
  virtual viewport with the world hidden (like credits' black), letterbox
  optional on top, typewriter text below/over it. Composes with `wait`
  beats, `music` stingers, `next` chains — and with `"credits"` for a
  storybook roll. `"image": ""` clears it; the script closing or a room
  change drops it. One texture slot at a time (the console budget).
- [done] **Scene asset bake** — generator reads a new `resources/scenes/`
  directory into a `SCENES[]` table (path + dimensions), syncs files to
  `assets/scenes/` (the mirror already carries it), and synthesizes a
  placeholder test card (color bars, crosshair, registration marks) for
  art referenced by `"image"` cues but missing — the same way it does
  `resources/ui` bitmaps (existing files never overwritten). The binary
  keeps loading by baked path; no parsing.
- [done] **Editor: scene image upload/replace** — `resources/scenes/` is in
  the Files upload-target list and shows up as asset-audit rows (the audit
  lists whatever the generator would placeholder — image-cue references
  with no file behind them).
- [done] **Editor: image cue in the Dialogue line cards** — scene picker
  from the scan + thumbnail preview + a clear option, next to the existing
  cutscene cue fields, plus a "+ Image scene" insert button. Demo ships a
  wired pair (`10.json` → `11.json` behind the legend sign in room 01).

In-world gaps (the blocking niceties LttP cutscenes lean on):

- [done] **`"face"` cue** — `"face": {"actor": "hero"|<npc index>,
  "dir": "..."}`: turn an actor to speak without a glide. `move` already
  faces the stride; turning in place is its own beat.
- [done] **`"show"`/`"hide"` cues** — NPC spawn tables evaluate only at room
  entry (`if`/`unless` flags), so mid-scene entrances/exits (the uncle
  collapsing, a character running off) need per-line actor-visibility cues
  the game resolves against the room's npc list.
- [done] **`"fade"` cue** — `"fade": {"to": "black"|"world", "time": <s>}`:
  screen-wide fades exist today only for room transitions and death; a
  scripted fade gives image scenes their cuts and in-world scenes their
  punctuation. (Full-screen fades are already the sanctioned non-sprite
  chrome.)
- [done] **Boot intro hook** — `game.json` `"intro": "<dialogue>"`: a new
  game (not continue) plays the named script before/at the first room drop,
  gated by an auto-registered `intro_seen`-style flag so it never replays.
  Today the title starts gameplay directly; the workaround (a flag-gated
  instant encounter at the spawn tile) works but lives implicitly in room 0
  where editing can break it.
- [done] **Editor forms for the new cues** — face/show/hide/fade fields in
  the cutscene cue card once the vocabulary lands (the actor picker is
  already an index into the room's npc list).
- [done] **(optional) Concurrent blocking** — one glide slot per actor (the
  hero + every npc): simultaneous `move` cues glide their actors together,
  and a re-cue replaces only that actor's glide, never another's. LttP
  scenes stay serialized by their beats; the ensemble glides are the QoL.

Smoke path for the whole feature: boot intro from a fresh slot (no re-run on
continue or after the flag banks), an image scene (image + text + stinger +
`next`), an in-world scene (bars → pan → blocking → face → hide → fade), and
the boss endgame sample already shipping (`06.json` → `07.json` credits) —
plus the section-12 runbook checks (generator zero warnings, both builds
clean, editor round-trips).

---

### Out of scope (by design)

- Sprite/pixel editing — GIMP.
- Music/tracker editing — MilkyTracker.
- Anything that breaks "bake, don't parse" or puts game concepts in engine
  code.