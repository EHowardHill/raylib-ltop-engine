#include "engine.h"
#include "assets.h"
#include "room.h"
#include "hero.h"
#include "enemy.h"
#include "dialogue.h"
#include "sfx.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#if defined(PLATFORM_WEB)
    #include <emscripten/emscripten.h>
#endif

static Room room;
static Hero hero;
static DialogueBox dialogue;

static Music music = {};
static bool musicLoaded = false;
static char musicFile[512] = "";

// Danger layer: a room's alternate track that swells over the bgm while
// an arena fight (boss or mini-boss) lives — the dynamic music crossfade.
// Same no-restart convention as the bgm; the mix lerps ~1.2s each way.
static Music dangerMusic = {};
static bool dangerLoaded = false;
static char dangerFile[512] = "";
static float dangerMix = 0.0f;   // 0 = calm bgm, 1 = full danger

static Texture2D tilesetTexture = {};

static int prevTileX = -1;
static int prevTileY = -1;
static Camera2D camera = {};

// One-shot encounter state. Global memory: survives room (re)loads and is
// only reset when the game process starts.
static bool* encounterFired = 0;

// Combat & world systems.
static Rng worldRng(0xC0FFEE1Du);
static Enemy enemies[16];
static int enemyCount = 0;
static Texture2D enemySheets[16] = {};

static const int MAX_PICKUPS = 32;
struct Pickup
{
    bool active;
    int kind;
    float x;
    float y;
    float bobTime;
    float life;  // 0 = forever; else despawn timer
};
static Pickup pickups[MAX_PICKUPS] = {};

struct Poof
{
    bool active;
    float x;
    float y;
    float age;
};
static Poof poofs[32] = {};

// Sprite-effect particles: grass chunks, bush leaves, pot shards — little
// physics chips bursting out of world events (code-drawn, no art).
static const int MAX_PARTICLES = 64;
struct Particle
{
    bool active;
    float x;
    float y;
    float vx;
    float vy;
    float life;
    float maxLife;
    float size;
    Color color;
};
static Particle particles[MAX_PARTICLES] = {};

static void spawnParticles(float x, float y, int count,
                           unsigned char r, unsigned char g, unsigned char b,
                           float spread, float upSpeed)
{
    for (int i = 0; i < MAX_PARTICLES; i++)
    {
        if (count <= 0) return;
        if (particles[i].active) continue;

        particles[i].active = true;
        particles[i].x = x;
        particles[i].y = y;
        particles[i].vx = (worldRng.nextFloat() * 2.0f - 1.0f) * spread;
        particles[i].vy = -upSpeed * (0.5f + worldRng.nextFloat() * 0.8f);
        particles[i].maxLife = 0.35f + worldRng.nextFloat() * 0.25f;
        particles[i].life = particles[i].maxLife;
        particles[i].size = 2.0f + worldRng.nextFloat() * 2.5f;
        // Slight per-chunk shade variation so bursts don't look cloned.
        int shade = worldRng.range(40) - 20;
        int cr = (int)r + shade;
        int cg = (int)g + shade;
        int cb = (int)b + shade;
        if (cr < 0) cr = 0; if (cr > 255) cr = 255;
        if (cg < 0) cg = 0; if (cg > 255) cg = 255;
        if (cb < 0) cb = 0; if (cb > 255) cb = 255;
        particles[i].color = Color{ (unsigned char)cr, (unsigned char)cg,
                                    (unsigned char)cb, 255 };
        count--;
    }
}

static void updateParticles(float dt)
{
    for (int i = 0; i < MAX_PARTICLES; i++)
    {
        Particle& p = particles[i];
        if (!p.active) continue;
        p.life -= dt;
        if (p.life <= 0.0f)
        {
            p.active = false;
            continue;
        }
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        p.vy += 420.0f * dt;  // gravity
    }
}

static void drawParticles()
{
    for (int i = 0; i < MAX_PARTICLES; i++)
    {
        const Particle& p = particles[i];
        if (!p.active) continue;
        float t = p.life / p.maxLife;
        Color c = p.color;
        c.a = (unsigned char)(int)(255.0f * clampf(t, 0.0f, 1.0f));
        float size = p.size * (0.5f + 0.5f * t);
        DrawRectangle((int)(p.x - size * 0.5f), (int)(p.y - size * 0.5f),
                      (int)size + 1, (int)size + 1, c);
    }
}

// Projectile kinds: rocks from octoroks (possibly deflected) vs sword beams,
// distinguished by the `beam` flag on Projectile.

struct Projectile
{
    bool active;
    bool friendly;  // true once deflected (or born friendly): can't hurt the hero
    bool beam;      // sword beam: damages enemies, drawn with the beam sheet
    bool arrow;     // bow arrow: damages enemies, drawn with the pickups sheet
    int element;    // ELEMENT_FIRE/ICE rod magic: code-drawn, element effects
    int dir;        // beam sprite column (down/right/left/up)
    int id;         // unique action id so a beam hits a given enemy once
    float x;
    float y;
    float dx;
    float dy;
    float speed;
    int damage;      // half-hearts (rocks, to the hero)
    int enemyDamage;  // hits per strike on enemies (arrows; silver = 2)
};
static Projectile projectiles[8] = {};
static int nextBeamSwingId = -1000;  // unique negative ids for beam hits

// Buried treasure ("buried" room list): dug up once per visit (the shovel
// turns the tile over; a dug spot stays empty until the room reloads).
static const int MAX_BURIED = 16;
static bool buriedDug[MAX_BURIED] = {};

// Boomerang (the item button): flies out, homing return, stuns enemies and
// fetches pickups. One in the air at a time.
static Texture2D beamTexture = {};
static Texture2D boomerangTexture = {};
static LazySound sndBoomerangHit = { "small enemy hit.wav" };
static LazySound sndTink = { "tink.wav" };
static LazySound sndSwordBeam = { "sword beam.wav" };

struct Boomerang
{
    bool active;
    float x;
    float y;
    float startX;
    float startY;
    float dirX;
    float dirY;
    bool returning;
    float time;
    float speed;  // px/s outbound (the magical boomerang flies faster)
    float range;  // px of outbound travel before the homing return
};
static Boomerang boomer = {};

// Hookshot (the item): a chained hook on a wire. It flies out in the
// facing; hooks latch onto hookable objects and reel the hero across
// (over pits and water), everything else bounces the chain home. It
// stuns enemies and collects pickups it touches on the way.
struct Hookshot
{
    bool active;
    bool retracting;   // chain returning to the hero
    bool latched;      // reeling the hero in toward the latch point
    float x;           // head center
    float y;
    float startX;      // hero's position at the throw
    float startY;
    float dirX;
    float dirY;
};
static Hookshot hook = {};
static LazySound sndHookshot = { "hookshot.wav" };
static const float HOOK_SPEED = 420.0f;   // px/s chain flight
static const float HOOK_RANGE = 176.0f;   // px of chain before recoil

// World objects (pots, bushes): liftable, throwable, solid on the ground.
// Rooms spawn their sets on entry; smashed objects return on room reload.
static const int MAX_OBJECTS = 16;
enum ObjectState
{
    OBJ_GROUND,
    OBJ_LIFTING,
    OBJ_CARRIED,
    OBJ_THROWN,
    OBJ_SLIDING,  // push block gliding one tile
    OBJ_SUNK      // sunk into the floor: latch-switched block, lowered barrier
};
struct WorldObject
{
    bool active;
    int type;
    int state;
    float x;       // left of the 32x32 cell on the ground plane
    float y;
    float groundX; // position the lift started from
    float groundY;
    float velX;
    float velY;
    float z;       // height over the ground plane (carried/thrown)
    float vz;
    int throwId;   // unique id so one throw hits a given enemy once
    int aux1;       // baked payload: switch mode / sign dialogue index / chest kind
    int aux2;       // baked payload: switch/barrier link group; chest grant kind
    int aux3;       // baked payload: chest grant item index or amount
    float pushTimer;      // sustained-contact seconds before a push block slides
    float pushDirX;
    float pushDirY;
    float slideFromX;     // OBJ_SLIDING interpolation
    float slideFromY;
    float slideToX;
    float slideToY;
    float slideT;
    // Moving platform (OBJINT_PLATFORM): tile waypoints glided in a loop.
    const short* path;
    int pathCount;
    int pathIndex;   // waypoint the platform glides toward (x,y pairs)
    float pathT;     // 0..1 progress of the current leg
    float modeTimer;  // timed switches: seconds left once the weight leaves
    bool switchActive;    // switches: currently pressed/latched
    bool opened;           // chests: lid open (permanent until reload/save)
};
static WorldObject objects[MAX_OBJECTS] = {};
static Texture2D objectSheets[32] = {};
static int carriedIndex = -1;   // objects[] index riding overhead
static int liftingIndex = -1;   // objects[] index mid-hoist
static int grabbedIndex = -1;   // objects[] index held in a grab (pullable)
static int nextThrowId = -2000; // unique negative ids, like beam hits
static LazySound sndThrow = { "throw.wav" };
static LazySound sndBreak = { "break.wav" };
static LazySound sndItemWater = { "item in water.wav" };
static LazySound sndMoveObject = { "move object.wav" };
static LazySound sndButton = { "button.wav" };
static LazySound sndDoorOpen = { "door open.wav" };
static LazySound sndDoorClose = { "door close.wav" };
static LazySound sndHammerPound = { "quake 1.wav" };
static LazySound sndDig = { "dig.wav" };

static Rectangle objectBox(const WorldObject& o);

// Inventory (items, quantities, wallet, magic) — process-lifetime until
// the save system lands. The demo starts with the full kit so it's all
// reachable; chests/item-get sequences will grant these properly later.
static Inventory inventory;

// Toggle-item states (Byrna aura, magic cape): drain the magic meter
// while on (updateToggleItems); Byrna protects the hero, the cape also
// blinds enemy AI to them.
static bool byrnaActive = false;
static float byrnaDrain = 0.0f;
static float byrnaSting = 0.0f;
static bool capeActive = false;
static float capeDrain = 0.0f;
// Boss intro: entering a live arena holds the world while a letterbox
// names the boss (~1.5s).
static const float BOSS_INTRO_DURATION = 1.5f;
static float bossIntroTime = 0.0f;
static char bossIntroName[64] = "";
static LazySound sndBossIntro = { "boss hit.wav" };

static float shakeTime = 0.0f;
static float shakeStrength = 0.0f;
static float lowHpBeepTimer = 0.0f;

// Inventory query for dialogue conditions ("if_item"/"unless_item" lines).
static bool itemOwnedQuery(int item)
{
    return item >= 0 && item < Inventory::MAX_ITEMS && inventory.owned[item];
}

// Shop seam for dialogue choices ("cost"): returns true when the wallet
// covered the amount — deducting it — and false (nothing deducted) when
// the pick can't be afforded.
static bool tryPayRupeesQuery(int rupees)
{
    if (rupees < 0 || inventory.rupees < rupees) return false;
    inventory.addRupees(-rupees);
    return true;
}

// Passive light (the lantern, as data): the first owned item whose config
// carries a light radius. Items are game-defined; no item id is
// hard-coded here.
static int lightItemOwned()
{
    for (int i = 0; i < ITEM_COUNT && i < Inventory::MAX_ITEMS; i++)
        if (ITEMS[i].hasLight && inventory.owned[i]) return i;
    return -1;
}

// Swimming (flippers, as data): any owned item carrying the swim passive
// makes deep water swimmable. Same shape as the lantern scan.
static bool swimItemOwned()
{
    for (int i = 0; i < ITEM_COUNT && i < Inventory::MAX_ITEMS; i++)
        if (ITEMS[i].hasSwim && inventory.owned[i]) return true;
    return false;
}

// Heavy lifting (gloves, as data): any owned item carrying the lift
// passive lets the hero hoist heavy objects (big rocks, statues).
static bool liftItemOwned()
{
    for (int i = 0; i < ITEM_COUNT && i < Inventory::MAX_ITEMS; i++)
        if (ITEMS[i].hasLift && inventory.owned[i]) return true;
    return false;
}

// Compass (as data): any owned item carrying the compass passive marks
// chest/boss rooms on the map tab.
static bool compassItemOwned()
{
    for (int i = 0; i < ITEM_COUNT && i < Inventory::MAX_ITEMS; i++)
        if (ITEMS[i].hasCompass && inventory.owned[i]) return true;
    return false;
}

// World-flag upgrade (silver arrows, magical boomerang): while the item's
// upgrade flag is set, its use-action runs with the upgraded stats.
static bool itemUpgraded(const ItemConfig& item)
{
    return item.upgradeFlag >= 0 && item.upgradeFlag < WORLD_FLAG_COUNT
           && gWorldFlags[item.upgradeFlag];
}

// Enemy death drops: the enemy's own table when it has one, the engine
// default otherwise.
static int rollEnemyDrop(const Enemy& enemy)
{
    const EnemyConfig& cfg = enemy.config();
    if (cfg.drops && cfg.dropCount > 0)
        return rollDrop(worldRng, cfg.drops, cfg.dropCount);
    return rollDrop(worldRng);
}

// Hit-pause: a beat of frozen gameplay when a strike connects — the LttP
// impact feel on top of the screen shake.
static float hitPause = 0.0f;
static const float HIT_PAUSE_CONNECT = 0.045f;  // sword/spin/dash connects
static const float HIT_PAUSE_KILL = 0.08f;      // the blow that kills
static const float HIT_PAUSE_BOSS = 0.11f;      // boss armor rings louder

// Screen flash: full-screen color pulse that eases out over its duration
// (bomb blasts, boss deaths). The world keeps rendering underneath.
static float flashTime = 0.0f;
static float flashDuration = 0.0f;
static float flashPeak = 0.0f;
static Color flashColor = WHITE;

static void screenFlash(float duration, Color color, float peakAlpha)
{
    if (duration <= 0.0f) return;
    flashDuration = duration;
    flashTime = duration;
    flashColor = color;
    flashPeak = clampf(peakAlpha, 0.0f, 1.0f);
}

// Fanfare/jingle convention: item-get fanfares and the secret jingle duck
// the bgm — paused (not stopped) while the jingle plays, then resumed
// exactly where it left off.
static Sound jingleSound = {};
static bool jingleDucking = false;

static void playJingle(Sound sound)
{
    StopSound(jingleSound);
    jingleSound = sound;
    jingleDucking = true;
    PlaySound(sound);
    if (musicLoaded) PauseMusicStream(music);
    if (dangerLoaded) PauseMusicStream(dangerMusic);
}

// World sfx bank.
static LazySound sndGrass = { "grass destroyed.wav" };
static LazySound sndEnemyDie = { "enemy dies.wav" };
static LazySound sndHeart = { "heart.wav" };
static LazySound sndRupee = { "rupee.wav" };
static LazySound sndLowHp = { "low hp.wav" };
static LazySound sndShot = { "fireball.wav" };
static LazySound sndShotWall = { "arrow hit wall.wav" };

static Texture2D pickupsTexture = {};

static bool overlap(Rectangle a, Rectangle b)
{
    if (a.width <= 0.0f || a.height <= 0.0f) return false;
    if (a.x > b.x + b.width) return false;
    if (a.x + a.width < b.x) return false;
    if (a.y > b.y + b.height) return false;
    if (a.y + a.height < b.y) return false;
    return true;
}

static void spawnPickup(int kind, float x, float y)
{
    for (int i = 0; i < MAX_PICKUPS; i++)
    {
        if (pickups[i].active) continue;
        pickups[i].active = true;
        pickups[i].kind = kind;
        pickups[i].x = x;
        pickups[i].y = y;
        pickups[i].bobTime = worldRng.nextFloat() * 6.28f;
        pickups[i].life = 0.0f;
        return;
    }
}

static void spawnPoof(float x, float y)
{
    for (int i = 0; i < 32; i++)
    {
        if (poofs[i].active) continue;
        poofs[i].active = true;
        poofs[i].x = x;
        poofs[i].y = y;
        poofs[i].age = 0.0f;
        return;
    }
}

static Rectangle objectBox(const WorldObject& o)
{
    // Base footprint on the ground plane (solidity + flight collisions).
    return Rectangle{ o.x + 6.0f, o.y + 12.0f, 20.0f, 16.0f };
}

// Object interactions: the engine's behavior seam. Everything that used to
// branch on object type identity branches on the baked interaction mask
// (resources/objects/types.json composes types from the OBJINT_*
// vocabulary), so game-defined types behave like the interactions they
// compose.
static bool objInteracts(const WorldObject& o, unsigned flag)
{
    return o.type >= 0 && o.type < OBJECT_COUNT
           && (OBJECTS[o.type].interactions & flag) != 0;
}

static bool objInteractions(int type, unsigned flag)
{
    return type >= 0 && type < OBJECT_COUNT
           && (OBJECTS[type].interactions & flag) != 0;
}

// NPCs: character-driven townsfolk (see NpcSpawn). Idle ones turn to face
// the hero when nearby; wanderers stroll with pauses; talking (action
// button) makes them face the hero and opens their dialogue.
static const int MAX_NPCS = 8;
static const float NPC_WALK_SPEED = 40.0f;
struct Npc
{
    bool active;
    const CharacterConfig* cfg;
    Texture2D sheet;
    float x;
    float y;
    Direction facing;
    bool wander;
    const short* path;   // tile waypoints (x,y pairs), 0 = none
    int pathCount;
    int pathIndex;       // current waypoint
    int dialogue;   // DIALOGUE index, -1 = silent
    float modeTime;
    float timer;     // wander pause countdown
    float animTime;
    bool moving;
    int lastFacingPick;  // avoids picking the same stroll direction twice
};
static Npc npcs[MAX_NPCS] = {};
static Texture2D characterSheets[16] = {};

// --- Cutscenes (the framework) -------------------------------------------
// Scripted camera pans, letterbox bars, and actor blocking, all queued by
// the dialogue interpreter's cutscene vocabulary and resolved here while
// the world holds still. The cues compose with everything else a script
// can do (music stingers, flags, warps, chains).
struct CutsceneCamera
{
    bool active;    // off the hero: gliding to, or holding at, the target
    bool panning;
    float fromX;
    float fromY;
    float toX;
    float toY;
    float t;
    float duration;
};
static CutsceneCamera cutsceneCam = {};

struct CutsceneMove
{
    bool active;
    int actor;   // -2 = the hero, else a room npc index
    float fromX;
    float fromY;
    float toX;
    float toY;
    float t;
    float duration;
};
static CutsceneMove cutsceneMove = {};

static int letterboxWanted = 0;  // target state: 1 = bars on
static float letterboxT = 0.0f;  // eased slide (0..1)

static void updateCutscene(float dt)
{
    // Camera pan: glide, then hold at the target.
    if (cutsceneCam.active && cutsceneCam.panning)
    {
        cutsceneCam.t += dt;
        if (cutsceneCam.duration <= 0.0f
            || cutsceneCam.t >= cutsceneCam.duration)
        {
            cutsceneCam.panning = false;
        }
    }

    // Actor blocking: hero and npc glides (the world is frozen while the
    // script runs; the glide animates across it).
    if (cutsceneMove.active)
    {
        cutsceneMove.t += dt;
        float u = (cutsceneMove.duration > 0.0f)
                      ? clampf(cutsceneMove.t / cutsceneMove.duration, 0.0f, 1.0f)
                      : 1.0f;
        float x = cutsceneMove.fromX + (cutsceneMove.toX - cutsceneMove.fromX) * u;
        float y = cutsceneMove.fromY + (cutsceneMove.toY - cutsceneMove.fromY) * u;
        if (cutsceneMove.actor == -2)
        {
            float dx = x - hero.center().x;
            float dy = y - hero.center().y;
            hero.nudge(x - hero.center().x, y - hero.center().y);
            if (fabsf(dx) > 0.5f || fabsf(dy) > 0.5f) hero.faceVector(dx, dy);
        }
        else if (cutsceneMove.actor >= 0 && cutsceneMove.actor < MAX_NPCS
                 && npcs[cutsceneMove.actor].active)
        {
            Npc& npc = npcs[cutsceneMove.actor];
            float dx = x - npc.x;
            float dy = y - npc.y;
            npc.x = x;
            npc.y = y;
            npc.moving = true;
            npc.animTime += dt;
            if (fabsf(dx) > 0.5f || fabsf(dy) > 0.5f)
            {
                if (fabsf(dx) >= fabsf(dy))
                    npc.facing = (dx > 0.0f) ? DIR_RIGHT : DIR_LEFT;
                else
                    npc.facing = (dy > 0.0f) ? DIR_DOWN : DIR_UP;
            }
        }
        if (u >= 1.0f)
        {
            if (cutsceneMove.actor >= 0 && cutsceneMove.actor < MAX_NPCS
                && npcs[cutsceneMove.actor].active)
                npcs[cutsceneMove.actor].moving = false;
            cutsceneMove.active = false;
        }
    }

    // Letterbox bars ease toward the scripted state.
    float target = (float)letterboxWanted;
    if (letterboxT < target) letterboxT += dt / 0.25f;
    else if (letterboxT > target) letterboxT -= dt / 0.25f;
    if (letterboxT < 0.0f) letterboxT = 0.0f;
    if (letterboxT > 1.0f) letterboxT = 1.0f;
}

static void applyCutsceneCameraRequest(int action, int tileX, int tileY, float seconds)
{
    if (action == 2)
    {
        // Back to the hero.
        cutsceneCam.active = false;
        cutsceneCam.panning = false;
        return;
    }
    if (action != 1) return;
    Vector2 c = hero.center();
    cutsceneCam.active = true;
    cutsceneCam.panning = true;
    cutsceneCam.fromX = c.x;
    cutsceneCam.fromY = c.y;
    cutsceneCam.toX = (float)tileX * TILE_SIZE + TILE_SIZE * 0.5f;
    cutsceneCam.toY = (float)tileY * TILE_SIZE + TILE_SIZE * 0.5f;
    cutsceneCam.t = 0.0f;
    cutsceneCam.duration = seconds > 0.0f ? seconds : 1.0f;
}

static void applyCutsceneMoveRequest(int actor, int tileX, int tileY, float seconds)
{
    cutsceneMove.active = false;
    if (actor == -2)
    {
        Vector2 c = hero.center();
        cutsceneMove.actor = -2;
        cutsceneMove.fromX = c.x;
        cutsceneMove.fromY = c.y;
    }
    else if (actor >= 0 && actor < MAX_NPCS && npcs[actor].active)
    {
        cutsceneMove.actor = actor;
        cutsceneMove.fromX = npcs[actor].x + TILE_SIZE * 0.5f;
        cutsceneMove.fromY = npcs[actor].y + TILE_SIZE * 0.5f;
    }
    else
    {
        return;
    }
    cutsceneMove.toX = (float)tileX * TILE_SIZE + TILE_SIZE * 0.5f;
    cutsceneMove.toY = (float)tileY * TILE_SIZE + TILE_SIZE * 0.5f;
    cutsceneMove.t = 0.0f;
    cutsceneMove.duration = seconds > 0.0f ? seconds : 1.0f;
    cutsceneMove.active = true;
}

static Rectangle npcBox(const Npc& npc)
{
    return Rectangle{ npc.x + 8.0f, npc.y + 12.0f, 16.0f, 16.0f };
}

// Anything solid at the box for NPC movement: tiles, objects, other NPCs
// (the moving NPC itself is excluded).
static bool npcBlocked(const Npc& self, Rectangle box)
{
    if (room.solidTilesBox(box)) return true;
    for (int i = 0; i < MAX_OBJECTS; i++)
    {
        const WorldObject& o = objects[i];
        if (!o.active || !objInteracts(o, OBJINT_SOLID)) continue;
        if (o.state != OBJ_GROUND && o.state != OBJ_SLIDING) continue;
        if (overlap(box, objectBox(o))) return true;
    }
    for (int i = 0; i < MAX_NPCS; i++)
    {
        if (!npcs[i].active || &npcs[i] == &self) continue;
        if (overlap(box, npcBox(npcs[i]))) return true;
    }
    return false;
}


static bool objectSolidExtra(Rectangle box, void*)
{
    // Room solidity hook: ground-state and sliding objects block movement
    // like tile colliders. Floor switches and warp pads are walkable plates;
    // lifted, carried, thrown and sunk things don't block. NPCs stand in
    // the way too (their own movement checks exclude themselves).
    for (int i = 0; i < MAX_OBJECTS; i++)
    {
        if (!objects[i].active) continue;
        if (!objInteracts(objects[i], OBJINT_SOLID)) continue;
        if (objects[i].state != OBJ_GROUND && objects[i].state != OBJ_SLIDING) continue;
        if (overlap(box, objectBox(objects[i]))) return true;
    }
    for (int i = 0; i < MAX_NPCS; i++)
    {
        if (!npcs[i].active) continue;
        if (overlap(box, npcBox(npcs[i]))) return true;
    }
    return false;
}

// Anything solid at the box except the given object (for block-push
// destination checks): other objects + tile colliders.
static bool objectBlockedExcept(Rectangle box, int exceptIndex)
{
    if (room.solidTilesBox(box)) return true;
    for (int i = 0; i < MAX_OBJECTS; i++)
    {
        if (!objects[i].active || i == exceptIndex) continue;
        if (!objInteracts(objects[i], OBJINT_SOLID)) continue;
        if (objects[i].state != OBJ_GROUND && objects[i].state != OBJ_SLIDING) continue;
        if (overlap(box, objectBox(objects[i]))) return true;
    }
    return false;
}

static bool objectLiftable(int type)
{
    return objInteractions(type, OBJINT_LIFTABLE);
}

static void smashObject(int index, bool spawnDrops)
{
    WorldObject& o = objects[index];
    o.active = false;
    if (carriedIndex == index) carriedIndex = -1;
    if (liftingIndex == index) liftingIndex = -1;
    if (objInteracts(o, OBJINT_LEAVES))
    {
        // Leafy things (bushes) rustle apart like cut grass.
        PlaySound(sndGrass);
        spawnParticles(o.x + 16.0f, o.y + 14.0f, 8, 80, 140, 60, 70.0f, 130.0f);
    }
    else
    {
        PlaySound(sndBreak);
        spawnParticles(o.x + 16.0f, o.y + 14.0f, 8, 165, 130, 95, 80.0f, 120.0f);
    }
    spawnPoof(o.x + 16.0f, o.y + 16.0f);
    if (spawnDrops)
    {
        int drop = rollDrop(worldRng);
        if (drop != PICKUP_NONE)
            spawnPickup(drop, o.x + 8.0f, o.y + 8.0f);
    }
}

static void spawnProjectile(float x, float y, float dx, float dy, int damage)
{
    for (int i = 0; i < 8; i++)
    {
        if (projectiles[i].active) continue;
        projectiles[i].active = true;
        projectiles[i].friendly = false;
        projectiles[i].beam = false;
        projectiles[i].arrow = false;
        projectiles[i].element = ELEMENT_NONE;
        projectiles[i].dir = 0;
        projectiles[i].id = 0;
        projectiles[i].x = x;
        projectiles[i].y = y;
        projectiles[i].dx = dx;
        projectiles[i].dy = dy;
        projectiles[i].speed = 140.0f;
        projectiles[i].damage = damage;
        projectiles[i].enemyDamage = 1;
        return;
    }
}

static void spawnBeam(float x, float y, Direction facing)
{
    for (int i = 0; i < 8; i++)
    {
        if (projectiles[i].active) continue;
        projectiles[i].active = true;
        projectiles[i].friendly = true;
        projectiles[i].beam = true;
        projectiles[i].dir = (int)facing;
        projectiles[i].id = nextBeamSwingId--;  // unique per beam
        projectiles[i].x = x;
        projectiles[i].y = y;
        float dx = 0.0f;
        float dy = 0.0f;
        switch (facing)
        {
        case DIR_DOWN: dy = 1.0f; break;
        case DIR_UP: dy = -1.0f; break;
        case DIR_RIGHT: dx = 1.0f; break;
        case DIR_LEFT: dx = -1.0f; break;
        default: break;
        }
        projectiles[i].dx = dx;
        projectiles[i].dy = dy;
        projectiles[i].speed = 300.0f;
        projectiles[i].damage = 0;
        projectiles[i].enemyDamage = 1;
        return;
    }
}

static void throwBoomerang(float x, float y, Direction facing,
                            float speed, float range)
{
    if (boomer.active) return;  // cannot be reused while airborne
    boomer.active = true;
    boomer.returning = false;
    boomer.x = x;
    boomer.y = y;
    boomer.startX = x;
    boomer.startY = y;
    boomer.dirX = (facing == DIR_LEFT) ? -1.0f : (facing == DIR_RIGHT) ? 1.0f : 0.0f;
    boomer.dirY = (facing == DIR_UP) ? -1.0f : (facing == DIR_DOWN) ? 1.0f : 0.0f;
    boomer.time = 0.0f;
    boomer.speed = speed;
    boomer.range = range;
    // Throw sfx is item data (useSfx), played by the use interpreter.
}

// Bombs (the item): placed at the hero's feet, fuse ticking to a blast that
// damages enemies and blows bombable tiles open.
static const int MAX_BOMBS_LIVE = 4;
static const float BOMB_FUSE = 2.0f;
static const float BOMB_RADIUS = 44.0f;
struct Bomb
{
    bool active;
    float x;
    float y;
    float fuse;
};
static Bomb bombs[MAX_BOMBS_LIVE] = {};

// Explosion visuals (expanding rings + white flash) outlive the blast.
static const int MAX_EXPLOSIONS = 4;
struct Explosion
{
    bool active;
    float x;
    float y;
    float age;
};
static Explosion explosions[MAX_EXPLOSIONS] = {};

static LazySound sndBombExplode = { "bomb explode.wav" };
static LazySound sndBombWall = { "bombable wall.wav" };
static LazySound sndItemGet = { "item get 1.wav" };
static LazySound sndError = { "error.wav" };
static LazySound sndUiCursor = { "cursor.wav" };
static LazySound sndUiButton = { "button.wav" };
static LazySound sndChestOpen = { "chest open.wav" };
static LazySound sndFanfare = { "item get 2.wav" };
static LazySound sndFire = { "fire.wav" };  // element vocabulary: fire ignites torches
static LazySound sndUseKey = { "use key.wav" };
static LazySound sndFairy = { "fairy.wav" };
static LazySound sndBossDies = { "boss dies.wav" };
static LazySound sndSecret = { "secret.wav" };
static LazySound sndWarp = { "warp.wav" };
static LazySound sndCrystal = { "crystal.wav" };

// Darkness masks (dark rooms): a lightmap rendered additively (ambient
// gloom + radial lights) then multiplied over the frame.
static RenderTexture2D lightRT = {};
static Texture2D lightGradient = {};
static float lanternDrain = 0.0f;

// Chests stay open across room reloads (permanent until the save system
// gives them a real schema): keyed by room * MAX_OBJECTS + spawn slot.
// ROOM_COUNT isn't a compile-time constant here, so the registry is a
// fixed-size table with bounds-checked keys.
static const int MAX_CHEST_SLOTS = 4096;
static bool chestOpened[MAX_CHEST_SLOTS] = {};

static int currentRoom = 0;

// Opened-door registry (closed/locked/big/bombable once opened), keyed by
// room*4+edge with the reciprocal side kept in sync so a door is the same
// door from both rooms. Shutter doors are live (open while the room is
// cleared), so they don't use the registry.
static const int MAX_DOOR_SLOTS = 1024;
static bool doorOpened[MAX_DOOR_SLOTS] = {};

// Bosses stay dead (LttP): keyed by room * MAX_ENEMY_SLOTS + spawn slot.
static const int MAX_BOSS_SLOTS = 512;
static bool bossDefeated[MAX_BOSS_SLOTS] = {};

// Visited rooms, for the pause-screen map. Bounded like the other
// registries since ROOM_COUNT is extern.
static const int MAX_ROOM_MARKS = 128;
static bool roomVisited[MAX_ROOM_MARKS] = {};

// Dungeon treatment: per-room bitmask of defeated spawn slots (rooms cap
// at 16 spawns, so one ushort holds a room's set). Snapshotted on the way
// out of a dungeon room, consulted on the way back in — session-lifetime;
// overworld rooms respawn fresh on every entry.
static unsigned short dungeonDead[MAX_ROOM_MARKS] = {};

// Blasted-tile registry (bombable walls/floors blown open): keyed by room
// + tile; re-applied on room load so passages stay open, and persisted in
// the save so they stay open across sessions too.
static const int MAX_BLAST_SLOTS = 256;
static short blastRoom[MAX_BLAST_SLOTS] = {};
static short blastX[MAX_BLAST_SLOTS] = {};
static short blastY[MAX_BLAST_SLOTS] = {};
static int blastCount = 0;

static void recordBlastedTile(int roomIndex, int tileX, int tileY)
{
    for (int i = 0; i < blastCount; i++)
    {
        if (blastRoom[i] == (short)roomIndex && blastX[i] == (short)tileX
            && blastY[i] == (short)tileY)
            return;
    }
    if (blastCount >= MAX_BLAST_SLOTS) return;
    blastRoom[blastCount] = (short)roomIndex;
    blastX[blastCount] = (short)tileX;
    blastY[blastCount] = (short)tileY;
    blastCount++;
}

