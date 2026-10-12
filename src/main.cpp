#include "engine.h"
#include "assets.h"
#include "room.h"
#include "hero.h"
#include "enemy.h"
#include "dialogue.h"

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
    int damage;  // half-hearts (rocks)
};
static Projectile projectiles[8] = {};
static int nextBeamSwingId = -1000;  // unique negative ids for beam hits

// Boomerang (the item button): flies out, homing return, stuns enemies and
// fetches pickups. One in the air at a time.
static Texture2D beamTexture = {};
static Texture2D boomerangTexture = {};
static Sound sndBoomerang = {};
static Sound sndBoomerangHit = {};
static Sound sndTink = {};
static Sound sndSwordBeam = {};

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
};
static Boomerang boomer = {};

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
    bool switchActive;    // switches: currently pressed/latched
    bool opened;           // chests: lid open (permanent until reload/save)
};
static WorldObject objects[MAX_OBJECTS] = {};
static Texture2D objectSheets[8] = {};
static int carriedIndex = -1;   // objects[] index riding overhead
static int liftingIndex = -1;   // objects[] index mid-hoist
static int nextThrowId = -2000; // unique negative ids, like beam hits
static Sound sndThrow = {};
static Sound sndBreak = {};
static Sound sndItemWater = {};
static Sound sndMoveObject = {};
static Sound sndButton = {};
static Sound sndDoorOpen = {};
static Sound sndDoorClose = {};

static Rectangle objectBox(const WorldObject& o);

// Inventory (items, quantities, wallet, magic) — process-lifetime until
// the save system lands. The demo starts with the full kit so it's all
// reachable; chests/item-get sequences will grant these properly later.
static Inventory inventory;
static float shakeTime = 0.0f;
static float shakeStrength = 0.0f;
static float lowHpBeepTimer = 0.0f;

// World sfx bank.
static Sound sndGrass = {};
static Sound sndEnemyDie = {};
static Sound sndHeart = {};
static Sound sndRupee = {};
static Sound sndLowHp = {};
static Sound sndShot = {};
static Sound sndShotWall = {};

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
    int dialogue;   // DIALOGUE index, -1 = silent
    float modeTime;
    float timer;     // wander pause countdown
    float animTime;
    bool moving;
    int lastFacingPick;  // avoids picking the same stroll direction twice
};
static Npc npcs[MAX_NPCS] = {};
static Texture2D characterSheets[16] = {};

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
        if (!o.active || o.type == OBJECT_SWITCH) continue;
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
    // like tile colliders. Floor switches are walkable plates; lifted,
    // carried, thrown and sunk things don't block. NPCs stand in the way
    // too (their own movement checks exclude themselves).
    for (int i = 0; i < MAX_OBJECTS; i++)
    {
        if (!objects[i].active) continue;
        if (objects[i].type == OBJECT_SWITCH) continue;
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
        if (objects[i].type == OBJECT_SWITCH) continue;
        if (objects[i].state != OBJ_GROUND && objects[i].state != OBJ_SLIDING) continue;
        if (overlap(box, objectBox(objects[i]))) return true;
    }
    return false;
}

static bool objectLiftable(int type)
{
    return type == OBJECT_POT || type == OBJECT_BUSH;
}

static void smashObject(int index, bool spawnDrops)
{
    WorldObject& o = objects[index];
    o.active = false;
    if (carriedIndex == index) carriedIndex = -1;
    if (liftingIndex == index) liftingIndex = -1;
    PlaySound(sndBreak);
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
        projectiles[i].dir = 0;
        projectiles[i].id = 0;
        projectiles[i].x = x;
        projectiles[i].y = y;
        projectiles[i].dx = dx;
        projectiles[i].dy = dy;
        projectiles[i].speed = 140.0f;
        projectiles[i].damage = damage;
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
        return;
    }
}

static void throwBoomerang(float x, float y, Direction facing)
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
    PlaySound(sndBoomerang);
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

static Sound sndBombExplode = {};
static Sound sndBombWall = {};
static Sound sndLayBomb = {};
static Sound sndItemGet = {};
static Sound sndError = {};
static Sound sndUiCursor = {};
static Sound sndUiButton = {};
static Sound sndChestOpen = {};
static Sound sndFanfare = {};
static Sound sndFireRod = {};
static Sound sndIceRod = {};
static Sound sndFire = {};
static Sound sndUseKey = {};
static Sound sndBossDies = {};

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

