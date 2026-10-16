[ ] - Not done
[X] - Done
[?] - Needs question answered
[/] - Partially done

Issues:

[X]
Right now, moving from one map to another when I go north, south, etc.. is just static. I want there to be some kind of visual transition, perhaps like Link to the Past where it looks sort of like it scrolls.
Fixed (v2, fade style — replaced the v2 scroll-snapshot pan): the hero keeps striding out of the map while the screen dips to black through the departing room's palette; the room swap + autosave happen under full cover; then the destination fades back in (its own tint) over the hero's walk-on glide from the entry edge. Phase machine in updateRoomTransition, fade drawn in drawTransitionFadeOut, arm in maybeStartRoomTransition (src/main.cpp). Total ~0.7s.

[X]
For some reason, a fullscreen graphic appears in the background whenever the dialogue comes after getting an item. This feels like a glitch. It's the same background as the "Developer" opening logo.
Fixed: the item-get line left imageScene unset (0 = company logo scene); it is now reset to -1 before the dialogue opens (src/main.cpp updateItemGet).

[X]
Every time I get an item, after its dialogue plays, it continues with the sword dialogue every single time. This feels wrong.
Fixed: itemGetDialogue left nextDialogue unset (0 = aaron.json chain); it is now -1 (src/main.cpp).

New issues found in a full-source audit (src/, include/, resources/code/, build scripts). Grouped by severity, ranked within groups.

HIGH:

[X]
Starting a new game inherits the previous session's world state. resetGameState() (src/main.cpp:1655) never clears dungeonDead[] (dead-enemy masks for dungeon rooms) or the settled-object registry (pushed blocks, latched switches, lit torches) — those are only reset in applySave(), the continue/load path. So: play a session, save+quit to title, start a new game, and the crypt rooms spawn pre-solved/pre-cleared. Worse, startNewGame() immediately saveToFile()s, baking the stale settled registry into the brand-new save permanently.
Fixed: resetGameState() now clears dungeonDead[] and settledCount (src/main.cpp).

[X]
Lit torches don't stay lit. In loadRoom()'s object spawn loop, the settled-registry restore sets a torch's opened=true (src/main.cpp:3943-3947), but the unconditional chest-registry assignment right after (src/main.cpp:3956-3962) forces opened back to false for anything that isn't a chest. Light a torch, walk out, walk back in: the dark room is dark again, linked barriers re-raise, and the next snapshot permanently downgrades the registry.
Fixed: the chest-registry assignment only applies to actual chests; non-chests keep the settled restore's opened (which is now explicitly initialized to false first) (src/main.cpp loadRoom).

[X]
Dark rooms break the new edge-scroll snapshot. captureRoomScroll() wraps drawWorldView() in BeginTextureMode(roomScroll.texture) (src/main.cpp:2983), but drawDarkness() issues its own BeginTextureMode(lightRT) (src/main.cpp:3092) — raylib render targets don't nest, so the inner EndTextureMode rebinds the default framebuffer. The darkness/tint/weather layers end up drawn to the screen mid-update, and the sliding snapshot has no darkness: a pitch-black crypt flashes fully lit for the 0.42s transition. Reachable in-game: the two dark crypt rooms both have edge exits.
Fixed: drawDarkness() split into renderLightmap() (default framebuffer) + compositeDarkness() (safe inside a texture mode); captureRoomScroll() renders the lightmap first, then bakes layers + composite + tint + weather into the snapshot.

[X]
Chest and boss rewards can be permanently lost. Opening a chest banks chestOpened=true and saveToFile()s immediately (src/main.cpp:4791-4813), but the grant is only applied when the item-get rise finishes 0.65s later (src/main.cpp:4515-4519) — and pausing isn't blocked during the rise. Open a chest, then save+quit during the rise: the save has the chest open with the item never granted. Same pattern for boss heart containers (src/main.cpp:5783-5809).
Fixed: the grant is applied at arm time (chest open / boss kill, before the save banks the state); the rise + dialogue still present it.

[X]
The hollow king's own intended fight mechanic is dead. All enemy projectiles spawn with hardcoded id = 0 (src/main.cpp:1004), and shield/sword deflection flips p.friendly without assigning a fresh negative swing id (unlike beams/arrows/bombs/pots). tryHit() rejects swingId >= 0 against strikeImmune enemies (src/enemy.cpp:242), so batted volleys pass through the hollow king harmlessly — the fight is designed around batting his magic back (see wizard.cpp's comment and the in-game hint). Also, all deflected rocks share id 0, so the lastSwing dedupe eats repeat hits.
Fixed: both deflection paths assign a fresh negative id (nextBeamSwingId--), so batted volleys damage strike-immune bosses and repeat hits land.