// Settled-object registry (room-local state, bake-slot keyed, saved):
// moved or sunk push blocks, latched latch-switches, pulled levers, and
// lit torches persist across room reloads and sessions — the puzzle
// stays solved, LttP dungeon style. Snapshotted on the way out of a
// room, re-applied on the way back in (see loadRoom). Transient states
// (hold/timed plates, thrown pots, conjured Somaria blocks) stay local
// by design.
static const int MAX_SETTLED_SLOTS = 256;
static const unsigned char SETTLED_SUNK = 0x1;     // block sank onto a latch
static const unsigned char SETTLED_LATCHED = 0x2;  // switch/lever/torch on
static short settledRoom[MAX_SETTLED_SLOTS] = {};
static short settledSlot[MAX_SETTLED_SLOTS] = {};
static short settledTileX[MAX_SETTLED_SLOTS] = {};
static short settledTileY[MAX_SETTLED_SLOTS] = {};
static unsigned char settledFlags[MAX_SETTLED_SLOTS] = {};
static int settledCount = 0;

// While a save loads, the outgoing world's snapshot must not contaminate
// the just-restored registry (the load's own room switch is the arrival,
// not a departure).
static bool suppressSettleSnapshot = false;

static void settleObject(int room, int slot, int tileX, int tileY,
                         bool sunk, bool latched)
{
    for (int i = 0; i < settledCount; i++)
    {
        if (settledRoom[i] != (short)room || settledSlot[i] != (short)slot)
            continue;
        settledTileX[i] = (short)tileX;
        settledTileY[i] = (short)tileY;
        if (sunk) settledFlags[i] |= SETTLED_SUNK;
        else settledFlags[i] &= (unsigned char)~SETTLED_SUNK;
        if (latched) settledFlags[i] |= SETTLED_LATCHED;
        else settledFlags[i] &= (unsigned char)~SETTLED_LATCHED;
        return;
    }
    if (settledCount >= MAX_SETTLED_SLOTS) return;
    settledRoom[settledCount] = (short)room;
    settledSlot[settledCount] = (short)slot;
    settledTileX[settledCount] = (short)tileX;
    settledTileY[settledCount] = (short)tileY;
    settledFlags[settledCount] = (unsigned char)((sunk ? SETTLED_SUNK : 0)
                                                 | (latched ? SETTLED_LATCHED : 0));
    settledCount++;
}

// Capture the room being left: which persistable objects sit away from
// their baked spawn, sunk, latched, or lit.
static void snapshotRoomState(int room)
{
    if (suppressSettleSnapshot) return;
    if (room < 0 || room >= ROOM_COUNT) return;
    const RoomConfig& cfg = ROOMS[room];
    int objectTotal = cfg.objectCount < MAX_OBJECTS ? cfg.objectCount : MAX_OBJECTS;
    for (int i = 0; i < objectTotal; i++)
    {
        const WorldObject& o = objects[i];
        if (!o.active) continue;
        // Only the baked object itself counts: dynamically-spawned pool
        // squatters (conjured Somaria blocks) must not settle over a
        // room-data object's slot.
        if (o.type != cfg.objects[i].type) continue;

        if (objInteracts(o, OBJINT_SWITCH) && o.aux1 == 1)
        {
            // Latch switch: stays pressed once solved.
            int tileX = (int)((o.x + TILE_SIZE * 0.5f) / TILE_SIZE);
            int tileY = (int)((o.y + TILE_SIZE * 0.5f) / TILE_SIZE);
            settleObject(room, i, tileX, tileY, false, o.switchActive);
            continue;
        }
        if (objInteracts(o, OBJINT_LEVER))
        {
            int tileX = (int)((o.x + TILE_SIZE * 0.5f) / TILE_SIZE);
            int tileY = (int)((o.y + TILE_SIZE * 0.5f) / TILE_SIZE);
            settleObject(room, i, tileX, tileY, false, o.switchActive);
            continue;
        }
        if (objInteracts(o, OBJINT_TORCH))
        {
            int tileX = (int)((o.x + TILE_SIZE * 0.5f) / TILE_SIZE);
            int tileY = (int)((o.y + TILE_SIZE * 0.5f) / TILE_SIZE);
            settleObject(room, i, tileX, tileY, false, o.opened);
            continue;
        }
        if (objInteracts(o, OBJINT_PUSHABLE))
        {
            // Blocks and statues: where they ended up, sunk or not. Only
            // worth an entry once they left their baked tile.
            int tileX = (int)((o.x + TILE_SIZE * 0.5f) / TILE_SIZE);
            int tileY = (int)((o.y + TILE_SIZE * 0.5f) / TILE_SIZE);
            bool sunk = (o.state == OBJ_SUNK);
            if (tileX == cfg.objects[i].tileX && tileY == cfg.objects[i].tileY
                && !sunk)
                continue;
            settleObject(room, i, tileX, tileY, sunk, false);
        }
    }
}

static bool bossAliveInRoom()
{
    // The arena seal holds while a boss OR a mid-fight mini-boss lives.
    for (int i = 0; i < enemyCount; i++)
        if (enemies[i].alive() && enemies[i].isArenaLock()) return true;
    return false;
}

static int doorKind(int roomIndex, int edge)  // edge: 0 N, 1 S, 2 E, 3 W
{
    if (roomIndex < 0 || roomIndex >= ROOM_COUNT) return DOOR_NONE;
    const RoomConfig& cfg = ROOMS[roomIndex];
    switch (edge)
    {
    case 0: return cfg.doorN;
    case 1: return cfg.doorS;
    case 2: return cfg.doorE;
    case 3: return cfg.doorW;
    default: return DOOR_NONE;
    }
}

static int livingEnemies()
{
    int count = 0;
    for (int i = 0; i < enemyCount; i++)
        if (enemies[i].alive()) count++;
    return count;
}

static int oppositeEdge(int edge)
{
    switch (edge)
    {
    case 0: return 1;
    case 1: return 0;
    case 2: return 3;
    case 3: return 2;
    default: return -1;
    }
}

static void markDoorOpened(int roomIndex, int edge)
{
    int slot = roomIndex * 4 + edge;
    if (slot >= 0 && slot < MAX_DOOR_SLOTS) doorOpened[slot] = true;
    // Pair-sync: the same door seen from the linked room opens too.
    int target = -1;
    const RoomConfig& cfg = ROOMS[roomIndex];
    switch (edge)
    {
    case 0: target = cfg.exitNorth; break;
    case 1: target = cfg.exitSouth; break;
    case 2: target = cfg.exitEast; break;
    case 3: target = cfg.exitWest; break;
    default: break;
    }
    int back = oppositeEdge(edge);
    if (target >= 0 && target < ROOM_COUNT && back >= 0
        && doorKind(target, back) != DOOR_NONE)
    {
        slot = target * 4 + back;
        if (slot >= 0 && slot < MAX_DOOR_SLOTS) doorOpened[slot] = true;
    }
}

static bool doorIsOpen(int roomIndex, int edge)
{
    // Arena lock: a living boss seals the whole room, door or not.
    if (roomIndex == currentRoom && bossAliveInRoom()) return false;

    int kind = doorKind(roomIndex, edge);
    if (kind == DOOR_NONE || kind >= DOOR_KIND_COUNT) return true;
    const DoorRule& rule = DOOR_RULES[kind];
    if (rule.startsOpen) return true;
    // Live conditions (the classic shutter): open while they hold.
    if (rule.liveEnemiesDead) return livingEnemies() == 0;
    // Opened-door registry (keys, pushed-open, blasted doors stay open).
    int slot = roomIndex * 4 + edge;
    return slot >= 0 && slot < MAX_DOOR_SLOTS && doorOpened[slot];
}

// Item-get sequence: the grant rises from the chest to over the hero's
// head (fanfare), then the description dialogue plays; the world is
// frozen until it closes.
struct ItemGetSequence
{
    bool active;
    bool dialogueOpened;
    float t;
    int grantKind;    // GRANT_*
    int grantParam;   // item index or amount
    int pickupFrame;  // pickups sheet frame, -1 = drawn as a medallion
    float startX;
    float startY;
    float endX;
    float endY;
    float itemX;
    float itemY;
};
static ItemGetSequence itemGet = {};
static DialogueLine itemGetLines[1] = {};
static char itemGetText[160] = "";
static Dialogue itemGetDialogue = { "item-get", 1, itemGetLines };

// --- Save system -----------------------------------------------------------
// Forward declarations: the save/flow helpers below call into systems that
// are defined further down this file.
static void clearWorldSystems();
static void loadRoom(int index, int enterMotion = -1, int tileX = -1, int tileY = -1);
static void startMusic(const char* file);
static void startAmbient(const char* file);
static void startDangerMusic(const char* file);
static void updateTitle();
static void drawTitleScreen();

// Binary blob, versioned; WORLD_FLAG_COUNT / ENCOUNTER_TOTAL are extern so
// the schema uses fixed caps with bounds-checked copies. The web build
// writes into raylib's IDBFS-backed /savefile directory so it persists in
// the browser.
static void loadRoomFromSave(int room, int tileX, int tileY);
static const int SAVE_MAGIC = 0x4C544F50;  // "LTOP"
static const int SAVE_VERSION = 9;  // v9: settled-object registry (room-local
                                     // blocks/switches/levers/torches persist)
static const int SAVE_SLOT_COUNT = 3;     // selectable once a title screen exists
static const int SAVE_FLAG_CAP = 256;
static const int SAVE_ENCOUNTER_CAP = 512;
static const int SAVE_BLAST_CAP = 256;

struct SaveData
{
    int magic;
    int version;
    char game[16];    // game slug (generator-baked): a save from one game
                      // is never loaded by another
    int room;          // current room index
    int tileX;
    int tileY;         // hero tile
    int health;        // half-hearts
    int maxHealth;
    int equippedItem;
    bool owned[Inventory::MAX_ITEMS];
    int rupees, rupeeMax, arrows, arrowsMax, bombs, bombsMax;
    int magicMeter, magicMax;
    int smallKeys, bigKeys;
    int bottles;                            // owned bottle slots
    int bottleContents[Inventory::BOTTLE_MAX];
    unsigned int rngState;
    bool flags[SAVE_FLAG_CAP];
    bool chests[MAX_CHEST_SLOTS];
    bool doors[MAX_DOOR_SLOTS];
    bool bosses[MAX_BOSS_SLOTS];
    bool visited[MAX_ROOM_MARKS];
    bool encounters[SAVE_ENCOUNTER_CAP];
    short blastRooms[SAVE_BLAST_CAP];
    short blastTilesX[SAVE_BLAST_CAP];
    short blastTilesY[SAVE_BLAST_CAP];
    int blastTotal;
    // Settled-object registry (v9): room-local puzzle state, bake-slot
    // keyed, capped like the other registries.
    short settledRooms[256];
    short settledSlots[256];
    short settledTilesX[256];
    short settledTilesY[256];
    unsigned char settledFlags[256];
    int settledTotal;
};

static int saveSlotInUse = 0;

static void saveFilePath(int slot, char* out, int size)
{
#if defined(PLATFORM_WEB)
    // raylib's web platform mounts IDBFS here (persisted by the browser).
    snprintf(out, size, "/savefile/slot%d.dat", slot);
#else
    snprintf(out, size, "save%d.dat", slot);
#endif
}

static void applySave(const SaveData& data)
{
    inventory.equippedItem = data.equippedItem;
    for (int i = 0; i < Inventory::MAX_ITEMS; i++) inventory.owned[i] = data.owned[i];
    inventory.rupees = data.rupees;
    inventory.rupeeMax = data.rupeeMax;
    inventory.arrows = data.arrows;
    inventory.arrowsMax = data.arrowsMax;
    inventory.bombs = data.bombs;
    inventory.bombsMax = data.bombsMax;
    inventory.magic = data.magicMeter;
    inventory.magicMax = data.magicMax;
    inventory.smallKeys = data.smallKeys;
    inventory.bigKeys = data.bigKeys;
    inventory.bottles = data.bottles;
    if (inventory.bottles < 0) inventory.bottles = 0;
    if (inventory.bottles > Inventory::BOTTLE_MAX) inventory.bottles = Inventory::BOTTLE_MAX;
    for (int i = 0; i < Inventory::BOTTLE_MAX; i++)
    {
        inventory.bottleContents[i] = (i < inventory.bottles)
            ? data.bottleContents[i] : Inventory::BOTTLE_EMPTY;
    }

    for (int i = 0; i < SAVE_FLAG_CAP; i++)
        if (WORLD_FLAG_COUNT > 0 && i < WORLD_FLAG_COUNT && i < SAVE_FLAG_CAP)
            gWorldFlags[i] = data.flags[i];
    for (int i = 0; i < MAX_CHEST_SLOTS; i++) chestOpened[i] = data.chests[i];
    for (int i = 0; i < MAX_DOOR_SLOTS; i++) doorOpened[i] = data.doors[i];
    for (int i = 0; i < MAX_BOSS_SLOTS; i++) bossDefeated[i] = data.bosses[i];
    for (int i = 0; i < MAX_ROOM_MARKS; i++) roomVisited[i] = data.visited[i];
    // Session-lifetime dungeon clears don't travel between save files:
    // restoring a slot starts its rooms fresh (LttP dungeons repopulate
    // on a new session; banked boss defeats come through the save).
    for (int i = 0; i < MAX_ROOM_MARKS; i++) dungeonDead[i] = 0;
    if (encounterFired)
    {
        int total = ENCOUNTER_TOTAL < SAVE_ENCOUNTER_CAP ? ENCOUNTER_TOTAL : SAVE_ENCOUNTER_CAP;
        for (int i = 0; i < total; i++) encounterFired[i] = data.encounters[i];
    }
    worldRng.seed(data.rngState);

    blastCount = data.blastTotal > MAX_BLAST_SLOTS ? MAX_BLAST_SLOTS : data.blastTotal;
    if (blastCount < 0) blastCount = 0;
    for (int i = 0; i < blastCount; i++)
    {
        blastRoom[i] = data.blastRooms[i];
        blastX[i] = data.blastTilesX[i];
        blastY[i] = data.blastTilesY[i];
    }

    // Settled-object registry (v9): the loaded file's puzzle state
    // replaces whatever the outgoing world had.
    settledCount = data.settledTotal > MAX_SETTLED_SLOTS ? MAX_SETTLED_SLOTS
                                                         : data.settledTotal;
    if (settledCount < 0) settledCount = 0;
    for (int i = 0; i < settledCount; i++)
    {
        settledRoom[i] = data.settledRooms[i];
        settledSlot[i] = data.settledSlots[i];
        settledTileX[i] = data.settledTilesX[i];
        settledTileY[i] = data.settledTilesY[i];
        settledFlags[i] = data.settledFlags[i];
    }

    // Land in the saved room at the saved tile (fade in). The arrival is
    // not a departure: the outgoing world must not snapshot over the
    // registry that was just restored.
    if (data.room >= 0 && data.room < ROOM_COUNT)
    {
        suppressSettleSnapshot = true;
        loadRoomFromSave(data.room, data.tileX, data.tileY);
        suppressSettleSnapshot = false;
        hero.setHealth(data.health, data.maxHealth);
    }
}

// Read + validate a save without applying it (title summaries).
static bool peekSave(int slot, SaveData& out)
{
    char path[128];
    saveFilePath(slot, path, sizeof(path));
    if (!FileExists(path)) return false;

    int bytesRead = 0;
    unsigned char* bytes = LoadFileData(path, &bytesRead);
    if (!bytes || (size_t)bytesRead < sizeof(SaveData))
    {
        if (bytes) UnloadFileData(bytes);
        return false;
    }
    memcpy(&out, bytes, sizeof(out));
    UnloadFileData(bytes);

    return out.magic == SAVE_MAGIC && out.version == SAVE_VERSION
           && strncmp(out.game, GAME_SLUG, sizeof(out.game)) == 0;
}

static bool loadFromFile(int slot)
{
    SaveData data;
    if (!peekSave(slot, data)) return false;
    applySave(data);
    fprintf(stderr, "loaded save from slot %d (room %d)\n", slot, data.room);
    return true;
}

// Test-from-here (editor hook): launching with --room/--tile suppresses
// saving — a throwaway session banking nothing, so quick previews never
// clobber the real slots.
static bool testMode = false;

static void saveToFile(int slot)
{
    if (testMode) return;
    SaveData data;
    memset(&data, 0, sizeof(data));
    data.magic = SAVE_MAGIC;
    data.version = SAVE_VERSION;
    snprintf(data.game, sizeof(data.game), "%s", GAME_SLUG);
    data.room = currentRoom;
    int tileX = 0;
    int tileY = 0;
    hero.tileUnder(tileX, tileY);
    data.tileX = tileX;
    data.tileY = tileY;
    data.health = hero.healthHalves();
    data.maxHealth = hero.maxHealthHalves();
    data.equippedItem = inventory.equippedItem;
    for (int i = 0; i < Inventory::MAX_ITEMS; i++) data.owned[i] = inventory.owned[i];
    data.rupees = inventory.rupees;
    data.rupeeMax = inventory.rupeeMax;
    data.arrows = inventory.arrows;
    data.arrowsMax = inventory.arrowsMax;
    data.bombs = inventory.bombs;
    data.bombsMax = inventory.bombsMax;
    data.magicMeter = inventory.magic;
    data.magicMax = inventory.magicMax;
    data.smallKeys = inventory.smallKeys;
    data.bigKeys = inventory.bigKeys;
    data.bottles = inventory.bottles;
    for (int i = 0; i < Inventory::BOTTLE_MAX; i++)
        data.bottleContents[i] = inventory.bottleContents[i];
    data.rngState = worldRng.rawState();

    for (int i = 0; i < SAVE_FLAG_CAP; i++)
        data.flags[i] = (WORLD_FLAG_COUNT > 0 && i < WORLD_FLAG_COUNT) ? gWorldFlags[i] : false;
    for (int i = 0; i < MAX_CHEST_SLOTS; i++) data.chests[i] = chestOpened[i];
    for (int i = 0; i < MAX_DOOR_SLOTS; i++) data.doors[i] = doorOpened[i];
    for (int i = 0; i < MAX_BOSS_SLOTS; i++) data.bosses[i] = bossDefeated[i];
    for (int i = 0; i < MAX_ROOM_MARKS; i++) data.visited[i] = roomVisited[i];
    if (encounterFired)
    {
        int total = ENCOUNTER_TOTAL < SAVE_ENCOUNTER_CAP ? ENCOUNTER_TOTAL : SAVE_ENCOUNTER_CAP;
        for (int i = 0; i < total; i++) data.encounters[i] = encounterFired[i];
    }

    for (int i = 0; i < blastCount && i < SAVE_BLAST_CAP; i++)
    {
        data.blastRooms[i] = blastRoom[i];
        data.blastTilesX[i] = blastX[i];
        data.blastTilesY[i] = blastY[i];
    }
    data.blastTotal = blastCount;
    data.settledTotal = settledCount;
    for (int i = 0; i < settledCount && i < 256; i++)
    {
        data.settledRooms[i] = settledRoom[i];
        data.settledSlots[i] = settledSlot[i];
        data.settledTilesX[i] = settledTileX[i];
        data.settledTilesY[i] = settledTileY[i];
        data.settledFlags[i] = settledFlags[i];
    }

    char path[128];
    if (slot < 0 || slot >= SAVE_SLOT_COUNT) return;
    saveFilePath(slot, path, sizeof(path));
    SaveFileData(path, &data, sizeof(data));
}

// --- Game flow: title screen, save slots, save-and-quit -------------------
enum GameState
{
    STATE_TITLE,
    STATE_PLAYING
};
static int gameState = STATE_TITLE;
static int titleCursor = 0;

// Fresh session: every piece of process-global state back to defaults
// (called when starting/continuing a slot so slots don't leak into each
// other).
static void resetGameState()
{
    for (int i = 0; WORLD_FLAG_COUNT > 0 && i < WORLD_FLAG_COUNT; i++) gWorldFlags[i] = false;
    for (int i = 0; i < MAX_CHEST_SLOTS; i++) chestOpened[i] = false;
    for (int i = 0; i < MAX_DOOR_SLOTS; i++) doorOpened[i] = false;
    for (int i = 0; i < MAX_BOSS_SLOTS; i++) bossDefeated[i] = false;
    for (int i = 0; i < MAX_ROOM_MARKS; i++) roomVisited[i] = false;
    if (encounterFired)
        for (int i = 0; i < ENCOUNTER_TOTAL; i++) encounterFired[i] = false;
    blastCount = 0;
    worldRng.seed(0xC0FFEE1Au);
    lanternDrain = 0.0f;
    byrnaActive = false;
    capeActive = false;
    itemGet.active = false;
    clearWorldSystems();

    // Demo starting inventory: everything owned, boomerang equipped, so
    // the whole item surface is reachable (chests/item-get sequences
    // will grant these properly later).
    inventory = Inventory();
    for (int i = 0; i < ITEM_COUNT && i < Inventory::MAX_ITEMS; i++) inventory.give(i);
    inventory.equippedItem = (ITEM_COUNT > 0) ? ITEM_BOOMERANG : -1;
    inventory.arrows = 4;
    inventory.bombs = 4;
    inventory.magic = inventory.magicMax;
}

static void startNewGame(int slot)
{
    saveSlotInUse = slot;
    resetGameState();
    loadRoom(0);
    gameState = STATE_PLAYING;
    saveToFile(slot);  // bank the fresh start
}

static void continueGame(int slot)
{
    saveSlotInUse = slot;
    resetGameState();
    if (!loadFromFile(slot))
    {
        loadRoom(0);
        saveToFile(slot);
    }
    gameState = STATE_PLAYING;
}

static void quitToTitle()
{
    dialogue.close();  // no half-finished boxes across sessions
    saveToFile(saveSlotInUse);
    gameState = STATE_TITLE;
    startMusic(NULL);
    startAmbient(NULL);
    startDangerMusic(NULL);
}

static void updateTitle()
{
    bool prev = IsKeyPressed(KEY_UP);
    bool next = IsKeyPressed(KEY_DOWN);
    for (int i = 0; i < 4; i++)
    {
        if (!IsGamepadAvailable(i)) continue;
        if (IsGamepadButtonPressed(i, GAMEPAD_BUTTON_LEFT_FACE_UP)) prev = true;
        if (IsGamepadButtonPressed(i, GAMEPAD_BUTTON_LEFT_FACE_DOWN)) next = true;
    }
    if (prev || next)
    {
        titleCursor = (titleCursor + (next ? 1 : SAVE_SLOT_COUNT - 1)) % SAVE_SLOT_COUNT;
        PlaySound(sndUiCursor);
    }

    if (actionPressed())
    {
        PlaySound(sndUiButton);
        SaveData peek;
        if (peekSave(titleCursor, peek)) continueGame(titleCursor);
        else startNewGame(titleCursor);
    }
}

static void drawTitleScreen()
{
    float scale = virtualScale();
    int sw = GetScreenWidth();
    int sh = GetScreenHeight();

    const char* title = "LTOP ENGINE";
    int titleSize = (int)(24 * scale);
    int width = MeasureText(title, titleSize);
    DrawText(title, (sw - width) / 2, (int)(34 * scale), titleSize, RAYWHITE);

    const char* sub = GAME_TITLE;
    int subSize = (int)(8 * scale);
    width = MeasureText(sub, subSize);
    DrawText(sub, (sw - width) / 2, (int)(64 * scale), subSize,
             Color{ 170, 170, 200, 255 });

    // Save slots with a summary of what's inside.
    float listX = sw * 0.5f - 100.0f * scale;
    float listY = 104.0f * scale;
    for (int slot = 0; slot < SAVE_SLOT_COUNT; slot++)
    {
        char line[96];
        SaveData peek;
        if (peekSave(slot, peek))
        {
            snprintf(line, sizeof(line), "SLOT %d   %d HEARTS   %d RUPEES",
                     slot + 1, peek.maxHealth / 2, peek.rupees);
        }
        else
        {
            snprintf(line, sizeof(line), "SLOT %d   - EMPTY -", slot + 1);
        }

        int size = (int)(12 * scale);
        float y = listY + (float)slot * 22.0f * scale;
        Color color = (slot == titleCursor) ? RAYWHITE : Color{ 130, 130, 150, 255 };
        DrawText(line, (int)(listX + 16.0f * scale), (int)y, size, color);

        if (slot == titleCursor)
        {
            float cy = y + size * 0.5f;
            Vector2 a = { listX + 6.0f * scale, cy - 5.0f * scale };
            Vector2 b = { listX + 6.0f * scale, cy + 5.0f * scale };
            Vector2 c = { listX + 14.0f * scale, cy };
            DrawTriangle(a, b, c, RAYWHITE);
        }
    }

    const char* hint = "ACTION: START";
    int hintSize = (int)(8 * scale);
    width = MeasureText(hint, hintSize);
    DrawText(hint, (sw - width) / 2, sh - (int)(16 * scale), hintSize,
             Color{ 170, 170, 190, 255 });
}

static void spawnExplosion(float x, float y)
{
    for (int i = 0; i < MAX_EXPLOSIONS; i++)
    {
        if (explosions[i].active) continue;
        explosions[i].active = true;
        explosions[i].x = x;
        explosions[i].y = y;
        explosions[i].age = 0.0f;
        return;
    }
}

static void detonateBomb(Bomb& bomb)
{
    bomb.active = false;
    PlaySound(sndBombExplode);
    spawnExplosion(bomb.x, bomb.y);

    // Blast feedback.
    shakeTime = 0.35f;
    shakeStrength = 5.0f;
    screenFlash(0.16f, Color{ 255, 255, 245, 255 }, 0.40f);

    // Damage enemies in the radius (one hit each, knocked from the blast).
    int blastId = nextBeamSwingId--;
    for (int e = 0; e < enemyCount; e++)
    {
        if (!enemies[e].alive()) continue;
        Vector2 ec = enemies[e].center();
        float dx = ec.x - bomb.x;
        float dy = ec.y - bomb.y;
        if (dx * dx + dy * dy > BOMB_RADIUS * BOMB_RADIUS) continue;
        if (enemies[e].tryHit(Rectangle{ bomb.x - BOMB_RADIUS, bomb.y - BOMB_RADIUS,
                                         BOMB_RADIUS * 2.0f, BOMB_RADIUS * 2.0f },
                              blastId, Vector2{ dx, dy }))
        {
            PlaySound(sndEnemyDie);
            spawnPoof(ec.x, ec.y);
            int drop = rollEnemyDrop(enemies[e]);
            if (drop != PICKUP_NONE)
                spawnPickup(drop, ec.x - 8.0f, ec.y - 8.0f);
        }
    }

    // Blow bombable tiles open (cracked walls/floors).
    int tx0 = (int)((bomb.x - BOMB_RADIUS) / TILE_SIZE);
    int ty0 = (int)((bomb.y - BOMB_RADIUS) / TILE_SIZE);
    int tx1 = (int)((bomb.x + BOMB_RADIUS) / TILE_SIZE);
    int ty1 = (int)((bomb.y + BOMB_RADIUS) / TILE_SIZE);
    bool blasted = false;
    for (int ty = ty0; ty <= ty1; ty++)
    {
        for (int tx = tx0; tx <= tx1; tx++)
        {
            float cx = (float)tx * TILE_SIZE + TILE_SIZE * 0.5f;
            float cy = (float)ty * TILE_SIZE + TILE_SIZE * 0.5f;
            float dx = cx - bomb.x;
            float dy = cy - bomb.y;
            if (dx * dx + dy * dy > BOMB_RADIUS * BOMB_RADIUS) continue;
            if (room.bombTileAt(tx, ty))
            {
                blasted = true;
                recordBlastedTile(currentRoom, tx, ty);
            }
        }
    }
    if (blasted) PlaySound(sndBombWall);

    // Bombable doors on this room's edges blow open when the blast reaches.
    bool secretFound = blasted;
    {
        const RoomConfig& cfg = room.config();
        float mapW = (float)(cfg.mapWidth * TILE_SIZE);
        float mapH = (float)(cfg.mapHeight * TILE_SIZE);
        for (int edge = 0; edge < 4; edge++)
        {
        int edgeKind = doorKind(currentRoom, edge);
        if (edgeKind < 0 || edgeKind >= DOOR_KIND_COUNT
            || !DOOR_RULES[edgeKind].bombable) continue;
        if (doorIsOpen(currentRoom, edge)) continue;
        float doorX = mapW * 0.5f;
        float doorY = mapH * 0.5f;
        switch (edge)
        {
        case 0: doorX = mapW * 0.5f; doorY = 0.0f; break;
        case 1: doorX = mapW * 0.5f; doorY = mapH; break;
        case 2: doorX = mapW; doorY = mapH * 0.5f; break;
        case 3: doorX = 0.0f; doorY = mapH * 0.5f; break;
        default: break;
        }
            float dx = doorX - bomb.x;
            float dy = doorY - bomb.y;
            if (dx * dx + dy * dy > (BOMB_RADIUS + 16.0f) * (BOMB_RADIUS + 16.0f)) continue;
            markDoorOpened(currentRoom, edge);
            PlaySound(sndBombWall);
            secretFound = true;
        }
    }

    // "Secret discovered" jingle convention: blasting a passage open
    // ducks the bgm for the sting, then the music resumes.
    if (secretFound) playJingle(sndSecret);
}

static void updateBombs(float dt)
{
    for (int i = 0; i < MAX_BOMBS_LIVE; i++)
    {
        if (!bombs[i].active) continue;
        bombs[i].fuse -= dt;
        if (bombs[i].fuse <= 0.0f) detonateBomb(bombs[i]);
    }

    for (int i = 0; i < MAX_EXPLOSIONS; i++)
    {
        if (!explosions[i].active) continue;
        explosions[i].age += dt;
        if (explosions[i].age >= 0.45f) explosions[i].active = false;
    }
}

static void placeBomb(float x, float y)
{
    for (int i = 0; i < MAX_BOMBS_LIVE; i++)
    {
        if (bombs[i].active) continue;
        bombs[i].active = true;
        bombs[i].x = x;
        bombs[i].y = y;
        bombs[i].fuse = BOMB_FUSE;
        return;
    }
}

// Bow: spawn the arrow projectile (the use-action interpreter paid the
// cost - arrows, or a rupee when the quiver is dry, LttP behavior).
static void spawnArrow(float x, float y, Direction facing, float speed,
                       int enemyDamage)
{
    for (int i = 0; i < 8; i++)
    {
        if (projectiles[i].active) continue;
        projectiles[i].active = true;
        projectiles[i].friendly = true;
        projectiles[i].beam = false;
        projectiles[i].arrow = true;
        projectiles[i].dir = (int)facing;
        projectiles[i].id = nextBeamSwingId--;
        projectiles[i].x = x - 8.0f;
        projectiles[i].y = y - 8.0f;
        float dx = 0.0f;
        float dy = 0.0f;
        switch (facing)
        {
        case DIR_DOWN: dy = 1.0f; break;
        case DIR_UP: dy = -1.0f; break;
        case DIR_RIGHT: dx = 1.0f; break;
        case DIR_LEFT: dx = -1.0f; break;
        default: break;
        }
        projectiles[i].dx = dx;
        projectiles[i].dy = dy;
        projectiles[i].speed = speed;
        projectiles[i].damage = 0;
        projectiles[i].enemyDamage = enemyDamage;
        return;
    }
}

// Rods: fire/ice magic bolts that consume the magic meter.
static void spawnElementProjectile(float x, float y, Direction facing, int element)
{
    for (int i = 0; i < 8; i++)
    {
        if (projectiles[i].active) continue;
        projectiles[i].active = true;
        projectiles[i].friendly = true;
        projectiles[i].beam = false;
        projectiles[i].arrow = false;
        projectiles[i].element = element;
        projectiles[i].dir = (int)facing;
        projectiles[i].id = nextBeamSwingId--;
        projectiles[i].x = x - 8.0f;
        projectiles[i].y = y - 8.0f;
        float dx = 0.0f;
        float dy = 0.0f;
        switch (facing)
        {
        case DIR_DOWN: dy = 1.0f; break;
        case DIR_UP: dy = -1.0f; break;
        case DIR_RIGHT: dx = 1.0f; break;
        case DIR_LEFT: dx = -1.0f; break;
        default: break;
        }
        projectiles[i].dx = dx;
        projectiles[i].dy = dy;
        projectiles[i].speed = 200.0f;
        projectiles[i].damage = 0;
        return;
    }
}