static bool bossAliveInRoom()
{
    for (int i = 0; i < enemyCount; i++)
        if (enemies[i].alive() && enemies[i].isBoss()) return true;
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

    switch (doorKind(roomIndex, edge))
    {
    case DOOR_NONE:
    case DOOR_OPEN:
        return true;
    case DOOR_SHUTTER:
        return livingEnemies() == 0;
    default:
    {
        int slot = roomIndex * 4 + edge;
        return slot >= 0 && slot < MAX_DOOR_SLOTS && doorOpened[slot];
    }
    }
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
static void updateTitle();
static void drawTitleScreen();

// Binary blob, versioned; WORLD_FLAG_COUNT / ENCOUNTER_TOTAL are extern so
// the schema uses fixed caps with bounds-checked copies. The web build
// writes into raylib's IDBFS-backed /savefile directory so it persists in
// the browser.
static void loadRoomFromSave(int room, int tileX, int tileY);
static const int SAVE_MAGIC = 0x4C544F50;  // "LTOP"
static const int SAVE_VERSION = 4;
static const int SAVE_SLOT_COUNT = 3;     // selectable once a title screen exists
static const int SAVE_FLAG_CAP = 256;
static const int SAVE_ENCOUNTER_CAP = 512;

struct SaveData
{
    int magic;
    int version;
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
    unsigned int rngState;
    bool flags[SAVE_FLAG_CAP];
    bool chests[MAX_CHEST_SLOTS];
    bool doors[MAX_DOOR_SLOTS];
    bool bosses[MAX_BOSS_SLOTS];
    bool visited[MAX_ROOM_MARKS];
    bool encounters[SAVE_ENCOUNTER_CAP];
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

    for (int i = 0; i < SAVE_FLAG_CAP; i++)
        if (WORLD_FLAG_COUNT > 0 && i < WORLD_FLAG_COUNT && i < SAVE_FLAG_CAP)
            gWorldFlags[i] = data.flags[i];
    for (int i = 0; i < MAX_CHEST_SLOTS; i++) chestOpened[i] = data.chests[i];
    for (int i = 0; i < MAX_DOOR_SLOTS; i++) doorOpened[i] = data.doors[i];
    for (int i = 0; i < MAX_BOSS_SLOTS; i++) bossDefeated[i] = data.bosses[i];
    for (int i = 0; i < MAX_ROOM_MARKS; i++) roomVisited[i] = data.visited[i];
    if (encounterFired)
    {
        int total = ENCOUNTER_TOTAL < SAVE_ENCOUNTER_CAP ? ENCOUNTER_TOTAL : SAVE_ENCOUNTER_CAP;
        for (int i = 0; i < total; i++) encounterFired[i] = data.encounters[i];
    }
    worldRng.seed(data.rngState);

    // Land in the saved room at the saved tile (fade in).
    if (data.room >= 0 && data.room < ROOM_COUNT)
    {
        loadRoomFromSave(data.room, data.tileX, data.tileY);
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

    return out.magic == SAVE_MAGIC && out.version == SAVE_VERSION;
}

static bool loadFromFile(int slot)
{
    SaveData data;
    if (!peekSave(slot, data)) return false;
    applySave(data);
    fprintf(stderr, "loaded save from slot %d (room %d)\n", slot, data.room);
    return true;
}

static void saveToFile(int slot)
{
    SaveData data;
    memset(&data, 0, sizeof(data));
    data.magic = SAVE_MAGIC;
    data.version = SAVE_VERSION;
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
    worldRng.seed(0xC0FFEE1Au);
    lanternDrain = 0.0f;
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

    const char* sub = "A LINK TO THE PAST-STYLE ENGINE DEMO";
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
            int drop = rollDrop(worldRng);
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
            if (room.bombTileAt(tx, ty)) blasted = true;
        }
    }
    if (blasted) PlaySound(sndBombWall);

    // Bombable doors on this room's edges blow open when the blast reaches.
    {
        const RoomConfig& cfg = room.config();
        float mapW = (float)(cfg.mapWidth * TILE_SIZE);
        float mapH = (float)(cfg.mapHeight * TILE_SIZE);
        for (int edge = 0; edge < 4; edge++)
        {
        if (doorKind(currentRoom, edge) != DOOR_BOMBABLE) continue;
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
        }
    }
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

// Bow: arrows cost 1 from the quiver, or one rupee when the quiver is dry
// (LttP behavior); no ammo at all -> an error cue.
static void fireArrow(float x, float y, Direction facing)
{
    if (inventory.arrows > 0) inventory.addArrows(-1);
    else if (inventory.rupees > 0) inventory.addRupees(-1);
    else
    {
        PlaySound(sndError);
        return;
    }

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
        projectiles[i].speed = 340.0f;
        projectiles[i].damage = 0;
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

// The item button runs whatever is equipped.
static void useEquippedItem()
{
    if (inventory.equippedItem < 0) return;
    Vector2 heroCenter = hero.center();
    switch (inventory.equippedItem)
    {
    case ITEM_BOOMERANG:
        throwBoomerang(heroCenter.x, heroCenter.y, hero.facingDirection());
        break;
    case ITEM_BOW:
        fireArrow(heroCenter.x, heroCenter.y, hero.facingDirection());
        break;
    case ITEM_BOMBS:
        if (inventory.bombs > 0)
        {
            inventory.addBombs(-1);
            // Placed at the hero's feet (bottom of the sprite).
            placeBomb(heroCenter.x, heroCenter.y + 8.0f);
            PlaySound(sndLayBomb);
        }
        else PlaySound(sndError);
        break;
    case ITEM_FIRE_ROD:
        if (inventory.consumeMagic(2))
        {
            spawnElementProjectile(heroCenter.x, heroCenter.y, hero.facingDirection(),
                                   ELEMENT_FIRE);
            PlaySound(sndFireRod);
        }
        else PlaySound(sndError);
        break;
    case ITEM_ICE_ROD:
        if (inventory.consumeMagic(2))
        {
            spawnElementProjectile(heroCenter.x, heroCenter.y, hero.facingDirection(),
                                   ELEMENT_ICE);
            PlaySound(sndIceRod);
        }
        else PlaySound(sndError);
        break;
    default:
        PlaySound(sndError);
        break;
    }
}

static void clearWorldSystems()
{
    for (int i = 0; i < 16; i++) enemies[i] = Enemy();
    enemyCount = 0;
    for (int i = 0; i < MAX_PICKUPS; i++) pickups[i].active = false;
    for (int i = 0; i < 32; i++) poofs[i].active = false;
    for (int i = 0; i < 8; i++) projectiles[i].active = false;
    boomer.active = false;
    for (int i = 0; i < MAX_OBJECTS; i++) objects[i].active = false;
    carriedIndex = -1;
    liftingIndex = -1;
    for (int i = 0; i < MAX_BOMBS_LIVE; i++) bombs[i].active = false;
    for (int i = 0; i < MAX_EXPLOSIONS; i++) explosions[i].active = false;
    for (int i = 0; i < MAX_NPCS; i++) npcs[i].active = false;
}

static bool encounterAvailable(const Encounter& encounter)
{
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
// the hero stands next to.
static void updateLantern(float dt)
{
    if (!room.config().dark) return;
    if (ITEM_LANTERN >= ITEM_COUNT || !inventory.owned[ITEM_LANTERN]) return;

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
            if (!torch.active || torch.type != OBJECT_TORCH) continue;
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

    // Hero light: wide with the lantern, a dying ember without it.
    bool hasLantern = (ITEM_LANTERN < ITEM_COUNT && inventory.owned[ITEM_LANTERN]);
    float heroRadius = hasLantern ? 92.0f : 30.0f;
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
        if (!o.active || o.type != OBJECT_TORCH || !o.opened) continue;
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
        const char* label = ITEMS[inventory.equippedItem].label;
        char text[24];
        int count = -1;
        if (inventory.equippedItem == ITEM_BOW) count = inventory.arrows;
        else if (inventory.equippedItem == ITEM_BOMBS) count = inventory.bombs;
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

    // Arena boss health bar, bottom-center while the fight rages.
    for (int i = 0; i < enemyCount; i++)
    {
        const Enemy& boss = enemies[i];
        if (!boss.alive() || !boss.isBoss()) continue;

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
        toasts[i].age += GetFrameTime();
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

    // Magic gauge.
    DrawText("MAGIC", (int)(qx * scale), (int)((qy + 46.0f) * scale), size, RAYWHITE);
    float barX = (qx + 30.0f) * scale;
    float barY = (qy + 46.0f) * scale + size * 0.5f - 3.0f * scale;
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
            if (meta.flags & (TILE_FLAG_PIT | TILE_FLAG_WATER_DEEP)) continue;
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
    currentRoom = index;
    const RoomConfig& cfg = ROOMS[index];
    if (index >= 0 && index < MAX_ROOM_MARKS) roomVisited[index] = true;

    room.unload();
    room.load(cfg);

    if (cfg.hero >= 0 && cfg.hero < CHARACTER_COUNT)
        hero.load(CHARACTERS[cfg.hero]);

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

    prevTileX = -1;
    prevTileY = -1;

    // Spawn the room's enemy set fresh (defeated enemies stay dead until the
    // room reloads).
    clearWorldSystems();
    enemyCount = cfg.enemyCount < 16 ? cfg.enemyCount : 16;
    for (int i = 0; i < enemyCount; i++)
    {
        const EnemySpawn& spawn = cfg.enemies[i];
        if (spawn.enemy < 0 || spawn.enemy >= ENEMY_COUNT) continue;
        // Bosses stay dead: defeated ones don't respawn.
        int bossSlot = index * 16 + i;
        if (ENEMIES[spawn.enemy].boss
            && bossSlot >= 0 && bossSlot < MAX_BOSS_SLOTS
            && bossDefeated[bossSlot]) continue;
        enemies[i].spawn(ENEMIES[spawn.enemy], enemySheets[spawn.enemy],
                         spawn.tileX, spawn.tileY);
    }

    // Objects (pots, bushes): fresh copies each entry, like LttP pots that
    // are back on the next visit.
    int objectTotal = cfg.objectCount < MAX_OBJECTS ? cfg.objectCount : MAX_OBJECTS;
    for (int i = 0; i < objectTotal; i++)
    {
        const ObjectSpawn& spawn = cfg.objects[i];
        if (spawn.type < 0 || spawn.type >= OBJECT_COUNT || spawn.type >= 8) continue;
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
        // Chests stay open across reloads (permanent-until-save registry).
        {
            int slot = index * MAX_OBJECTS + i;
            objects[i].opened = (objects[i].type == OBJECT_CHEST
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
        npcs[i].dialogue = spawn.dialogue;
        npcs[i].modeTime = 0.0f;
        npcs[i].timer = 0.5f + worldRng.nextFloat() * 2.0f;
        npcs[i].animTime = 0.0f;
        npcs[i].moving = false;
        npcs[i].lastFacingPick = -1;
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
    bool cutting = hero.attacking() || hero.spinning() || hero.dashing();
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
                int drop = rollDrop(worldRng);
                if (drop != PICKUP_NONE)
                    spawnPickup(drop, (float)(tx * TILE_SIZE) + 8.0f,
                                (float)(ty * TILE_SIZE) + 8.0f);
            }
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

// Chest grants: apply to the inventory/hero and describe for the item-get
// dialogue.
static void applyChestGrant(int kind, int param)
{
    switch (kind)
    {
    case GRANT_ITEM:
        if (param >= 0 && param < ITEM_COUNT) inventory.give(param);
        break;
    case GRANT_RUPEES: inventory.addRupees(param > 0 ? param : 1); break;
    case GRANT_ARROWS: inventory.addArrows(param > 0 ? param : 1); break;
    case GRANT_BOMBS: inventory.addBombs(param > 0 ? param : 1); break;
    case GRANT_MAGIC: inventory.addMagic(param > 0 ? param : 2); break;
    case GRANT_HEART_CONTAINER: hero.addMaxHearts(2); break;
    case GRANT_SMALL_KEY: inventory.smallKeys++; break;
    case GRANT_BIG_KEY: inventory.bigKeys++; break;
    default: break;
    }
}

static int grantPickupFrame(int kind)
{
    switch (kind)
    {
    case GRANT_RUPEES: return PICKUP_RUPEE1;
    case GRANT_ARROWS: return PICKUP_ARROW;
    case GRANT_BOMBS: return PICKUP_BOMB;
    case GRANT_MAGIC: return PICKUP_MAGIC;
    case GRANT_HEART_CONTAINER: return PICKUP_HEART_CONTAINER;
    default: return -1;  // items/keys: drawn as a medallion
    }
}

static void buildGrantText(int kind, int param)
{
    switch (kind)
    {
    case GRANT_ITEM:
        if (param >= 0 && param < ITEM_COUNT)
            snprintf(itemGetText, sizeof(itemGetText), "You got the %s!", ITEMS[param].label);
        else
            snprintf(itemGetText, sizeof(itemGetText), "You got... something?");
        break;
    case GRANT_RUPEES:
        snprintf(itemGetText, sizeof(itemGetText), "You found %d rupees!", param > 0 ? param : 1);
        break;
    case GRANT_ARROWS:
        snprintf(itemGetText, sizeof(itemGetText), "You got %d arrows!", param > 0 ? param : 1);
        break;
    case GRANT_BOMBS:
        snprintf(itemGetText, sizeof(itemGetText), "You got %d bombs!", param > 0 ? param : 1);
        break;
    case GRANT_MAGIC:
        snprintf(itemGetText, sizeof(itemGetText), "Your magic power was restored!");
        break;
    case GRANT_HEART_CONTAINER:
        snprintf(itemGetText, sizeof(itemGetText), "You got a heart container! Your life force grew!");
        break;
    case GRANT_SMALL_KEY:
        snprintf(itemGetText, sizeof(itemGetText), "You got a small key!");
        break;
    case GRANT_BIG_KEY:
        snprintf(itemGetText, sizeof(itemGetText), "You got the big key! It opens the biggest chest.");
        break;
    default:
        snprintf(itemGetText, sizeof(itemGetText), "It's empty...");
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

        if (npc.wander)
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
            if (!o.active || o.type != OBJECT_SIGN) continue;
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
            if (!o.active || o.type != OBJECT_CHEST) continue;
            if (o.state != OBJ_GROUND) continue;
            if (!overlap(probe, objectBox(o))) continue;

            if (o.opened) return false;  // nothing left inside
            if (o.aux1 == CHEST_LOCKED && inventory.smallKeys <= 0)
            {
                PlaySound(sndError);  // rattles, no key
                return true;
            }
            if (o.aux1 == CHEST_BIG && inventory.bigKeys <= 0)
            {
                PlaySound(sndError);
                return true;
            }

            if (o.aux1 == CHEST_LOCKED) inventory.smallKeys--;
            if (o.aux1 == CHEST_BIG) inventory.bigKeys--;

            o.opened = true;
            {
                int slot = currentRoom * MAX_OBJECTS + i;
                if (slot >= 0 && slot < MAX_CHEST_SLOTS) chestOpened[slot] = true;
            }
            PlaySound(sndChestOpen);
            PlaySound(sndFanfare);

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
    Rectangle probe = { heroBox.x + dx * 20.0f, heroBox.y + dy * 20.0f,
                        heroBox.width, heroBox.height };
    int best = -1;
    float bestDist = 1e9f;
    for (int i = 0; i < MAX_OBJECTS; i++)
    {
        if (!objects[i].active || objects[i].state != OBJ_GROUND) continue;
        if (!objectLiftable(objects[i].type)) continue;
        if (!overlap(probe, objectBox(objects[i]))) continue;
        float dist = fabsf(objects[i].x - heroBox.x) + fabsf(objects[i].y - heroBox.y);
        if (dist < bestDist)
        {
            bestDist = dist;
            best = i;
        }
    }
    if (best < 0) return false;

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
        if (!o.active || o.type != OBJECT_BLOCK) continue;
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

static WorldObject* switchAtTile(int tileX, int tileY)
{
    for (int i = 0; i < MAX_OBJECTS; i++)
    {
        WorldObject& s = objects[i];
        if (!s.active || s.type != OBJECT_SWITCH) continue;
        int sx = (int)((s.x + TILE_SIZE * 0.5f) / TILE_SIZE);
        int sy = (int)((s.y + TILE_SIZE * 0.5f) / TILE_SIZE);
        if (sx == tileX && sy == tileY) return &s;
    }
    return 0;
}

static void updateSwitches()
{
    // Press state: weight = the hero standing on the plate or a block
    // resting on it; latch switches stay pressed once triggered.
    Rectangle heroBox = hero.box();
    for (int i = 0; i < MAX_OBJECTS; i++)
    {
        WorldObject& s = objects[i];
        if (!s.active || s.type != OBJECT_SWITCH) continue;

        Rectangle trigger = { s.x + 5.0f, s.y + 5.0f, 22.0f, 22.0f };
        bool weight = overlap(trigger, heroBox);
        if (!weight)
        {
            for (int j = 0; j < MAX_OBJECTS; j++)
            {
                WorldObject& b = objects[j];
                if (!b.active || b.type != OBJECT_BLOCK) continue;
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
        else if (weight != s.switchActive)
        {
            s.switchActive = weight;
            PlaySound(sndButton);
        }
    }

    // Barriers lower while any switch in their link group is active.
    for (int i = 0; i < MAX_OBJECTS; i++)
    {
        WorldObject& b = objects[i];
        if (!b.active || b.type != OBJECT_BARRIER) continue;

        bool lowered = false;
        for (int j = 0; j < MAX_OBJECTS && !lowered; j++)
        {
            const WorldObject& s = objects[j];
            if (!s.active) continue;
            if (s.type == OBJECT_SWITCH && s.aux2 == b.aux2 && s.switchActive)
                lowered = true;
            // Lit torches latch their link group open.
            if (s.type == OBJECT_TORCH && s.aux2 == b.aux2 && s.opened)
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
    float dt = GetFrameTime();

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
    updateSwitches();

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
                int drop = rollDrop(worldRng);
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

    Vector2 heroCenter = hero.center();
    Rectangle heroBox = hero.box();
    bool dashContact = hero.dashing();
    Rectangle attack = dashContact ? hero.box() : hero.attackBox();
    int swing = hero.swingId();

    for (int i = 0; i < enemyCount; i++)
    {
        Enemy& enemy = enemies[i];
        if (!enemy.alive()) continue;

        enemy.update(room, heroCenter, worldRng, GetFrameTime());

        // Offensive contact (sword arc, spin radius, dash): knockback away
        // from the hero; one hit per action id.
        Vector2 enemyCenter = enemy.center();
        float dx = enemyCenter.x - heroCenter.x;
        float dy = enemyCenter.y - heroCenter.y;
        if (enemy.tryHit(attack, swing, Vector2{ dx, dy }))
        {
            if (enemy.isBoss())
            {
                // Boss defeat: big blast, the arena unlocks, a heart
                // container rises, and the boss stays dead.
                PlaySound(sndBossDies);
                PlaySound(sndFanfare);
                shakeTime = 0.5f;
                shakeStrength = 6.0f;
                spawnExplosion(enemyCenter.x, enemyCenter.y);
                spawnExplosion(enemyCenter.x - 28.0f, enemyCenter.y - 18.0f);
                spawnExplosion(enemyCenter.x + 28.0f, enemyCenter.y + 18.0f);
                int bossSlot = currentRoom * 16 + i;
                if (bossSlot >= 0 && bossSlot < MAX_BOSS_SLOTS)
                    bossDefeated[bossSlot] = true;
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
            int drop = rollDrop(worldRng);
            if (drop != PICKUP_NONE)
                spawnPickup(drop, enemyCenter.x - 8.0f, enemyCenter.y - 8.0f);
            continue;
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

        p.x += p.dx * p.speed * GetFrameTime();
        p.y += p.dy * p.speed * GetFrameTime();

        Rectangle rock = { p.x, p.y, 16.0f, 16.0f };

        // Passive shield: while free to move, rocks arriving from the faced
        // direction get knocked away at a deflected angle.
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
                if (oncoming > 0.45f && overlap(rock, heroBox))
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
                if (!torch.active || torch.type != OBJECT_TORCH) continue;
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

        if (p.beam || p.arrow)
        {
            // Sword beams and arrows strike enemies (one hit per id); both
            // stop on the first enemy touched.
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
                if (enemies[e].tryHit(rock, p.id, Vector2{ kx, ky }))
                {
                    PlaySound(sndEnemyDie);
                    spawnPoof(enemyCenter.x, enemyCenter.y);
                    int drop = rollDrop(worldRng);
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
                        int drop = rollDrop(worldRng);
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

        p.bobTime += GetFrameTime();
        if (p.life > 0.0f)
        {
            p.life -= GetFrameTime();
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
    float dt = GetFrameTime();
    boomer.time += dt;

    if (!boomer.returning)
    {
        // Outbound leg: straight flight with a lateral arc wobble.
        float along = boomer.time * 200.0f;
        float perp = sinf(boomer.time * 14.0f) * 16.0f;
        float nx = boomer.startX + boomer.dirX * along - boomer.dirY * perp;
        float ny = boomer.startY + boomer.dirY * along + boomer.dirX * perp;

        Rectangle flight = { nx - 8.0f, ny - 8.0f, 16.0f, 16.0f };
        if (along >= 104.0f || room.solidBox(flight))
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

static void updatePoofs()
{
    for (int i = 0; i < 32; i++)
    {
        if (!poofs[i].active) continue;
        poofs[i].age += GetFrameTime();
        if (poofs[i].age >= 0.4f) poofs[i].active = false;
    }
}

static void drawGroundFx()
{
    // Draw inside the camera transform: pickups, poofs, rocks, boomerang.
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
    // Floor switches and sunk objects render under everything that stands.
    for (int i = 0; i < MAX_OBJECTS; i++)
    {
        const WorldObject& o = objects[i];
        if (!o.active) continue;
        if (o.type == OBJECT_SWITCH)
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
        if (o.type == OBJECT_SWITCH) continue;
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
            if (hs != HERO_FALL && hs != HERO_SPLASH && hs != HERO_DEAD)
            {
                // Tree-blob shadow at the feet.
                DrawEllipse((int)(heroBox.x + heroBox.width * 0.5f),
                           (int)(heroBox.y + heroBox.height - 2.0f),
                           8.0f, 3.0f, Color{ 20, 20, 40, 70 });
            }
            hero.draw();
        }
        else if (d.kind == 1)
        {
            Rectangle b = enemies[d.index].box();
            DrawEllipse((int)(b.x + b.width * 0.5f), (int)(b.y + b.height - 2.0f),
                        8.0f, 3.0f, Color{ 20, 20, 40, 70 });
            enemies[d.index].draw();
        }
        else if (d.kind == 3)
        {
            const Npc& npc = npcs[d.index];
            Rectangle b = npcBox(npc);
            DrawEllipse((int)(b.x + b.width * 0.5f), (int)(b.y + b.height - 2.0f),
                        8.0f, 3.0f, Color{ 20, 20, 40, 70 });
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
            // Chests have a two-frame sheet: closed / lid open.
            float srcX = (o.type == OBJECT_CHEST && o.opened) ? 32.0f : 0.0f;
            Rectangle src = { srcX, 0.0f, 32.0f, 32.0f };
            Rectangle dest = { o.x, o.y, 32.0f, 32.0f };
            Vector2 origin = { 0.0f, 0.0f };
            DrawTexturePro(objectSheets[o.type], src, dest, origin, 0.0f, WHITE);

            // Lit torches burn with a flickering flame over the bowl.
            if (o.type == OBJECT_TORCH && o.opened)
            {
                float flicker = sinf((float)GetTime() * 22.0f + o.x) * 1.5f;
                float fx = o.x + 16.0f;
                float fy = o.y + 9.0f + sinf((float)GetTime() * 13.0f + o.y) * 1.0f;
                DrawCircle((int)fx, (int)fy, 5.5f + flicker, Color{ 240, 130, 40, 235 });
                DrawCircle((int)fx, (int)(fy - 2.0f), 3.5f + flicker * 0.7f,
                           Color{ 255, 220, 120, 245 });
                DrawCircle((int)fx, (int)(fy - 3.5f), 1.8f, Color{ 255, 250, 220, 255 });
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
    if (dir.x == 0.0f && dir.y == 0.0f) return;

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
    // press.
    if (!doorIsOpen(currentRoom, edge))
    {
        int kind = doorKind(currentRoom, edge);
        if (kind == DOOR_LOCKED)
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
        else if (kind == DOOR_BIG)
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
        else if (kind == DOOR_CLOSED)
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
        if (kind == DOOR_NONE || kind == DOOR_OPEN) continue;

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
        if (kind == DOOR_SHUTTER)
        {
            base = Color{ 120, 120, 134, 255 };
            trim = Color{ 70, 70, 84, 255 };
        }
        DrawRectangleRec(slab, base);
        DrawRectangleLinesEx(slab, 3.0f, trim);

        if (kind == DOOR_LOCKED || kind == DOOR_BIG)
        {
            // Gold keyhole plate.
            DrawRectangle((int)(cx - 5.0f), (int)(cy - 6.0f), 10, 12,
                          Color{ 216, 168, 60, 255 });
            DrawCircle((int)cx, (int)cy, 2.5f, Color{ 70, 46, 22, 255 });
        }
        else if (kind == DOOR_BOMBABLE)
        {
            // Crack marks over the slab.
            for (int i = 0; i < 5; i++)
            {
                float ox = -12.0f + (float)i * 6.0f;
                DrawLine((int)(cx + ox), (int)(cy - 8.0f), (int)(cx + ox + 4.0f),
                         (int)(cy + 8.0f), Color{ 40, 30, 24, 255 });
            }
        }
        else if (kind == DOOR_SHUTTER)
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

    if (gameState == STATE_TITLE)
    {
        updateTitle();
        BeginDrawing();
        ClearBackground(BLACK);
        drawTitleScreen();
        EndDrawing();
        return;
    }

    if (pausePressed()) paused = !paused;

    if (roomFade > 0.0f)
    {
        roomFade -= GetFrameTime() / ROOM_FADE_DURATION;
        if (roomFade < 0.0f) roomFade = 0.0f;
    }

    if (paused)
    {
        // Item subscreen: cursor + equip (the world stays frozen).
        updatePauseMenu();
    }

    if (dialogue.isActive())
    {
        if (!paused) dialogue.update();

        // Scripted music switches drain here (once per request).
        const char* musicRequest = dialogue.takeMusicRequest();
        if (musicRequest) startMusic(musicRequest);
    }
    else if (!paused)
    {
        if (roomTransition.active)
        {
            // Scroll-in: world frozen while the hero walks in.
            updateRoomTransition(GetFrameTime());
        }
        else if (itemGet.active && !itemGet.dialogueOpened)
        {
            // Item-get rise: world frozen while the grant floats up.
            updateItemGet(GetFrameTime());
        }
        else if (hero.dead())
        {
            // Death sequence: world frozen, waiting on the continue prompt
            // (handled in drawGameOver flow below).
            hero.update(room, GetFrameTime());
        }
        else
        {
            int healthBefore = hero.healthHalves();
            HeroState stateBefore = hero.state();

            hero.update(room, GetFrameTime());
            bool actionUsed = updateObjectActions();
            updateEncounters(!actionUsed);
            updateGrass();
            updateSwordBeam();
            if (itemPressed() && !hero.busy() && !hero.carrying())
            {
                useEquippedItem();
            }
            updateBoomerang();
            updateNpcs(GetFrameTime());
            updateLantern(GetFrameTime());
            updateBombs(GetFrameTime());
            updateObjects();
            updateEnemies();
            updateProjectiles();
            updatePickups();
            updatePoofs();

            // Feedback: damage taken -> screen shake; low health -> beeping.
            if (hero.healthHalves() < healthBefore || (stateBefore != HERO_HURT && hero.state() == HERO_HURT))
            {
                shakeTime = 0.25f;
                shakeStrength = 3.0f;
            }

            // Walking off a linked map edge glides into the next room.
            maybeStartRoomTransition();
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
    }

    // The item-get sequence ends when its description dialogue closes.
    if (itemGet.active && itemGet.dialogueOpened && !dialogue.isActive())
    {
        itemGet.active = false;
    }

    if (shakeTime > 0.0f) shakeTime -= GetFrameTime();

    // Low-health beeping (flashing handled in the HUD).
    if (hero.healthHalves() > 0 && hero.healthHalves() <= 2 && !paused)
    {
        lowHpBeepTimer += GetFrameTime();
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

    // Room-load fade (spawns, death-continue, future warp tiles).
    if (roomFade > 0.0f)
    {
        unsigned char alpha = (unsigned char)(int)(clampf(roomFade, 0.0f, 1.0f) * 255.0f);
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{ 0, 0, 0, alpha });
    }

    drawHud();
    drawToasts();
    drawPauseOverlay();
    drawGameOver();
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

    for (int i = 0; i < OBJECT_COUNT && i < 8; i++)
    {
        objectSheets[i] = LoadTexture(OBJECTS[i].image);
        SetTextureFilter(objectSheets[i], TEXTURE_FILTER_POINT);
    }

    // Ground objects block movement through the room's solidity check.
    room.setSolidExtras(objectSolidExtra, 0);

    sndGrass = LoadSound("assets/audio/grass destroyed.wav");
    sndEnemyDie = LoadSound("assets/audio/enemy dies.wav");
    sndHeart = LoadSound("assets/audio/heart.wav");
    sndRupee = LoadSound("assets/audio/rupee.wav");
    sndLowHp = LoadSound("assets/audio/low hp.wav");
    sndShot = LoadSound("assets/audio/fireball.wav");
    sndShotWall = LoadSound("assets/audio/arrow hit wall.wav");
    sndBoomerang = LoadSound("assets/audio/boomerang.wav");
    sndBoomerangHit = LoadSound("assets/audio/small enemy hit.wav");
    sndTink = LoadSound("assets/audio/tink.wav");
    sndSwordBeam = LoadSound("assets/audio/sword beam.wav");
    sndThrow = LoadSound("assets/audio/throw.wav");
    sndBreak = LoadSound("assets/audio/break.wav");
    sndItemWater = LoadSound("assets/audio/item in water.wav");
    sndMoveObject = LoadSound("assets/audio/move object.wav");
    sndButton = LoadSound("assets/audio/button.wav");
    sndDoorOpen = LoadSound("assets/audio/door open.wav");
    sndDoorClose = LoadSound("assets/audio/door close.wav");
    sndBombExplode = LoadSound("assets/audio/bomb explode.wav");
    sndBombWall = LoadSound("assets/audio/bombable wall.wav");
    sndLayBomb = LoadSound("assets/audio/lay bomb.wav");
    sndItemGet = LoadSound("assets/audio/item get 1.wav");
    sndError = LoadSound("assets/audio/error.wav");
    sndUiCursor = LoadSound("assets/audio/cursor.wav");
    sndUiButton = LoadSound("assets/audio/button.wav");
    sndChestOpen = LoadSound("assets/audio/chest open.wav");
    sndFanfare = LoadSound("assets/audio/item get 2.wav");
    sndFireRod = LoadSound("assets/audio/fire rod.wav");
    sndUseKey = LoadSound("assets/audio/use key.wav");
    sndBossDies = LoadSound("assets/audio/boss dies.wav");
    sndIceRod = LoadSound("assets/audio/ice rod.wav");
    sndFire = LoadSound("assets/audio/fire.wav");

    lightGradient = LoadTexture("assets/effects/light.png");
    SetTextureFilter(lightGradient, TEXTURE_FILTER_POINT);
    lightRT = LoadRenderTexture(GetScreenWidth(), GetScreenHeight());

    dialogue.init();

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

    camera.rotation = 0.0f;

#if defined(PLATFORM_WEB)
    emscripten_set_main_loop(updateAndDraw, 0, 1);
#else
    while (!WindowShouldClose()) updateAndDraw();

    startMusic(NULL);
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
    for (int i = 0; i < 8; i++)
    {
        if (objectSheets[i].id != 0) UnloadTexture(objectSheets[i]);
    }
    UnloadSound(sndGrass);
    UnloadSound(sndEnemyDie);
    UnloadSound(sndHeart);
    UnloadSound(sndRupee);
    UnloadSound(sndLowHp);
    UnloadSound(sndShot);
    UnloadSound(sndShotWall);
    UnloadSound(sndBoomerang);
    UnloadSound(sndBoomerangHit);
    UnloadSound(sndTink);
    UnloadSound(sndSwordBeam);
    UnloadSound(sndThrow);
    UnloadSound(sndBreak);
    UnloadSound(sndItemWater);
    UnloadSound(sndMoveObject);
    UnloadSound(sndButton);
    UnloadSound(sndDoorOpen);
    UnloadSound(sndDoorClose);
    UnloadSound(sndBombExplode);
    UnloadSound(sndBombWall);
    UnloadSound(sndLayBomb);
    UnloadSound(sndItemGet);
    UnloadSound(sndError);
    UnloadSound(sndUiCursor);
    UnloadSound(sndUiButton);
    UnloadSound(sndChestOpen);
    UnloadSound(sndFanfare);
    UnloadSound(sndFireRod);
    UnloadSound(sndUseKey);
    UnloadSound(sndBossDies);
    UnloadSound(sndIceRod);
    UnloadSound(sndFire);
    UnloadTexture(lightGradient);
    if (lightRT.texture.id != 0) UnloadRenderTexture(lightRT);

    CloseAudioDevice();
    CloseWindow();
#endif
    return 0;
}