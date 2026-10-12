#pragma once

#include <raylib.h>

constexpr int TILE_SIZE = 32;
constexpr float VIRTUAL_HEIGHT = 224.0f;

enum Direction
{
    DIR_DOWN = 0,
    DIR_RIGHT = 1,
    DIR_LEFT = 2,
    DIR_UP = 3,
    DIR_COUNT = 4
};

struct DirectionalFrames
{
    int count;
    const int* frames;
};

struct CharacterConfig
{
    const char* name;
    const char* image;
    int sheetColumns;
    int frameCount;
    DirectionalFrames stand[DIR_COUNT];
    DirectionalFrames move[DIR_COUNT];
};

// One selectable option of a dialogue choice ("choice" on a line).
struct DialogueChoice
{
    const char* text;
    int gotoLine;          // -1 = advance to the next line
    const int* setFlags;   // world flags to set when chosen
    int setCount;
    const int* clearFlags;  // world flags to clear when chosen
    int clearCount;
    const char* sfx;        // audio file, 0 = none
};

// One line of dialogue with optional scripting. Effects:
// - if/unless: the line is only shown when the flag matches (skipped
//   lines never execute anything).
// - set/clear/sfx/music: applied when the line is SHOWN.
// - goto: jump when the line completes (action press after typing).
// - warp: queued when the line completes; the game executes it once the
//   dialogue closes (loading the room at the tile).
// - choice: after the line types out, the action press opens the option
//   list; up/down moves the cursor, action selects.
struct DialogueLine
{
    const char* text;
    int gotoLine;           // -1 = advance normally
    int ifFlag;             // -1 = no condition
    int unlessFlag;         // -1 = no condition
    const int* setFlags;
    int setCount;
    const int* clearFlags;
    int clearCount;
    const char* sfx;        // audio file, 0 = none
    const char* music;      // music file, 0 = none, "" = stop music
    int warpRoom;           // -1 = none
    int warpTileX;
    int warpTileY;
    int choiceCount;
    const DialogueChoice* choices;
};

struct Dialogue
{
    const char* name;
    int lineCount;
    const DialogueLine* lines;
};

struct Encounter
{
    int tileX;
    int tileY;
    int dialogue;
    int id;
    bool repeat;
};

// Enemy behavior archetypes, baked from the "behavior" stat in
// resources/enemies/*.json. Numbers are generator-side constants; keep both
// sides in sync (BEHAVIORS in scripts/generate_assets.py).
constexpr int BEHAVIOR_NONE = 0;
constexpr int BEHAVIOR_SOLDIER = 1;  // patrol, charges on sight
constexpr int BEHAVIOR_OCTOROK = 2; // walks, stops, spits rocks
constexpr int BEHAVIOR_BOSS = 3;    // arena boss: phased charges, doors lock

struct EnemyConfig
{
    const char* name;
    const char* image;
    int sheetColumns;
    int frameCount;
    DirectionalFrames stand[DIR_COUNT];
    DirectionalFrames move[DIR_COUNT];
    int hp;
    int speed;              // px/s while walking
    int contactDamage;      // half-hearts
    int behavior;
    int sight;              // px; soldier charge radius
    int shootPeriod;        // tenths of a second between shots; 0 = never
    bool boss;              // arena boss: bigger sprite, phased AI, defeat
};

struct EnemySpawn
{
    int tileX;
    int tileY;
    int enemy;
};

// Object types, generator-side constants (OBJECT_TYPES in
// scripts/generate_assets.py). Keep both sides in sync when adding kinds
// (chests, statues...).
constexpr int OBJECT_POT = 0;      // liftable, throwable
constexpr int OBJECT_BUSH = 1;     // liftable, throwable
constexpr int OBJECT_BLOCK = 2;    // pushable one tile; sinks onto latch switches
constexpr int OBJECT_SWITCH = 3;   // floor switch: hold (aux1 0) or latch (aux1 1)
constexpr int OBJECT_BARRIER = 4;  // raised gate; lowers while linked (aux2) switches are active
constexpr int OBJECT_SIGN = 5;    // readable: aux1 = dialogue index
constexpr int OBJECT_CHEST = 6;   // treasure chest: aux1 = kind, aux2/aux3 = grant
constexpr int OBJECT_TORCH = 7;   // torch post: aux2 = link group; fire lights it (latched)