// The item button runs whatever is equipped. Items are game content: the
// baked use-action composes the engine's vocabulary (costs + spawn kind +
// sfx), and this interpreter runs it - no per-item branches exist.
// Medallions (use-kind "medallion", the element decides the storm):
// screen-wide bursts sweeping every enemy in the room — fire burns them
// to ash, ice freezes them solid, and no element is the earth-quake
// that flips them over. Arena bosses keep their armor (medallions
// bounce off boss battles, like LttP).
static void castMedallion(int element)
{
    playNamedSfx("spin energy.wav");

    // Weather-ish visuals across the visible screen: embers rising,
    // frost falling, or the whole ground shuddering.
    float scale = virtualScale();
    float left = camera.target.x - (float)GetScreenWidth() * 0.5f / scale;
    float top = camera.target.y - VIRTUAL_HEIGHT * 0.5f;
    float width = (float)GetScreenWidth() / scale;
    float height = VIRTUAL_HEIGHT;

    if (element == ELEMENT_FIRE)
    {
        // Bombos: the firestorm burns every enemy dead.
        screenFlash(0.35f, Color{ 255, 170, 70, 255 }, 0.45f);
        shakeTime = 0.5f;
        shakeStrength = 5.0f;
        playNamedSfx("fire.wav");
        for (int i = 0; i < 24; i++)
        {
            spawnParticles(left + worldRng.nextFloat() * width,
                           top + worldRng.nextFloat() * height,
                           1, 255, 150, 50, 40.0f, 160.0f);
        }
        for (int e = 0; e < enemyCount; e++)
        {
            if (!enemies[e].alive() || enemies[e].isBoss()) continue;
            Vector2 c = enemies[e].center();
            if (enemies[e].tryHit(Rectangle{ c.x - 44.0f, c.y - 44.0f, 88.0f, 88.0f },
                                   nextBeamSwingId--, Vector2{ 0.0f, -1.0f }, 99))
            {
                PlaySound(sndEnemyDie);
                spawnPoof(c.x, c.y);
                spawnParticles(c.x, c.y, 6, 255, 140, 40, 90.0f, 140.0f);
                int drop = rollEnemyDrop(enemies[e]);
                if (drop != PICKUP_NONE)
                    spawnPickup(drop, c.x - 8.0f, c.y - 8.0f);
            }
        }
        return;
    }

    if (element == ELEMENT_ICE)
    {
        // Ether: the air freezes solid — everything afloat stands still.
        screenFlash(0.35f, Color{ 150, 200, 255, 255 }, 0.45f);
        playNamedSfx("ice rod.wav");
        for (int i = 0; i < 24; i++)
        {
            spawnParticles(left + worldRng.nextFloat() * width, top - 6.0f,
                           1, 190, 220, 255, 26.0f, 40.0f);
        }
        for (int e = 0; e < enemyCount; e++)
        {
            if (!enemies[e].alive() || enemies[e].isBoss()) continue;
            enemies[e].freeze(4.0f);
            PlaySound(sndBoomerangHit);
        }
        return;
    }

    // Quake: the ground bucks and every enemy flips over on its back.
    screenFlash(0.30f, Color{ 210, 205, 190, 255 }, 0.35f);
    shakeTime = 0.6f;
    shakeStrength = 7.0f;
    playNamedSfx("quake 2.wav");
    for (int i = 0; i < 24; i++)
    {
        spawnParticles(left + worldRng.nextFloat() * width,
                       top + height - 4.0f,
                       1, 150, 120, 90, 70.0f, 120.0f);
    }
    for (int e = 0; e < enemyCount; e++)
    {
        if (!enemies[e].alive() || enemies[e].isBoss()) continue;
        enemies[e].stun(1.5f);
    }
}

// Somaria-style block conjuring (defined later, beside the object pool).
static void toggleSomariaBlock(const ItemConfig& item);

// Flute fast travel (defined later, beside the pause subscreen): the
// item button opens the destination picker over the visited-room map.
static bool fluteMenu = false;
static int fluteCursor = 0;
static void fluteWarp();  // defined beside the warp pads it banks like

// Mirror (use-kind "mirror", defined later beside the warp pads): the
// reflection slips the hero into the parallel-world pair.
static void useMirror();

// Mirror landing: the parallel room's paired tile — layouts may differ,
// so clamp to its bounds and spiral out to the nearest free ground (the
// same conditions the edge placement uses).
static bool mirrorTileBlocked(const RoomConfig& cfg, int tileX, int tileY)
{
    int w = cfg.mapWidth;
    int h = cfg.mapHeight;
    if (tileX < 0 || tileY < 0 || tileX >= w || tileY >= h) return true;
    if (cfg.colliders[tileY * w + tileX] != 0) return true;
    int ids[2] = { cfg.decor[tileY * w + tileX],
                   cfg.colliders[tileY * w + tileX] };
    for (int i = 0; i < 2; i++)
    {
        int id = ids[i];
        if (id <= 0 || id > TILE_META_COUNT) continue;
        unsigned short flags = TILE_META[id - 1].flags;
        if (flags & TILE_FLAG_PIT) return true;
        // Deep water is fine with the flippers, a dunk without.
        if ((flags & TILE_FLAG_WATER_DEEP) && !swimItemOwned()) return true;
    }
    return false;
}

// Bottle use: standing in fountain water dips an empty bottle in;
// otherwise the first filled slot pours out its contents (the fairy
// spirals out with a full refill, the bee simply departs).
static void useBottle()
{
    int tileX = 0;
    int tileY = 0;
    hero.tileUnder(tileX, tileY);
    TileMeta meta = room.tileMetaAt(tileX, tileY);

    // Fountain water: an empty bottle fills first (LttP bottles hold
    // whatever the water offers).
    if (meta.heal)
    {
        int slot = inventory.firstEmptyBottle();
        if (slot >= 0)
        {
            inventory.bottleContents[slot] = Inventory::BOTTLE_WATER;
            playNamedSfx("item in water.wav");
            return;
        }
    }

    int contents = Inventory::BOTTLE_EMPTY;
    int slot = inventory.firstFilledBottle(contents);
    if (slot < 0)
    {
        PlaySound(sndError);  // nothing to drink, nothing to fill with
        return;
    }

    Vector2 c = hero.center();
    switch (contents)
    {
    case Inventory::BOTTLE_WATER:
        hero.heal(2);  // a heart's worth of fountain water
        PlaySound(sndHeart);
        break;
    case Inventory::BOTTLE_POTION_RED:
        hero.heal(hero.maxHealthHalves());
        playNamedSfx("life refill.wav");
        break;
    case Inventory::BOTTLE_POTION_GREEN:
        inventory.addMagic(inventory.magicMax);
        playNamedSfx("magic meter 1.wav");
        break;
    case Inventory::BOTTLE_POTION_BLUE:
        hero.heal(hero.maxHealthHalves());
        inventory.addMagic(inventory.magicMax);
        playNamedSfx("life refill.wav");
        break;
    case Inventory::BOTTLE_FAIRY:
        hero.heal(hero.maxHealthHalves());
        inventory.addMagic(inventory.magicMax);
        PlaySound(sndFairy);
        spawnParticles(c.x, c.y, 12, 255, 200, 220, 70.0f, 120.0f);
        break;
    case Inventory::BOTTLE_BEE:
        playNamedSfx("bee.wav");
        spawnParticles(c.x, c.y - 8.0f, 6, 230, 200, 60, 60.0f, 100.0f);
        break;
    default:
        break;
    }
    inventory.bottleContents[slot] = Inventory::BOTTLE_EMPTY;
}

static void useEquippedItem()
{
    if (inventory.equippedItem < 0 || inventory.equippedItem >= ITEM_COUNT) return;
    const ItemConfig& item = ITEMS[inventory.equippedItem];
    if (item.useKind == ITEM_USE_NONE)
    {
        PlaySound(sndError);  // passive item (or nothing) equipped
        return;
    }

    // Costs, in the order the vocabulary defines them: magic, bombs,
    // arrows (with the optional rupee fallback). An unaffordable cost
    // plays the error cue and cancels the use.
    if (item.magicCost > 0 && !inventory.consumeMagic(item.magicCost))
    {
        PlaySound(sndError);
        return;
    }
    if (item.bombCost > 0)
    {
        if (inventory.bombs < item.bombCost)
        {
            PlaySound(sndError);
            return;
        }
        inventory.addBombs(-item.bombCost);
    }
    if (item.arrowCost > 0)
    {
        if (inventory.arrows >= item.arrowCost)
        {
            inventory.addArrows(-item.arrowCost);
        }
        else if (item.arrowRupeeFallback && inventory.rupees > 0)
        {
            inventory.addRupees(-1);
        }
        else
        {
            PlaySound(sndError);
            return;
        }
    }

    Vector2 heroCenter = hero.center();
    switch (item.useKind)
    {
    case ITEM_USE_BOOMERANG:
        // The magical upgrade flies faster and reaches farther.
        throwBoomerang(heroCenter.x, heroCenter.y, hero.facingDirection(),
                       itemUpgraded(item) && item.upgradeSpeed > 0
                           ? (float)item.upgradeSpeed : 200.0f,
                       itemUpgraded(item) && item.upgradeSpeed > 0
                           ? 176.0f : 104.0f);
        break;
    case ITEM_USE_ARROW:
        // Silver arrows strike harder as well as faster.
        spawnArrow(heroCenter.x, heroCenter.y, hero.facingDirection(),
                   itemUpgraded(item) && item.upgradeSpeed > 0
                       ? (float)item.upgradeSpeed : 340.0f,
                   itemUpgraded(item) && item.upgradeDamage > 0
                       ? item.upgradeDamage : 1);
        break;
    case ITEM_USE_BOMB:
        // Placed at the hero's feet (bottom of the sprite).
        placeBomb(heroCenter.x, heroCenter.y + 8.0f);
        break;
    case ITEM_USE_BOLT:
        spawnElementProjectile(heroCenter.x, heroCenter.y, hero.facingDirection(),
                               item.element);
        break;
    case ITEM_USE_HAMMER:
        // The hero pounds: windup, then the quake lands in the facing
        // (updateHammer resolves the effects at HAMMER_WINDUP).
        hero.startHammer();
        break;
    case ITEM_USE_SHOVEL:
        // The hero digs: windup, then the tile ahead turns over
        // (updateShovel resolves the dig at DIG_WINDUP).
        hero.startDig();
        break;
    case ITEM_USE_HOOKSHOT:
        // The chained hook flies from the hero's hands (only one airborne;
        // the hero stays locked until the chain resolves).
        if (!hook.active)
        {
            Vector2 c = hero.center();
            Direction facing = hero.facingDirection();
            hook.active = true;
            hook.retracting = false;
            hook.latched = false;
            hook.x = c.x;
            hook.y = c.y;
            hook.startX = c.x;
            hook.startY = c.y;
            hook.dirX = (facing == DIR_LEFT) ? -1.0f : (facing == DIR_RIGHT) ? 1.0f : 0.0f;
            hook.dirY = (facing == DIR_UP) ? -1.0f : (facing == DIR_DOWN) ? 1.0f : 0.0f;
            hero.startHookshot();
            PlaySound(sndHookshot);
        }
        break;
    case ITEM_USE_MEDALLION:
        // Screen-wide element burst; the element decides the storm.
        castMedallion(item.element);
        break;
    case ITEM_USE_BYRNA:
        // Toggle the protective aura; it drains the meter while on.
        if (byrnaActive)
        {
            byrnaActive = false;
        }
        else if (inventory.magic > 0)
        {
            byrnaActive = true;
            byrnaDrain = 0.0f;
            byrnaSting = 0.0f;
        }
        else
        {
            PlaySound(sndError);  // nothing in the meter to burn
        }
        break;
    case ITEM_USE_CAPE:
        // Toggle the ghost cloak; it drains the meter while on.
        if (capeActive)
        {
            capeActive = false;
        }
        else if (inventory.magic > 0)
        {
            capeActive = true;
            capeDrain = 0.0f;
        }
        else
        {
            PlaySound(sndError);
        }
        break;
    case ITEM_USE_SOMARIA:
        // Conjure/shatter the linked block type (puzzle payloads).
        toggleSomariaBlock(item);
        break;
    case ITEM_USE_POWDER:
        // Sprinkle in the facing (updatePowder resolves the puff at
        // SPRINKLE_WINDUP: stuns, torches, shrines).
        hero.startSprinkle();
        break;
    case ITEM_USE_FLUTE:
        // Open the destination picker (updateFluteMenu flies the bird).
        if (!fluteMenu)
        {
            fluteCursor = currentRoom;
            fluteMenu = true;
        }
        break;
    case ITEM_USE_NET:
        // The hero swipes: windup, then the sweep in the facing
        // (updateNet resolves catches/agitation at NET_WINDUP).
        hero.startNet();
        break;
    case ITEM_USE_BOTTLE:
        useBottle();
        break;
    case ITEM_USE_MIRROR:
        // Reflection: slip through to the parallel-world pair (defined
        // beside the warp pads, where the landing machinery lives).
        useMirror();
        break;
    default:
        PlaySound(sndError);
        break;
    }

    if (item.useSfx) playNamedSfx(item.useSfx);
}

static void clearWorldSystems()
{
    for (int i = 0; i < 16; i++) enemies[i] = Enemy();
    enemyCount = 0;
    for (int i = 0; i < MAX_PICKUPS; i++) pickups[i].active = false;
    for (int i = 0; i < 32; i++) poofs[i].active = false;
    for (int i = 0; i < MAX_PARTICLES; i++) particles[i].active = false;
    for (int i = 0; i < 8; i++) projectiles[i].active = false;
    boomer.active = false;
    hook.active = false;
    hook.retracting = false;
    hook.latched = false;
    for (int i = 0; i < MAX_OBJECTS; i++) objects[i].active = false;
    carriedIndex = -1;
    liftingIndex = -1;
    grabbedIndex = -1;
    for (int i = 0; i < MAX_BURIED; i++) buriedDug[i] = false;
    for (int i = 0; i < MAX_BOMBS_LIVE; i++) bombs[i].active = false;
    for (int i = 0; i < MAX_EXPLOSIONS; i++) explosions[i].active = false;
    for (int i = 0; i < MAX_NPCS; i++) npcs[i].active = false;
    // Cutscene stragglers never survive a room change: pans release, a
    // blocking glide stops mid-stride, the bars ease back out.
    cutsceneCam.active = false;
    cutsceneCam.panning = false;
    cutsceneMove.active = false;
    letterboxWanted = 0;
}

static bool encounterAvailable(const Encounter& encounter)
{
    // Story-flag conditions: the trigger only fires while they hold, and
    // a failed condition never consumes the one-shot.
    if (encounter.ifFlag >= 0 && encounter.ifFlag < WORLD_FLAG_COUNT
        && !gWorldFlags[encounter.ifFlag])
        return false;
    if (encounter.unlessFlag >= 0 && encounter.unlessFlag < WORLD_FLAG_COUNT
        && gWorldFlags[encounter.unlessFlag])
        return false;
    if (encounter.repeat) return true;
    if (encounter.id < 0 || encounter.id >= ENCOUNTER_TOTAL) return true;
    return !encounterFired[encounter.id];
}

static void fireEncounter(const Encounter& encounter)
{
    if (!encounter.repeat && encounter.id >= 0 && encounter.id < ENCOUNTER_TOTAL)
        encounterFired[encounter.id] = true;
}

static void startMusic(const char* file)
{
    char path[600];
    if (file && file[0])
    {
        snprintf(path, sizeof(path), "assets/music/%s", file);
    }
    else
    {
        path[0] = '\0';
    }

    if (path[0] == '\0')
    {
        if (musicLoaded)
        {
            StopMusicStream(music);
            UnloadMusicStream(music);
            musicLoaded = false;
        }
        musicFile[0] = '\0';
        return;
    }

    if (musicLoaded && strcmp(musicFile, path) == 0) return;

    if (musicLoaded)
    {
        StopMusicStream(music);
        UnloadMusicStream(music);
    }

    music = LoadMusicStream(path);
    music.looping = true;
    strcpy(musicFile, path);
    musicLoaded = true;
    PlayMusicStream(music);
}

// Danger layer (see the globals up top): loaded per room like the bgm
// (same file keeps playing), volume driven by the fight state.
static void startDangerMusic(const char* file)
{
    char path[600];
    if (file && file[0])
    {
        snprintf(path, sizeof(path), "assets/music/%s", file);
    }
    else
    {
        path[0] = '\0';
    }

    if (path[0] == '\0')
    {
        if (dangerLoaded)
        {
            StopMusicStream(dangerMusic);
            UnloadMusicStream(dangerMusic);
            dangerLoaded = false;
        }
        dangerFile[0] = '\0';
        dangerMix = 0.0f;
        if (musicLoaded) SetMusicVolume(music, 1.0f);
        return;
    }

    if (dangerLoaded && strcmp(dangerFile, path) == 0) return;

    if (dangerLoaded)
    {
        StopMusicStream(dangerMusic);
        UnloadMusicStream(dangerMusic);
        dangerLoaded = false;
    }

    dangerMusic = LoadMusicStream(path);
    dangerMusic.looping = true;
    strcpy(dangerFile, path);
    dangerLoaded = true;
    SetMusicVolume(dangerMusic, 0.0f);
    PlayMusicStream(dangerMusic);
}

// Room ambience (birds, rain): a second, quieter music stream that loops
// under the bgm. Same no-restart convention as the music.
static Music ambientMusic = {};
static bool ambientLoaded = false;
static char ambientFile[600] = "";

static void startAmbient(const char* file)
{
    char path[600];
    if (file && file[0])
    {
        snprintf(path, sizeof(path), "assets/audio/%s", file);
    }
    else
    {
        path[0] = '\0';
    }

    if (path[0] == '\0')
    {
        if (ambientLoaded)
        {
            StopMusicStream(ambientMusic);
            UnloadMusicStream(ambientMusic);
            ambientLoaded = false;
        }
        ambientFile[0] = '\0';
        return;
    }

    if (ambientLoaded && strcmp(ambientFile, path) == 0) return;

    if (ambientLoaded)
    {
        StopMusicStream(ambientMusic);
        UnloadMusicStream(ambientMusic);
        ambientLoaded = false;
    }

    ambientMusic = LoadMusicStream(path);
    ambientMusic.looping = true;
    strcpy(ambientFile, path);
    ambientLoaded = true;
    SetMusicVolume(ambientMusic, 0.55f);
    PlayMusicStream(ambientMusic);
}

static Texture2D heartsTexture = {};
static bool paused = false;

// Room-edge transition: walking off a map edge glides the hero into the
// linked room (camera follows, world frozen) — the LttP scroll-in.
static const float ROOM_TRANSITION_DURATION = 0.42f;
static const float ROOM_FADE_DURATION = 0.30f;
struct RoomTransition
{
    bool active;
    float t;
    float startX;
    float startY;
    float endX;
    float endY;
};
static RoomTransition roomTransition = {};
static float roomFade = 0.0f;

static void drawGameOver()
{
    if (!hero.dead()) return;
    float t = hero.stateElapsed();
    if (t < 1.0f) return;

    // Fade the world out behind the collapse...
    float fade = clampf((t - 1.0f) / 0.8f, 0.0f, 1.0f);
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(),
                  Color{ 0, 0, 0, (unsigned char)(int)(fade * 210.0f) });

    float scale = virtualScale();
    if (t > 1.8f)
    {
        const char* text = "GAME OVER";
        int size = (int)(16 * scale);
        int width = MeasureText(text, size);
        DrawText(text, (GetScreenWidth() - width) / 2,
                 (GetScreenHeight() - size) / 2 - (int)(10 * scale), size, RAYWHITE);
    }
    if (t > 2.2f && ((int)(t * 2.0f) % 2 == 0))
    {
        const char* text = "PRESS ACTION TO CONTINUE";
        int size = (int)(8 * scale);
        int width = MeasureText(text, size);
        DrawText(text, (GetScreenWidth() - width) / 2,
                 (GetScreenHeight()) / 2 + (int)(8 * scale), size, RAYWHITE);
    }
}

// Lantern upkeep: slow magic drain in dark rooms + auto-lighting torches
// the hero stands next to. "The lantern" is whatever item carries the
// passive light (data); a game can name it anything.
static void updateLantern(float dt)
{
    if (!room.config().dark) return;
    if (lightItemOwned() < 0) return;

    lanternDrain += dt;
    if (lanternDrain >= 4.0f)
    {
        lanternDrain = 0.0f;
        inventory.magic--;
        if (inventory.magic < 0) inventory.magic = 0;
    }

    if (inventory.magic > 0)
    {
        Vector2 heroCenter = hero.center();
        for (int i = 0; i < MAX_OBJECTS; i++)
        {
            WorldObject& torch = objects[i];
            if (!torch.active || !objInteracts(torch, OBJINT_TORCH)) continue;
            if (torch.state != OBJ_GROUND || torch.opened) continue;
            float dx = (torch.x + 16.0f) - heroCenter.x;
            float dy = (torch.y + 16.0f) - heroCenter.y;
            if (dx * dx + dy * dy > 44.0f * 44.0f) continue;
            torch.opened = true;
            PlaySound(sndFire);
        }
    }
}

// Dark rooms: build the lightmap (ambient gloom + additive radial lights
// from the hero's lantern and lit torches) and multiply it over the frame.
static void drawDarkness()
{
    if (!room.config().dark) return;

    int sw = GetScreenWidth();
    int sh = GetScreenHeight();

    // Recreate the render texture on resize.
    if (lightRT.texture.width != sw || lightRT.texture.height != sh)
    {
        if (lightRT.texture.id != 0) UnloadRenderTexture(lightRT);
        lightRT = LoadRenderTexture(sw, sh);
    }
    if (lightRT.texture.id == 0 || lightGradient.id == 0) return;

    float scale = virtualScale();
    float camLeft = camera.target.x - camera.offset.x / camera.zoom;
    float camTop = camera.target.y - camera.offset.y / camera.zoom;

    BeginTextureMode(lightRT);
    ClearBackground(Color{ 46, 46, 66, 255 });  // ambient gloom

    BeginBlendMode(BLEND_ADDITIVE);
    Rectangle src = { 0.0f, 0.0f, (float)lightGradient.width, (float)lightGradient.height };
    Vector2 origin = { lightGradient.width * 0.5f, lightGradient.height * 0.5f };

    // Hero light: wide with the passive light item, a dying ember without.
    int lightItem = lightItemOwned();
    bool hasLantern = lightItem >= 0;
    float heroRadius = hasLantern ? (float)ITEMS[lightItem].lightRadius : 30.0f;
    Color heroTint = hasLantern ? Color{ 255, 255, 255, 255 }
                                : Color{ 140, 140, 150, 255 };
    if (hasLantern && inventory.magic <= 0) heroRadius = 42.0f;
    Vector2 heroCenter = hero.center();
    Rectangle dest = { (heroCenter.x - camLeft) * scale,
                       (heroCenter.y - camTop) * scale,
                       heroRadius * 2.0f * scale, heroRadius * 2.0f * scale };
    DrawTexturePro(lightGradient, src, dest, origin, 0.0f, heroTint);

    // Lit torches: warm, flickering pools of light.
    for (int i = 0; i < MAX_OBJECTS; i++)
    {
        const WorldObject& o = objects[i];
        if (!o.active || !objInteracts(o, OBJINT_TORCH) || !o.opened) continue;
        float flicker = 1.0f + sinf((float)GetTime() * 17.0f + o.x) * 0.06f;
        float radius = 140.0f * flicker;
        Rectangle tdest = { (o.x + 16.0f - camLeft) * scale,
                            (o.y + 10.0f - camTop) * scale,
                            radius * scale, radius * scale };
        DrawTexturePro(lightGradient, src, tdest, origin, 0.0f,
                       Color{ 255, 235, 205, 255 });
    }
    EndBlendMode();
    EndTextureMode();

    // Multiply the lightmap over the frame (dark areas dim, lights keep).
    BeginBlendMode(BLEND_MULTIPLIED);
    Rectangle rtSrc = { 0.0f, 0.0f, (float)lightRT.texture.width,
                        -(float)lightRT.texture.height };
    Rectangle rtDest = { 0.0f, 0.0f, (float)sw, (float)sh };
    Vector2 zero = { 0.0f, 0.0f };
    DrawTexturePro(lightRT.texture, rtSrc, rtDest, zero, 0.0f, WHITE);
    EndBlendMode();
}

// Weather (room "weather", engine vocabulary): pure presentation over
// the world in screen space — rain streaks, storm lightning, fog bands.
// The seeded Rng drives the particle seeding and lightning cadence so
// weather stays deterministic like every other world roll.
static const int RAIN_DROPS = 90;
struct RainDrop
{
    float x;
    float y;
    float speed;
};
static RainDrop rain[RAIN_DROPS] = {};
static bool rainSeeded = false;
static float lightningTimer = 8.0f;
static LazySound sndThunder = { "thunder.wav" };

static void updateWeather(float dt)
{
    int weather = room.config().weather;
    if (weather == WEATHER_NONE) return;

    int sw = GetScreenWidth();
    int sh = GetScreenHeight();

    if (!rainSeeded)
    {
        rainSeeded = true;
        for (int i = 0; i < RAIN_DROPS; i++)
        {
            rain[i].x = worldRng.nextFloat() * (float)sw;
            rain[i].y = worldRng.nextFloat() * (float)sh;
            rain[i].speed = 380.0f + worldRng.nextFloat() * 190.0f;
        }
    }

    if (weather == WEATHER_RAIN || weather == WEATHER_STORM)
    {
        for (int i = 0; i < RAIN_DROPS; i++)
        {
            rain[i].y += rain[i].speed * dt;
            rain[i].x -= 46.0f * dt;  // slight wind slant
            if (rain[i].y > (float)sh || rain[i].x < -12.0f)
            {
                rain[i].y = -12.0f;
                rain[i].x = worldRng.nextFloat() * ((float)sw + 40.0f);
            }
        }
    }

    if (weather == WEATHER_STORM)
    {
        lightningTimer -= dt;
        if (lightningTimer <= 0.0f)
        {
            lightningTimer = 6.0f + worldRng.nextFloat() * 8.0f;
            screenFlash(0.26f, Color{ 240, 240, 255, 255 }, 0.55f);
            PlaySound(sndThunder);
        }
    }
}

static void drawWeather()
{
    int weather = room.config().weather;
    if (weather == WEATHER_NONE) return;

    int sw = GetScreenWidth();
    int sh = GetScreenHeight();

    if (weather == WEATHER_RAIN || weather == WEATHER_STORM)
    {
        for (int i = 0; i < RAIN_DROPS; i++)
        {
            DrawLine((int)rain[i].x, (int)rain[i].y,
                     (int)(rain[i].x + 2.6f), (int)(rain[i].y + 8.5f),
                     Color{ 150, 175, 215, 130 });
        }
    }

    if (weather == WEATHER_FOG || weather == WEATHER_STORM)
    {
        // Drifting fog bands (storm fog runs under the rain for weight).
        float t = (float)GetTime();
        for (int band = 0; band < 3; band++)
        {
            float y = (float)(sh / 4) + (float)band * (sh / 4.0f)
                      + sinf(t * (0.13f + band * 0.05f) + band * 2.1f) * 18.0f;
            unsigned char alpha = (unsigned char)(36 - band * 8);
            DrawRectangle(0, (int)y, sw, sh / 8 + 14, Color{ 200, 208, 222, alpha });
        }
    }
}

// Ambient tint (room "tint" / "night"): a color multiply over the world
// (under the HUD), composing with dark rooms the way the lightmap does.
static void drawTint()
{
    const RoomConfig& cfg = room.config();
    if (cfg.tintR >= 255 && cfg.tintG >= 255 && cfg.tintB >= 255) return;
    BeginBlendMode(BLEND_MULTIPLIED);
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(),
                  Color{ (unsigned char)cfg.tintR, (unsigned char)cfg.tintG,
                         (unsigned char)cfg.tintB, 255 });
    EndBlendMode();
}

static void drawHud()
{
    // Hearts row in virtual pixels over the world: 16x16 each, 1px gap.
    float scale = virtualScale();
    bool low = hero.healthHalves() <= 2 && hero.healthHalves() > 0;
    bool heartsBlink = low && ((int)(lowHpBeepTimer * 2.4f) % 2 == 1);
    int maxHearts = hero.maxHealthHalves() / 2;
    for (int i = 0; i < maxHearts; i++)
    {
        if (heartsBlink) continue;
        int filled = hero.healthHalves() - i * 2;
        int frame = filled >= 2 ? 0 : (filled == 1 ? 1 : 2);  // full / half / empty
        Rectangle src = { (float)(frame * 16), 0.0f, 16.0f, 16.0f };
        Rectangle dest = { (6.0f + (float)i * 17.0f) * scale, 6.0f * scale,
                           16.0f * scale, 16.0f * scale };
        Vector2 origin = { 0.0f, 0.0f };
        DrawTexturePro(heartsTexture, src, dest, origin, 0.0f, WHITE);
    }

    // Rupee counter, top-right: rupee sprite + count.
    if (inventory.rupees > 0)
    {
        float rx = (float)GetScreenWidth() - 44.0f * scale;
        Rectangle src = { 16.0f, 0.0f, 16.0f, 16.0f };
        Rectangle dest = { rx, 6.0f * scale, 16.0f * scale, 16.0f * scale };
        Vector2 origin = { 0.0f, 0.0f };
        DrawTexturePro(pickupsTexture, src, dest, origin, 0.0f, WHITE);
        char text[16];
        snprintf(text, sizeof(text), "x%d", inventory.rupees);
        DrawText(text, (int)(rx + 18.0f * scale), (int)(10.0f * scale),
                 (int)(10 * scale), RAYWHITE);
    }

    // Equipped item + its quantity (arrows/bombs), under the rupee counter.
    if (inventory.equippedItem >= 0 && inventory.equippedItem < ITEM_COUNT)
    {
        const ItemConfig& item = ITEMS[inventory.equippedItem];
        const char* label = item.label;
        char text[24];
        int count = -1;
        if (item.arrowCost > 0) count = inventory.arrows;
        else if (item.bombCost > 0) count = inventory.bombs;
        if (count >= 0) snprintf(text, sizeof(text), "%s x%d", label, count);
        else snprintf(text, sizeof(text), "%s", label);
        DrawText(text, (int)((float)GetScreenWidth() - 6.0f * scale - MeasureText(text, (int)(8 * scale))),
                 (int)(26.0f * scale), (int)(8 * scale), RAYWHITE);
    }

    // Key counts (small + big), under the equipped item.
    if (inventory.smallKeys > 0 || inventory.bigKeys > 0)
    {
        char text[24];
        snprintf(text, sizeof(text), "KEY x%d%s", inventory.smallKeys,
                 inventory.bigKeys > 0 ? "  BIG x1" : "");
        DrawText(text, (int)((float)GetScreenWidth() - 6.0f * scale - MeasureText(text, (int)(8 * scale))),
                 (int)(38.0f * scale), (int)(8 * scale), Color{ 250, 220, 120, 255 });
    }

    // Arena boss (and mid-fight mini-boss) health bar, bottom-center while
    // the fight rages.
    for (int i = 0; i < enemyCount; i++)
    {
        const Enemy& boss = enemies[i];
        if (!boss.alive() || !boss.isArenaLock()) continue;

        float barW = 120.0f * scale;
        float barH = 6.0f * scale;
        float bx = ((float)GetScreenWidth() - barW) * 0.5f;
        float by = (VIRTUAL_HEIGHT - 26.0f) * scale;
        DrawRectangle((int)bx - 2, (int)by - 2, (int)barW + 4, (int)barH + 4,
                      Color{ 20, 20, 40, 220 });
        DrawRectangle((int)bx, (int)by, (int)barW, (int)barH,
                      Color{ 70, 20, 26, 255 });
        if (boss.maxHealth() > 0)
        {
            float fill = (float)boss.health() / (float)boss.maxHealth();
            DrawRectangle((int)bx, (int)by, (int)(barW * fill), (int)barH,
                          Color{ 216, 40, 48, 255 });
        }
        const char* label = "BOSS";
        int labelSize = (int)(8 * scale);
        int labelW = MeasureText(label, labelSize);
        DrawText(label, ((int)bx + (int)barW / 2 - labelW / 2), (int)(by - 11.0f * scale),
                 labelSize, RAYWHITE);
        break;
    }

    // Magic gauge, bottom-left: thin vertical bar, LttP style.
    if (inventory.magicMax > 0)
    {
        float barH = 40.0f;
        float barW = 5.0f;
        float bx = 8.0f * scale;
        float by = (VIRTUAL_HEIGHT - 8.0f - barH) * scale;
        DrawRectangle((int)bx, (int)by, (int)(barW * scale), (int)(barH * scale),
                      Color{ 30, 30, 50, 200 });
        float fill = (float)inventory.magic / (float)inventory.magicMax;
        float fillH = barH * fill;
        DrawRectangle((int)bx, (int)(by + (barH - fillH) * scale), (int)(barW * scale),
                      (int)(fillH * scale), Color{ 90, 150, 255, 255 });
        DrawText("M", (int)(bx + barW * scale), (int)(by + barH * scale - 8.0f * scale),
                 (int)(8 * scale), Color{ 120, 170, 255, 255 });
    }
}

// Item subscreen (the pause menu): grid cursor over the item list; the
// action button equips the highlighted item to the item button.
static void mapLayout(float* xs, float* ys, int roomCount);

static int pauseCursor = 0;
static int pauseTab = 0;  // 0 = items, 1 = map
static Texture2D uiBorderTexture = {};

// Generic toasts: small top-center banners (autosave cues, hints).
static const float TOAST_LIFE = 2.0f;
struct Toast
{
    char text[64];
    float age;
};
static Toast toasts[3] = {};

static void showToast(const char* text)
{
    for (int i = 0; i < 3; i++)
    {
        if (toasts[i].text[0] != '\0' && toasts[i].age < TOAST_LIFE) continue;
        snprintf(toasts[i].text, sizeof(toasts[i].text), "%s", text);
        toasts[i].age = 0.0f;
        return;
    }
}

static void drawToasts()
{
    float scale = virtualScale();
    for (int i = 0; i < 3; i++)
    {
        if (toasts[i].text[0] == '\0') continue;
        toasts[i].age += frameDt();
        if (toasts[i].age >= TOAST_LIFE)
        {
            toasts[i].text[0] = '\0';
            continue;
        }
        float t = toasts[i].age / TOAST_LIFE;
        // Quick fade in, hold, quick fade out.
        float alpha = (t < 0.15f) ? t / 0.15f : (t > 0.75f ? (1.0f - t) / 0.25f : 1.0f);
        if (alpha < 0.0f) alpha = 0.0f;
        int size = (int)(10 * scale);
        int width = MeasureText(toasts[i].text, size);
        float y = (34.0f + (float)i * 14.0f) * scale;
        DrawRectangle((GetScreenWidth() - width) / 2 - 6 * (int)scale / 2, (int)y - 2,
                      width + 6 * (int)scale, size + 4,
                      Color{ 0, 0, 0, (unsigned char)(int)(140 * alpha) });
        DrawText(toasts[i].text, (GetScreenWidth() - width) / 2, (int)y, size,
                 Color{ 250, 250, 220, (unsigned char)(int)(255 * alpha) });
    }
}