[X]
One ice bolt + any sword swing kills anything, including bosses. tryHit()'s frozen branch (src/enemy.cpp:244-252) sets hp = 0 and returns before the attack-box validation/overlap checks at lines 254-259 — only the swing-id dedupe runs. Freeze any enemy (or boss) with the ice rod or Ether, then swing anywhere in the room (even with the hammer's zero-width windup box): every frozen enemy shatters instantly, and boss weak-point design (moldorm, hollow king HP bars) is bypassed entirely.
Fixed: the frozen branch now runs after the box-validation/overlap checks — a hit must actually land to shatter (src/enemy.cpp tryHit).

[X]
Dialogue can soft-lock the game. In DialogueBox::update(), the wait/credits branch always returns before the choice branch (src/dialogue.cpp:422-441 vs. 443+): a line authored with both "choice" and "wait" (or any choice line in a credits script) can never be answered — the cursor can't move, selection is blocked by the !choiceActive guards, and the world stays frozen while dialogue.isActive() is true. The box hangs forever.
Fixed: the wait/credits branch is skipped for choice lines (their wait is ignored) — choices always take the interactive path (src/dialogue.cpp update).

MEDIUM:

[X]
Arrows and beams inherit stale magic fields from the shared 8-slot projectile pool. spawnArrow (src/main.cpp:2271-2302) never resets element; spawnBeam (src/main.cpp:1016-1045) omits element; spawnElementProjectile (src/main.cpp:2305-2334) omits enemyDamage. Fire a fire-rod bolt, then fire an arrow once the slot frees: it's drawn as a fireball, ignites torches it passes, and never looks like an arrow.
Fixed: spawnArrow/spawnBeam reset element (and arrow) to none; spawnElementProjectile resets enemyDamage (src/main.cpp).

[X]
Save+quit from the pause menu silently loses room-local puzzle progress. quitToTitle() (src/main.cpp:1710-1720) saveToFile()s without snapshotRoomState(currentRoom) or a dungeonDead update — those only happen inside loadRoom(). Push a block onto its latch or light a torch, then save+quit: on continue it's back where it was at the last room exit, despite the "saved" promise.
Fixed: new snapshotDeparture() (dungeon dead-masks + settled registry) shared by loadRoom and quitToTitle (src/main.cpp).

[X]
The roomScroll render texture leaks if you quit during an edge transition. Pausing isn't blocked mid-scroll; save+quit calls quitToTitle() with roomScroll.valid true, and the release only exists in updateRoomTransition's natural completion (src/main.cpp:6948-6949) and the next capture. loadRoom() clears roomTransition.active directly (src/main.cpp:3824, 3858) without releaseRoomScroll(), so a full-screen FBO lingers across the title screen into the next session.
Fixed: quitToTitle() calls releaseRoomScroll() (src/main.cpp).