// Chest kinds (aux1 on chest spawns).
constexpr int CHEST_NORMAL = 0;
constexpr int CHEST_LOCKED = 1;   // consumes a small key to open
constexpr int CHEST_BIG = 2;      // consumes a big key to open

// Chest grant kinds (aux2 on chest spawns; aux3 = item index or amount).
constexpr int GRANT_ITEM = 0;            // aux3 = ITEM_* index
constexpr int GRANT_RUPEES = 1;         // aux3 = amount
constexpr int GRANT_ARROWS = 2;          // aux3 = amount
constexpr int GRANT_BOMBS = 3;          // aux3 = amount
constexpr int GRANT_MAGIC = 4;          // aux3 = half-units
constexpr int GRANT_HEART_CONTAINER = 5;
constexpr int GRANT_SMALL_KEY = 6;
constexpr int GRANT_BIG_KEY = 7;

struct ObjectConfig
{
    const char* name;
    const char* image;  // single 32x32 frame
};

// Door kinds on map-graph edges ("doors" in room json). A door gates its
// edge: the hero can't scroll through until it's open.
constexpr int DOOR_NONE = -1;
constexpr int DOOR_OPEN = 0;      // always passable (cosmetic archway)
constexpr int DOOR_CLOSED = 1;    // pushes open from either side, stays open
constexpr int DOOR_LOCKED = 2;   // small key, stays open
constexpr int DOOR_BIG = 3;      // big key, stays open
constexpr int DOOR_BOMBABLE = 4; // bomb blast opens it
constexpr int DOOR_SHUTTER = 5;  // shutters while enemies live in the room

struct ObjectSpawn
{
    int tileX;
    int tileY;
    int type;
    int aux1;  // switch: 0 hold / 1 latch; sign: dialogue index; chest: kind
    int aux2;  // switch/barrier: link group; chest: grant kind
    int aux3;  // chest: grant item index or amount
};

// NPCs: character-driven townsfolk. Idle NPCs turn to face the hero when
// nearby; wanderers stroll with pauses; talking opens their dialogue.
// Spawn conditions gate presence on world flags (story state).
struct NpcSpawn
{
    int tileX;
    int tileY;
    int character;     // CHARACTER index
    int dialogue;      // DIALOGUE index, -1 = silent
    bool wander;       // idle/stroll when false/true
    int ifFlag;        // only spawned when the flag is set (-1 = always)
    int unlessFlag;    // ...or when the flag is NOT set
};

struct RoomConfig
{
    const char* name;
    int hero;
    bool hasSpawn;
    int spawnTileX;
    int spawnTileY;
    const char* music;
    const char* walkSfx;
    const Encounter* instant;
    int instantCount;
    const Encounter* interactive;
    int interactiveCount;
    const EnemySpawn* enemies;
    int enemyCount;
    const ObjectSpawn* objects;
    int objectCount;
    const NpcSpawn* npcs;
    int npcCount;
    bool dark;       // darkness mask: lantern + lit torches reveal
    int doorN;      // DOOR_* kind on each edge (-1 = no door)
    int doorS;
    int doorE;
    int doorW;
    int mapWidth;
    int mapHeight;
    const unsigned short* decor;
    const unsigned short* colliders;
    const unsigned short* upper;    // optional decor-above layer, drawn over entities
    // Map-graph edges: which ROOMS[] index each map edge leads to (-1 = a
    // wall). Walking off an edge glides the hero into the linked room.
    int exitNorth;   // walking up off the top
    int exitSouth;   // walking down off the bottom
    int exitEast;    // walking right off the right
    int exitWest;    // walking left off the left
};

struct TilesetConfig
{
    const char* image;
    int firstGid;
    int columns;
    int tileCount;
    int tileWidth;
    int tileHeight;
};