// Flute (use-kind "flute"): the bird-summon fast travel. The item button
// opens a destination picker over the visited-room map; picking a room
// flies the hero there on the bird (fade-in at the room's spawn tile,
// banked like every warp).
static void updateFluteMenu()
{
    static float mapX[MAX_ROOM_MARKS];
    static float mapY[MAX_ROOM_MARKS];
    int roomTotal = ROOM_COUNT < MAX_ROOM_MARKS ? ROOM_COUNT : MAX_ROOM_MARKS;
    mapLayout(mapX, mapY, roomTotal);

    // The cursor always rests on a visited room (the current one to start).
    if (fluteCursor < 0 || fluteCursor >= roomTotal || !roomVisited[fluteCursor])
        fluteCursor = currentRoom;

    // Anything but arrows/action cancels.
    if (swordPressed() || itemPressed() || dashPressed())
    {
        fluteMenu = false;
        return;
    }

    // Directional cursor: hop to the nearest visited room along the press.
    bool pressLeft = IsKeyPressed(KEY_LEFT);
    bool pressRight = IsKeyPressed(KEY_RIGHT);
    bool pressUp = IsKeyPressed(KEY_UP);
    bool pressDown = IsKeyPressed(KEY_DOWN);
    for (int i = 0; i < 4; i++)
    {
        if (!IsGamepadAvailable(i)) continue;
        if (IsGamepadButtonPressed(i, GAMEPAD_BUTTON_LEFT_FACE_LEFT)) pressLeft = true;
        if (IsGamepadButtonPressed(i, GAMEPAD_BUTTON_LEFT_FACE_RIGHT)) pressRight = true;
        if (IsGamepadButtonPressed(i, GAMEPAD_BUTTON_LEFT_FACE_UP)) pressUp = true;
        if (IsGamepadButtonPressed(i, GAMEPAD_BUTTON_LEFT_FACE_DOWN)) pressDown = true;
    }
    float dirX = 0.0f;
    float dirY = 0.0f;
    if (pressLeft) dirX -= 1.0f;
    if (pressRight) dirX += 1.0f;
    if (pressUp) dirY -= 1.0f;
    if (pressDown) dirY += 1.0f;

    if (dirX != 0.0f || dirY != 0.0f)
    {
        float len = sqrtf(dirX * dirX + dirY * dirY);
        dirX /= len;
        dirY /= len;

        int bestRoom = -1;
        float bestScore = 1e9f;
        for (int i = 0; i < roomTotal; i++)
        {
            if (!roomVisited[i] || i == fluteCursor) continue;
            float dx = mapX[i] - mapX[fluteCursor];
            float dy = mapY[i] - mapY[fluteCursor];
            float along = dx * dirX + dy * dirY;  // progress along the press
            if (along <= 0.0f) continue;          // behind (or dead on) the cursor
            float offX = dx - dirX * along;       // perpendicular offset
            float offY = dy - dirY * along;
            float score = along + (fabsf(offX) + fabsf(offY)) * 2.0f;
            if (score < bestScore)
            {
                bestScore = score;
                bestRoom = i;
            }
        }
        if (bestRoom >= 0)
        {
            fluteCursor = bestRoom;
            PlaySound(sndUiCursor);
        }
    }

    // Action: the bird takes off for the cursor room (the hero's own
    // room just folds the menu; nothing to fly to).
    if (actionPressed())
    {
        if (fluteCursor == currentRoom) fluteMenu = false;
        else fluteWarp();
    }
}

static void drawFluteMenu()
{
    if (!fluteMenu) return;

    float scale = virtualScale();
    float viewWidth = (float)GetScreenWidth() / scale;
    float viewHeight = VIRTUAL_HEIGHT;

    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{ 0, 0, 0, 170 });

    // Panel, same nine-patch chrome as the pause subscreen.
    float panelW = 200.0f;
    float panelH = 108.0f;
    Rectangle panel = { (viewWidth - panelW) * 0.5f, (viewHeight - panelH) * 0.5f,
                        panelW, panelH };
    if (uiBorderTexture.id != 0)
    {
        int patch = uiBorderTexture.width / 3;
        if (patch < 1) patch = 1;
        NPatchInfo npatch;
        npatch.source = Rectangle{ 0.0f, 0.0f,
                                   (float)uiBorderTexture.width, (float)uiBorderTexture.height };
        npatch.left = patch;
        npatch.top = patch;
        npatch.right = patch;
        npatch.bottom = patch;
        npatch.layout = NPATCH_NINE_PATCH;
        Rectangle dest = { panel.x * scale, panel.y * scale,
                           panel.width * scale, panel.height * scale };
        Vector2 origin = { 0.0f, 0.0f };
        DrawTextureNPatch(uiBorderTexture, npatch, dest, origin, 0.0f, WHITE);
    }
    else
    {
        DrawRectangle((int)(panel.x * scale), (int)(panel.y * scale),
                      (int)(panel.width * scale), (int)(panel.height * scale), Color{ 16, 24, 96, 255 });
    }

    int titleSize = (int)(12 * scale);
    DrawText("FLUTE", (int)((panel.x + 8.0f) * scale), (int)((panel.y + 6.0f) * scale),
             titleSize, RAYWHITE);
    {
        const char* hint = "ACTION:FLY  SWORD:BACK";
        DrawText(hint, (int)((panel.x + panel.width - 8.0f) * scale)
                     - MeasureText(hint, (int)(8 * scale)),
                 (int)((panel.y + 8.0f) * scale), (int)(8 * scale),
                 Color{ 170, 170, 190, 255 });
    }

    // The visited-room map, same geometry as the pause map tab.
    static float mapX[MAX_ROOM_MARKS];
    static float mapY[MAX_ROOM_MARKS];
    int roomTotal = ROOM_COUNT < MAX_ROOM_MARKS ? ROOM_COUNT : MAX_ROOM_MARKS;
    mapLayout(mapX, mapY, roomTotal);

    float cellW = 18.0f;
    float cellH = 14.0f;
    float pitchX = 26.0f;
    float pitchY = 22.0f;

    // Center the visited bounds in the panel.
    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
    for (int i = 0; i < roomTotal; i++)
    {
        if (i < MAX_ROOM_MARKS && !roomVisited[i]) continue;
        if (mapX[i] < minX) minX = mapX[i];
        if (mapX[i] > maxX) maxX = mapX[i];
        if (mapY[i] < minY) minY = mapY[i];
        if (mapY[i] > maxY) maxY = mapY[i];
    }
    float originX = (minX + maxX) * 0.5f;
    float originY = (minY + maxY) * 0.5f;
    float baseX = panel.x + panel.width * 0.5f - originX * pitchX - cellW * 0.5f;
    float baseY = panel.y + panel.height * 0.5f - originY * pitchY - cellH * 0.5f;

    // Connections between visited rooms first (lines under boxes).
    for (int i = 0; i < roomTotal; i++)
    {
        if (!roomVisited[i]) continue;
        const RoomConfig& cfg = ROOMS[i];
        const int edges[4] = { cfg.exitNorth, cfg.exitSouth, cfg.exitEast, cfg.exitWest };
        for (int e = 0; e < 4; e++)
        {
            int target = edges[e];
            if (target < 0 || target >= roomTotal || !roomVisited[target]) continue;
            if (target < i) continue;  // draw each link once
            int x0 = (int)((baseX + mapX[i] * pitchX + cellW * 0.5f) * scale);
            int y0 = (int)((baseY + mapY[i] * pitchY + cellH * 0.5f) * scale);
            int x1 = (int)((baseX + mapX[target] * pitchX + cellW * 0.5f) * scale);
            int y1 = (int)((baseY + mapY[target] * pitchY + cellH * 0.5f) * scale);
            DrawLine(x0, y0, x1, y1, Color{ 120, 130, 170, 255 });
        }
    }

    // Visited room boxes; the cursor room wears the bird.
    for (int i = 0; i < roomTotal; i++)
    {
        if (!roomVisited[i]) continue;
        Rectangle cell = { (baseX + mapX[i] * pitchX) * scale,
                           (baseY + mapY[i] * pitchY) * scale,
                           cellW * scale, cellH * scale };
        bool current = (i == currentRoom);
        DrawRectangleRec(cell, current ? Color{ 90, 120, 200, 255 }
                                       : Color{ 40, 50, 90, 255 });
        DrawRectangleLinesEx(cell, 1.0f, current ? RAYWHITE : Color{ 120, 130, 170, 255 });
        if (current)
        {
            DrawCircle((int)(cell.x + cell.width * 0.5f),
                       (int)(cell.y + cell.height * 0.5f), 2.5f * scale, RAYWHITE);
        }

        if (i == fluteCursor)
        {
            // The bird: a flapping silhouette perched over the target.
            float flap = sinf((float)GetTime() * 12.0f);
            Vector2 bird = { cell.x + cell.width * 0.5f,
                             cell.y + cell.height * 0.5f - 6.0f * scale };
            Vector2 wingL = { bird.x - 7.0f * scale,
                              bird.y - 1.0f * scale - flap * 3.0f * scale };
            Vector2 wingR = { bird.x + 7.0f * scale,
                              bird.y - 1.0f * scale - flap * 3.0f * scale };
            DrawCircle((int)bird.x, (int)bird.y, (int)(2.0f * scale), RAYWHITE);
            DrawLineEx(bird, wingL, 2.0f * scale, RAYWHITE);
            DrawLineEx(bird, wingR, 2.0f * scale, RAYWHITE);
        }
    }
}

static void updatePauseMenu()
{
    // Save-and-quit works from either tab.
    if (dashPressed())
    {
        paused = false;
        quitToTitle();
        return;
    }

    // The sword button flips between the items grid and the map.
    if (swordPressed())
    {
        pauseTab = 1 - pauseTab;
        PlaySound(sndUiCursor);
    }

    if (pauseTab == 1) return;  // the map tab is view-only

    int count = ITEM_COUNT;
    if (count <= 0) return;

    if (pauseCursor < 0 || pauseCursor >= count) pauseCursor = 0;

    bool prevPressed = IsKeyPressed(KEY_LEFT);
    bool nextPressed = IsKeyPressed(KEY_RIGHT);
    for (int i = 0; i < 4; i++)
    {
        if (!IsGamepadAvailable(i)) continue;
        if (IsGamepadButtonPressed(i, GAMEPAD_BUTTON_LEFT_FACE_LEFT)
            || IsGamepadButtonPressed(i, GAMEPAD_BUTTON_LEFT_FACE_UP)) prevPressed = true;
        if (IsGamepadButtonPressed(i, GAMEPAD_BUTTON_LEFT_FACE_RIGHT)
            || IsGamepadButtonPressed(i, GAMEPAD_BUTTON_LEFT_FACE_DOWN)) nextPressed = true;
    }

    if (prevPressed)
    {
        pauseCursor = (pauseCursor + count - 1) % count;
        PlaySound(sndUiCursor);
    }
    if (nextPressed)
    {
        pauseCursor = (pauseCursor + 1) % count;
        PlaySound(sndUiCursor);
    }

    if (actionPressed())
    {
        inventory.equippedItem = pauseCursor;
        PlaySound(sndUiButton);
    }
}

static void drawPauseOverlay()
{
    if (!paused) return;

    float scale = virtualScale();
    float viewWidth = (float)GetScreenWidth() / scale;
    float viewHeight = VIRTUAL_HEIGHT;

    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{ 0, 0, 0, 170 });

    // Panel.
    float panelW = 200.0f;
    float panelH = 108.0f;
    Rectangle panel = { (viewWidth - panelW) * 0.5f, (viewHeight - panelH) * 0.5f,
                        panelW, panelH };
    if (uiBorderTexture.id != 0)
    {
        int patch = uiBorderTexture.width / 3;
        if (patch < 1) patch = 1;
        NPatchInfo npatch;
        npatch.source = Rectangle{ 0.0f, 0.0f,
                                   (float)uiBorderTexture.width, (float)uiBorderTexture.height };
        npatch.left = patch;
        npatch.top = patch;
        npatch.right = patch;
        npatch.bottom = patch;
        npatch.layout = NPATCH_NINE_PATCH;
        Rectangle dest = { panel.x * scale, panel.y * scale,
                           panel.width * scale, panel.height * scale };
        Vector2 origin = { 0.0f, 0.0f };
        DrawTextureNPatch(uiBorderTexture, npatch, dest, origin, 0.0f, WHITE);
    }
    else
    {
        DrawRectangle((int)(panel.x * scale), (int)(panel.y * scale),
                      (int)(panel.width * scale), (int)(panel.height * scale), Color{ 16, 24, 96, 255 });
    }

    int size = (int)(10 * scale);
    int titleSize = (int)(12 * scale);
    DrawText(pauseTab == 0 ? "ITEMS" : "MAP",
             (int)((panel.x + 8.0f) * scale), (int)((panel.y + 6.0f) * scale),
             titleSize, RAYWHITE);
    {
        // Tab switcher hint, top-right of the panel.
        const char* tabs = "SWORD: SWITCH";
        DrawText(tabs, (int)((panel.x + panel.width - 8.0f) * scale)
                     - MeasureText(tabs, (int)(8 * scale)),
                 (int)((panel.y + 8.0f) * scale), (int)(8 * scale),
                 Color{ 170, 170, 190, 255 });
    }

    if (pauseTab == 1)
    {
        // Map tab: visited rooms laid out by the exits graph.
        static float mapX[MAX_ROOM_MARKS];
        static float mapY[MAX_ROOM_MARKS];
        int roomTotal = ROOM_COUNT < MAX_ROOM_MARKS ? ROOM_COUNT : MAX_ROOM_MARKS;
        mapLayout(mapX, mapY, roomTotal);

        float cellW = 18.0f;
        float cellH = 14.0f;
        float pitchX = 26.0f;
        float pitchY = 22.0f;

        // Center the visited bounds in the panel.
        float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
        for (int i = 0; i < roomTotal; i++)
        {
            if (i < MAX_ROOM_MARKS && !roomVisited[i]) continue;
            if (mapX[i] < minX) minX = mapX[i];
            if (mapX[i] > maxX) maxX = mapX[i];
            if (mapY[i] < minY) minY = mapY[i];
            if (mapY[i] > maxY) maxY = mapY[i];
        }
        float originX = (minX + maxX) * 0.5f;
        float originY = (minY + maxY) * 0.5f;
        float baseX = panel.x + panel.width * 0.5f - originX * pitchX - cellW * 0.5f;
        float baseY = panel.y + panel.height * 0.5f - originY * pitchY - cellH * 0.5f;

        // Connections between visited rooms first (lines under boxes).
        for (int i = 0; i < roomTotal; i++)
        {
            if (!roomVisited[i]) continue;
            const RoomConfig& cfg = ROOMS[i];
            const int edges[4] = { cfg.exitNorth, cfg.exitSouth, cfg.exitEast, cfg.exitWest };
            for (int e = 0; e < 4; e++)
            {
                int target = edges[e];
                if (target < 0 || target >= roomTotal || !roomVisited[target]) continue;
                if (target < i) continue;  // draw each link once
                int x0 = (int)((baseX + mapX[i] * pitchX + cellW * 0.5f) * scale);
                int y0 = (int)((baseY + mapY[i] * pitchY + cellH * 0.5f) * scale);
                int x1 = (int)((baseX + mapX[target] * pitchX + cellW * 0.5f) * scale);
                int y1 = (int)((baseY + mapY[target] * pitchY + cellH * 0.5f) * scale);
                DrawLine(x0, y0, x1, y1, Color{ 120, 130, 170, 255 });
            }
        }

        // Visited room boxes; the current room glows with a hero dot.
        for (int i = 0; i < roomTotal; i++)
        {
            if (!roomVisited[i]) continue;
            Rectangle cell = { (baseX + mapX[i] * pitchX) * scale,
                               (baseY + mapY[i] * pitchY) * scale,
                               cellW * scale, cellH * scale };
            bool current = (i == currentRoom);
            DrawRectangleRec(cell, current ? Color{ 90, 120, 200, 255 }
                                           : Color{ 40, 50, 90, 255 });
            DrawRectangleLinesEx(cell, 1.0f, current ? RAYWHITE : Color{ 120, 130, 170, 255 });
            if (current)
            {
                DrawCircle((int)(cell.x + cell.width * 0.5f),
                           (int)(cell.y + cell.height * 0.5f), 2.5f * scale, RAYWHITE);
            }

            // Compass (as data): with the passive owned, the map marks
            // chest rooms (gold) and boss rooms (red).
            if (compassItemOwned() && (ROOMS[i].hasChest || ROOMS[i].hasBoss))
            {
                if (ROOMS[i].hasChest)
                    DrawRectangle((int)(cell.x + cell.width * 0.18f),
                                  (int)(cell.y + cell.height * 0.5f - 2.0f * scale),
                                  (int)(4.0f * scale), (int)(4.0f * scale),
                                  Color{ 235, 200, 70, 255 });
                if (ROOMS[i].hasBoss)
                    DrawCircle((int)(cell.x + cell.width * 0.72f),
                               (int)(cell.y + cell.height * 0.55f),
                               2.5f * scale, Color{ 225, 80, 80, 255 });
            }
        }
        return;
    }

    // Item grid (one row of up to three cells; a fourth wraps).
    float cellW = 56.0f;
    float cellH = 18.0f;
    for (int i = 0; i < ITEM_COUNT; i++)
    {
        float cx = panel.x + 8.0f + (float)(i % 3) * cellW;
        float cy = panel.y + 26.0f + (float)(i / 3) * cellH;
        const char* label = inventory.owned[i] ? ITEMS[i].label : "-----";
        DrawText(label, (int)((cx + 12.0f) * scale), (int)(cy * scale), size,
                 inventory.owned[i] ? RAYWHITE : Color{ 130, 130, 150, 255 });
        if (i == pauseCursor)
        {
            // Cursor triangle pointing at the cell.
            float ax = (cx + 2.0f) * scale;
            float ay = (cy * scale) + size * 0.5f;
            Vector2 a = { ax, ay - 4.0f * scale };
            Vector2 b = { ax, ay + 4.0f * scale };
            Vector2 c = { ax + 6.0f * scale, ay };
            DrawTriangle(a, b, c, RAYWHITE);
        }
        if (i == inventory.equippedItem && inventory.equippedItem >= 0)
        {
            DrawText("E", (int)((cx + cellW - 12.0f) * scale), (int)(cy * scale),
                     size, Color{ 120, 220, 140, 255 });
        }
    }

    // Quantities column.
    float qx = panel.x + 8.0f + 3.0f * cellW + 6.0f;
    float qy = panel.y + 26.0f;
    char text[32];
    snprintf(text, sizeof(text), "RUPEES %d/%d", inventory.rupees, inventory.rupeeMax);
    DrawText(text, (int)(qx * scale), (int)(qy * scale), size, RAYWHITE);
    snprintf(text, sizeof(text), "ARROWS %d/%d", inventory.arrows, inventory.arrowsMax);
    DrawText(text, (int)(qx * scale), (int)((qy + 12.0f) * scale), size, RAYWHITE);
    snprintf(text, sizeof(text), "BOMBS  %d/%d", inventory.bombs, inventory.bombsMax);
    DrawText(text, (int)(qx * scale), (int)((qy + 24.0f) * scale), size, RAYWHITE);
    snprintf(text, sizeof(text), "KEYS   %d%s", inventory.smallKeys,
             inventory.bigKeys > 0 ? "  BIG!" : "");
    DrawText(text, (int)(qx * scale), (int)((qy + 34.0f) * scale), size, RAYWHITE);

    // Bottles: one letter per slot (W water, R/G/B potions, F fairy,
    // b bee), dots while the set is short.
    if (inventory.bottles > 0)
    {
        const char* letters[Inventory::BOTTLE_MAX] = {};
        for (int i = 0; i < Inventory::BOTTLE_MAX; i++)
        {
            switch (inventory.bottleContents[i])
            {
            case Inventory::BOTTLE_WATER: letters[i] = "W"; break;
            case Inventory::BOTTLE_POTION_RED: letters[i] = "R"; break;
            case Inventory::BOTTLE_POTION_GREEN: letters[i] = "G"; break;
            case Inventory::BOTTLE_POTION_BLUE: letters[i] = "B"; break;
            case Inventory::BOTTLE_FAIRY: letters[i] = "F"; break;
            case Inventory::BOTTLE_BEE: letters[i] = "b"; break;
            default: letters[i] = "-"; break;
            }
        }
        snprintf(text, sizeof(text), "BOTTLE %s%s%s%s",
                 letters[0], letters[1], letters[2], letters[3]);
        DrawText(text, (int)(qx * scale), (int)((qy + 46.0f) * scale), size, RAYWHITE);
    }

    // Magic gauge.
    DrawText("MAGIC", (int)(qx * scale), (int)((qy + 58.0f) * scale), size, RAYWHITE);
    float barX = (qx + 30.0f) * scale;
    float barY = (qy + 58.0f) * scale + size * 0.5f - 3.0f * scale;
    DrawRectangle((int)barX, (int)barY, (int)(40.0f * scale), (int)(6.0f * scale),
                  Color{ 40, 40, 60, 255 });
    if (inventory.magicMax > 0)
    {
        float fill = (float)inventory.magic / (float)inventory.magicMax;
        DrawRectangle((int)barX, (int)barY, (int)(40.0f * fill * scale), (int)(6.0f * scale),
                      Color{ 90, 150, 255, 255 });
    }

    // Hint line.
    DrawText("ACTION:EQUIP  DASH:SAVE+QUIT  START:CLOSE", (int)((panel.x + 8.0f) * scale),
             (int)((panel.y + panelH - 16.0f) * scale), (int)(8 * scale),
             Color{ 170, 170, 190, 255 });
}

// Entry placement: arriving from `enterMotion` (the direction the hero was
// moving when it left the previous room) puts it on the opposite edge, the
// perpendicular coordinate preserved from the old room, nudged to the
// nearest free (non-solid, non-hazard) tile.
static void placeHeroAtEntry(const RoomConfig& cfg, int enterMotion)
{
    int w = cfg.mapWidth;
    int h = cfg.mapHeight;
    Vector2 c = hero.center();

    int fixed, desired;
    bool horizontal;
    switch (enterMotion)
    {
    case DIR_UP:    fixed = h - 1; desired = (int)(c.x / TILE_SIZE); horizontal = false; break;
    case DIR_DOWN:  fixed = 0;     desired = (int)(c.x / TILE_SIZE); horizontal = false; break;
    case DIR_LEFT:  fixed = w - 1; desired = (int)(c.y / TILE_SIZE); horizontal = true;  break;
    case DIR_RIGHT: fixed = 0;     desired = (int)(c.y / TILE_SIZE); horizontal = true;  break;
    default:       hero.place(0, 0); return;
    }
    if (desired < 0) desired = 0;
    if (horizontal && desired > w - 1) desired = w - 1;
    if (!horizontal && desired > h - 1) desired = h - 1;

    int span = horizontal ? w : h;
    int tx = horizontal ? fixed : desired;
    int ty = horizontal ? desired : fixed;
    bool ok = false;
    for (int d = 0; d < span && !ok; d++)
    {
        int candidates[2] = { desired, desired };
        if (d > 0)
        {
            candidates[0] = desired - d;
            candidates[1] = desired + d;
        }
        for (int si = 0; si < 2 && !ok; si++)
        {
            int probe = candidates[si];
            if (probe < 0 || probe >= span) continue;
            int px = horizontal ? fixed : probe;
            int py = horizontal ? probe : fixed;
            if (cfg.colliders[py * w + px] != 0) continue;
            TileMeta meta = room.tileMetaAt(px, py);
            if (meta.flags & TILE_FLAG_PIT) continue;
            // Deep water is a fine landing with flippers (swim in).
            if (!swimItemOwned() && (meta.flags & TILE_FLAG_WATER_DEEP)) continue;
            ok = true;
            tx = px;
            ty = py;
        }
    }
    if (!ok)
    {
        // Whole edge blocked (data problem): land where intended.
        tx = horizontal ? fixed : desired;
        ty = horizontal ? desired : fixed;
    }
    hero.place(tx, ty);
}

static void loadRoom(int index, int enterMotion, int tileX, int tileY)
{
    assert(index >= 0 && index < ROOM_COUNT);  // callers resolve ids at bake

    // Dungeon rooms keep their fallen: snapshot which spawn slots are dead
    // before leaving, so re-entry skips them (session-lifetime; overworld
    // rooms respawn fresh). Slot i in the mask is spawn i in the baked
    // set — dead slots stay dead-empty so the identity holds.
    if (currentRoom >= 0 && currentRoom < ROOM_COUNT
        && ROOMS[currentRoom].dungeon && currentRoom < MAX_ROOM_MARKS)
    {
        unsigned short mask = 0;
        for (int i = 0; i < enemyCount && i < 16; i++)
            if (!enemies[i].alive()) mask |= (unsigned short)(1 << i);
        dungeonDead[currentRoom] = mask;
    }

    // Settled-object registry: moved/sunk blocks, latched switches, pulled
    // levers, lit torches persist across reloads and sessions — snapshot
    // the room being left, re-apply to the room being entered.
    snapshotRoomState(currentRoom);

    currentRoom = index;
    const RoomConfig& cfg = ROOMS[index];
    if (index >= 0 && index < MAX_ROOM_MARKS) roomVisited[index] = true;

    room.unload();
    room.load(cfg);

    // Blasted passages re-apply: bombed-open walls/floors stay open (the
    // registry is also persisted in the save, so they survive sessions).
    for (int i = 0; i < blastCount; i++)
    {
        if (blastRoom[i] != (short)index) continue;
        room.bombTileAt(blastX[i], blastY[i]);
    }

    if (cfg.hero >= 0 && cfg.hero < CHARACTER_COUNT)
        hero.load(CHARACTERS[cfg.hero]);

    // Pit-landing: this room's pit tiles drop into another room layer
    // (armed per room; the hero drains the redirect when a fall lands).
    hero.setFallRedirect(cfg.pitRoom, cfg.pitTileX, cfg.pitTileY);

    if (tileX >= 0 && tileY >= 0)
    {
        // Warp-in: land at the exact tile, fade in.
        hero.place(tileX, tileY);
        roomFade = ROOM_FADE_DURATION > 0.0f ? 1.0f : 0.0f;
        roomTransition.active = false;
    }
    else if (enterMotion >= 0)
    {
        placeHeroAtEntry(cfg, enterMotion);
        roomFade = 0.0f;  // scroll-in, no fade
    }
    else
    {
        int spawnX = cfg.hasSpawn ? cfg.spawnTileX : 0;
        int spawnY = cfg.hasSpawn ? cfg.spawnTileY : 0;
        hero.place(spawnX, spawnY);
        roomFade = ROOM_FADE_DURATION > 0.0f ? 1.0f : 0.0f;
    }

    // Scroll-in glide: hero starts one tile off-map along the entry edge
    // and walks in; the camera follows (unclamped) until it lands.
    if (enterMotion >= 0 && tileX < 0)
    {
        float dirX = (enterMotion == DIR_LEFT) ? -1.0f
                    : (enterMotion == DIR_RIGHT) ? 1.0f : 0.0f;
        float dirY = (enterMotion == DIR_UP) ? -1.0f
                    : (enterMotion == DIR_DOWN) ? 1.0f : 0.0f;
        Vector2 c = hero.center();
        roomTransition.active = true;
        roomTransition.t = 0.0f;
        roomTransition.endX = c.x - TILE_SIZE * 0.5f;
        roomTransition.endY = c.y - TILE_SIZE * 0.5f;
        roomTransition.startX = roomTransition.endX - dirX * TILE_SIZE;
        roomTransition.startY = roomTransition.endY - dirY * TILE_SIZE;
        hero.nudge(-dirX * TILE_SIZE, -dirY * TILE_SIZE);
    }
    else
    {
        roomTransition.active = false;
    }

    startMusic(cfg.music);
    startAmbient(cfg.ambient);
    startDangerMusic(cfg.dangerMusic);

    prevTileX = -1;
    prevTileY = -1;

    // Spawn the room's enemy set fresh (defeated enemies stay dead until the
    // room reloads).
    clearWorldSystems();
    enemyCount = cfg.enemyCount < 16 ? cfg.enemyCount : 16;
    bool bossLives = false;
    for (int i = 0; i < enemyCount; i++)
    {
        const EnemySpawn& spawn = cfg.enemies[i];
        if (spawn.enemy < 0 || spawn.enemy >= ENEMY_COUNT) continue;
        // Dungeon rooms keep their fallen across re-entries (the mask was
        // snapshotted on the way out); the slot stays dead-empty.
        if (cfg.dungeon && index < MAX_ROOM_MARKS
            && (dungeonDead[index] & (1 << i))) continue;
        // Bosses stay dead: defeated ones don't respawn.
        int bossSlot = index * 16 + i;
        if (ENEMIES[spawn.enemy].boss
            && bossSlot >= 0 && bossSlot < MAX_BOSS_SLOTS
            && bossDefeated[bossSlot]) continue;
        enemies[i].spawn(ENEMIES[spawn.enemy], enemySheets[spawn.enemy],
                         spawn.tileX, spawn.tileY);
        if (ENEMIES[spawn.enemy].boss)
        {
            bossLives = true;
            snprintf(bossIntroName, sizeof(bossIntroName), "%s",
                     ENEMIES[spawn.enemy].name);
        }
    }
    // Entering a live arena plays the intro beat: the world holds while
    // the letterbox names the boss.
    bossIntroTime = bossLives ? BOSS_INTRO_DURATION : 0.0f;
    if (bossLives) PlaySound(sndBossIntro);

    // Objects (pots, bushes): fresh copies each entry, like LttP pots that
    // are back on the next visit.
    int objectTotal = cfg.objectCount < MAX_OBJECTS ? cfg.objectCount : MAX_OBJECTS;
    for (int i = 0; i < objectTotal; i++)
    {
        const ObjectSpawn& spawn = cfg.objects[i];
        if (spawn.type < 0 || spawn.type >= OBJECT_COUNT) continue;
        objects[i].active = true;
        objects[i].type = spawn.type;
        objects[i].state = OBJ_GROUND;
        objects[i].x = (float)(spawn.tileX * TILE_SIZE);
        objects[i].y = (float)(spawn.tileY * TILE_SIZE);
        objects[i].groundX = objects[i].x;
        objects[i].groundY = objects[i].y;
        objects[i].velX = 0.0f;
        objects[i].velY = 0.0f;
        objects[i].z = 0.0f;
        objects[i].vz = 0.0f;
        objects[i].throwId = 0;
        objects[i].aux1 = spawn.aux1;
        objects[i].aux2 = spawn.aux2;
        objects[i].aux3 = spawn.aux3;
        objects[i].pushTimer = 0.0f;
        objects[i].pushDirX = 0.0f;
        objects[i].pushDirY = 0.0f;
        objects[i].slideFromX = objects[i].x;
        objects[i].slideFromY = objects[i].y;
        objects[i].slideToX = objects[i].x;
        objects[i].slideToY = objects[i].y;
        objects[i].slideT = 0.0f;
        objects[i].switchActive = false;
        // Settled state (saved, bake-slot keyed): a moved or sunk block
        // reappears where it was left, a latched switch or pulled lever
        // stays solved, a lit torch stays lit.
        for (int s = 0; s < settledCount; s++)
        {
            if (settledRoom[s] != (short)index || settledSlot[s] != (short)i)
                continue;
            objects[i].x = (float)settledTileX[s] * TILE_SIZE;
            objects[i].y = (float)settledTileY[s] * TILE_SIZE;
            objects[i].groundX = objects[i].x;
            objects[i].groundY = objects[i].y;
            if (settledFlags[s] & SETTLED_SUNK) objects[i].state = OBJ_SUNK;
            if (settledFlags[s] & SETTLED_LATCHED)
            {
                objects[i].switchActive = true;
                objects[i].opened = true;
            }
            break;
        }
        // Platform rail (OBJINT_PLATFORM): start at the first waypoint.
        objects[i].path = spawn.path;
        objects[i].pathCount = spawn.pathCount;
        objects[i].pathIndex = 0;
        objects[i].pathT = 0.0f;
        objects[i].modeTimer = 0.0f;
        // Chests stay open across reloads (permanent-until-save registry).
        {
            int slot = index * MAX_OBJECTS + i;
            objects[i].opened = (objInteracts(objects[i], OBJINT_CHEST)
                                 && slot >= 0 && slot < MAX_CHEST_SLOTS
                                 && chestOpened[slot]);
        }
    }

    // NPCs: spawn-condition flags gate presence by story state (checked at
    // entry; conditions changing mid-room apply on the next visit).
    for (int i = 0; i < MAX_NPCS; i++) npcs[i].active = false;
    int npcTotal = cfg.npcCount < MAX_NPCS ? cfg.npcCount : MAX_NPCS;
    for (int i = 0; i < npcTotal; i++)
    {
        const NpcSpawn& spawn = cfg.npcs[i];
        if (spawn.character < 0 || spawn.character >= CHARACTER_COUNT) continue;
        if (spawn.ifFlag >= 0 && spawn.ifFlag < WORLD_FLAG_COUNT
            && !gWorldFlags[spawn.ifFlag]) continue;
        if (spawn.unlessFlag >= 0 && spawn.unlessFlag < WORLD_FLAG_COUNT
            && gWorldFlags[spawn.unlessFlag]) continue;

        npcs[i].active = true;
        npcs[i].cfg = &CHARACTERS[spawn.character];
        npcs[i].sheet = (spawn.character < 16) ? characterSheets[spawn.character] : Texture2D{};
        npcs[i].x = (float)(spawn.tileX * TILE_SIZE);
        npcs[i].y = (float)(spawn.tileY * TILE_SIZE);
        npcs[i].facing = DIR_DOWN;
        npcs[i].wander = spawn.wander;
        npcs[i].path = spawn.pathCount > 0 ? spawn.path : 0;
        npcs[i].pathCount = spawn.pathCount;
        npcs[i].pathIndex = 0;
        npcs[i].dialogue = spawn.dialogue;
        npcs[i].modeTime = 0.0f;
        npcs[i].timer = 0.5f + worldRng.nextFloat() * 2.0f;
        npcs[i].animTime = 0.0f;
        npcs[i].moving = false;
        npcs[i].lastFacingPick = -1;
    }

    // Placed pickups (hearts in grass, buried rupees): spawned fresh on
    // every entry, like LttP field pickups.
    int pickupTotal = cfg.pickupCount < MAX_PICKUPS ? cfg.pickupCount : MAX_PICKUPS;
    for (int i = 0; i < pickupTotal; i++)
    {
        const PickupSpawn& spawn = cfg.pickups[i];
        if (spawn.kind < 0) continue;
        spawnPickup(spawn.kind,
                    (float)(spawn.tileX * TILE_SIZE) + 8.0f,
                    (float)(spawn.tileY * TILE_SIZE) + 8.0f);
    }
}