[X]
maybeStartRoomTransition() is missing the itemGet.active freeze guard (src/main.cpp:6818 vs. updateWarps' check at 4296). If a boss seal drops or a chest opens on the same frame the hero crosses an edge, the room transition starts mid item-get; the rise then resumes with stale old-room coordinates.
Fixed: the guard now includes itemGet.active (src/main.cpp).

[X]
The SFX lazy cache (64 slots, src/sfx.cpp:11) is smaller than this game's own sound set (~73 distinct names); overflow silently returns namedSfx[0].sound — every cue past the 64th plays the wrong sound for the rest of the session, with no log.
Fixed: cache raised to 128 slots (covers the baked set plus headroom) and overflow now logs a warning (src/sfx.cpp).

[X]
The bug net bottles bees/fairies into phantom slots when the hero owns no bottles. firstEmptyBottle() (include/engine.h:733-738) scans bottleContents[4] without checking bottles > 0, so it returns slot 0 for a bottleless hero; the catch paths (src/main.cpp:5329, 5338-5367) write it anyway and the bee despawns from the world. When the first real bottle is later granted, it shows the phantom contents instead of what was granted.
Fixed: firstFilledBottle/firstEmptyBottle only scan owned slots (i < bottles), so a bottleless hero fails the catch and the bee stays in the world (include/engine.h).

[X]
The hero can be wedged inside colliders with no recovery. slideWithCollision (src/hero.cpp:278-289) rejects any move whose destination overlaps a solid, so once embedded beyond a walk step (~4.8px) every escape attempt fails and the game is permanently stuck. Three paths can embed him: the settle-shove nudges the hero up to 40px with no solid check when a block lands on him (src/main.cpp:5592-5599), cutscene glides never validate the target/path (src/main.cpp:806-810), and hop landings only validate the landing box at press time (src/hero.cpp:482-483).
Fixed: the settle-shove is collision-checked (straight back, then sidesteps); cutscene glides validate the hero's destination via the new Hero::canOccupyTile; hop landings re-check collision at land time and step out along the hop axis (src/main.cpp, src/hero.cpp).

[X]
Dialogue text wrapping silently misbehaves on long content. Lines that wrap past 8 rows truncate (src/dialogue.cpp:292) but totalChars still counts the tail (src/dialogue.cpp:281) — the typewriter types invisible characters and the box dead-airs before advancing. A single word ≥ 255 chars (src/dialogue.cpp:312) makes the line render as an empty box that still takes full reveal time.
Fixed: a word wider than the row buffer hard-breaks instead of stalling, and the typewriter's totalChars is capped at the truncation point — no more dead-air typing (src/dialogue.cpp wrapLine).

[X]
The Pegasus dash ignores tile hazards. Deep water/pits are only sampled in the IDLE/WALK case (src/hero.cpp:1140-1155); HERO_DASH (src/hero.cpp:685-703) only slides with collision, so a ~3.6-tile dash crosses pits and deep water with no consequence — unless the dash happens to end over one, where the result is arbitrary.
Fixed: HERO_DASH samples deep water/pits each frame and hands over exactly like a walk (splash, or dive with flippers) (src/hero.cpp).

LOW:

[X]
Spike tiles qualify as "safe" respawn tiles (the safe-tile record and the damage-tile check share a branch, src/hero.cpp:1156-1163), and place() zeroes invulnTime (src/hero.cpp:197) — so falling into a pit near spikes respawns the hero onto the spikes and he's instantly re-hurt.
Fixed: damage tiles are never recorded as safe respawn tiles (src/hero.cpp).

[X]
startLine() reads gWorldFlags[line.ifFlag/unlessFlag] with only a >= 0 check, no upper bound (src/dialogue.cpp:173-174) — every other flag access in the codebase is bounds-checked. An out-of-range index from a game file is an out-of-bounds read deciding script flow.
Fixed: both condition reads are bounds-checked against WORLD_FLAG_COUNT (fail open) (src/dialogue.cpp).

[X]
The spin-charge "hold through the swing" is dead code: swordHeldTime only accumulates in the IDLE/WALK case (src/hero.cpp:995-1003), so the check in HERO_ATTACK (src/hero.cpp:673-677) never fires — charged-spin timing is ~0.3s longer than the comment describes.
Fixed: swordHeldTime also accumulates during HERO_ATTACK while the button is held, so a held swing charges the spin as documented (src/hero.cpp).

[X]
gotoLine jumps (line goto at src/dialogue.cpp:348, choice goto at 491) are never bounds-checked against lineCount — an off-by-one goto silently skips the rest of the script (or chains into nextDialogue) with no warning.
Fixed: both goto sites bounds-check against lineCount; an out-of-range goto falls through as a normal advance (src/dialogue.cpp).

[X]
Choice prompts are clamped to 2 visible rows (src/dialogue.cpp:736) but the reveal/pick still requires the full multi-row text (src/dialogue.cpp:370-373) — a 3+ row prompt shows an incomplete question under the option list.
Fixed: the reveal/pick now gates on exactly the two visible rows (pageEnd caps at row 2 for choice lines) — the box's two-row-plus-options layout is the design constraint, but nothing invisible is typed or waited on anymore (src/dialogue.cpp update).

[X]
Toggle items (Byrna/Cape) pay their magic cost before the toggle-off branch runs (src/main.cpp:2590-2612 vs. 2688-2715): turning the aura off costs magic. Latent today (those items carry no magicCost in the data) but any priced toggle item triggers it.
Fixed: a toggle-off use skips all use-costs (src/main.cpp useEquippedItem).

[X]
Shutdown leaks textures: objectSheets are loaded for all 20 object types but the unload loop caps at 16 (src/main.cpp:7833-7845), and uiDoorsTexture (loaded via the uiSheets table) is missing from the unload list (src/main.cpp:7816-7828). Process-exit leaks, but they matter more on the web/Dreamcast targets.
Fixed: the unload loop covers the whole 32-slot pool, and uiCountersTexture/uiDoorsTexture join the unload list (src/main.cpp).

[X]
The music loaders strcpy a 600-byte path into 512-byte globals (musicFile at src/main.cpp:28/2841, dangerFile at 35/2885) — every sibling function uses bounded copies; a long filename overflows the static buffer.
Fixed: musicFile/dangerFile are sized 600 like the path buffers they copy (src/main.cpp).

[X]
Toasts age inside the draw pass (src/main.cpp:3396-3421): a "SAVED" banner keeps expiring while the game is paused/frozen instead of lasting its 2s of gameplay.
Fixed: toasts age in the update path on the game's clock (frozen worlds hold the banner); the draw pass only renders (src/main.cpp).

[X]
The dialogue give drain reads ITEMS[giveItem].label without a bounds check (src/main.cpp:7192-7199); applyChestGrant validates the identical lookup. Only reachable with hand-edited baked data, but the guards should match.
Fixed: the label lookup is bounds-checked like applyChestGrant (src/main.cpp).

[X]
markDoorOpened() indexes ROOMS[roomIndex] before validating roomIndex (src/main.cpp:1316); its sibling doorKind() validates. Latent (all callers pass currentRoom) but it's an unvalidated-index pattern the rest of the door code avoids.
Fixed: markDoorOpened validates roomIndex up front like its sibling doorKind (src/main.cpp).

[X]
Boss-defeat persistence silently fails for room indices >= 32: bossDefeated slots are keyed room*16+i against MAX_BOSS_SLOTS=512 (src/main.cpp:5793-5795, 3882-3885) while the engine supports 128 rooms — the bounds guard masks the dropped flag, so bosses past room 32 respawn despite the fanfare/save. Current game (17 rooms) unaffected.
Fixed: MAX_BOSS_SLOTS is sized MAX_ROOM_MARKS * 16 so every room registers; save version bumped to 10 (older saves are rejected as usual on version change) (src/main.cpp).

[X]
enemySheets[16] is indexed by spawn paths that only validate against ENEMY_COUNT (src/main.cpp:3886, 4869-4870) — a game with 17+ enemy types reads past the pool (the generator only warns). Current game (10 types) unaffected.
Fixed: the sheet pool is 32 (like the object pool), with the load/unload loops and every spawn path (room, ambush, test sandbox) clamped to it (src/main.cpp).

[X]
Pot-ambush enemies silently vanish when the enemy pool is full: the "hides" spawn is gated by enemyCount < 16 (src/main.cpp:4866-4876), and the generator permits 16 baked enemies per room, so a full room plays the ambush cue and poof with no enemy.
Fixed: rooms with a "hides" pot reserve one enemy slot (baked spawns cap at 15), so the ambush always has somewhere to spawn; the reserved slot isn't a baked spawn, so the dungeon dead-mask identity holds (src/main.cpp loadRoom).

[X]
The wizard behavior breaks engine contracts: it teleports immediately on hit-recovery pause (bypassing the 0.4s stun, resources/code/wizard.cpp:83-90), permanently idles on maps smaller than 4x4 tiles (wizard.cpp:42, wedging any clear-the-room door), and teleports to solidity-unchecked anchors that can bury its boss box in a wall.
Fixed: the PAUSE hop waits out the engine's 0.4s hold (hit stuns stay visible), anchors are clamped to the map instead of wedging small arenas, and hops solidity-check each anchor — boxed in, the wizard holds position and keeps firing (resources/code/wizard.cpp).

[X]
build-mac.sh isn't cwd-safe and doesn't fail fast: no cd "$(dirname "$0")" and no set -e (unlike build-web.sh and build-dreamcast.sh), so running it from another directory resolves paths against the wrong cwd, and a generator failure still compiles stale baked data.
Fixed: set -e + cd to the script directory, matching the other build scripts (build-mac.sh).

[X]
An enemy sitting on top of the hero keeps dealing damage until death: contact damage only recoils the hero (knockback + i-frames), while a same-speed pursuer strides right back in during the i-frames and re-overlaps at every expiry — the hero can never gain separation, especially pinned against a wall.
Fixed: the shove is mutual, LttP-style — when contact damage lands, the attacker bounces too (new Enemy::bounce: HURT knockback away from the hero + the 0.4s PAUSE recovery), so every hit cycle buys the hero real escape distance. Hero::hurt now returns whether the damage landed, so the bounce couples to actual hits only (i-frames, auras, dashing plows-through don't bounce). Bosses keep their choreography and frozen enemies stay frozen (src/main.cpp contact branch, src/enemy.cpp, src/hero.cpp, include/hero.h, include/enemy.h).