# ISSUES — engine + game polish backlog

Found by a full source pass (src/, include/, scripts/, resources/code/,
resources/ content). Each item cites where it lives and what a fix looks
like. Priorities: **P0** = players will notice, **P1** = polish that
reads as "unfinished", **P2** = hardening / ergonomics / tuning.

**Status after the second fix pass** — first pass resolved A1-A3, B1-B3,
B5, B6, C1, C2, C4, C5, D1, D2, F1, F2. Second pass resolved B4, B7, C3,
D3, D5, D6, E1, E2, E3, E4, F3, F4, F5. Still open: C6 and D4 (both
"keep it working" notes, no action), E5 (crypt footsteps want a stone
step cue the audio bank doesn't carry), E6 (the editor's asset audit is
the ongoing artist worklist by design). Resolved items keep their notes
below, marked **[FIXED]** with what the fix did.

---

## A. Dialogue & text (P0)

### A1. [FIXED] Long lines silently clip — text types out but never draws
`src/dialogue.cpp:8-11` — the box wraps up to `MAX_ROWS = 8` but only
`BOX_ROWS = 3` draw (`dialogue.cpp:669-686`). Anything wrapping past three
rows typewrites invisibly: the player hears the letter ticks, the blinking
arrow appears, and words are simply missing. The generator never warns
about overlong lines, so this shipped in 26 places. Verified offenders in
the current content (all >70 chars, ~24 chars fit per row):
`intro` lines 1 & 7, `elder` lines 1/3/5/8/9, `aaron` lines 1/4/5/7/9,
`beat_hollow`, `beat_meadow` (94 chars), `beat_ridge`, `beat_shore`,
`beat_village`, `keeper` lines 2/4/5, `sign_east` line 1 (90 chars),
`sign_controls` lines 0/2, `sign_crypt` line 0, `peek_big_door`,
`sign_village` line 1.
**Fix applied:** pagination — the box shows three rows of the wrap at a
time; the typewriter works within the page, a press (or, on timed
beats, the beat) turns the page, and the arrow prompts the turn. Long
lines now read completely; the full-line generator warning became moot
(a choice-prompt warning was still added, see D2).
1. Engine: paginate instead of clip — when a line wraps past 3 rows, draw
   rows 1-3, and on action-advance scroll to the remaining rows instead of
   advancing the line (LttP pages long text). Cheap alternative: cap the
   typewriter at the 3rd row and treat that as the line's end (content-safe
   but still needs the generator warning).
2. Generator: `generate_assets.py` should measure wrapped rows at bake
   (same wrap width math as the engine, `boxWidth - 2*(patch+TEXT_PAD_X)`,
   16px font) and `warn()` on any line over 3 rows.

### A2. [FIXED] Cutscene "wait" beats cannot be skipped — the code says they can
`src/dialogue.cpp:374-381` — the wait/credits branch `return`s before the
action-press check at line 439 ever runs, so the comment "the button can
still hurry a beat along" is false. The 3.4s intro beats and every credits
line (`CREDITS_BEAT`, 11 lines x ~1.6s+) are unskippable.
**Fix applied:** wait/credits beats now advance early on an action press
(first press finishes the page reveal, second skips the hold) — including
silent beats and credits lines.

### A3. [FIXED] Choices can overlap the prompt text
`src/dialogue.cpp:558-594` — options stack in the box's bottom-right
regardless of how many text rows the prompt used. A 2-row prompt with
3 choices overlaps the last text row. Current content stays safe (short
prompts, 2 choices) but it's one long shop line away from breaking.
**Fix applied:** prompts cap at two drawn rows while options are up, and
the option stack starts below them (right-aligned as before). A bake
warning now flags >48-char choice prompts.

---

## B. Combat & item feel (P0/P1)

### B1. [FIXED] Boomerang/hookshot stun + sound spam every frame
`src/main.cpp:5970-5978` and `src/main.cpp:6014-6022` — while the
boomerang (or hook head) overlaps an enemy it calls
`enemies[e].stun(1.2f)` + `PlaySound(sndBoomerangHit)` *every frame*.
The comment claims "once per stun period" but there is no dedupe: the
sound machine-guns (~60/sec) and the stun timer keeps resetting so the
enemy stays frozen as long as the tool sits on it.
**Fix applied:** the boomerang and hookshot passes skip enemies already
stunned or frozen — one stun, one cue.

### B2. [FIXED] Frozen and agitated bosses draw at the wrong size
`src/enemy.cpp:546-557` (frozen) and `src/enemy.cpp:580-590` (agitated
flicker) draw a plain 32x32 sprite at `(x, y)` — bosses are 1.5x (see the
normal path at `enemy.cpp:498-520`, and the hurt-flash branch at 566 which
gets it right). Freeze the Hollow King or the Ash Knight with the ice rod
and it visually shrinks by a third.
**Fix applied:** both branches draw the 1.5x boss rect (the frozen ice
overlay scales with it).