// Save-restore entry: land in the room at the saved tile (fade in).
static void loadRoomFromSave(int room, int tileX, int tileY)
{
    loadRoom(room, -1, tileX, tileY);
}

// Map-screen layout: BFS over the exits graph, one grid cell per room.
// Neighbors offset by their shared edge; collisions overwrite (fine for
// small worlds).
static void mapLayout(float* xs, float* ys, int roomCount)
{
    for (int i = 0; i < roomCount; i++)
    {
        xs[i] = 1e9f;
        ys[i] = 1e9f;
    }
    if (roomCount <= 0) return;
    xs[0] = 0.0f;
    ys[0] = 0.0f;
    // Frontier walk (no queue storage: rooms are few, loop until stable).
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (int i = 0; i < roomCount; i++)
        {
            if (xs[i] > 1e8f) continue;
            const RoomConfig& cfg = ROOMS[i];
            const int edges[4][2] = {
                { cfg.exitNorth, 0 }, { cfg.exitSouth, 1 },
                { cfg.exitEast, 2 }, { cfg.exitWest, 3 },
            };
            for (int e = 0; e < 4; e++)
            {
                int target = edges[e][0];
                if (target < 0 || target >= roomCount) continue;
                float ox = 0.0f;
                float oy = 0.0f;
                switch (edges[e][1])
                {
                case 0: oy = -1.0f; break;
                case 1: oy = 1.0f; break;
                case 2: ox = 1.0f; break;
                case 3: ox = -1.0f; break;
                default: break;
                }
                float nx = xs[i] + ox;
                float ny = ys[i] + oy;
                if (xs[target] > 1e8f)
                {
                    xs[target] = nx;
                    ys[target] = ny;
                    changed = true;
                }
            }
        }
    }

    // Rooms off the exits graph (stair destinations, warp chambers,
    // pit-landing basements) never got placed by the walk: shelf them to
    // the lower right of the map, in room order, so visiting one keeps
    // the map sane instead of blowing the bounds with the 1e9 sentinel.
    float maxX = 0.0f;
    float maxY = 0.0f;
    for (int i = 0; i < roomCount; i++)
    {
        if (xs[i] > 1e8f) continue;
        if (xs[i] > maxX) maxX = xs[i];
        if (ys[i] > maxY) maxY = ys[i];
    }
    int shelf = 0;
    for (int i = 0; i < roomCount; i++)
    {
        if (xs[i] < 1e8f) continue;
        xs[i] = maxX + 1.0f + (float)shelf;
        ys[i] = maxY + 1.0f;
        shelf++;
    }
}

static void updateEncounters(bool allowInteractive)
{
    const RoomConfig& cfg = room.config();
    int tileX = 0;
    int tileY = 0;
    hero.tileUnder(tileX, tileY);

    if (tileX != prevTileX || tileY != prevTileY)
    {
        for (int i = 0; i < cfg.instantCount; i++)
        {
            const Encounter& e = cfg.instant[i];
            if (e.tileX == tileX && e.tileY == tileY && e.dialogue >= 0 && e.dialogue < DIALOGUE_COUNT
                && encounterAvailable(e))
            {
                fireEncounter(e);
                dialogue.open(DIALOGUES[e.dialogue]);
            }
        }
        prevTileX = tileX;
        prevTileY = tileY;
    }

    if (allowInteractive && actionPressed())
    {
        for (int i = 0; i < cfg.interactiveCount; i++)
        {
            const Encounter& e = cfg.interactive[i];
            if (e.tileX == tileX && e.tileY == tileY && e.dialogue >= 0 && e.dialogue < DIALOGUE_COUNT
                && encounterAvailable(e))
            {
                fireEncounter(e);
                dialogue.open(DIALOGUES[e.dialogue]);
                break;
            }
        }
    }
}

static void collectPickup(Pickup& p)
{
    switch (p.kind)
    {
    case PICKUP_HEART:
        // Full hearts are left lying until there is room for them.
        if (hero.healthHalves() >= hero.maxHealthHalves()) return;
        hero.heal(2);
        PlaySound(sndHeart);
        break;
    case PICKUP_RUPEE1:
    case PICKUP_RUPEE5:
        inventory.addRupees(p.kind == PICKUP_RUPEE5 ? 5 : 1);
        PlaySound(sndRupee);
        break;
    case PICKUP_ARROW:
        if (inventory.arrows >= inventory.arrowsMax) return;
        inventory.addArrows(1);
        PlaySound(sndRupee);
        break;
    case PICKUP_BOMB:
        if (inventory.bombs >= inventory.bombsMax) return;
        inventory.addBombs(1);
        PlaySound(sndRupee);
        break;
    case PICKUP_MAGIC:
        if (inventory.magic >= inventory.magicMax) return;
        inventory.addMagic(2);
        PlaySound(sndRupee);
        break;
    case PICKUP_HEART_CONTAINER:
        // Raises the max by a heart and fills the whole gauge.
        hero.addMaxHearts(2);
        PlaySound(sndItemGet);
        break;
    case PICKUP_FAIRY:
        // A caught fairy: a full refill of hearts and magic, on the wing.
        hero.heal(hero.maxHealthHalves());
        inventory.addMagic(inventory.magicMax);
        spawnParticles(p.x + 8.0f, p.y + 8.0f, 10, 255, 200, 220,
                       60.0f, 90.0f);
        PlaySound(sndFairy);
        break;
    case PICKUP_KEY:
        inventory.smallKeys++;
        PlaySound(sndUseKey);
        break;
    default:
        break;
    }
    p.active = false;
}

static void updateGrass()
{
    // One cut pass per offensive action (swing/spin/dash): tiles overlapped
    // by the active hitbox get cut.
    static int lastCutSwing = -1;
    bool cutting = hero.attacking() || hero.spinning() || hero.dashing()
                   || hero.hammering();
    if (!cutting) return;
    if (hero.swingId() == lastCutSwing) return;
    lastCutSwing = hero.swingId();

    Rectangle attack = hero.dashing() ? hero.box() : hero.attackBox();
    if (attack.width <= 0.0f) return;

    int x0 = (int)(attack.x / TILE_SIZE);
    int y0 = (int)(attack.y / TILE_SIZE);
    int x1 = (int)((attack.x + attack.width - 0.01f) / TILE_SIZE);
    int y1 = (int)((attack.y + attack.height - 0.01f) / TILE_SIZE);
    for (int ty = y0; ty <= y1; ty++)
    {
        for (int tx = x0; tx <= x1; tx++)
        {
            if (room.cutTileAt(tx, ty))
            {
                PlaySound(sndGrass);
                spawnParticles((float)(tx * TILE_SIZE) + 16.0f,
                               (float)(ty * TILE_SIZE) + 14.0f,
                               6, 90, 150, 70, 60.0f, 120.0f);
                int drop = rollDrop(worldRng);
                if (drop != PICKUP_NONE)
                    spawnPickup(drop, (float)(tx * TILE_SIZE) + 8.0f,
                                (float)(ty * TILE_SIZE) + 8.0f);
            }
        }
    }

    // Bush objects cut like grass: standing bushes overlapped by the swing
    // smash apart (leaves + drop). Same once-per-swing pass as the tiles.
    for (int i = 0; i < MAX_OBJECTS; i++)
    {
        const WorldObject& o = objects[i];
        if (!o.active || !objInteracts(o, OBJINT_CUT)) continue;
        if (o.state != OBJ_GROUND) continue;
        if (!overlap(attack, objectBox(o))) continue;
        smashObject(i, true);
    }

    // Crystal switches: striking one flips the blue/red state — colored
    // barriers swap raised/lowered (the state is a saved world flag, so
    // it persists like story progress). Once per swing, like the rest.
    for (int i = 0; i < MAX_OBJECTS; i++)
    {
        const WorldObject& o = objects[i];
        if (!o.active || !objInteracts(o, OBJINT_CRYSTAL)) continue;
        if (o.state != OBJ_GROUND) continue;
        if (!overlap(attack, objectBox(o))) continue;
        if (WORLD_FLAG_CRYSTAL_RED >= 0 && WORLD_FLAG_CRYSTAL_RED < WORLD_FLAG_COUNT)
        {
            gWorldFlags[WORLD_FLAG_CRYSTAL_RED] = !gWorldFlags[WORLD_FLAG_CRYSTAL_RED];
            PlaySound(sndCrystal);
            screenFlash(0.15f, Color{ 160, 190, 255, 255 }, 0.25f);
            spawnParticles(o.x + 16.0f, o.y + 8.0f, 10, 160, 185, 255,
                           90.0f, 100.0f);
        }
    }
}

// Sword beam: every completed *normal* swing started at full hearts fires a
// beam along the facing direction.
static void updateSwordBeam()
{
    static int lastBeamSwing = -1;
    if (!hero.attacking()) return;
    if (hero.swingId() == lastBeamSwing) return;
    lastBeamSwing = hero.swingId();

    if (hero.healthHalves() != hero.maxHealthHalves()) return;

    Vector2 heroCenter = hero.center();
    float ox = 0.0f;
    float oy = 0.0f;
    switch (hero.facingDirection())
    {
    case DIR_DOWN: oy = 20.0f; break;
    case DIR_UP: oy = -20.0f; break;
    case DIR_RIGHT: ox = 20.0f; break;
    case DIR_LEFT: ox = -20.0f; break;
    default: break;
    }
    spawnBeam(heroCenter.x - 8.0f + ox, heroCenter.y - 8.0f + oy, hero.facingDirection());
    PlaySound(sndSwordBeam);
}

// Pegasus boots pre-step: pots and bushes shatter under the dash before the
// hero's collision check can treat them as walls (LttP dash-through), so
// the dash keeps running instead of crashing.
static void updateDashSmash()
{
    if (!hero.dashing()) return;
    Rectangle box = hero.box();
    Direction facing = hero.facingDirection();
    float dx = (facing == DIR_LEFT) ? -1.0f : (facing == DIR_RIGHT) ? 1.0f : 0.0f;
    float dy = (facing == DIR_UP) ? -1.0f : (facing == DIR_DOWN) ? 1.0f : 0.0f;
    Rectangle probe = { box.x + dx * 10.0f - 4.0f, box.y + dy * 10.0f - 4.0f,
                        box.width + 8.0f, box.height + 8.0f };
    for (int i = 0; i < MAX_OBJECTS; i++)
    {
        const WorldObject& o = objects[i];
        if (!o.active) continue;
        if (!objInteracts(o, OBJINT_DASH_SMASH)) continue;
        if (o.state != OBJ_GROUND) continue;
        if (!overlap(probe, objectBox(o))) continue;
        smashObject(i, true);
    }
}

// Warp pads and staircases: stepping onto one loads the target room at
// the target tile (fade in) and banks the save, like dialogue warps. A
// short cooldown keeps arrival pads from immediately re-triggering.
// The arena seal holds: no warping or stair-crawling out while a boss or
// mini-boss lives.
static float warpCooldown = 0.0f;

static void updateWarps()
{
    if (warpCooldown > 0.0f)
    {
        warpCooldown -= frameDt();
        return;
    }
    if (roomTransition.active || itemGet.active || hero.dead()) return;
    if (bossAliveInRoom()) return;

    Rectangle heroBox = hero.box();
    for (int i = 0; i < MAX_OBJECTS; i++)
    {
        const WorldObject& o = objects[i];
        if (!o.active) continue;
        bool stair = objInteracts(o, OBJINT_STAIR);
        if (!stair && !objInteracts(o, OBJINT_WARP)) continue;
        Rectangle trigger = { o.x + 6.0f, o.y + 6.0f, 20.0f, 20.0f };
        if (!overlap(trigger, heroBox)) continue;
        if (o.aux1 < 0 || o.aux1 >= ROOM_COUNT) continue;

        if (stair) playNamedSfx("stairs down 1.wav");
        else PlaySound(sndWarp);
        loadRoom(o.aux1, -1, o.aux2, o.aux3);
        saveToFile(saveSlotInUse);
        showToast("SAVED");
        warpCooldown = 0.6f;
        return;
    }
}

// The flute's bird flight: land at the chosen room's spawn tile, fade in,
// and bank the save like the warp pads (arrival pads stay cool too).
static void fluteWarp()
{
    const RoomConfig& cfg = ROOMS[fluteCursor];
    int tileX = cfg.hasSpawn ? cfg.spawnTileX : 0;
    int tileY = cfg.hasSpawn ? cfg.spawnTileY : 0;
    fluteMenu = false;
    playNamedSfx("flap.wav");
    loadRoom(fluteCursor, -1, tileX, tileY);
    saveToFile(saveSlotInUse);
    showToast("SAVED");
    warpCooldown = 0.6f;
}

// The mirror's reflection: slip through to the parallel-world pair. The
// landing is the hero's tile in the paired room (nearest free ground —
// layouts may differ); the arena seal holds like any warp, and the hop
// banks the save.
static void useMirror()
{
    const RoomConfig& current = room.config();
    if (current.mirrorRoom < 0 || current.mirrorRoom >= ROOM_COUNT
        || bossAliveInRoom())
    {
        PlaySound(sndError);  // no pair here / no fleeing the arena
        return;
    }

    const RoomConfig& target = ROOMS[current.mirrorRoom];
    int tileX = 0;
    int tileY = 0;
    hero.tileUnder(tileX, tileY);
    if (tileX >= target.mapWidth) tileX = target.mapWidth - 1;
    if (tileY >= target.mapHeight) tileY = target.mapHeight - 1;
    if (tileX < 0) tileX = 0;
    if (tileY < 0) tileY = 0;
    if (mirrorTileBlocked(target, tileX, tileY))
    {
        bool found = false;
        for (int r = 1; !found && r < 24; r++)
        {
            for (int dy = -r; !found && dy <= r; dy++)
            {
                for (int dx = -r; !found && dx <= r; dx++)
                {
                    int ax = (dx < 0) ? -dx : dx;
                    int ay = (dy < 0) ? -dy : dy;
                    if (ax != r && ay != r) continue;  // ring cells only
                    if (!mirrorTileBlocked(target, tileX + dx, tileY + dy))
                    {
                        tileX += dx;
                        tileY += dy;
                        found = true;
                    }
                }
            }
        }
        if (!found)
        {
            // Whole neighborhood blocked (data problem): the spawn.
            tileX = target.hasSpawn ? target.spawnTileX : 1;
            tileY = target.hasSpawn ? target.spawnTileY : 1;
        }
    }

    loadRoom(current.mirrorRoom, -1, tileX, tileY);
    saveToFile(saveSlotInUse);
    showToast("SAVED");
    warpCooldown = 0.6f;
}

// Fairy-fountain tiles ("heal" tile metadata): standing on the water slowly
// refills hearts and magic, with rising sparkle motes.
static float healTick = 0.0f;

static void updateHealTiles(float dt)
{
    int tileX = 0;
    int tileY = 0;
    hero.tileUnder(tileX, tileY);
    TileMeta meta = room.tileMetaAt(tileX, tileY);
    if (!meta.heal)
    {
        healTick = 0.0f;
        return;
    }

    healTick += dt;
    if (healTick < 0.5f) return;
    healTick = 0.0f;

    bool refilled = false;
    if (hero.healthHalves() < hero.maxHealthHalves())
    {
        hero.heal(1);
        refilled = true;
    }
    if (inventory.magic < inventory.magicMax)
    {
        inventory.addMagic(1);
        refilled = true;
    }
    if (refilled)
    {
        PlaySound(sndHeart);
        Vector2 c = hero.center();
        spawnParticles(c.x, c.y, 3, 200, 220, 255, 20.0f, 60.0f);
    }
}

// Grants (chest payloads, boss heart containers): grant kinds are game
// data; the engine interprets the baked GrantRule's effect, pickup frame,
// and description template.
static void applyChestGrant(int kind, int param)
{
    if (kind < 0 || kind >= GRANT_KIND_COUNT) return;
    switch (GRANT_RULES[kind].effect)
    {
    case GRANTEFFECT_GIVE_ITEM:
        if (param >= 0 && param < ITEM_COUNT) inventory.give(param);
        break;
    case GRANTEFFECT_ADD_RUPEES: inventory.addRupees(param > 0 ? param : 1); break;
    case GRANTEFFECT_ADD_ARROWS: inventory.addArrows(param > 0 ? param : 1); break;
    case GRANTEFFECT_ADD_BOMBS: inventory.addBombs(param > 0 ? param : 1); break;
    case GRANTEFFECT_ADD_MAGIC: inventory.addMagic(param > 0 ? param : 2); break;
    case GRANTEFFECT_ADD_HEARTS: hero.addMaxHearts(2); break;
    case GRANTEFFECT_ADD_SMALL_KEY: inventory.smallKeys++; break;
    case GRANTEFFECT_ADD_BIG_KEY: inventory.bigKeys++; break;
    case GRANTEFFECT_SET_FLAG:
        if (param >= 0 && param < WORLD_FLAG_COUNT) gWorldFlags[param] = true;
        break;
    case GRANTEFFECT_GIVE_BOTTLE:
        // A bottle slot with starting contents (param): a full set
        // silently ignores it.
        inventory.giveBottle(param);
        break;
    default: break;
    }
}

static int grantPickupFrame(int kind)
{
    if (kind < 0 || kind >= GRANT_KIND_COUNT) return -1;
    return GRANT_RULES[kind].pickupFrame;
}

static void buildGrantText(int kind, int param)
{
    if (kind < 0 || kind >= GRANT_KIND_COUNT)
    {
        snprintf(itemGetText, sizeof(itemGetText), "It's empty...");
        return;
    }
    const GrantRule& rule = GRANT_RULES[kind];
    int amount = param > 0 ? param : 1;
    switch (rule.effect)
    {
    case GRANTEFFECT_GIVE_ITEM:
        if (param >= 0 && param < ITEM_COUNT)
            snprintf(itemGetText, sizeof(itemGetText), rule.text, ITEMS[param].label);
        else
            snprintf(itemGetText, sizeof(itemGetText), "You got... something?");
        break;
    case GRANTEFFECT_ADD_RUPEES:
    case GRANTEFFECT_ADD_ARROWS:
    case GRANTEFFECT_ADD_BOMBS:
        snprintf(itemGetText, sizeof(itemGetText), rule.text, amount);
        break;
    case GRANTEFFECT_GIVE_BOTTLE:
        // The decanter-style rise carries the item-get text as-is.
        snprintf(itemGetText, sizeof(itemGetText), "%s", rule.text);
        break;
    default:
        snprintf(itemGetText, sizeof(itemGetText), "%s", rule.text);
        break;
    }
}

// Rise phase: grant floats from the chest to over the hero's head.
static void updateItemGet(float dt)
{
    const float RISE_DURATION = 0.65f;
    itemGet.t += dt;
    float t = clampf(itemGet.t / RISE_DURATION, 0.0f, 1.0f);
    float e = 1.0f - (1.0f - t) * (1.0f - t);
    itemGet.itemX = itemGet.startX + (itemGet.endX - itemGet.startX) * e;
    itemGet.itemY = itemGet.startY + (itemGet.endY - itemGet.startY) * e
                    - sinf(PI * t) * 14.0f;  // little hop arc

    if (itemGet.t >= RISE_DURATION && !itemGet.dialogueOpened)
    {
        // Hold-up: apply the grant, then play the description dialogue.
        applyChestGrant(itemGet.grantKind, itemGet.grantParam);
        buildGrantText(itemGet.grantKind, itemGet.grantParam);
        itemGetLines[0].text = itemGetText;
        itemGetLines[0].gotoLine = -1;
        itemGetLines[0].ifFlag = -1;
        itemGetLines[0].unlessFlag = -1;
        itemGetLines[0].setFlags = 0;
        itemGetLines[0].setCount = 0;
        itemGetLines[0].clearFlags = 0;
        itemGetLines[0].clearCount = 0;
        itemGetLines[0].sfx = 0;
        itemGetLines[0].music = 0;
        itemGetLines[0].warpRoom = -1;
        itemGetLines[0].warpTileX = 0;
        itemGetLines[0].warpTileY = 0;
        itemGetLines[0].ifItem = -1;
        itemGetLines[0].unlessItem = -1;
        itemGetLines[0].giveItem = -1;
        itemGetLines[0].choiceCount = 0;
        itemGetLines[0].choices = 0;
        itemGet.dialogueOpened = true;
        dialogue.open(itemGetDialogue);
    }
}

// NPCs: idle ones face the hero when nearby; wanderers stroll with pauses.
static void faceHero(Npc& npc)
{
    float dx = hero.center().x - (npc.x + 16.0f);
    float dy = hero.center().y - (npc.y + 16.0f);
    if (fabsf(dx) >= fabsf(dy)) npc.facing = (dx > 0.0f) ? DIR_RIGHT : DIR_LEFT;
    else npc.facing = (dy > 0.0f) ? DIR_DOWN : DIR_UP;
}

static void updateNpcs(float dt)
{
    Vector2 heroCenter = hero.center();
    for (int i = 0; i < MAX_NPCS; i++)
    {
        Npc& npc = npcs[i];
        if (!npc.active || !npc.cfg) continue;

        npc.animTime += dt;
        npc.modeTime += dt;

        if (npc.path && npc.pathCount > 0)
        {
            // Path-follower: walks the tile waypoints in a loop (the path
            // beats the wander flag). Blocked waypoints get retried, so a
            // shove only stalls the stroll momentarily.
            float targetX = (float)(npc.path[npc.pathIndex * 2] * TILE_SIZE);
            float targetY = (float)(npc.path[npc.pathIndex * 2 + 1] * TILE_SIZE);
            float toX = targetX - npc.x;
            float toY = targetY - npc.y;
            float dist = sqrtf(toX * toX + toY * toY);

            if (dist < 2.0f)
            {
                npc.x = targetX;
                npc.y = targetY;
                npc.pathIndex = (npc.pathIndex + 1) % npc.pathCount;
                npc.moving = false;
            }
            else
            {
                npc.moving = true;
                float step = NPC_WALK_SPEED * dt;
                if (step > dist) step = dist;
                float dx = toX / dist;
                float dy = toY / dist;
                Rectangle box = npcBox(npc);
                Rectangle nextX = { box.x + dx * step, box.y, box.width, box.height };
                Rectangle nextY = { box.x, box.y + dy * step, box.width, box.height };
                bool blocked = false;
                if (dx != 0.0f && !npcBlocked(npc, nextX)) npc.x += dx * step;
                else if (dx != 0.0f) blocked = true;
                if (dy != 0.0f && !npcBlocked(npc, nextY)) npc.y += dy * step;
                else if (dy != 0.0f) blocked = true;
                if (blocked && npc.modeTime > 1.0f)
                {
                    // Stuck against something: skip the waypoint and move
                    // on rather than grinding into it forever.
                    npc.pathIndex = (npc.pathIndex + 1) % npc.pathCount;
                    npc.modeTime = 0.0f;
                }

                // Face the dominant axis of travel.
                if (fabsf(toX) >= fabsf(toY))
                {
                    if (toX > 1.0f) npc.facing = DIR_RIGHT;
                    else if (toX < -1.0f) npc.facing = DIR_LEFT;
                }
                else
                {
                    if (toY > 1.0f) npc.facing = DIR_DOWN;
                    else if (toY < -1.0f) npc.facing = DIR_UP;
                }
            }

            // Face the hero while standing on a waypoint near them.
            if (!npc.moving)
            {
                float dx = heroCenter.x - (npc.x + 16.0f);
                float dy = heroCenter.y - (npc.y + 16.0f);
                if (dx * dx + dy * dy < 48.0f * 48.0f) faceHero(npc);
            }
        }
        else if (npc.wander)
        {
            if (npc.moving)
            {
                float dx = (npc.facing == DIR_LEFT) ? -1.0f : (npc.facing == DIR_RIGHT) ? 1.0f : 0.0f;
                float dy = (npc.facing == DIR_UP) ? -1.0f : (npc.facing == DIR_DOWN) ? 1.0f : 0.0f;
                float step = NPC_WALK_SPEED * dt;
                Rectangle box = npcBox(npc);
                Rectangle nextX = { box.x + dx * step, box.y, box.width, box.height };
                Rectangle nextY = { box.x, box.y + dy * step, box.width, box.height };
                bool blocked = false;
                if (dx != 0.0f && !npcBlocked(npc, nextX)) npc.x += dx * step;
                else if (dx != 0.0f) blocked = true;
                if (dy != 0.0f && !npcBlocked(npc, nextY)) npc.y += dy * step;
                else if (dy != 0.0f) blocked = true;

                if (blocked || npc.modeTime > 2.2f)
                {
                    npc.moving = false;
                    npc.timer = 1.0f + worldRng.nextFloat() * 2.5f;
                }
            }
            else
            {
                npc.timer -= dt;
                if (npc.timer <= 0.0f)
                {
                    int pick = worldRng.range(DIR_COUNT);
                    if (pick == npc.lastFacingPick) pick = (pick + 1) % DIR_COUNT;
                    npc.lastFacingPick = pick;
                    npc.facing = (Direction)pick;
                    npc.moving = true;
                    npc.modeTime = 0.0f;
                }
                // A wanderer that stops near the hero turns to face them.
                float dx = heroCenter.x - (npc.x + 16.0f);
                float dy = heroCenter.y - (npc.y + 16.0f);
                if (dx * dx + dy * dy < 48.0f * 48.0f) faceHero(npc);
            }
        }
        else
        {
            // Idle: face the hero while they're close.
            float dx = heroCenter.x - (npc.x + 16.0f);
            float dy = heroCenter.y - (npc.y + 16.0f);
            if (dx * dx + dy * dy < 64.0f * 64.0f) faceHero(npc);
        }
    }
}

// Action button on objects: throw what's carried, or hoist the liftable
// object in front of the hero. Returns true when the press was consumed
// (so encounters don't double-fire on it).
static bool updateObjectActions()
{
    if (!actionPressed()) return false;

    if (carriedIndex >= 0 && hero.carrying())
    {
        // Throw: the object arcs along the facing direction.
        WorldObject& o = objects[carriedIndex];
        Direction facing = hero.facingDirection();
        float dx = (facing == DIR_LEFT) ? -1.0f : (facing == DIR_RIGHT) ? 1.0f : 0.0f;
        float dy = (facing == DIR_UP) ? -1.0f : (facing == DIR_DOWN) ? 1.0f : 0.0f;
        o.state = OBJ_THROWN;
        o.velX = dx * 150.0f;
        o.velY = dy * 150.0f;
        o.z = 22.0f;
        o.vz = 90.0f;
        o.throwId = nextThrowId--;
        carriedIndex = -1;
        hero.setCarrying(false);
        PlaySound(sndThrow);
        return true;
    }

    if (hero.busy() || hero.carrying()) return false;

    Rectangle heroBox = hero.box();
    Direction facing = hero.facingDirection();
    float dx = (facing == DIR_LEFT) ? -1.0f : (facing == DIR_RIGHT) ? 1.0f : 0.0f;
    float dy = (facing == DIR_UP) ? -1.0f : (facing == DIR_DOWN) ? 1.0f : 0.0f;

    // NPCs: talking makes them face the hero and opens their dialogue.
    {
        Rectangle probe = { heroBox.x + dx * 20.0f, heroBox.y + dy * 20.0f,
                            heroBox.width, heroBox.height };
        for (int i = 0; i < MAX_NPCS; i++)
        {
            Npc& npc = npcs[i];
            if (!npc.active) continue;
            if (!overlap(probe, npcBox(npc))) continue;
            if (npc.dialogue < 0 || npc.dialogue >= DIALOGUE_COUNT) continue;
            faceHero(npc);
            npc.moving = false;  // stand still while talking
            dialogue.open(DIALOGUES[npc.dialogue]);
            return true;
        }
    }

    // Signs: readable via the action button from any side.
    {
        Rectangle probe = { heroBox.x + dx * 20.0f, heroBox.y + dy * 20.0f,
                            heroBox.width, heroBox.height };
        for (int i = 0; i < MAX_OBJECTS; i++)
        {
            const WorldObject& o = objects[i];
            if (!o.active || !objInteracts(o, OBJINT_READABLE)) continue;
            if (o.state != OBJ_GROUND) continue;
            if (o.aux1 < 0 || o.aux1 >= DIALOGUE_COUNT) continue;
            if (!overlap(probe, objectBox(o))) continue;
            dialogue.open(DIALOGUES[o.aux1]);
            return true;
        }
    }

    // Chests: locked need a small key, big ones the big key; opening plays
    // the item-get sequence (lid stays open for good).
    {
        Rectangle probe = { heroBox.x + dx * 20.0f, heroBox.y + dy * 20.0f,
                            heroBox.width, heroBox.height };
        for (int i = 0; i < MAX_OBJECTS; i++)
        {
            WorldObject& o = objects[i];
            if (!o.active || !objInteracts(o, OBJINT_CHEST)) continue;
            if (o.state != OBJ_GROUND) continue;
            if (!overlap(probe, objectBox(o))) continue;

            if (o.opened) return false;  // nothing left inside
            int keyCost = 0;
            if (o.aux1 >= 0 && o.aux1 < CHEST_KIND_COUNT)
                keyCost = CHEST_RULES[o.aux1].keyCost;
            if (keyCost == 1 && inventory.smallKeys <= 0)
            {
                PlaySound(sndError);  // rattles, no key
                return true;
            }
            if (keyCost == 2 && inventory.bigKeys <= 0)
            {
                PlaySound(sndError);
                return true;
            }

            if (keyCost == 1) inventory.smallKeys--;
            if (keyCost == 2) inventory.bigKeys--;

            o.opened = true;
            {
                int slot = currentRoom * MAX_OBJECTS + i;
                if (slot >= 0 && slot < MAX_CHEST_SLOTS) chestOpened[slot] = true;
            }
            PlaySound(sndChestOpen);
            playJingle(sndFanfare);

            // Grant rises from the chest to over the hero's head.
            Vector2 heroCenter = hero.center();
            itemGet.active = true;
            itemGet.dialogueOpened = false;
            itemGet.t = 0.0f;
            itemGet.grantKind = o.aux2;
            itemGet.grantParam = o.aux3;
            itemGet.pickupFrame = grantPickupFrame(o.aux2);
            itemGet.startX = o.x + 16.0f;
            itemGet.startY = o.y + 6.0f;
            itemGet.endX = heroCenter.x;
            itemGet.endY = heroCenter.y - 34.0f;
            itemGet.itemX = itemGet.startX;
            itemGet.itemY = itemGet.startY;
            saveToFile(saveSlotInUse);  // banked treasure persists
            return true;
        }
    }

    // Lift: a liftable ground-state object overlapping a probe in front of
    // the hero (pots and bushes; blocks/signs/barriers aren't liftable).
    // Heavy objects (big rocks) need the lift passive (gloves) to hoist.
    Rectangle probe = { heroBox.x + dx * 20.0f, heroBox.y + dy * 20.0f,
                        heroBox.width, heroBox.height };
    int best = -1;
    float bestDist = 1e9f;
    bool heavy = false;
    bool canLiftHeavy = liftItemOwned();
    for (int i = 0; i < MAX_OBJECTS; i++)
    {
        if (!objects[i].active || objects[i].state != OBJ_GROUND) continue;
        if (!objectLiftable(objects[i].type)) continue;
        if (objInteracts(objects[i], OBJINT_HEAVY) && !canLiftHeavy)
        {
            heavy = true;
            continue;
        }
        if (!overlap(probe, objectBox(objects[i]))) continue;
        float dist = fabsf(objects[i].x - heroBox.x) + fabsf(objects[i].y - heroBox.y);
        if (dist < bestDist)
        {
            bestDist = dist;
            best = i;
        }
    }
    if (best < 0)
    {
        // Grab: a pullable ground-state object in front (statues, blocks)
        // — the hero holds it, and pressing away drags it one tile
        // (updateGrabPull). Falls through here, so a liftable object
        // lifts instead of grabbing.
        for (int i = 0; i < MAX_OBJECTS; i++)
        {
            WorldObject& o = objects[i];
            if (!o.active || !objInteracts(o, OBJINT_PULLABLE)) continue;
            if (o.state != OBJ_GROUND) continue;
            if (!overlap(probe, objectBox(o))) continue;
            grabbedIndex = i;
            hero.startGrab();
            return true;
        }
        if (heavy) PlaySound(sndError);  // bare-handed hero, giant rock
        return false;
    }

    // Pot ambush ("hides" enemy): lifting reveals what was living
    // underneath — the room's shutter logic sees it too, like LttP.
    if (objInteracts(objects[best], OBJINT_HIDES) && objects[best].aux1 >= 0
        && enemyCount < 16 && objects[best].aux1 < ENEMY_COUNT)
    {
        enemies[enemyCount].spawn(ENEMIES[objects[best].aux1],
                                  enemySheets[objects[best].aux1],
                                  (int)(objects[best].x / TILE_SIZE),
                                  (int)(objects[best].y / TILE_SIZE));
        enemyCount++;
        PlaySound(sndBoomerangHit);
        spawnPoof(objects[best].x + 16.0f, objects[best].y + 16.0f);
    }

    objects[best].state = OBJ_LIFTING;
    objects[best].groundX = objects[best].x;
    objects[best].groundY = objects[best].y;
    liftingIndex = best;
    hero.startLift();
    return true;
}