// Per-tile behavior metadata, baked from resources/tiles/tiles.json by the
// generator. Indexed by tile id - 1, matching the ids stored in room layers.
struct TileMeta
{
    unsigned char flags;
    unsigned char damage;      // half-hearts, when TILE_FLAG_DAMAGE is set
    unsigned char cutResult;  // tile id left behind when cut, 0 = empty
    unsigned char bombResult; // tile id left behind when bombed, 255 = empty
    signed char pushX;      // px/s conveyor/current/wind drift
    signed char pushY;
    unsigned char ledgeDir; // Direction when TILE_FLAG_LEDGE is set
};

constexpr unsigned char TILE_FLAG_PIT = 0x01;      // hole: hero falls in
constexpr unsigned char TILE_FLAG_DAMAGE = 0x02;   // hazard: hurts on contact
constexpr unsigned char TILE_FLAG_CUTTABLE = 0x04;  // grass: sword cuts it
constexpr unsigned char TILE_FLAG_WATER_DEEP = 0x08;    // deep water: splash + bounce back to the bank
constexpr unsigned char TILE_FLAG_WATER_SHALLOW = 0x10; // shallow water: wading
constexpr unsigned char TILE_FLAG_ICE = 0x20;           // ice: slippery momentum
constexpr unsigned char TILE_FLAG_LEDGE = 0x40;         // ledge: vault over it along ledgeDir
constexpr unsigned char TILE_FLAG_BOMBABLE = 0x80;       // cracked wall/floor: bombs blast it open

inline float virtualScale()
{
    return (float)GetScreenHeight() / VIRTUAL_HEIGHT;
}