### B3. [FIXED (engine seam; the ripple-frame art stays as E3)] Submerged zoras hurt on contact and never actually submerge
`resources/code/zora.cpp` models idle as "under water", but the engine
(`src/main.cpp` contact-damage pass in `updateEnemies`) applies contact
damage in every mode, and the sprite always draws its stand frame. So a
"lurking" zora both stabs the hero on contact while invisible-by-intent
and shows its full body. LttP zoras are harmless while submerged.
**Fix applied:** new `Enemy::setHarmless` seam; the zora behavior holds
contact damage off while submerged (spikes it back on when surfaced).
The ripple-frame art remains open as E3.

### B4. [FIXED] Costs pay and sfx play before the use-action is known to succeed
`src/main.cpp:2541-2701` — costs are consumed and `item.useSfx` plays at
line 2701 even when the action was a no-op: pressing the item button while
the boomerang is airborne (`throwBoomerang` returns silently at
`src/main.cpp:1050`), a second hookshot press while the chain flies
(`2612-2627`), or the flute while the picker is already open. Harmless
with cost-free tools today, but the pattern will charge real magic for
future items that can no-op.
**Fix applied:** an `itemUseReady` pre-pass answers "can this press do
anything?" (boomerang airborne, chain out, picker open, toggle with no
magic, empty bottle, mirrorless room) — no-op presses rattle the error
cue and never reach the costs or the use sfx; every real use pays once.

### B5. [FIXED] Knockback pushes the hero across water and pit tiles
The `HERO_HURT` case in `Hero::update` (`src/hero.cpp`, knockback pass)
never re-runs the tile checks that `HERO_WALK/IDLE` do at
`src/hero.cpp:1100-1128`, so a hit near deep water or a pit slides the
hero onto it for the hurt duration (~0.28s) — she stands on the sea, then
splashes/falls when the flinch ends.
**Fix applied:** the hurt state re-checks the underfoot tile each frame
— deep water splashes (or dives with flippers) and pits swallow mid-flinch.

### B6. [FIXED] Enemies hover over pit tiles
`Enemy::slide` (`src/enemy.cpp:103-136`) treats pits (and water) as
walkable, so knockback or wandering drops octoroks onto the Mire's pit
lake where they float. In `13_crypt_pit` this is visible from the bank.
**Fix applied:** `Enemy::slide` refuses pit and deep-water tiles —
walkers and knockback turn at the lip (stationary water natives like the
zora are unaffected).

### B7. [FIXED] Octorok "period" semantics
`src/enemy.cpp:482` — the shoot cadence is `rng.range(shootPeriod) == 0`
per idle leg; with the shipped `shoot_period: 2` that's a coin flip each
leg (~every other leg), and the walk leg *also* lasts `shootPeriod`
seconds, so the number means two different things.
**Fix applied:** the idle beat queues its rock unconditionally — the
number is the cadence (idle pause + `shootPeriod`-second walk leg), not
a dice roll. No content change needed; `shoot_period: 2` now means a
spit roughly every 2.2s.

---

## C. Boot / title / rooms (P1)

### C1. [FIXED] The scroll-in glide plays idle frames, not a walk cycle
`Hero::place` (`src/hero.cpp:181`) resets the state to `HERO_IDLE`, and
`placeHeroAtEntry` (`src/main.cpp:3618`) places the hero before the glide
starts. `advanceWalkCycle` then ticks `animTime`, but `animFrames()`
(`src/hero.cpp:1140+`) picks the *stand* channel for `HERO_IDLE` — so the
walk-in glide (fixed for camera snap + clock earlier) still glides with a
slow breathing frame instead of striding.
**Fix applied:** `advanceWalkCycle` switches the gliding hero from idle
into the walk state; the next update settles back to idle on landing.

### C2. [FIXED] "Open" doors render as nothing
`src/main.cpp` `drawDoorways` (~6820-6880) — `if (rule.startsOpen)
continue;` means the `open` kind draws no archway at all. The village's
east gate and the grove's west gate declare `"open"` doors that are
invisible; the player gets a slab for locked/shutter/bombable but bare
wall for the one door type meant to look welcoming.
**Fix applied:** removed the `startsOpen` skip — open doors draw the same
archway as an opened one.

### C3. [FIXED] Doorways are code-drawn rectangles
Same function — slabs, keyhole slabs, shutter slats, archways are all
`DrawRectangle`/`DrawRectangleLinesEx` shapes in an engine whose stated
rule is "all chrome is sprites". They read as debug gizmos next to the
pixel-art tileset.
**Fix applied:** `ui/doors.png` (six kinds x two orientations, 40x48
cells, columns matching the generator's kind order with the open
archway at column 0) draws via `drawDoorways`; the generator synthesizes
the sheet for scaffold games, and the code-drawn shapes remain as the
missing-art fallback.