// Push blocks: sustained face-first contact, then a one-tile slide.
static const float PUSH_DELAY = 0.4f;      // seconds of pushing before it moves
static const float SLIDE_DURATION = 0.22f;

static void updatePushBlocks(float dt)
{
    for (int i = 0; i < MAX_OBJECTS; i++)
    {
        WorldObject& o = objects[i];
        if (!o.active || !objInteracts(o, OBJINT_PUSHABLE)) continue;
        if (o.state != OBJ_GROUND) { o.pushTimer = 0.0f; continue; }
        if (hero.carrying() || hero.busy() || hero.dead())
        {
            o.pushTimer = 0.0f;
            continue;
        }

        Vector2 dir = hero.inputDir();
        if (dir.x == 0.0f && dir.y == 0.0f) { o.pushTimer = 0.0f; continue; }

        // Push along the facing (cardinal-dominant) direction.
        Direction facing = hero.facingDirection();
        float ddx = (facing == DIR_LEFT) ? -1.0f : (facing == DIR_RIGHT) ? 1.0f : 0.0f;
        float ddy = (facing == DIR_UP) ? -1.0f : (facing == DIR_DOWN) ? 1.0f : 0.0f;

        Rectangle heroBox = hero.box();
        Rectangle probe = { heroBox.x + ddx * 4.0f, heroBox.y + ddy * 4.0f,
                            heroBox.width, heroBox.height };
        if (!overlap(probe, objectBox(o))) { o.pushTimer = 0.0f; continue; }

        if (o.pushDirX != ddx || o.pushDirY != ddy)
        {
            o.pushTimer = 0.0f;
            o.pushDirX = ddx;
            o.pushDirY = ddy;
        }
        o.pushTimer += dt;
        if (o.pushTimer < PUSH_DELAY) continue;

        o.pushTimer = 0.0f;

        // Slide one tile along the push if the destination is clear.
        int tileX = (int)((o.x + TILE_SIZE * 0.5f) / TILE_SIZE);
        int tileY = (int)((o.y + TILE_SIZE * 0.5f) / TILE_SIZE);
        float destX = (float)((tileX + (int)ddx) * TILE_SIZE);
        float destY = (float)((tileY + (int)ddy) * TILE_SIZE);
        Rectangle destBox = { destX + 6.0f, destY + 12.0f, 20.0f, 16.0f };
        if (objectBlockedExcept(destBox, i)) continue;
        // Refuse to start under the hero's feet.
        if (overlap(hero.box(), destBox)) continue;

        o.state = OBJ_SLIDING;
        o.slideFromX = o.x;
        o.slideFromY = o.y;
        o.slideToX = destX;
        o.slideToY = destY;
        o.slideT = 0.0f;
        PlaySound(sndMoveObject);
    }
}

// Grab-and-pull: the hero holds a pullable object (grab via the action
// button); pressing straight away from it drags the pair one tile — the
// object follows into the tile the hero vacates. Both paths are checked
// before the pull starts, then the hero glides and the object slides in
// step (matching durations), so the settle logic (latch switches, sink
// blocks) treats a pull exactly like a push.
static void updateGrabPull()
{
    if (grabbedIndex < 0) return;
    if (!hero.grabbing())
    {
        grabbedIndex = -1;
        return;
    }
    if (hero.pulling()) return;

    WorldObject& o = objects[grabbedIndex];
    if (!o.active || o.state != OBJ_GROUND)
    {
        grabbedIndex = -1;
        return;
    }

    // Pull trigger: pressing the exact opposite of the facing it was
    // grabbed with (the hero walked up facing the object).
    Direction facing = hero.facingDirection();
    float faceX = (facing == DIR_LEFT) ? -1.0f : (facing == DIR_RIGHT) ? 1.0f : 0.0f;
    float faceY = (facing == DIR_UP) ? -1.0f : (facing == DIR_DOWN) ? 1.0f : 0.0f;
    Vector2 input = hero.inputDir();
    if (input.x != -faceX || input.y != -faceY) return;

    float pullX = -faceX;
    float pullY = -faceY;

    // Levers don't travel: pulling one flips the handle instead (the hero
    // still does the pull stumble) and the link group's barriers follow
    // like a switch group — a re-pull flips it back.
    if (objInteracts(o, OBJINT_LEVER))
    {
        o.switchActive = !o.switchActive;
        PlaySound(sndButton);
        hero.startPull(pullX, pullY);
        return;
    }

    // The object's destination is the hero's current tile (it follows the
    // hero); the hero's is one tile further back. Both must be clear.
    int tileX = (int)((o.x + TILE_SIZE * 0.5f) / TILE_SIZE);
    int tileY = (int)((o.y + TILE_SIZE * 0.5f) / TILE_SIZE);
    float destX = (float)((tileX + (int)pullX) * TILE_SIZE);
    float destY = (float)((tileY + (int)pullY) * TILE_SIZE);
    Rectangle destBox = { destX + 6.0f, destY + 12.0f, 20.0f, 16.0f };
    if (objectBlockedExcept(destBox, grabbedIndex)) return;

    Rectangle heroBox = hero.box();
    Rectangle heroDest = { heroBox.x + pullX * TILE_SIZE,
                           heroBox.y + pullY * TILE_SIZE,
                           heroBox.width, heroBox.height };
    if (room.solidBox(heroDest)) return;

    o.state = OBJ_SLIDING;
    o.slideFromX = o.x;
    o.slideFromY = o.y;
    o.slideToX = destX;
    o.slideToY = destY;
    o.slideT = 0.0f;
    o.pushTimer = 0.0f;
    hero.startPull(pullX, pullY);
    PlaySound(sndMoveObject);
}

// Timed switches ("mode": "timed"): how long the plate holds after the
// weight leaves.
static const float TIMED_SWITCH_SECONDS = 3.0f;

// Toggle items (Byrna aura, magic cape): pressing the item button flips
// them on; each drains a magic half every 2 seconds while on and falls
// off the moment the meter empties. Byrna's aura makes the hero
// untouchable and stings whatever presses against it; the cape turns
// the hero ghostly and enemies lose sight of them.
static void updateToggleItems(float dt)
{
    if (byrnaActive)
    {
        byrnaDrain += dt;
        if (byrnaDrain >= 2.0f)
        {
            byrnaDrain = 0.0f;
            inventory.magic--;
            if (inventory.magic <= 0)
            {
                inventory.magic = 0;
                byrnaActive = false;
            }
        }
        byrnaSting += dt;
        if (byrnaSting >= 0.8f)
        {
            // The aura stings whatever is pressed against the hero.
            byrnaSting = 0.0f;
            Rectangle aura = hero.box();
            for (int e = 0; e < enemyCount; e++)
            {
                if (!enemies[e].alive()) continue;
                if (!overlap(aura, enemies[e].box())) continue;
                Vector2 c = enemies[e].center();
                if (enemies[e].tryHit(aura, nextBeamSwingId--,
                                       Vector2{ c.x - hero.center().x,
                                                c.y - hero.center().y }))
                {
                    PlaySound(sndEnemyDie);
                    spawnPoof(c.x, c.y);
                    int drop = rollEnemyDrop(enemies[e]);
                    if (drop != PICKUP_NONE)
                        spawnPickup(drop, c.x - 8.0f, c.y - 8.0f);
                }
            }
        }
    }
    if (capeActive)
    {
        capeDrain += dt;
        if (capeDrain >= 2.0f)
        {
            capeDrain = 0.0f;
            inventory.magic--;
            if (inventory.magic <= 0)
            {
                inventory.magic = 0;
                capeActive = false;
            }
        }
    }
    hero.setProtected(byrnaActive || capeActive);
    hero.setCloaked(capeActive);
}

// Somaria-style use-actions ("spawn" on the item): the item button
// conjures the linked block type in front of the hero (on clear ground),
// or shatters it where it stands — push it onto switches for puzzles.
// Room-local: the block is gone when the room reloads.
static void toggleSomariaBlock(const ItemConfig& item)
{
    if (item.spawnObject < 0 || item.spawnObject >= OBJECT_COUNT)
    {
        PlaySound(sndError);
        return;
    }

    // Existing block of the type: shatter it.
    for (int i = 0; i < MAX_OBJECTS; i++)
    {
        WorldObject& o = objects[i];
        if (!o.active || o.type != item.spawnObject) continue;
        spawnPoof(o.x + 16.0f, o.y + 16.0f);
        o.active = false;
        return;
    }

    // None out there: conjure one on the tile in front of the hero.
    Rectangle heroBox = hero.box();
    Direction facing = hero.facingDirection();
    float dx = (facing == DIR_LEFT) ? -1.0f : (facing == DIR_RIGHT) ? 1.0f : 0.0f;
    float dy = (facing == DIR_UP) ? -1.0f : (facing == DIR_DOWN) ? 1.0f : 0.0f;
    int tx = (int)((heroBox.x + heroBox.width * 0.5f + dx * 22.0f) / TILE_SIZE);
    int ty = (int)((heroBox.y + heroBox.height * 0.5f + dy * 22.0f) / TILE_SIZE);
    Rectangle spot = { (float)(tx * TILE_SIZE) + 4.0f,
                       (float)(ty * TILE_SIZE) + 4.0f, 24.0f, 24.0f };
    if (room.solidBox(spot))  // walls, other objects, the hero itself
    {
        PlaySound(sndError);
        return;
    }
    for (int i = 0; i < MAX_OBJECTS; i++)
    {
        if (objects[i].active) continue;
        WorldObject& o = objects[i];
        o.active = true;
        o.type = item.spawnObject;
        o.state = OBJ_GROUND;
        o.x = (float)(tx * TILE_SIZE);
        o.y = (float)(ty * TILE_SIZE);
        o.groundX = o.x;
        o.groundY = o.y;
        o.velX = 0.0f;
        o.velY = 0.0f;
        o.z = 0.0f;
        o.vz = 0.0f;
        o.aux1 = -1;
        o.aux2 = 0;
        o.aux3 = 0;
        o.pushTimer = 0.0f;
        o.pushDirX = 0.0f;
        o.pushDirY = 0.0f;
        o.slideFromX = o.x;
        o.slideFromY = o.y;
        o.slideToX = o.x;
        o.slideToY = o.y;
        o.slideT = 0.0f;
        o.switchActive = false;
        o.opened = false;
        o.path = 0;
        o.pathCount = 0;
        o.pathIndex = 0;
        o.pathT = 0.0f;
        o.modeTimer = 0.0f;
        spawnPoof(o.x + 16.0f, o.y + 16.0f);
        return;
    }
    PlaySound(sndError);  // no free object slot this visit
}

// Moving platforms (OBJINT_PLATFORM): a rail of tile waypoints glided in a
// loop at raft speed. Whoever stands on the platform (the hero) rides
// along with its per-frame delta, and riding suppresses tile hazards —
// a raft over deep water carries the hero without a swim.
static const float PLATFORM_SPEED = 34.0f;  // px/s along the rail

static void updatePlatforms(float dt)
{
    bool riding = false;
    for (int i = 0; i < MAX_OBJECTS; i++)
    {
        WorldObject& o = objects[i];
        if (!o.active || !objInteracts(o, OBJINT_PLATFORM)) continue;
        if (o.pathCount < 2 || !o.path) continue;

        float oldX = o.x;
        float oldY = o.y;

        // Glide the current leg; short legs may complete several times in
        // one frame (guarded against pathological loops).
        int guard = 0;
        while (guard++ < 8)
        {
            int from = o.pathIndex;
            int to = (from + 1) % o.pathCount;
            float fx = (float)(o.path[from * 2] * TILE_SIZE);
            float fy = (float)(o.path[from * 2 + 1] * TILE_SIZE);
            float tx = (float)(o.path[to * 2] * TILE_SIZE);
            float ty = (float)(o.path[to * 2 + 1] * TILE_SIZE);
            float legX = tx - fx;
            float legY = ty - fy;
            float legLen = sqrtf(legX * legX + legY * legY);
            if (legLen <= 0.0001f)
            {
                o.pathIndex = to;
                continue;
            }
            o.pathT += dt * PLATFORM_SPEED / legLen;
            if (o.pathT < 1.0f)
            {
                o.x = fx + legX * o.pathT;
                o.y = fy + legY * o.pathT;
                break;
            }
            // Leg complete: land exactly on the waypoint and take the next.
            o.x = tx;
            o.y = ty;
            o.pathIndex = to;
            o.pathT = 0.0f;
        }

        // Carry whoever stands on it (the hero), along with the delta.
        float dx = o.x - oldX;
        float dy = o.y - oldY;
        if (dx != 0.0f || dy != 0.0f)
        {
            if (overlap(hero.box(), objectBox(o)))
            {
                riding = true;
                hero.nudge(dx, dy);
            }
        }
        else if (overlap(hero.box(), objectBox(o)))
        {
            riding = true;  // stationary this frame, still standing on it
        }
    }
    hero.setRiding(riding);
}

// Hammer pound: once the windup lands (HAMMER_WINDUP), the quake shakes
// the screen and the pound box shatters hammer-smashable objects (rocks,
// pots, stakes). One quake per swing, deduped by the action id.
static void updateHammer()
{
    static int lastPoundSwing = -1;
    if (!hero.hammering()) return;
    if (hero.stateElapsed() < Hero::HAMMER_WINDUP) return;
    if (lastPoundSwing == hero.swingId()) return;
    lastPoundSwing = hero.swingId();

    PlaySound(sndHammerPound);
    shakeTime = 0.2f;
    shakeStrength = 2.5f;

    Rectangle box = hero.attackBox();
    if (box.width <= 0.0f) return;
    for (int i = 0; i < MAX_OBJECTS; i++)
    {
        const WorldObject& o = objects[i];
        if (!o.active || !objInteracts(o, OBJINT_HAMMER_SMASH)) continue;
        if (o.state != OBJ_GROUND) continue;
        if (!overlap(box, objectBox(o))) continue;
        smashObject(i, true);
    }
}

// Magic powder (use-action item): once the windup lands, the puff in the
// hero's facing stuns enemies it catches, lights torch posts, and
// awakens shrine objects — setting their baked story flag (a sprinkle
// secret that persists in the save). One puff per action id.
static void updatePowder()
{
    static int lastSprinkleSwing = -1;
    if (!hero.sprinkling()) return;
    if (hero.stateElapsed() < Hero::SPRINKLE_WINDUP) return;
    if (lastSprinkleSwing == hero.swingId()) return;
    lastSprinkleSwing = hero.swingId();

    Rectangle box = hero.attackBox();
    if (box.width <= 0.0f) return;

    // Enemies caught in the puff reel stunned in a sparkle shower.
    for (int e = 0; e < enemyCount; e++)
    {
        if (!enemies[e].alive()) continue;
        if (!overlap(box, enemies[e].box())) continue;
        enemies[e].stun(2.0f);
        Vector2 c = enemies[e].center();
        spawnParticles(c.x, c.y, 8, 150, 220, 120, 70.0f, 110.0f);
        PlaySound(sndBoomerangHit);
    }

    for (int i = 0; i < MAX_OBJECTS; i++)
    {
        WorldObject& o = objects[i];
        if (!o.active || o.state != OBJ_GROUND) continue;
        if (!overlap(box, objectBox(o))) continue;

        // Torches catch fire from a sprinkle, like a fire bolt.
        if (objInteracts(o, OBJINT_TORCH) && !o.opened)
        {
            o.opened = true;
            PlaySound(sndFire);
        }

        // Shrine-style awakening: the sprinkle sets its story flag
        // (aux2 through the shared world-flag registry — it persists).
        if (objInteracts(o, OBJINT_POWDER) && !o.switchActive
            && o.aux2 >= 0 && o.aux2 < WORLD_FLAG_COUNT)
        {
            gWorldFlags[o.aux2] = true;
            o.switchActive = true;  // awakened (room-local visual state)
            spawnParticles(o.x + 16.0f, o.y + 12.0f, 12, 170, 230, 140,
                           80.0f, 130.0f);
            playNamedSfx("secret.wav");
        }
    }
}

// Bug net (use-action item): once the windup lands, the swipe in the
// facing bottles catchable critters and winged fairies (first empty
// bottle) and agitates everything else — no damage, just a dither.
// One sweep per action id.
static void updateNet()
{
    static int lastNetSwing = -1;
    if (!hero.netting()) return;
    if (hero.stateElapsed() < Hero::NET_WINDUP) return;
    if (lastNetSwing == hero.swingId()) return;
    lastNetSwing = hero.swingId();

    Rectangle box = hero.attackBox();
    if (box.width <= 0.0f) return;

    int slot = inventory.firstEmptyBottle();

    for (int e = 0; e < enemyCount; e++)
    {
        Enemy& enemy = enemies[e];
        if (!enemy.alive() || enemy.isArenaLock()) continue;
        if (!overlap(box, enemy.box())) continue;

        // Catchable critter (bee) with a bottle waiting: swept inside.
        if (enemy.catchable() && slot >= 0)
        {
            Vector2 c = enemy.center();
            spawnPoof(c.x, c.y);
            enemy.despawn();
            inventory.bottleContents[slot] = Inventory::BOTTLE_BEE;
            slot = inventory.firstEmptyBottle();
            playNamedSfx("bee.wav");
            continue;
        }

        // Otherwise the swish just aggravates: it dithers about,
        // faster and erratic, for a beat.
        enemy.agitate(3.0f);
        Vector2 c = enemy.center();
        spawnParticles(c.x, c.y, 4, 255, 230, 120, 50.0f, 90.0f);
    }

    // Fairies on the wing get bottled too.
    for (int i = 0; i < MAX_PICKUPS && slot >= 0; i++)
    {
        Pickup& p = pickups[i];
        if (!p.active || p.kind != PICKUP_FAIRY) continue;
        Rectangle item = { p.x, p.y, 16.0f, 16.0f };
        if (!overlap(box, item)) continue;
        p.active = false;
        spawnParticles(p.x + 8.0f, p.y + 8.0f, 8, 255, 200, 220, 70.0f, 110.0f);
        inventory.bottleContents[slot] = Inventory::BOTTLE_FAIRY;
        slot = inventory.firstEmptyBottle();
        PlaySound(sndFairy);
    }
}

// Shovel (use-action item): once the windup lands, the tile ahead turns
// over — a dirt burst + drop roll on soft soil, with the room's buried
// items surfacing at their spots first. One dig turn per action id.
static void updateShovel()
{
    static int lastDigSwing = -1;
    if (!hero.digging()) return;
    if (hero.stateElapsed() < Hero::DIG_WINDUP) return;
    if (lastDigSwing == hero.swingId()) return;
    lastDigSwing = hero.swingId();

    Rectangle heroBox = hero.box();
    Direction facing = hero.facingDirection();
    float dx = (facing == DIR_LEFT) ? -1.0f : (facing == DIR_RIGHT) ? 1.0f : 0.0f;
    float dy = (facing == DIR_UP) ? -1.0f : (facing == DIR_DOWN) ? 1.0f : 0.0f;
    float px = heroBox.x + heroBox.width * 0.5f + dx * 22.0f;
    float py = heroBox.y + heroBox.height * 0.5f + dy * 22.0f;
    int tx = (int)(px / TILE_SIZE);
    int ty = (int)(py / TILE_SIZE);
    TileMeta meta = room.tileMetaAt(tx, ty);
    if (!(meta.flags & TILE_FLAG_DIGGABLE)) return;
    // No digging into walls (solid tiles don't turn over).
    if (room.solidTilesBox(Rectangle{ (float)(tx * TILE_SIZE) + 4.0f,
                                      (float)(ty * TILE_SIZE) + 4.0f, 24.0f, 24.0f }))
        return;

    PlaySound(sndDig);
    spawnParticles((float)(tx * TILE_SIZE) + 16.0f, (float)(ty * TILE_SIZE) + 14.0f,
                   10, 128, 96, 60, 80.0f, 150.0f);

    // Buried treasure at this spot surfaces (once per visit).
    const RoomConfig& cfg = room.config();
    for (int i = 0; i < cfg.buriedCount && i < MAX_BURIED; i++)
    {
        if (buriedDug[i]) continue;
        if (cfg.buried[i].tileX != tx || cfg.buried[i].tileY != ty) continue;
        buriedDug[i] = true;
        spawnPickup(cfg.buried[i].kind,
                    (float)(tx * TILE_SIZE) + 8.0f, (float)(ty * TILE_SIZE) + 8.0f);
        return;
    }

    // Empty soil: the standard drop roll (mostly nothing, like LttP digs).
    int drop = rollDrop(worldRng);
    if (drop != PICKUP_NONE)
        spawnPickup(drop, (float)(tx * TILE_SIZE) + 8.0f,
                    (float)(ty * TILE_SIZE) + 8.0f);
}

static WorldObject* switchAtTile(int tileX, int tileY)
{
    for (int i = 0; i < MAX_OBJECTS; i++)
    {
        WorldObject& s = objects[i];
        if (!s.active || !objInteracts(s, OBJINT_SWITCH)) continue;
        int sx = (int)((s.x + TILE_SIZE * 0.5f) / TILE_SIZE);
        int sy = (int)((s.y + TILE_SIZE * 0.5f) / TILE_SIZE);
        if (sx == tileX && sy == tileY) return &s;
    }
    return 0;
}

static void updateSwitches(float dt)
{
    // Press state: weight = the hero standing on the plate or a block
    // resting on it; latch switches stay pressed once triggered, timed
    // ones hold for a few seconds after the weight leaves.
    Rectangle heroBox = hero.box();
    for (int i = 0; i < MAX_OBJECTS; i++)
    {
        WorldObject& s = objects[i];
        if (!s.active || !objInteracts(s, OBJINT_SWITCH)) continue;

        Rectangle trigger = { s.x + 5.0f, s.y + 5.0f, 22.0f, 22.0f };
        bool weight = overlap(trigger, heroBox);
        if (!weight)
        {
            for (int j = 0; j < MAX_OBJECTS; j++)
            {
                WorldObject& b = objects[j];
                if (!b.active || !objInteracts(b, OBJINT_WEIGHT)) continue;
                if (b.state != OBJ_GROUND && b.state != OBJ_SUNK) continue;
                if (overlap(trigger, objectBox(b))) { weight = true; break; }
            }
        }

        if (s.aux1 == 1)
        {
            // Latch: pressed forever once weight lands on it.
            if (weight && !s.switchActive)
            {
                s.switchActive = true;
                PlaySound(sndButton);
            }
        }
        else if (s.aux1 == 2)
        {
            // Timed: holds for TIMED_SWITCH_SECONDS after the weight
            // leaves (every weighted frame refills the clock).
            if (weight)
            {
                if (!s.switchActive)
                {
                    s.switchActive = true;
                    PlaySound(sndButton);
                }
                s.modeTimer = TIMED_SWITCH_SECONDS;
            }
            else if (s.switchActive)
            {
                s.modeTimer -= dt;
                if (s.modeTimer <= 0.0f)
                {
                    s.switchActive = false;
                    PlaySound(sndButton);
                }
            }
        }
        else if (weight != s.switchActive)
        {
            s.switchActive = weight;
            PlaySound(sndButton);
        }
    }

    // Barriers lower while any switch in their link group is active.
    // Colored barriers (blue/red) follow the crystal-switch state instead.
    bool crystalRed = (WORLD_FLAG_CRYSTAL_RED >= 0
                       && WORLD_FLAG_CRYSTAL_RED < WORLD_FLAG_COUNT
                       && gWorldFlags[WORLD_FLAG_CRYSTAL_RED]);
    for (int i = 0; i < MAX_OBJECTS; i++)
    {
        WorldObject& b = objects[i];
        if (!b.active || !objInteracts(b, OBJINT_BARRIER)) continue;

        if (b.aux3 == BARRIER_BLUE || b.aux3 == BARRIER_RED)
        {
            // Crystal barrier: lowered while the crystal matches its color.
            bool lowered = (b.aux3 == BARRIER_BLUE) ? !crystalRed : crystalRed;

            if (lowered && b.state == OBJ_GROUND)
            {
                b.state = OBJ_SUNK;
                PlaySound(sndDoorOpen);
            }
            else if (!lowered && b.state == OBJ_SUNK)
            {
                b.state = OBJ_GROUND;
                PlaySound(sndDoorClose);
            }

            // Wrong approach: touching a raised crystal barrier shocks.
            if (b.state == OBJ_GROUND)
            {
                Rectangle shock = { b.x + 2.0f, b.y + 2.0f, 28.0f, 28.0f };
                if (overlap(shock, hero.box()))
                {
                    Vector2 hc = hero.center();
                    hero.hurt(1, Vector2{ hc.x - (b.x + TILE_SIZE * 0.5f),
                                          hc.y - (b.y + TILE_SIZE * 0.5f) });
                }
            }
            continue;
        }

        bool lowered = false;
        for (int j = 0; j < MAX_OBJECTS && !lowered; j++)
        {
            const WorldObject& s = objects[j];
            if (!s.active) continue;
            if (objInteracts(s, OBJINT_SWITCH) && s.aux2 == b.aux2 && s.switchActive)
                lowered = true;
            // Lit torches latch their link group open.
            if (objInteracts(s, OBJINT_TORCH) && s.aux2 == b.aux2 && s.opened)
                lowered = true;
            // Pulled levers latch their link group open (a grab-pull flips
            // the handle either way, so a re-pull closes the gate again).
            if (objInteracts(s, OBJINT_LEVER) && s.aux2 == b.aux2 && s.switchActive)
                lowered = true;
        }

        if (lowered && b.state == OBJ_GROUND)
        {
            b.state = OBJ_SUNK;
            PlaySound(sndDoorOpen);
        }
        else if (!lowered && b.state == OBJ_SUNK)
        {
            b.state = OBJ_GROUND;
            PlaySound(sndDoorClose);
        }
    }
}

static void updateObjects()
{
    float dt = frameDt();

    // Grab-and-pull pairs (holding a statue, dragging it a tile).
    updateGrabPull();

    // Push blocks: wind up, slide, settle.
    updatePushBlocks(dt);
    for (int i = 0; i < MAX_OBJECTS; i++)
    {
        WorldObject& o = objects[i];
        if (!o.active || o.state != OBJ_SLIDING) continue;

        o.slideT += dt;
        float t = o.slideT / SLIDE_DURATION;
        if (t >= 1.0f)
        {
            float dirX = (o.slideToX > o.slideFromX) ? 1.0f
                        : (o.slideToX < o.slideFromX) ? -1.0f : 0.0f;
            float dirY = (o.slideToY > o.slideFromY) ? 1.0f
                        : (o.slideToY < o.slideFromY) ? -1.0f : 0.0f;
            o.x = o.slideToX;
            o.y = o.slideToY;
            o.state = OBJ_GROUND;
            o.pushTimer = 0.0f;

            // Settle-shove: the hero squeezed onto the destination gets
            // pushed back out along the slide direction.
            int guard = 0;
            while (overlap(hero.box(), objectBox(o)) && guard < 10)
            {
                hero.nudge(-dirX * 4.0f, -dirY * 4.0f);
                guard++;
            }

            // Latch switch underneath: the block sinks in and latches it.
            int tileX = (int)((o.x + TILE_SIZE * 0.5f) / TILE_SIZE);
            int tileY = (int)((o.y + TILE_SIZE * 0.5f) / TILE_SIZE);
            WorldObject* sw = switchAtTile(tileX, tileY);
            if (sw && sw->aux1 == 1)
            {
                o.state = OBJ_SUNK;
                if (!sw->switchActive)
                {
                    sw->switchActive = true;
                    PlaySound(sndButton);
                }
            }
        }
        else
        {
            o.x = o.slideFromX + (o.slideToX - o.slideFromX) * t;
            o.y = o.slideFromY + (o.slideToY - o.slideFromY) * t;
        }
    }

    // Switches and their linked barriers.
    updateSwitches(dt);

    // Hoist: the object glides from its tile to over the hero's head while
    // the lift pause plays.
    if (liftingIndex >= 0)
    {
        WorldObject& o = objects[liftingIndex];
        if (!o.active)
        {
            liftingIndex = -1;
        }
        else if (hero.state() == HERO_LIFT)
        {
            float t = clampf(hero.stateElapsed() / Hero::LIFT_DURATION, 0.0f, 1.0f);
            Vector2 c = hero.center();
            float targetX = c.x - 16.0f;
            float targetY = c.y - 38.0f;
            o.x = o.groundX + (targetX - o.groundX) * t;
            o.y = o.groundY + (targetY - o.groundY) * t;
            o.z = t * 22.0f;
        }
        else if (hero.carrying())
        {
            o.state = OBJ_CARRIED;
            carriedIndex = liftingIndex;
            liftingIndex = -1;
        }
        else
        {
            // Lift got cancelled (e.g. hurt mid-hoist): fumble smash.
            smashObject(liftingIndex, true);
            liftingIndex = -1;
        }
    }

    // Carried: ride overhead; drop-smash if the hero gets interrupted.
    if (carriedIndex >= 0 && objects[carriedIndex].active)
    {
        WorldObject& o = objects[carriedIndex];
        HeroState hs = hero.state();
        if (hs == HERO_HURT || hs == HERO_FALL || hs == HERO_SPLASH || hs == HERO_DEAD)
        {
            smashObject(carriedIndex, true);
        }
        else
        {
            Vector2 c = hero.center();
            o.x = c.x - 16.0f;
            o.y = c.y - 38.0f;
            o.z = 22.0f;
        }
    }

    // Thrown: arc flight until it hits an enemy, a wall, or the ground.
    for (int i = 0; i < MAX_OBJECTS; i++)
    {
        WorldObject& o = objects[i];
        if (!o.active || o.state != OBJ_THROWN) continue;

        o.x += o.velX * dt;
        o.y += o.velY * dt;
        o.z += o.vz * dt;
        o.vz -= 320.0f * dt;

        Rectangle flight = objectBox(o);

        // Enemies take the hit (knocked back from the pot); the pot
        // shatters on the first contact.
        bool hitEnemy = false;
        for (int e = 0; e < enemyCount; e++)
        {
            if (!enemies[e].alive()) continue;
            if (!overlap(flight, enemies[e].box())) continue;
            Vector2 ec = enemies[e].center();
            Vector2 knock = { ec.x - o.x - 16.0f, ec.y - o.y - 16.0f };
            if (enemies[e].tryHit(flight, o.throwId, knock))
            {
                PlaySound(sndEnemyDie);
                spawnPoof(ec.x, ec.y);
                int drop = rollEnemyDrop(enemies[e]);
                if (drop != PICKUP_NONE)
                    spawnPickup(drop, ec.x - 8.0f, ec.y - 8.0f);
            }
            hitEnemy = true;
            break;
        }
        if (hitEnemy)
        {
            smashObject(i, true);
            continue;
        }

        // Walls (tile colliders and other ground objects).
        if (room.solidBox(flight))
        {
            smashObject(i, true);
            continue;
        }

        // Touchdown: splash into water, smash everywhere else.
        if (o.z <= 0.0f)
        {
            int tx = (int)((o.x + 16.0f) / TILE_SIZE);
            int ty = (int)((o.y + 16.0f) / TILE_SIZE);
            TileMeta meta = room.tileMetaAt(tx, ty);
            if (meta.flags & (TILE_FLAG_WATER_DEEP | TILE_FLAG_WATER_SHALLOW))
            {
                objects[i].active = false;
                PlaySound(sndItemWater);
                spawnParticles(o.x + 16.0f, o.y + 12.0f, 8, 140, 180, 235,
                               70.0f, 130.0f);
            }
            else
            {
                smashObject(i, true);
            }
        }
    }
}