inline float clampf(float value, float low, float high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

// Input remapping layer: every logical button maps to one keyboard key plus
// one gamepad button. All action checks go through a binding so remapping
// only ever touches this table.
struct InputBinding
{
    int key;
    int padButton;
};

constexpr InputBinding BINDING_ACTION = { KEY_Z, GAMEPAD_BUTTON_RIGHT_FACE_DOWN };
constexpr InputBinding BINDING_SWORD = { KEY_X, GAMEPAD_BUTTON_RIGHT_FACE_LEFT };
constexpr InputBinding BINDING_ITEM = { KEY_C, GAMEPAD_BUTTON_RIGHT_FACE_UP };
constexpr InputBinding BINDING_DASH = { KEY_V, GAMEPAD_BUTTON_RIGHT_TRIGGER_1 };
constexpr InputBinding BINDING_PAUSE = { KEY_ENTER, GAMEPAD_BUTTON_MIDDLE_RIGHT };

inline bool bindingPressed(const InputBinding& binding)
{
    if (IsKeyPressed(binding.key)) return true;
    for (int i = 0; i < 4; i++)
    {
        if (IsGamepadAvailable(i) && IsGamepadButtonPressed(i, binding.padButton)) return true;
    }
    return false;
}

inline bool bindingHeld(const InputBinding& binding)
{
    if (IsKeyDown(binding.key)) return true;
    for (int i = 0; i < 4; i++)
    {
        if (IsGamepadAvailable(i) && IsGamepadButtonDown(i, binding.padButton)) return true;
    }
    return false;
}

inline bool actionPressed()
{
    return bindingPressed(BINDING_ACTION);
}

inline bool swordPressed()
{
    return bindingPressed(BINDING_SWORD);
}

inline bool itemPressed()
{
    return bindingPressed(BINDING_ITEM);
}

inline bool dashPressed()
{
    return bindingPressed(BINDING_DASH);
}

inline bool pausePressed()
{
    return bindingPressed(BINDING_PAUSE);
}

// Deterministic xorshift RNG for drops and enemy behavior. Seeded once at
// boot (later: restored from the save file so world state replays alike).
class Rng
{
public:
    Rng() : state(0x6D2B79F5u) {}
    explicit Rng(unsigned int seed) : state(seed ? seed : 0x6D2B79F5u) {}

    unsigned int next()
    {
        unsigned int x = state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state = x;
        return x;
    }

    float nextFloat()  // [0, 1)
    {
        return (float)(next() & 0x00FFFFFF) / 16777216.0f;
    }

    int range(int highExclusive)  // [0, highExclusive)
    {
        if (highExclusive <= 0) return 0;
        return (int)(nextFloat() * (float)highExclusive);
    }

    int range(int low, int highExclusive)  // [low, highExclusive)
    {
        return low + range(highExclusive - low);
    }

    void seed(unsigned int value)
    {
        state = value ? value : 0x6D2B79F5u;
    }

    // Current generator state (for save/restore so world rolls replay alike).
    unsigned int rawState() const { return state; }

private:
    unsigned int state;
};

// Pickup kinds, matching the frames of assets/pickups/pickups.png.
constexpr int PICKUP_NONE = -1;
constexpr int PICKUP_HEART = 0;
constexpr int PICKUP_RUPEE1 = 1;
constexpr int PICKUP_RUPEE5 = 2;
constexpr int PICKUP_ARROW = 4;
constexpr int PICKUP_BOMB = 5;
constexpr int PICKUP_MAGIC = 6;
constexpr int PICKUP_HEART_CONTAINER = 7;

// Item button items, generator-side constants (ITEMS in
// scripts/generate_assets.py); the index order is baked as ITEM_* here.
// Keep both sides in sync when adding items (hookshot, rods, shovel...).
constexpr int ITEM_BOOMERANG = 0;
constexpr int ITEM_BOW = 1;
constexpr int ITEM_BOMBS = 2;
constexpr int ITEM_FIRE_ROD = 3;
constexpr int ITEM_ICE_ROD = 4;
constexpr int ITEM_LANTERN = 5;  // passive: light in dark rooms, slow magic drain

// Projectile elements (rods, future medallions).
constexpr int ELEMENT_NONE = 0;
constexpr int ELEMENT_FIRE = 1;  // burns: kills grunts, lights torches, thaws+shatters
constexpr int ELEMENT_ICE = 2;   // freezes enemies solid

struct ItemConfig
{
    const char* name;
    const char* label;  // subscreen display label
};

// Inventory: quantities + owned items + the equipped item (the item
// button runs it). Health lives on the hero; everything else lives here.
// Process-lifetime until the save system lands.
struct Inventory
{
    static const int MAX_ITEMS = 16;

    int equippedItem;    // ITEM_* or -1 (empty hands)
    bool owned[MAX_ITEMS];

    int rupees;
    int rupeeMax;   // wallet level cap
    int arrows;
    int arrowsMax;
    int bombs;
    int bombsMax;
    int magic;      // half-units
    int magicMax;   // half-units
    int smallKeys;   // per-dungeon keys pending the dungeon system
    int bigKeys;

    Inventory()
        : equippedItem(-1), rupees(0), rupeeMax(99),
          arrows(0), arrowsMax(10), bombs(0), bombsMax(10),
          magic(0), magicMax(16), smallKeys(0), bigKeys(0)
    {
        for (int i = 0; i < MAX_ITEMS; i++) owned[i] = false;
    }

    void give(int item) { if (item >= 0 && item < MAX_ITEMS) owned[item] = true; }
    void addRupees(int amount)
    {
        rupees += amount;
        if (rupees > rupeeMax) rupees = rupeeMax;
        if (rupees < 0) rupees = 0;
    }
    void addArrows(int amount) { arrows += amount; if (arrows > arrowsMax) arrows = arrowsMax; }
    void addBombs(int amount) { bombs += amount; if (bombs > bombsMax) bombs = bombsMax; }
    void addMagic(int halves)
    {
        magic += halves;
        if (magic > magicMax) magic = magicMax;
    }
    // Item consumption: true when the cost was available and paid.
    bool consumeMagic(int halves)
    {
        if (magic < halves) return false;
        magic -= halves;
        return true;
    }
};

// Weighted drop roll (grass + enemy deaths): mostly nothing, hearts/rupees
// otherwise, occasionally arrows/bombs/magic, in LttP spirit.
inline int rollDrop(Rng& rng)
{
    int roll = rng.range(100);
    if (roll < 55) return PICKUP_NONE;
    if (roll < 75) return PICKUP_HEART;
    if (roll < 86) return PICKUP_RUPEE1;
    if (roll < 92) return PICKUP_RUPEE5;
    if (roll < 95) return PICKUP_ARROW;
    if (roll < 98) return PICKUP_BOMB;
    return PICKUP_MAGIC;
}