### C4. [FIXED] Keeper reveal requires leaving the room
NPC spawn tables evaluate at room entry only. The shrine sets
`keeper_awake` mid-visit (`main.cpp` shrine/powder pass), but the keeper
NPC (`03_grove`, `if: keeper_awake`) won't appear until the player exits
and re-enters — the powder's payoff is invisible exactly when it happens.
**Fix applied:** shrines take an optional `"dialogue"` (baked like a
sign's, played at the awakening moment). The grove shrine now plays a
short keeper-wakes beat — the stone stirs and says he'll wait by the
path — and the keeper spawns there (next to the path) on re-entry.

### C5. [FIXED] The title logo skip cuts before its fade-out
`src/main.cpp` `updateLogo` — skipping with the action jumps straight to
`enterTitleScreen(0)`, so the logo's fade-out never renders; only the
title card's fade-in covers the cut. Feels abrupt on repeated boots.
**Fix applied:** a press clamps the logo clock to the fade-out start, so
the cut to the title card is always a fade.

### C6. `--room` test sessions and the boot cards
`main.cpp` boot (`if (GAME_LOGO_SCENE >= 0 ...)`) runs before the
`--room` parsing, harmless — but worth keeping the test session immune to
future boot-flow work (e.g. a skip-all flag for CI runs).

---

## D. Generator / validation / editor gaps (P2)

### D1. [FIXED] Dialogue references don't accept stems
`scripts/generate_assets.py:4144` — `dialogue_index` keys by file name
only (`"sign_x.json"`); rooms/scenes accept file-or-stem
(`room_index`, `scene_lookup` do both). Rooms referencing `"sign_x"`
silently warn-then-bake as unknown. Inconsistent reference rules.
**Fix applied:** dialogue references accept the file name or the stem,
matching rooms and scenes.

### D2. [FIXED (superseded)] No bake-time check for dialogue lines that clip
See A1 — the generator should warn on >3-row wraps so content can't ship
clipped again.

### D3. [FIXED] Editor asset rename doesn't know about the boot cards
`editor/src-tauri` rename-in-place rewrites JSON references across
rooms/dialogue — verify (and extend it to) `game.json`'s
`logo.image` / `title_card.image` / `logo.sfx` / `title_card.music`, or
renaming `company_logo.png` orphans the splash.
**Fix applied:** the exact file-name form was already covered by the
generic reference pass; a new `patch_game_card_stems` also rewrites
stem-form `logo.image` / `title_card.image` on scene renames (sfx and
music fields are plain file names — exact pass covers them).

### D4. `game.json` logo with text but no image
A text-only logo (`"logo": {"text": ...}`) does play (boot checks
`GAME_LOGO_TEXT`), but `enterLogoScreen` calls
`applySceneImageRequest(-1)` which *clears* the scene — correct, just
confirm the black-card + studio-line path stays intended (it currently
renders fine; this is a "keep it working" note for future card work).

### D5. [FIXED] `tileMetaAt` ORs both layers' flags
`src/room.cpp:75-109` — decor + collider metadata merge. A solid tile
drawn over a hazard decor tile (e.g. a rock collider over a spike decor)
would both block *and* damage. No shipped room does this; either assert
against it at bake or document the layering semantics.
**Fix applied:** the bake now warns when a colliders-layer tile's metadata
marks it a pit, cuttable, or deep water — solids can't cut, fall, or
swim.

### D6. [FIXED] `cutTileAt` only rewrites the decor layer
`src/room.cpp:111-121` — a cuttable tile living in the colliders layer
would cut visually (decor) but stay solid. Not reachable with the shipped
tileset (grass tuft is decor-only); guard at bake instead (warn when a
cuttable tile is also a collider).
**Fix applied:** the same layer-conflict guard warns when cuttable decor
sits under a collider — the cut would leave the wall standing.

---

## E. Art / presentation (P1 — the "all chrome is sprites" debt)

- **E1. [FIXED] Code-drawn hero tools** — the hammer mallet
  (`src/hero.cpp:1406-1456`), shovel (`1458-1501`), bug net
  (`1503-1530`), and raised shield plate (`1585-1603`) are rectangles
  and lines over the sprite. They work but read as placeholders next to
  the item icons. Add small sprite sheets (like `link_sword.png`) for
  hammer/shovel/net/shield and draw from those.
  **Fix applied:** `effects/tools.png` (hammer/shovel/net/shield), drawn
  by rotation around the handle-base pivot so one frame per tool serves
  every facing; the code-drawn shapes remain as the missing-art fallback.

- **E2. [FIXED] Moldorm trail is brown circles** — `src/enemy.cpp:526-536`; the
  worm body is placeholder `DrawCircle` segments. Ship tail-segment
  sprites (the trail API exposes per-segment positions).
  **Fix applied:** the trail draws the worm's own sprite frames, shrinking
  down the chain — no new assets, and the body and tail read as one
  creature.

- **E3. [FIXED] Zora has no submerged frame** — see B3; its idle channel should
  be a ripple, its move channel the surfaced body (the behavior's
  comment says the channels were designed for exactly this — the sheet
  just doesn't carry it).
  **Fix applied:** the zora sheet's stand row is regenerated as water
  ripple rings (the move row keeps the surfaced body), and the json's
  move channels now point at the body frames only — surfacing never
  flickers back to the ripple.
  **Fix applied:** the zora sheet's stand row is regenerated as water
  ripple rings (the move row keeps the surfaced body), and the json's move
  channels point at the body frames only — surfacing never flickers back
  to the ripple.

- **E4. [FIXED] The house door is a warp pad** — village Rowan's door is a
  `wall_house` gap with the generic swirl pad sprite. A small door-frame
  sprite (or a dedicated `door` object type drawing over the gap) would
  sell "this is a house" much better.
  **Fix applied:** new `door_warp` object type (composes the same floor +
  warp interactions) with a generated front-door sprite; the village and
  elder-house entrances use it, and the cave mouths keep the swirl.

- **E5. Crypt footsteps say grass** — every room, stone crypts included,
  cues `walk_grass.wav` (room `walk_sfx` in `scripts/game_rooms.py`).
  No stone-step variant exists in `resources/audio/`; either source one
  or accept the swap as a known compromise and note it.
- **E6. Placeholder object/scene art audit** — `resources/objects/*.png`
  and the storybook cards are the generator's shaded-shape placeholders
  plus the new generated scenes; fine at distance but the art audit in
  the editor (`Music & Files` tab) should be run as the artist worklist
  it was built to be.

---

## F. Content bugs in the shipped game (P0/P1)

- **F1. [FIXED] Mara's stall blocks the north path column** —
  `scripts/game_rooms.py` `village()`: the stall paints `F.F` at
  y2 x11-13, putting a fence post at (13,2) in the middle of the x13
  gate path. Walking straight south from the gate at x13 bumps the
  post. Move the stall west one tile or narrow the gate path usage.
- **F2. [FIXED by pagination] 26 dialogue lines at clipping risk** — the full list is in A1;
  even with the engine fix, splitting the longest (90+ chars) reads
  better as two beats.
- **F3. [FIXED] Buried treasure requires the shovel but the soil reads as
  decoration** — the `dd` soil patches in meadow/grove/shore/ridge are
  two bare dirt tiles; without a sign or visual cue players will walk
  past them. Consider a sparkly "dig here" hint (a dithered soil border
  variant) once the shovel is owned, or a sign in the meadow teaching it.
  **Fix applied:** a meadow signpost beside the first soil patch teaches
  the dig ("SOFT SOIL. The field keeps its promises buried.").

- **F4. [FIXED] The crypt's `peek_big_door` encounter sits at (10,2)** — one
  tile left of the north gate column (x10-11). Triggering it requires
  facing-into the gap; confirm it doesn't overlap the big-door open
  flow awkwardly (it repeats, so it re-rattles after the door opens —
  consider `unless: boss_defeated_10_crypt_hall`-style gating or moving
  it off the approach).
  **Fix applied:** the tease is one-shot now — an opened door never
  re-rattles.
- **F5. [FIXED] Hollow-grove arrival beat may miss** — the mirror lands at the
  hero's paired tile, spiraled to free ground; `beat_hollow` only sits at
  (13,6). Most arrivals (shrine-adjacent mirroring) hit it, but a
  player mirroring from the pond edge lands elsewhere and misses the
  "the air is wrong here" beat. Add 2-3 one-shot instant triggers
  around the likely landing zone, or fire the beat from the keeper's
  dialogue on first Hollow entry instead.
  **Fix applied:** three likely landing tiles share the beat, each gated
  by a new `hollow_seen` flag that the beat itself sets — it plays once,
  wherever the mirror lands the hero.

---

## G. Verified-fine (checked, not bugs)

- Camera clamp through room transitions (fixed earlier — stable band,
  seamless handoff).
- Chest key costs / item-get banking / opened-lid persistence.
- Door open reciprocity and save persistence (`markDoorOpened`).
- Dark-room lightmap resize handling (`drawDarkness` re-creates the RT).
- Boss defeat flag banking + autosave on the kill.
- Wizard reflect fight: volley is a plain projectile, sword-batting sets
  `friendly`, and friendly non-magic rocks strike enemies — the fight
  works (and arrows/silver arrows work).
- Somaria conjure blocks on the hero or inside walls (`solidBox` probe).
- The soak round-trip and bake determinism (as far as this environment
  can run them; the GL window needs a GUI session).