static void updateEnemies()
{
    if (hero.dead()) return;

    // The cape (magic cloak): enemies lose sight of the hero entirely —
    // the AI (enemy.update below) chases a ghost far offscreen, while
    // combat math keeps the true center; intangibility blocks their
    // strikes through hero protection.
    Vector2 heroCenter = hero.center();
    Rectangle heroBox = hero.box();
    bool dashContact = hero.dashing();
    Rectangle attack = dashContact ? hero.box() : hero.attackBox();
    int swing = hero.swingId();
    // Only offensive states strike through the attack box (the powder
    // flick and bug-net swipe expose a facing box for their own effect
    // passes, not for sword-style damage).
    bool offensive = hero.attacking() || hero.spinning() || hero.dashing()
                     || hero.hammering();

    for (int i = 0; i < enemyCount; i++)
    {
        Enemy& enemy = enemies[i];
        if (!enemy.alive()) continue;

        enemy.update(room, capeActive ? Vector2{ heroCenter.x, heroCenter.y - 9000.0f }
                                      : heroCenter,
                      worldRng, frameDt());

        // Offensive contact (sword arc, spin radius, dash): knockback away
        // from the hero; one hit per action id.
        Vector2 enemyCenter = enemy.center();
        float dx = enemyCenter.x - heroCenter.x;
        float dy = enemyCenter.y - heroCenter.y;
        int hitsBefore = enemy.hitsLanded();
        if (offensive && enemy.tryHit(attack, swing, Vector2{ dx, dy }))
        {
            // The killing blow lands heavier; boss armor rings loudest.
            hitPause = enemy.isBoss() ? HIT_PAUSE_BOSS : HIT_PAUSE_KILL;
            if (enemy.isBoss())
            {
                // Boss defeat: big blast, the arena unlocks, a heart
                // container rises, and the boss stays dead.
                PlaySound(sndBossDies);
                playJingle(sndFanfare);
                shakeTime = 0.5f;
                shakeStrength = 6.0f;
                screenFlash(0.30f, Color{ 255, 255, 255, 255 }, 0.50f);
                spawnExplosion(enemyCenter.x, enemyCenter.y);
                spawnExplosion(enemyCenter.x - 28.0f, enemyCenter.y - 18.0f);
                spawnExplosion(enemyCenter.x + 28.0f, enemyCenter.y + 18.0f);
                int bossSlot = currentRoom * 16 + i;
                if (bossSlot >= 0 && bossSlot < MAX_BOSS_SLOTS)
                    bossDefeated[bossSlot] = true;
                // The victory banks the room's story flag
                // ("boss_defeated_<room>", generator-baked) so dialogue
                // can branch on it; it persists in the save like any flag.
                const RoomConfig& rc = room.config();
                if (rc.bossFlag >= 0 && rc.bossFlag < WORLD_FLAG_COUNT)
                    gWorldFlags[rc.bossFlag] = true;
                saveToFile(saveSlotInUse);

                itemGet.active = true;
                itemGet.dialogueOpened = false;
                itemGet.t = 0.0f;
                itemGet.grantKind = GRANT_HEART_CONTAINER;
                itemGet.grantParam = 0;
                itemGet.pickupFrame = grantPickupFrame(GRANT_HEART_CONTAINER);
                itemGet.startX = enemyCenter.x;
                itemGet.startY = enemyCenter.y;
                itemGet.endX = heroCenter.x;
                itemGet.endY = heroCenter.y - 34.0f;
                itemGet.itemX = itemGet.startX;
                itemGet.itemY = itemGet.startY;
                continue;
            }
            PlaySound(sndEnemyDie);
            spawnPoof(enemyCenter.x, enemyCenter.y);
            int drop = rollEnemyDrop(enemy);
            if (drop != PICKUP_NONE)
                spawnPickup(drop, enemyCenter.x - 8.0f, enemyCenter.y - 8.0f);
            continue;
        }

        // Non-lethal connect: a lighter hit-pause beat. Hammer pounds
        // also flip the enemy over — it reels stunned on its back.
        if (enemy.hitsLanded() > hitsBefore)
        {
            hitPause = enemy.isBoss() ? HIT_PAUSE_BOSS : HIT_PAUSE_CONNECT;
            if (hero.hammering()) enemy.stun(1.2f);
        }

        // Octorok spit.
        float sdx = 0.0f;
        float sdy = 0.0f;
        if (enemy.popShot(sdx, sdy))
        {
            spawnProjectile(enemyCenter.x + sdx * 16.0f - 8.0f,
                            enemyCenter.y + sdy * 16.0f - 8.0f, sdx, sdy, 1);
            PlaySound(sndShot);
        }

        // Contact damage: pushed away from the enemy (dashing plows
        // through without taking damage).
        if (!dashContact && overlap(enemy.box(), heroBox))
        {
            float hdx = heroCenter.x - enemyCenter.x;
            float hdy = heroCenter.y - enemyCenter.y;
            hero.hurt(enemy.contactDamage(), Vector2{ hdx, hdy });
        }
    }
}

static void updateProjectiles()
{
    Rectangle heroBox = hero.box();
    for (int i = 0; i < 8; i++)
    {
        Projectile& p = projectiles[i];
        if (!p.active) continue;

        p.x += p.dx * p.speed * frameDt();
        p.y += p.dy * p.speed * frameDt();

        Rectangle rock = { p.x, p.y, 16.0f, 16.0f };

        // Shield block: while free to move, rocks arriving from the faced
        // direction get knocked away at a deflected angle. Holding the
        // guard button raises the shield and widens the blocked arc to
        // nearly everything but a projectile from directly behind.
        if (!p.friendly && !p.beam && hero.shielding())
        {
            Vector2 face = { 0.0f, 0.0f };
            switch (hero.facingDirection())
            {
            case DIR_DOWN: face.y = 1.0f; break;
            case DIR_UP: face.y = -1.0f; break;
            case DIR_RIGHT: face.x = 1.0f; break;
            case DIR_LEFT: face.x = -1.0f; break;
            default: break;
            }
            float vlen = sqrtf(p.dx * p.dx + p.dy * p.dy);
            if (vlen > 0.0001f)
            {
                float oncoming = -(p.dx / vlen * face.x + p.dy / vlen * face.y);
                float blockArc = hero.guarding() ? -0.2f : 0.45f;
                if (oncoming > blockArc && overlap(rock, heroBox))
                {
                    // Deflect: reflect and jitter the flight angle.
                    float angle = atan2f(p.dy, p.dx) + 3.14159f
                                  + (worldRng.nextFloat() - 0.5f) * 1.2f;
                    p.dx = cosf(angle);
                    p.dy = sinf(angle);
                    p.friendly = true;
                    PlaySound(sndTink);
                    continue;
                }
            }
        }

        // Sword batting: an offensive swing/spin/pound that catches an
        // incoming projectile reflects it back along its flight —
        // reflectable magic (the wizard falls to his own volleys).
        if (!p.friendly && !p.beam
            && (hero.attacking() || hero.spinning() || hero.hammering()))
        {
            Rectangle swing = hero.attackBox();
            if (swing.width > 0.0f && overlap(rock, swing))
            {
                p.dx = -p.dx;
                p.dy = -p.dy;
                p.friendly = true;
                PlaySound(sndTink);
                continue;
            }
        }

        bool isMagic = (p.element != ELEMENT_NONE);

        if (room.solidBox(rock))
        {
            p.active = false;
            if ((!p.beam && !p.arrow && !isMagic) || p.arrow) PlaySound(sndShotWall);
            continue;
        }

        // Fire bolts light torches as they pass (latched; links to barriers).
        if (p.element == ELEMENT_FIRE)
        {
            for (int i = 0; i < MAX_OBJECTS; i++)
            {
                WorldObject& torch = objects[i];
                if (!torch.active || !objInteracts(torch, OBJINT_TORCH)) continue;
                if (torch.state != OBJ_GROUND || torch.opened) continue;
                if (!overlap(rock, objectBox(torch))) continue;
                torch.opened = true;
                PlaySound(sndFire);
            }
        }

        // Beams/arrows/bolts dissipate when they leave the visible screen.
        if (p.beam || p.arrow || isMagic)
        {
            float sc = virtualScale();
            float left = camera.target.x - (float)GetScreenWidth() * 0.5f / sc;
            float top = camera.target.y - VIRTUAL_HEIGHT * 0.5f;
            float right = left + (float)GetScreenWidth() / sc;
            float bottom = top + VIRTUAL_HEIGHT;
            if (p.x < left || p.x > right || p.y < top || p.y > bottom)
            {
                p.active = false;
                continue;
            }
        }

        if (p.beam || p.arrow || (p.friendly && !isMagic))
        {
            // Sword beams, arrows, and deflected projectiles strike enemies
            // (one hit per id); all stop on the first enemy touched.
            for (int e = 0; e < enemyCount; e++)
            {
                if (!enemies[e].alive()) continue;
                if (!overlap(rock, enemies[e].box())) continue;

                Vector2 enemyCenter = enemies[e].center();
                float kx = enemyCenter.x - hero.center().x;
                float ky = enemyCenter.y - hero.center().y;
                if (p.arrow)
                {
                    // Arrows knock along their flight direction.
                    kx = p.dx;
                    ky = p.dy;
                }
                if (enemies[e].tryHit(rock, p.id, Vector2{ kx, ky },
                                      p.enemyDamage))
                {
                    PlaySound(sndEnemyDie);
                    spawnPoof(enemyCenter.x, enemyCenter.y);
                    int drop = rollEnemyDrop(enemies[e]);
                    if (drop != PICKUP_NONE)
                        spawnPickup(drop, enemyCenter.x - 8.0f, enemyCenter.y - 8.0f);
                }
                p.active = false;
                break;
            }
        }
        else if (isMagic)
        {
            // Rod bolts: fire burns (and shatters frozen enemies), ice
            // freezes solid.
            for (int e = 0; e < enemyCount; e++)
            {
                if (!enemies[e].alive()) continue;
                if (!overlap(rock, enemies[e].box())) continue;

                Vector2 enemyCenter = enemies[e].center();
                if (p.element == ELEMENT_FIRE)
                {
                    bool killed = enemies[e].tryHit(rock, p.id,
                                                    Vector2{ p.dx, p.dy });
                    if (killed)
                    {
                        PlaySound(sndEnemyDie);
                        spawnPoof(enemyCenter.x, enemyCenter.y);
                        int drop = rollEnemyDrop(enemies[e]);
                        if (drop != PICKUP_NONE)
                            spawnPickup(drop, enemyCenter.x - 8.0f, enemyCenter.y - 8.0f);
                    }
                }
                else
                {
                    enemies[e].freeze(4.0f);
                    PlaySound(sndBoomerangHit);
                }
                p.active = false;
                break;
            }
        }
        else if (!p.friendly && overlap(rock, heroBox))
        {
            p.active = false;
            Vector2 heroCenter = hero.center();
            hero.hurt(p.damage, Vector2{ heroCenter.x - (p.x + 8.0f),
                                          heroCenter.y - (p.y + 8.0f) });
        }
    }
}

static void updatePickups()
{
    Rectangle heroBox = hero.box();
    for (int i = 0; i < MAX_PICKUPS; i++)
    {
        Pickup& p = pickups[i];
        if (!p.active) continue;

        p.bobTime += frameDt();
        if (p.life > 0.0f)
        {
            p.life -= frameDt();
            if (p.life <= 0.0f)
            {
                p.active = false;
                continue;
            }
        }

        Rectangle item = { p.x, p.y, 16.0f, 16.0f };
        if (overlap(item, heroBox)) collectPickup(p);
    }
}

static void updateBoomerang()
{
    if (!boomer.active) return;
    float dt = frameDt();
    boomer.time += dt;

    if (!boomer.returning)
    {
        // Outbound leg: straight flight with a lateral arc wobble.
        float along = boomer.time * boomer.speed;
        float perp = sinf(boomer.time * 14.0f) * 16.0f;
        float nx = boomer.startX + boomer.dirX * along - boomer.dirY * perp;
        float ny = boomer.startY + boomer.dirY * along + boomer.dirX * perp;

        Rectangle flight = { nx - 8.0f, ny - 8.0f, 16.0f, 16.0f };
        if (along >= boomer.range || room.solidBox(flight))
        {
            boomer.returning = true;
            return;
        }
        boomer.x = nx;
        boomer.y = ny;
    }
    else
    {
        // Homing return to the hero.
        Vector2 heroCenter = hero.center();
        float dx = heroCenter.x - boomer.x;
        float dy = heroCenter.y - boomer.y;
        float dist = sqrtf(dx * dx + dy * dy);
        if (dist < 14.0f)
        {
            boomer.active = false;  // caught
            return;
        }
        float move = 230.0f * dt;
        if (move > dist) move = dist;
        boomer.x += dx / dist * move;
        boomer.y += dy / dist * move;
    }

    // Stun enemies it touches (once per stun period).
    Rectangle flight = { boomer.x - 8.0f, boomer.y - 8.0f, 16.0f, 16.0f };
    for (int e = 0; e < enemyCount; e++)
    {
        if (!enemies[e].alive()) continue;
        if (!overlap(flight, enemies[e].box())) continue;
        enemies[e].stun(1.2f);
        PlaySound(sndBoomerangHit);
    }

    // Fetch pickups it passes through.
    for (int i = 0; i < MAX_PICKUPS; i++)
    {
        if (!pickups[i].active) continue;
        Rectangle item = { pickups[i].x, pickups[i].y, 16.0f, 16.0f };
        if (overlap(flight, item)) collectPickup(pickups[i]);
    }
}

// Hookshot (see struct): the chain head extends, latches hookables
// (reeling the hero to the tile in front of the latch), bounces off
// anything solid, stuns enemies and collects pickups in passing, and
// recoils home on a miss. The hero stays locked in HERO_HOOKSHOT until
// the chain resolves.
static void updateHookshot()
{
    if (!hook.active) return;
    float dt = frameDt();
    Vector2 heroCenter = hero.center();

    if (hook.latched)
    {
        // Reeling: the hero glides (the hero state owns the motion);
        // the chain resolves once the hero has landed.
        if (!hero.hookPulling())
        {
            hook.active = false;
            hook.latched = false;
        }
        return;
    }

    Rectangle head = { hook.x - 6.0f, hook.y - 6.0f, 12.0f, 12.0f };

    // Stun enemies / collect pickups the head passes through.
    for (int e = 0; e < enemyCount; e++)
    {
        if (!enemies[e].alive()) continue;
        if (!overlap(head, enemies[e].box())) continue;
        enemies[e].stun(1.2f);
        PlaySound(sndBoomerangHit);
        hook.retracting = true;
    }
    for (int i = 0; i < MAX_PICKUPS; i++)
    {
        if (!pickups[i].active) continue;
        Rectangle item = { pickups[i].x, pickups[i].y, 16.0f, 16.0f };
        if (overlap(head, item)) collectPickup(pickups[i]);
    }

    // Latch: a hookable object grabs the chain — the hero reels in to
    // the tile in front of the latch (approach side), gliding over
    // anything between.
    for (int i = 0; i < MAX_OBJECTS; i++)
    {
        const WorldObject& o = objects[i];
        if (!o.active || !objInteracts(o, OBJINT_HOOKABLE)) continue;
        if (o.state != OBJ_GROUND) continue;
        if (!overlap(head, objectBox(o))) continue;

        hook.latched = true;
        hook.retracting = false;
        hook.x = o.x + TILE_SIZE * 0.5f;
        hook.y = o.y + TILE_SIZE * 0.5f;
        float landX = o.x - hook.dirX * TILE_SIZE;
        float landY = o.y - hook.dirY * TILE_SIZE;
        hero.startHookPull(landX, landY);
        PlaySound(sndHookshot);
        return;
    }

    if (hook.retracting)
    {
        // Recoil: fly home, resolving the shot on arrival.
        float dx = heroCenter.x - hook.x;
        float dy = heroCenter.y - hook.y;
        float dist = sqrtf(dx * dx + dy * dy);
        float move = HOOK_SPEED * dt;
        if (move >= dist)
        {
            hook.active = false;
            hero.endHookshot();
            return;
        }
        hook.x += dx / dist * move;
        hook.y += dy / dist * move;
        return;
    }

    // Extending: walls bounce the chain, range exhausts it.
    hook.x += hook.dirX * HOOK_SPEED * dt;
    hook.y += hook.dirY * HOOK_SPEED * dt;
    Rectangle probe = { hook.x - 4.0f, hook.y - 4.0f, 8.0f, 8.0f };
    if (room.solidBox(probe))
    {
        hook.retracting = true;
        return;
    }
    float dx = hook.x - hook.startX;
    float dy = hook.y - hook.startY;
    if (dx * dx + dy * dy >= HOOK_RANGE * HOOK_RANGE)
    {
        hook.retracting = true;  // out of wire
    }
}

// Draw the hookshot chain: a dotted wire from the hero to the head,
// with a claw tip.
static void drawHookshotChain()
{
    if (!hook.active) return;
    Vector2 from = hero.center();
    float dx = hook.x - from.x;
    float dy = hook.y - from.y;
    float dist = sqrtf(dx * dx + dy * dy);
    if (dist < 1.0f) return;
    float ux = dx / dist;
    float uy = dy / dist;
    for (float d = 4.0f; d < dist - 2.0f; d += 8.0f)
    {
        DrawLine((int)(from.x + ux * d), (int)(from.y + uy * d),
                 (int)(from.x + ux * (d + 4.0f)), (int)(from.y + uy * (d + 4.0f)),
                 Color{ 225, 225, 235, 255 });
    }
    DrawCircle((int)hook.x, (int)hook.y, 3.0f, Color{ 235, 235, 245, 255 });
    DrawCircle((int)(hook.x - hook.dirX * 3.0f), (int)(hook.y - hook.dirY * 3.0f),
               2.0f, Color{ 140, 140, 155, 255 });
}

static void updatePoofs()
{
    for (int i = 0; i < 32; i++)
    {
        if (!poofs[i].active) continue;
        poofs[i].age += frameDt();
        if (poofs[i].age >= 0.4f) poofs[i].active = false;
    }
}

static void drawGroundFx()
{
    // Draw inside the camera transform: pickups, poofs, particles, rocks,
    // boomerang.
    drawParticles();
    for (int i = 0; i < MAX_PICKUPS; i++)
    {
        const Pickup& p = pickups[i];
        if (!p.active) continue;
        float bob = sinf(p.bobTime * 3.0f) * 2.0f;
        Rectangle src = { (float)(p.kind * 16), 0.0f, 16.0f, 16.0f };
        Rectangle dest = { p.x, p.y + bob, 16.0f, 16.0f };
        Vector2 origin = { 0.0f, 0.0f };
        DrawTexturePro(pickupsTexture, src, dest, origin, 0.0f, WHITE);
    }

    for (int i = 0; i < 32; i++)
    {
        const Poof& poof = poofs[i];
        if (!poof.active) continue;
        float t = poof.age / 0.4f;
        float radius = 6.0f + t * 12.0f;
        unsigned char alpha = (unsigned char)((int)(255 * (1.0f - t)));
        DrawCircle((int)poof.x, (int)poof.y, radius, Color{ 230, 230, 240, alpha });
        DrawCircle((int)poof.x, (int)poof.y, radius * 0.55f, Color{ 200, 200, 210, alpha });
    }

    for (int i = 0; i < 8; i++)
    {
        const Projectile& p = projectiles[i];
        if (!p.active) continue;
        Vector2 origin = { 0.0f, 0.0f };
        if (p.element != ELEMENT_NONE)
        {
            float cx = p.x + 8.0f;
            float cy = p.y + 8.0f;
            if (p.element == ELEMENT_FIRE)
            {
                float flicker = sinf((float)GetTime() * 30.0f) * 1.5f;
                DrawCircle((int)cx, (int)cy, 6.5f + flicker, Color{ 240, 120, 40, 235 });
                DrawCircle((int)cx, (int)cy, 4.0f + flicker * 0.6f, Color{ 255, 210, 110, 245 });
                DrawCircle((int)cx, (int)(cy - 2.0f), 2.0f, Color{ 255, 250, 220, 255 });
            }
            else
            {
                DrawCircle((int)cx, (int)cy, 6.5f, Color{ 120, 190, 250, 220 });
                DrawCircle((int)cx, (int)cy, 4.0f, Color{ 200, 235, 255, 245 });
            }
            continue;
        }
        if (p.arrow)
        {
            // Arrow pickup frame, rotated to face the flight direction.
            Rectangle src = { 64.0f, 0.0f, 16.0f, 16.0f };
            float rotation = 0.0f;
            switch (p.dir)
            {
            case DIR_LEFT: rotation = 180.0f; break;
            case DIR_DOWN: rotation = 90.0f; break;
            case DIR_UP: rotation = -90.0f; break;
            default: break;
            }
            Rectangle dest = { p.x + 8.0f, p.y + 8.0f, 16.0f, 16.0f };
            Vector2 center = { 8.0f, 8.0f };
            DrawTexturePro(pickupsTexture, src, dest, center, rotation, WHITE);
            continue;
        }
        Rectangle src = p.beam ? Rectangle{ (float)(p.dir * 16), 0.0f, 16.0f, 16.0f }
                               : Rectangle{ 48.0f, 0.0f, 16.0f, 16.0f };
        Rectangle dest = { p.x, p.y, 16.0f, 16.0f };
        DrawTexturePro(p.beam ? beamTexture : pickupsTexture, src, dest, origin, 0.0f, WHITE);
    }

    if (boomer.active && boomerangTexture.id != 0)
    {
        int frame = (int)(boomer.time * 18.0f) % 4;
        Rectangle src = { (float)(frame * 16), 0.0f, 16.0f, 16.0f };
        Rectangle dest = { boomer.x - 8.0f, boomer.y - 8.0f, 16.0f, 16.0f };
        Vector2 origin = { 0.0f, 0.0f };
        DrawTexturePro(boomerangTexture, src, dest, origin, 0.0f, WHITE);
    }

    // Hookshot chain: a dotted wire from the hero out to the claw.
    drawHookshotChain();

    // Byrna aura: protective sparks orbiting the hero while it burns.
    if (byrnaActive)
    {
        Vector2 c = hero.center();
        for (int i = 0; i < 6; i++)
        {
            float a = (float)GetTime() * 5.0f + i * (PI * 2.0f / 6.0f);
            DrawCircle((int)(c.x + cosf(a) * 17.0f), (int)(c.y + sinf(a) * 17.0f),
                       2.0f, Color{ 255, 240, 170, 230 });
        }
    }

    // Live bombs: blink faster as the fuse runs out.
    for (int i = 0; i < MAX_BOMBS_LIVE; i++)
    {
        if (!bombs[i].active) continue;
        float urgency = (BOMB_FUSE - bombs[i].fuse) / BOMB_FUSE;
        if (urgency > 0.5f && ((int)(bombs[i].fuse * (6.0f + urgency * 18.0f)) % 2 == 0))
            continue;  // flicker
        Rectangle src = { 80.0f, 0.0f, 16.0f, 16.0f };
        Rectangle dest = { bombs[i].x - 8.0f, bombs[i].y - 10.0f, 16.0f, 16.0f };
        Vector2 origin = { 0.0f, 0.0f };
        DrawTexturePro(pickupsTexture, src, dest, origin, 0.0f, WHITE);
    }

    // Explosions: expanding scorch rings with a white-hot core flash.
    for (int i = 0; i < MAX_EXPLOSIONS; i++)
    {
        const Explosion& ex = explosions[i];
        if (!ex.active) continue;
        float t = ex.age / 0.45f;
        float radius = 10.0f + t * BOMB_RADIUS;
        unsigned char alpha = (unsigned char)((int)(210 * (1.0f - t)));
        DrawCircle((int)ex.x, (int)ex.y, radius * 0.55f,
                   Color{ 255, 220, 140, (unsigned char)(alpha / 2) });
        DrawCircleLines((int)ex.x, (int)ex.y, radius, Color{ 250, 240, 220, alpha });
        if (t < 0.3f)
        {
            unsigned char flash = (unsigned char)((int)(200 * (1.0f - t / 0.3f)));
            DrawCircle((int)ex.x, (int)ex.y, radius * 0.7f, Color{ 255, 255, 255, flash });
        }
    }
}

static void drawFloorObjects()
{
    // Floor switches, warp pads and sunk objects render under everything
    // that stands.
    for (int i = 0; i < MAX_OBJECTS; i++)
    {
        const WorldObject& o = objects[i];
        if (!o.active) continue;
        if (objInteracts(o, OBJINT_SWITCH))
        {
            Rectangle src = { 0.0f, 0.0f, 32.0f, 32.0f };
            Rectangle dest = { o.x, o.y, 32.0f, 32.0f };
            Vector2 origin = { 0.0f, 0.0f };
            DrawTexturePro(objectSheets[o.type], src, dest, origin, 0.0f, WHITE);
            if (o.switchActive)
            {
                // Pressed plate: dark inset button.
                DrawRectangle((int)(o.x + 10.0f), (int)(o.y + 10.0f), 12, 12,
                              Color{ 96, 90, 82, 255 });
            }
            continue;
        }
        if (objInteracts(o, OBJINT_WARP))
        {
            // Warp pad: gently pulsing so it reads as a step-on trigger.
            Rectangle src = { 0.0f, 0.0f, 32.0f, 32.0f };
            Rectangle dest = { o.x, o.y, 32.0f, 32.0f };
            Vector2 origin = { 0.0f, 0.0f };
            DrawTexturePro(objectSheets[o.type], src, dest, origin, 0.0f, WHITE);
            float pulse = 0.5f + 0.5f * sinf((float)GetTime() * 4.0f);
            unsigned char alpha = (unsigned char)(int)(90 + 100 * pulse);
            DrawRectangle((int)(o.x + 8.0f), (int)(o.y + 8.0f), 16, 16,
                          Color{ 120, 200, 255, alpha });
            continue;
        }
        if (objInteracts(o, OBJINT_PLATFORM))
        {
            // Moving platform: rides under everything that stands on it.
            Rectangle src = { 0.0f, 0.0f, 32.0f, 32.0f };
            Rectangle dest = { o.x, o.y, 32.0f, 32.0f };
            Vector2 origin = { 0.0f, 0.0f };
            DrawTexturePro(objectSheets[o.type], src, dest, origin, 0.0f, WHITE);
            continue;
        }
        if (o.state == OBJ_SUNK)
        {
            // Sunk into the floor: squashed flat and darkened.
            Rectangle src = { 0.0f, 0.0f, 32.0f, 32.0f };
            Rectangle dest = { o.x, o.y + 22.0f, 32.0f, 10.0f };
            Vector2 origin = { 0.0f, 0.0f };
            DrawTexturePro(objectSheets[o.type], src, dest, origin, 0.0f,
                           Color{ 130, 130, 140, 255 });
        }
    }
}

// Depth/y-sort: hero, enemies and standing objects draw back-to-front by
// baseline (bottom of the collision box), like LttP. Everything taller
// south paints over what stands north of it.
static void drawEntitiesSorted()
{
    struct Drawable
    {
        int kind;      // 0 hero, 1 enemy, 2 object, 3 npc
        int index;
        float baseline;
    };
    Drawable list[1 + 16 + MAX_OBJECTS + MAX_NPCS];
    int n = 0;

    list[n].kind = 0;
    list[n].index = 0;
    Rectangle heroBox = hero.box();
    list[n].baseline = heroBox.y + heroBox.height;
    n++;

    for (int i = 0; i < enemyCount; i++)
    {
        if (!enemies[i].alive()) continue;
        Rectangle b = enemies[i].box();
        list[n].kind = 1;
        list[n].index = i;
        list[n].baseline = b.y + b.height;
        n++;
    }

    for (int i = 0; i < MAX_OBJECTS; i++)
    {
        const WorldObject& o = objects[i];
        if (!o.active) continue;
        if (o.state != OBJ_GROUND && o.state != OBJ_SLIDING) continue;
        if (!objInteracts(o, OBJINT_SOLID)) continue;
        Rectangle b = objectBox(o);
        list[n].kind = 2;
        list[n].index = i;
        list[n].baseline = b.y + b.height;
        n++;
    }

    for (int i = 0; i < MAX_NPCS; i++)
    {
        if (!npcs[i].active) continue;
        Rectangle b = npcBox(npcs[i]);
        list[n].kind = 3;
        list[n].index = i;
        list[n].baseline = b.y + b.height;
        n++;
    }

    // Insertion sort by baseline (stable, tiny n).
    for (int i = 1; i < n; i++)
    {
        Drawable key = list[i];
        int j = i - 1;
        while (j >= 0 && list[j].baseline > key.baseline)
        {
            list[j + 1] = list[j];
            j--;
        }
        list[j + 1] = key;
    }

    for (int i = 0; i < n; i++)
    {
        const Drawable& d = list[i];
        if (d.kind == 0)
        {
            HeroState hs = hero.state();
            if (hs != HERO_FALL && hs != HERO_SPLASH && hs != HERO_DEAD
                && hs != HERO_SWIM)
            {
                // Tree-blob shadow at the feet.
                DrawEllipse((int)(heroBox.x + heroBox.width * 0.5f),
                           (int)(heroBox.y + heroBox.height - 2.0f),
                           8.0f, 3.0f, Color{ 20, 20, 40, 70 });
            }
            // Wading in shallow water: a mirrored copy below the feet.
            if (hero.wading()) hero.drawReflection();
            hero.draw();
        }
        else if (d.kind == 1)
        {
            Rectangle b = enemies[d.index].box();
            DrawEllipse((int)(b.x + b.width * 0.5f), (int)(b.y + b.height - 2.0f),
                        8.0f, 3.0f, Color{ 20, 20, 40, 70 });
            TileMeta em = room.tileMetaAt((int)((b.x + b.width * 0.5f) / TILE_SIZE),
                                          (int)((b.y + b.height * 0.5f) / TILE_SIZE));
            if (em.flags & TILE_FLAG_WATER_SHALLOW) enemies[d.index].drawReflection();
            enemies[d.index].draw();
        }
        else if (d.kind == 3)
        {
            const Npc& npc = npcs[d.index];
            Rectangle b = npcBox(npcs[d.index]);
            DrawEllipse((int)(b.x + b.width * 0.5f), (int)(b.y + b.height - 2.0f),
                        8.0f, 3.0f, Color{ 20, 20, 40, 70 });
            // Wading NPCs reflect too (same frame the draw below shows).
            TileMeta em = room.tileMetaAt((int)((b.x + b.width * 0.5f) / TILE_SIZE),
                                          (int)((b.y + b.height * 0.5f) / TILE_SIZE));
            if ((em.flags & TILE_FLAG_WATER_SHALLOW) && npc.cfg && npc.sheet.id != 0)
            {
                const DirectionalFrames* rframes = npc.moving
                    ? &npc.cfg->move[npc.facing] : &npc.cfg->stand[npc.facing];
                if (rframes->count <= 0) rframes = &npc.cfg->stand[npc.facing];
                if (rframes->count > 0)
                {
                    int rcolumns = npc.cfg->sheetColumns;
                    if (rcolumns <= 0) rcolumns = 1;
                    int rindex = (int)(npc.animTime * 8.0f) % rframes->count;
                    int rframe = rframes->frames[rindex];
                    if (rframe < 0) rframe = 0;
                    if (npc.cfg->frameCount > 0 && rframe >= npc.cfg->frameCount)
                        rframe = npc.cfg->frameCount - 1;
                    Rectangle rsrc = { (float)((rframe % rcolumns) * TILE_SIZE),
                                      (float)((rframe / rcolumns) * TILE_SIZE),
                                      (float)TILE_SIZE, -(float)TILE_SIZE };
                    Rectangle rdest = { npc.x, npc.y + (float)TILE_SIZE,
                                        (float)TILE_SIZE, (float)TILE_SIZE * 0.62f };
                    Vector2 rorigin = { 0.0f, 0.0f };
                    DrawTexturePro(npc.sheet, rsrc, rdest, rorigin, 0.0f,
                                   Color{ 175, 200, 240, 100 });
                }
            }
            if (npc.cfg && npc.sheet.id != 0)
            {
                const DirectionalFrames* frames = npc.moving
                    ? &npc.cfg->move[npc.facing] : &npc.cfg->stand[npc.facing];
                if (frames->count <= 0) frames = &npc.cfg->stand[npc.facing];
                if (frames->count > 0)
                {
                    int columns = npc.cfg->sheetColumns;
                    if (columns <= 0) columns = 1;
                    int index = (int)(npc.animTime * 8.0f) % frames->count;
                    int frame = frames->frames[index];
                    if (frame < 0) frame = 0;
                    if (npc.cfg->frameCount > 0 && frame >= npc.cfg->frameCount)
                        frame = npc.cfg->frameCount - 1;
                    Rectangle src = { (float)((frame % columns) * TILE_SIZE),
                                     (float)((frame / columns) * TILE_SIZE),
                                     (float)TILE_SIZE, (float)TILE_SIZE };
                    Vector2 pos = { npc.x, npc.y };
                    DrawTextureRec(npc.sheet, src, pos, WHITE);
                }
            }
        }
        else
        {
            const WorldObject& o = objects[d.index];
            // Chests have a two-frame sheet: closed / lid open. Levers
            // mirror their single frame: the handle leans with the state.
            float srcX = (objInteracts(o, OBJINT_TWO_FRAME) && o.opened) ? 32.0f : 0.0f;
            float srcW = 32.0f;
            if (objInteracts(o, OBJINT_LEVER) && o.switchActive) srcW = -32.0f;
            Rectangle src = { srcX, 0.0f, srcW, 32.0f };
            Rectangle dest = { o.x, o.y, 32.0f, 32.0f };
            Vector2 origin = { 0.0f, 0.0f };
            DrawTexturePro(objectSheets[o.type], src, dest, origin, 0.0f, WHITE);

            // Lit torches burn with a flickering flame over the bowl.
            if (objInteracts(o, OBJINT_TORCH) && o.opened)
            {
                float flicker = sinf((float)GetTime() * 22.0f + o.x) * 1.5f;
                float fx = o.x + 16.0f;
                float fy = o.y + 9.0f + sinf((float)GetTime() * 13.0f + o.y) * 1.0f;
                DrawCircle((int)fx, (int)fy, 5.5f + flicker, Color{ 240, 130, 40, 235 });
                DrawCircle((int)fx, (int)(fy - 2.0f), 3.5f + flicker * 0.7f,
                           Color{ 255, 220, 120, 245 });
                DrawCircle((int)fx, (int)(fy - 3.5f), 1.8f, Color{ 255, 250, 220, 255 });
            }

            // Crystal switch: the gem glows blue or red with the state.
            if (objInteracts(o, OBJINT_CRYSTAL))
            {
                bool red = (WORLD_FLAG_CRYSTAL_RED >= 0
                            && WORLD_FLAG_CRYSTAL_RED < WORLD_FLAG_COUNT
                            && gWorldFlags[WORLD_FLAG_CRYSTAL_RED]);
                float pulse = 0.5f + 0.5f * sinf((float)GetTime() * 6.0f);
                Color core = red ? Color{ 235, 90, 90, 255 } : Color{ 110, 150, 235, 255 };
                Color halo = core;
                halo.a = (unsigned char)(int)(110 + 90 * pulse);
                DrawCircle((int)(o.x + 16.0f), (int)(o.y + 8.0f), 5.0f, halo);
                DrawCircle((int)(o.x + 16.0f), (int)(o.y + 8.0f), 3.0f, core);
            }
        }
    }
}

// Item-get hold-up: the grant hovers over the hero with sparkle rays
// (after the sorted entity pass so it rides above everything).
static void drawItemGet()
{
    if (!itemGet.active) return;

    // Sparkle rays orbiting the grant.
    for (int i = 0; i < 6; i++)
    {
        float angle = itemGet.t * 3.0f + (float)i * 1.047f;
        float sx = itemGet.itemX + cosf(angle) * 14.0f;
        float sy = itemGet.itemY + sinf(angle) * 10.0f;
        DrawCircle((int)sx, (int)sy, 1.5f, Color{ 255, 240, 160, 230 });
    }

    if (itemGet.pickupFrame >= 0)
    {
        Rectangle src = { (float)(itemGet.pickupFrame * 16), 0.0f, 16.0f, 16.0f };
        Rectangle dest = { itemGet.itemX - 8.0f, itemGet.itemY - 8.0f, 16.0f, 16.0f };
        Vector2 origin = { 0.0f, 0.0f };
        DrawTexturePro(pickupsTexture, src, dest, origin, 0.0f, WHITE);
    }
    else
    {
        // Medallion placeholder for items/keys with no pickup sprite.
        DrawCircle((int)itemGet.itemX, (int)itemGet.itemY, 9.0f, Color{ 216, 168, 60, 255 });
        DrawCircle((int)itemGet.itemX, (int)itemGet.itemY, 6.0f, Color{ 250, 232, 160, 255 });
    }
}

// Lifted, carried and thrown objects ride above the hero sprite: drawn
// after the hero so the pot is visually held overhead.
static void drawObjectsOverhead()
{
    for (int i = 0; i < MAX_OBJECTS; i++)
    {
        const WorldObject& o = objects[i];
        if (!o.active || o.state == OBJ_GROUND) continue;
        if (o.state == OBJ_THROWN)
        {
            // Arc shadow on the ground plane.
            float shrink = clampf(1.0f - o.z / 40.0f, 0.4f, 1.0f);
            DrawEllipse((int)(o.x + 16.0f), (int)(o.y + 22.0f),
                        8.0f * shrink, 3.5f * shrink, Color{ 20, 20, 40, 90 });
        }
        Rectangle src = { 0.0f, 0.0f, 32.0f, 32.0f };
        Rectangle dest = { o.x, o.y - o.z, 32.0f, 32.0f };
        Vector2 origin = { 0.0f, 0.0f };
        DrawTexturePro(objectSheets[o.type], src, dest, origin, 0.0f, WHITE);
    }
}

// Map-edge exit: pressed into a map boundary along the held direction with
// a linked room -> glide into it.
static void maybeStartRoomTransition()
{
    if (roomTransition.active || dialogue.isActive() || hero.dead()) return;

    const RoomConfig& cfg = room.config();
    Vector2 dir = hero.inputDir();
    bool drifting = false;  // a push-tile drift, not a player press
    if (dir.x == 0.0f && dir.y == 0.0f)
    {
        // Water current routing: a push tile drifts the hero with no
        // input at all, and the drift carries it off a linked edge just
        // like a walk — the river decides where the hero goes. A current
        // presses the hero against a closed door but never works the
        // door itself (no key spends, no slabs push open).
        int tileX = 0;
        int tileY = 0;
        hero.tileUnder(tileX, tileY);
        TileMeta meta = room.tileMetaAt(tileX, tileY);
        dir.x = (float)meta.pushX / 48.0f;
        dir.y = (float)meta.pushY / 48.0f;
        if (dir.x == 0.0f && dir.y == 0.0f) return;
        drifting = true;
    }

    Rectangle hb = hero.box();
    float mapW = (float)(cfg.mapWidth * TILE_SIZE);
    float mapH = (float)(cfg.mapHeight * TILE_SIZE);

    int motion = -1;
    int target = -1;
    int edge = -1;
    // The hero clamps its sprite inside the map, so its box rests a few px
    // off each boundary — the thresholds allow for that slack.
    if (dir.y < 0.0f && hb.y <= 24.0f && cfg.exitNorth >= 0)
    {
        motion = DIR_UP;
        target = cfg.exitNorth;
        edge = 0;
    }
    else if (dir.y > 0.0f && hb.y + hb.height >= mapH - 8.0f && cfg.exitSouth >= 0)
    {
        motion = DIR_DOWN;
        target = cfg.exitSouth;
        edge = 1;
    }
    else if (dir.x < 0.0f && hb.x <= 12.0f && cfg.exitWest >= 0)
    {
        motion = DIR_LEFT;
        target = cfg.exitWest;
        edge = 3;
    }
    else if (dir.x > 0.0f && hb.x + hb.width >= mapW - 12.0f && cfg.exitEast >= 0)
    {
        motion = DIR_RIGHT;
        target = cfg.exitEast;
        edge = 2;
    }

    if (motion < 0 || target < 0 || target >= ROOM_COUNT) return;

    // Doors (and the boss arena lock) gate the edge. Pushing handles the
    // door first (it opens with a cue); walking through happens on the next
    // press. A drift (push tile) can only ride through an already-open
    // door — currents never work the lock.
    if (!doorIsOpen(currentRoom, edge))
    {
        if (drifting) return;
        // Door rules are data (DOOR_RULES): keys, push-open slabs, and
        // everything else the kind's rule composes.
        int kind = doorKind(currentRoom, edge);
        int keyCost = 0;
        bool pushOpen = false;
        if (kind >= 0 && kind < DOOR_KIND_COUNT)
        {
            keyCost = DOOR_RULES[kind].keyCost;
            pushOpen = DOOR_RULES[kind].pushOpen;
        }
        if (keyCost == 1)
        {
            if (inventory.smallKeys > 0)
            {
                inventory.smallKeys--;
                markDoorOpened(currentRoom, edge);
                PlaySound(sndUseKey);
                PlaySound(sndDoorOpen);
            }
            else PlaySound(sndError);
        }
        else if (keyCost == 2)
        {
            if (inventory.bigKeys > 0)
            {
                inventory.bigKeys--;
                markDoorOpened(currentRoom, edge);
                PlaySound(sndUseKey);
                PlaySound(sndDoorOpen);
            }
            else PlaySound(sndError);
        }
        else if (pushOpen)
        {
            markDoorOpened(currentRoom, edge);
            PlaySound(sndDoorOpen);
        }
        else  // bombable (needs a bomb), shutter (win the fight), or the
              // boss arena lock
        {
            PlaySound(sndError);
        }
        return;
    }

    loadRoom(target, motion);
    saveToFile(saveSlotInUse);  // LttP-style autosave on room change
    showToast("SAVED");
}

static void updateRoomTransition(float dt)
{
    // Glide the hero from the off-map start to the entry tile (ease-out);
    // the camera branch below follows it unclamped until it lands.
    roomTransition.t += dt / ROOM_TRANSITION_DURATION;
    if (roomTransition.t >= 1.0f)
    {
        roomTransition.active = false;
        return;
    }
    float e = 1.0f - (1.0f - roomTransition.t) * (1.0f - roomTransition.t);
    float nx = roomTransition.startX + (roomTransition.endX - roomTransition.startX) * e;
    float ny = roomTransition.startY + (roomTransition.endY - roomTransition.startY) * e;
    Vector2 c = hero.center();
    hero.nudge(nx - (c.x - TILE_SIZE * 0.5f), ny - (c.y - TILE_SIZE * 0.5f));
}

// Doors render at their edge midpoint: a dark open archway, or a slab
// (keyhole / cracks / shutter bars) while closed.
static void drawDoorways()
{
    const RoomConfig& cfg = room.config();
    float mapW = (float)(cfg.mapWidth * TILE_SIZE);
    float mapH = (float)(cfg.mapHeight * TILE_SIZE);

    for (int edge = 0; edge < 4; edge++)
    {
        int kind = doorKind(currentRoom, edge);
        if (kind == DOOR_NONE || kind >= DOOR_KIND_COUNT) continue;
        const DoorRule& rule = DOOR_RULES[kind];
        if (rule.startsOpen) continue;

        float cx = mapW * 0.5f;
        float cy = mapH * 0.5f;
        switch (edge)
        {
        case 0: cy = 0.0f; break;               // north wall
        case 1: cy = mapH; break;               // south wall
        case 2: cx = mapW; cy = mapH * 0.5f; break;  // east wall
        case 3: cx = 0.0f; cy = mapH * 0.5f; break;  // west wall
        default: break;
        }

        float w = 32.0f;
        float h = 48.0f;
        // Slabs run along the wall; archways open along the walk direction.
        if (edge <= 1) { w = 40.0f; h = 24.0f; }
        Rectangle slab = { cx - w * 0.5f, cy - h * 0.5f, w, h };

        if (doorIsOpen(currentRoom, edge))
        {
            // Open: dark archway with a frame.
            DrawRectangleRec(slab, Color{ 10, 10, 20, 255 });
            DrawRectangleLinesEx(slab, 3.0f, Color{ 110, 90, 60, 255 });
            continue;
        }

        Color base = Color{ 150, 104, 52, 255 };
        Color trim = Color{ 100, 66, 30, 255 };
        if (rule.liveEnemiesDead)
        {
            // Shutter-style: metal slats instead of a wooden slab.
            base = Color{ 120, 120, 134, 255 };
            trim = Color{ 70, 70, 84, 255 };
        }
        DrawRectangleRec(slab, base);
        DrawRectangleLinesEx(slab, 3.0f, trim);

        if (rule.keyCost > 0)
        {
            // Gold keyhole plate.
            DrawRectangle((int)(cx - 5.0f), (int)(cy - 6.0f), 10, 12,
                          Color{ 216, 168, 60, 255 });
            DrawCircle((int)cx, (int)cy, 2.5f, Color{ 70, 46, 22, 255 });
        }
        else if (rule.bombable)
        {
            // Crack marks over the slab.
            for (int i = 0; i < 5; i++)
            {
                float ox = -12.0f + (float)i * 6.0f;
                DrawLine((int)(cx + ox), (int)(cy - 8.0f), (int)(cx + ox + 4.0f),
                         (int)(cy + 8.0f), Color{ 40, 30, 24, 255 });
            }
        }
        else if (rule.liveEnemiesDead)
        {
            // Horizontal shutter slats.
            for (int i = 1; i < 4; i++)
            {
                float y = slab.y + slab.height * (float)i / 4.0f;
                DrawLine((int)slab.x, (int)y, (int)(slab.x + slab.width), (int)y,
                         trim);
            }
        }
    }
}

static void updateAndDraw(void)
{
    if (musicLoaded) UpdateMusicStream(music);
    if (ambientLoaded) UpdateMusicStream(ambientMusic);
    if (dangerLoaded) UpdateMusicStream(dangerMusic);

    // Danger crossfade: while an arena fight lives in a room with a
    // danger track, the layer swells in over the bgm (~1.2s each way);
    // the fight ending settles the calm track back over it.
    if (dangerLoaded)
    {
        float mixTarget = bossAliveInRoom() ? 1.0f : 0.0f;
        float rate = frameDt() / 1.2f;
        if (rate > 1.0f) rate = 1.0f;
        dangerMix += (mixTarget - dangerMix) * rate;
        if (dangerMix < 0.001f) dangerMix = 0.0f;
        if (dangerMix > 0.999f) dangerMix = 1.0f;
        if (musicLoaded) SetMusicVolume(music, 1.0f - dangerMix);
        SetMusicVolume(dangerMusic, dangerMix);
    }

    // Jingle ducking: once the fanfare/secret sting finishes, the bgm
    // picks up where it was paused.
    if (jingleDucking && !IsSoundPlaying(jingleSound))
    {
        jingleDucking = false;
        if (musicLoaded) ResumeMusicStream(music);
        if (dangerLoaded) ResumeMusicStream(dangerMusic);
    }

    if (gameState == STATE_TITLE)
    {
        updateTitle();
        BeginDrawing();
        ClearBackground(BLACK);
        drawTitleScreen();
        EndDrawing();
        return;
    }

    if (pausePressed())
    {
        if (fluteMenu) fluteMenu = false;
        else paused = !paused;
    }

    if (roomFade > 0.0f)
    {
        roomFade -= frameDt() / ROOM_FADE_DURATION;
        if (roomFade < 0.0f) roomFade = 0.0f;
    }

    if (paused)
    {
        // Item subscreen: cursor + equip (the world stays frozen).
        updatePauseMenu();
    }

    // Cutscene machinery (pans, blocking, bars) runs even while the world
    // is frozen behind a script.
    updateCutscene(frameDt());

    // Flute destination picker (world frozen while it's up). The freeze
    // outlives a same-frame cancel so the close button can't re-fire the
    // item use in the same press.
    bool fluteWasOpen = fluteMenu;
    if (fluteMenu) updateFluteMenu();

    if (dialogue.isActive())
    {
        if (!paused) dialogue.update();

        // Scripted music switches and item grants drain here (once per
        // request). "give" lines hand the item over with a toast.
        const char* musicRequest = dialogue.takeMusicRequest();
        if (musicRequest) startMusic(musicRequest);

        int giveItem = -1;
        if (dialogue.takeGiveRequest(giveItem))
        {
            inventory.give(giveItem);
            static char giveText[96];
            snprintf(giveText, sizeof(giveText), "GOT %s", ITEMS[giveItem].label);
            showToast(giveText);
            PlaySound(sndItemGet);
        }

        // Cutscene cues: camera pans/returns, letterbox state, and actor
        // blocking — one of each per shown line, like music/warp.
        {
            int camAction = 0;
            int camTileX = 0;
            int camTileY = 0;
            float camSeconds = 0.0f;
            if (dialogue.takeCameraRequest(camAction, camTileX, camTileY, camSeconds))
                applyCutsceneCameraRequest(camAction, camTileX, camTileY, camSeconds);

            int letterboxState = -1;
            if (dialogue.takeLetterboxRequest(letterboxState))
                letterboxWanted = letterboxState;

            int actor = -1;
            int moveTileX = 0;
            int moveTileY = 0;
            float moveSeconds = 0.0f;
            if (dialogue.takeMoveRequest(actor, moveTileX, moveTileY, moveSeconds))
                applyCutsceneMoveRequest(actor, moveTileX, moveTileY, moveSeconds);
        }
    }
    else if (!paused && !fluteWasOpen)
    {
        if (roomTransition.active)
        {
            // Scroll-in: world frozen while the hero walks in.
            updateRoomTransition(frameDt());
        }
        else if (itemGet.active && !itemGet.dialogueOpened)
        {
            // Item-get rise: world frozen while the grant floats up.
            updateItemGet(frameDt());
        }
        else if (hero.dead())
        {
            // Death sequence: world frozen, waiting on the continue prompt
            // (handled in drawGameOver flow below).
            hero.update(room, frameDt());
        }
        else if (bossIntroTime > 0.0f)
        {
            // Boss intro: the world holds while the letterbox names the
            // arena's boss (drawn with the world, after the HUD).
            bossIntroTime -= frameDt();
            if (bossIntroTime < 0.0f) bossIntroTime = 0.0f;
        }
        else
        {
            // Hit-pause: the world holds its breath for a beat after an
            // impactful strike connects (set by updateEnemies).
            if (hitPause > 0.0f)
            {
                hitPause -= frameDt();
                if (hitPause < 0.0f) hitPause = 0.0f;
            }
            else
            {
            int healthBefore = hero.healthHalves();
            HeroState stateBefore = hero.state();

            updateDashSmash();  // pegasus boots shatter pots/bushes pre-move
            hero.setCanSwim(swimItemOwned());
            updatePlatforms(frameDt());  // rafts glide + carry before the walk
            hero.update(room, frameDt());
            // Pit-landing: a completed fall-through warps the hero into
            // the room layer below (fade-in at the landing tile).
            {
                int pitRoom = -1;
                int pitX = 0;
                int pitY = 0;
                if (hero.takeFallRedirect(pitRoom, pitX, pitY))
                    loadRoom(pitRoom, -1, pitX, pitY);
            }
            updateHammer();  // the pound lands once the windup elapses
            updateShovel();  // the dig turns the tile ahead over
            updatePowder();   // the powder puff lands in the facing
            updateNet();      // the bug-net sweep bottles or agitates
            bool actionUsed = updateObjectActions();
            updateEncounters(!actionUsed);
            updateGrass();
            updateSwordBeam();
            if (itemPressed() && !hero.busy() && !hero.carrying())
            {
                useEquippedItem();
            }
            updateBoomerang();
            updateHookshot();
            updateNpcs(frameDt());
            updateLantern(frameDt());
            updateToggleItems(frameDt());  // Byrna/cape drains + gates
            updateHealTiles(frameDt());
            updateBombs(frameDt());
            updateObjects();
            updateEnemies();
            updateProjectiles();
            updatePickups();
            updatePoofs();
            updateParticles(frameDt());
            updateWeather(frameDt());

            // Feedback: damage taken -> screen shake; a deep-water dive
            // kicks up droplets; low health -> beeping.
            if (hero.healthHalves() < healthBefore || (stateBefore != HERO_HURT && hero.state() == HERO_HURT))
            {
                shakeTime = 0.25f;
                shakeStrength = 3.0f;
            }
            if (stateBefore != HERO_SPLASH && hero.state() == HERO_SPLASH)
            {
                Vector2 c = hero.center();
                spawnParticles(c.x, c.y - 4.0f, 10, 140, 180, 235,
                               85.0f, 160.0f);
            }

            // Warp pads underfoot (after the move so step-ons register).
            updateWarps();

            // Walking off a linked map edge glides into the next room.
            maybeStartRoomTransition();
            }
        }
    }

    // Game over: prompt to continue once the collapse finishes.
    if (hero.dead() && hero.stateElapsed() > 2.2f && actionPressed())
    {
        loadRoom(currentRoom);
        hero.revive();
    }

    // Scripted warps drain here, once the dialogue box has closed.
    {
        int warpRoom = -1;
        int warpTileX = -1;
        int warpTileY = -1;
        if (!dialogue.isActive() && !paused && !hero.dead()
            && dialogue.takeWarpRequest(warpRoom, warpTileX, warpTileY))
        {
            loadRoom(warpRoom, -1, warpTileX, warpTileY);
            saveToFile(saveSlotInUse);
            showToast("SAVED");
        }

        // Script chaining: a dialogue that ran out hands the baton to the
        // linked one (the endgame cutscene rolls into its credits).
        int nextDialogue = -1;
        if (!dialogue.isActive() && !paused && !hero.dead()
            && dialogue.takeNextRequest(nextDialogue)
            && nextDialogue >= 0 && nextDialogue < DIALOGUE_COUNT)
        {
            dialogue.open(DIALOGUES[nextDialogue]);
        }
    }

    // The item-get sequence ends when its description dialogue closes.
    if (itemGet.active && itemGet.dialogueOpened && !dialogue.isActive())
    {
        itemGet.active = false;
    }

    if (shakeTime > 0.0f) shakeTime -= frameDt();
    if (flashTime > 0.0f)
    {
        flashTime -= frameDt();
        if (flashTime < 0.0f) flashTime = 0.0f;
    }

    // Low-health beeping (flashing handled in the HUD).
    if (hero.healthHalves() > 0 && hero.healthHalves() <= 2 && !paused)
    {
        lowHpBeepTimer += frameDt();
        if (lowHpBeepTimer >= 0.42f)
        {
            lowHpBeepTimer = 0.0f;
            PlaySound(sndLowHp);
        }
    }
    else
    {
        lowHpBeepTimer = 0.0f;
    }

    float scale = virtualScale();
    camera.zoom = scale;
    camera.offset = Vector2{ (float)GetScreenWidth() * 0.5f, (float)GetScreenHeight() * 0.5f };

    float viewWidth = (float)GetScreenWidth() / scale;
    float viewHeight = VIRTUAL_HEIGHT;
    float mapWidth = (float)(room.config().mapWidth * TILE_SIZE);
    float mapHeight = (float)(room.config().mapHeight * TILE_SIZE);

    Vector2 center = hero.center();
    float camX = (mapWidth <= viewWidth) ? mapWidth * 0.5f
                                         : clampf(center.x, viewWidth * 0.5f, mapWidth - viewWidth * 0.5f);
    float camY = (mapHeight <= viewHeight) ? mapHeight * 0.5f
                                            : clampf(center.y, viewHeight * 0.5f, mapHeight - viewHeight * 0.5f);

    if (cutsceneCam.active)
    {
        // Scripted pan: smoothstep from where the camera was taken to the
        // scripted tile, then hold there until a line returns to the hero
        // (clamped to the map like the hero-follow, so pans stay sane).
        float u = (cutsceneCam.duration > 0.0f)
                      ? clampf(cutsceneCam.t / cutsceneCam.duration, 0.0f, 1.0f)
                      : 1.0f;
        float e = u * u * (3.0f - 2.0f * u);
        float px = cutsceneCam.fromX + (cutsceneCam.toX - cutsceneCam.fromX) * e;
        float py = cutsceneCam.fromY + (cutsceneCam.toY - cutsceneCam.fromY) * e;
        camX = (mapWidth <= viewWidth) ? mapWidth * 0.5f
                                       : clampf(px, viewWidth * 0.5f, mapWidth - viewWidth * 0.5f);
        camY = (mapHeight <= viewHeight) ? mapHeight * 0.5f
                                         : clampf(py, viewHeight * 0.5f, mapHeight - viewHeight * 0.5f);
    }

    if (shakeTime > 0.0f)
    {
        float amp = shakeStrength * (shakeTime / 0.25f);
        camX += (worldRng.nextFloat() * 2.0f - 1.0f) * amp;
        camY += (worldRng.nextFloat() * 2.0f - 1.0f) * amp;
    }
    camera.target = Vector2{ camX, camY };

    if (roomTransition.active)
    {
        // Scroll-in: follow the gliding hero unclamped (it starts a tile
        // off-map); it converges to the clamped position as it lands.
        camera.target = hero.center();
    }

    BeginDrawing();
    ClearBackground(BLACK);

    BeginMode2D(camera);
    room.draw(tilesetTexture, camera);
    drawFloorObjects();
    drawGroundFx();
    drawEntitiesSorted();
    drawObjectsOverhead();
    drawItemGet();
    // Decor-above layer (tree tops, archways): over everything that walks.
    room.drawUpper(tilesetTexture, camera);
    // Edge doors: archways and slabs at the wall midpoints.
    drawDoorways();
    EndMode2D();

    // Dark rooms: lantern + torch lightmap over the world (HUD stays lit).
    drawDarkness();

    // Ambient tint and weather layers, under the HUD like the lightmap.
    drawTint();
    drawWeather();

    // Room-load fade (spawns, death-continue, warps, mirror travel):
    // arrivals dip through the destination room's palette — its ambient
    // tint, darkened — instead of plain black, the LttP screen-transition
    // flavor. Neutral rooms (no tint) fade black.
    if (roomFade > 0.0f)
    {
        unsigned char alpha = (unsigned char)(int)(clampf(roomFade, 0.0f, 1.0f) * 255.0f);
        const RoomConfig& rc = room.config();
        unsigned char fr = 0;
        unsigned char fg = 0;
        unsigned char fb = 0;
        if (rc.tintR < 255 || rc.tintG < 255 || rc.tintB < 255)
        {
            fr = (unsigned char)(rc.tintR * 2 / 5);
            fg = (unsigned char)(rc.tintG * 2 / 5);
            fb = (unsigned char)(rc.tintB * 2 / 5);
        }
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(),
                      Color{ fr, fg, fb, alpha });
    }

    // Screen flash pulses (bomb blasts, boss deaths), under the HUD.
    if (flashTime > 0.0f)
    {
        float t = flashDuration > 0.0f ? flashTime / flashDuration : 0.0f;
        Color c = flashColor;
        c.a = (unsigned char)(int)(clampf(flashPeak * t, 0.0f, 1.0f) * 255.0f);
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), c);
    }

    drawHud();
    drawToasts();
    drawPauseOverlay();
    drawFluteMenu();
    drawGameOver();

    // Boss intro letterbox: bars slide in, the boss gets named, the
    // world holds underneath (see the bossIntroTime gate).
    if (bossIntroTime > 0.0f)
    {
        float scale = virtualScale();
        float elapsed = BOSS_INTRO_DURATION - bossIntroTime;
        float slide = clampf(elapsed / 0.25f, 0.0f, 1.0f);
        float barH = 26.0f * slide * scale;
        DrawRectangle(0, 0, GetScreenWidth(), (int)barH, BLACK);
        DrawRectangle(0, GetScreenHeight() - (int)barH, GetScreenWidth(),
                      (int)barH + 1, BLACK);
        if (slide >= 1.0f)
        {
            const char* name = bossIntroName[0] ? bossIntroName : "BOSS";
            int size = (int)(9.0f * scale);
            int width = MeasureText(name, size);
            DrawText(name, (GetScreenWidth() - width) / 2,
                     (int)(barH * 0.5f - size * 0.5f), size,
                     Color{ 235, 235, 245, 255 });
        }
    }

    // Cutscene letterbox: bars slide with the scripted state (drawn over
    // the world, under the dialogue/credits).
    if (letterboxT > 0.001f)
    {
        float scale = virtualScale();
        float barH = 26.0f * letterboxT * scale;
        DrawRectangle(0, 0, GetScreenWidth(), (int)barH, BLACK);
        DrawRectangle(0, GetScreenHeight() - (int)barH, GetScreenWidth(),
                      (int)barH + 1, BLACK);
    }

    dialogue.draw();

    EndDrawing();
}

int main(int argc, char** argv)
{
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(896, 448, "LttP Engine");

    InitAudioDevice();
    SetTargetFPS(60);

    tilesetTexture = LoadTexture(TILESET.image);
    SetTextureFilter(tilesetTexture, TEXTURE_FILTER_POINT);

    heartsTexture = LoadTexture("assets/ui/hearts.png");
    SetTextureFilter(heartsTexture, TEXTURE_FILTER_POINT);

    pickupsTexture = LoadTexture("assets/pickups/pickups.png");
    SetTextureFilter(pickupsTexture, TEXTURE_FILTER_POINT);

    beamTexture = LoadTexture("assets/effects/beam.png");
    SetTextureFilter(beamTexture, TEXTURE_FILTER_POINT);

    boomerangTexture = LoadTexture("assets/effects/boomerang.png");
    SetTextureFilter(boomerangTexture, TEXTURE_FILTER_POINT);

    uiBorderTexture = LoadTexture("assets/ui/border.png");
    SetTextureFilter(uiBorderTexture, TEXTURE_FILTER_POINT);

    // Character sheets (the hero + every NPC character).
    for (int i = 0; i < CHARACTER_COUNT && i < 16; i++)
    {
        characterSheets[i] = LoadTexture(CHARACTERS[i].image);
        SetTextureFilter(characterSheets[i], TEXTURE_FILTER_POINT);
    }

    for (int i = 0; i < ENEMY_COUNT && i < 16; i++)
    {
        enemySheets[i] = LoadTexture(ENEMIES[i].image);
        SetTextureFilter(enemySheets[i], TEXTURE_FILTER_POINT);
    }

    for (int i = 0; i < OBJECT_COUNT && i < 32; i++)
    {
        objectSheets[i] = LoadTexture(OBJECTS[i].image);
        SetTextureFilter(objectSheets[i], TEXTURE_FILTER_POINT);
    }

    // Ground objects block movement through the room's solidity check.
    room.setSolidExtras(objectSolidExtra, 0);

    lightGradient = LoadTexture("assets/effects/light.png");
    SetTextureFilter(lightGradient, TEXTURE_FILTER_POINT);
    lightRT = LoadRenderTexture(GetScreenWidth(), GetScreenHeight());

    dialogue.init();
    gDialogueItemOwned = itemOwnedQuery;
    gDialogueTryPayRupees = tryPayRupeesQuery;

    if (ENCOUNTER_TOTAL > 0)
    {
        encounterFired = new bool[ENCOUNTER_TOTAL];
        for (int i = 0; i < ENCOUNTER_TOTAL; i++) encounterFired[i] = false;
    }

#if !defined(PLATFORM_WEB)
    // Headless save/load soak: fresh game -> save -> reload -> re-save;
    // both blobs must be byte-identical (every persisted field round-trips,
    // including flags, registries, and the RNG state).
    if (argc > 1 && strcmp(argv[1], "--soak") == 0)
    {
        char path[128];
        saveFilePath(0, path, sizeof(path));

        startNewGame(0);
        saveToFile(0);
        int sizeA = 0;
        unsigned char* blobA = LoadFileData(path, &sizeA);

        resetGameState();
        if (!loadFromFile(0) || !blobA)
        {
            fprintf(stderr, "SOAK FAIL: save did not round trip\n");
            return 1;
        }
        saveToFile(0);
        int sizeB = 0;
        unsigned char* blobB = LoadFileData(path, &sizeB);

        // The rngState field legitimately advances across a load (spawning
        // a room's NPCs rolls the world rng), so the comparison masks it:
        // every other persisted byte must round-trip identically.
        size_t rngOffset = offsetof(SaveData, rngState);
        bool ok = blobB && sizeA == (int)sizeof(SaveData) && sizeB == sizeA
                  && memcmp(blobA, blobB, rngOffset) == 0
                  && memcmp(blobA + rngOffset + sizeof(unsigned int),
                            blobB + rngOffset + sizeof(unsigned int),
                            sizeA - rngOffset - sizeof(unsigned int)) == 0;
        if (blobA) UnloadFileData(blobA);
        if (blobB) UnloadFileData(blobB);
        fprintf(stderr, "%s (%d bytes)%s\n", ok ? "SOAK OK" : "SOAK FAIL",
                sizeA, ok ? ", round trip identical (rng state excluded)"
                          : ", blobs differ");
        CloseAudioDevice();
        CloseWindow();
        return ok ? 0 : 1;
    }
#endif

    // Title screen: the chosen slot starts (or continues) the game.
    gameState = STATE_TITLE;

    // Test-from-here (editor hook): --room <name|stem> [--tile x,y] jumps
    // straight into that room — a fresh demo start (full hearts, the demo
    // inventory) with saving suppressed, skipping the title.
    {
        const char* testRoom = 0;
        int testTileX = -1;
        int testTileY = -1;
        for (int i = 1; i < argc; i++)
        {
            if (strcmp(argv[i], "--room") == 0 && i + 1 < argc)
            {
                testRoom = argv[++i];
            }
            else if (strcmp(argv[i], "--tile") == 0 && i + 1 < argc)
            {
                int tx = -1;
                int ty = -1;
                if (sscanf(argv[++i], "%d,%d", &tx, &ty) == 2)
                {
                    testTileX = tx;
                    testTileY = ty;
                }
            }
        }
        if (testRoom && testRoom[0])
        {
            int roomIndex_ = roomIndex(testRoom);
            if (roomIndex_ >= 0)
            {
                testMode = true;
                resetGameState();
                loadRoom(roomIndex_, -1, testTileX, testTileY);
                gameState = STATE_PLAYING;
            }
            else
            {
                fprintf(stderr, "test-from-here: unknown room '%s'\n", testRoom);
            }
        }
    }

    camera.rotation = 0.0f;

#if defined(PLATFORM_WEB)
    emscripten_set_main_loop(updateAndDraw, 0, 1);
#else
    while (!WindowShouldClose()) updateAndDraw();

    startMusic(NULL);
    startAmbient(NULL);
    startDangerMusic(NULL);
    delete[] encounterFired;
    encounterFired = 0;
    hero.unload();
    room.unload();
    dialogue.shutdown();
    UnloadTexture(tilesetTexture);
    UnloadTexture(heartsTexture);
    UnloadTexture(pickupsTexture);
    UnloadTexture(beamTexture);
    UnloadTexture(boomerangTexture);
    UnloadTexture(uiBorderTexture);
    for (int i = 0; i < 16; i++)
    {
        if (characterSheets[i].id != 0) UnloadTexture(characterSheets[i]);
    }
    for (int i = 0; i < 16; i++)
    {
        if (enemySheets[i].id != 0) UnloadTexture(enemySheets[i]);
    }
    for (int i = 0; i < 16; i++)
    {
        if (objectSheets[i].id != 0) UnloadTexture(objectSheets[i]);
    }
    shutdownNamedSfx();
    UnloadTexture(lightGradient);
    if (lightRT.texture.id != 0) UnloadRenderTexture(lightRT);

    CloseAudioDevice();
    CloseWindow();
#endif
    return 0;
}