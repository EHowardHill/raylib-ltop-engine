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
// - if/unless (flags) and if_item/unless_item (inventory): the line is
//   only shown when the condition matches (skipped lines never execute
//   anything).
// - set/clear/sfx/music/give: applied when the line is SHOWN (give hands
//   the named item over via the game's give-request drain).
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
    int ifItem;             // game-defined item id or -1 = no condition
    int unlessItem;         // game-defined item id or -1 = no condition
    int giveItem;           // item id granted when shown, -1 = none
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

// Enemy component vocabulary. The engine interprets these parameters
// (composed by the generator from behavior presets or explicit
// "components" in the enemy json); no archetype ids exist at runtime.
// Presets are game data; the interpreter in src/enemy.cpp is engine
// vocabulary.
struct EnemyPhase
{
    float hpBelow;    // phase active at or below this hp ratio
    float speedScale; // charger speed multiplier while in the phase
};

// Per-enemy drop table entry (kind = pickups-sheet frame, weight = roll
// weight). No table = the engine's default drop roll.
struct DropEntry
{
    int kind;
    int weight;
};

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
    float chargeSight;      // charger: px radius to notice the hero (0 = never)
    float chargeSpeedScale; // charger: speed multiplier while charging
    bool chargeStunOnWall;  // charger: wall impact stuns
    int shootPeriod;        // shooter: cadence knob (0 = never shoots)
    float walkSegment;      // mover: seconds per wander leg (0 = engine default)
    bool turnInPlace;       // mover: occasional random course change
    const EnemyPhase* phases; // boss phase table (ascending hpBelow)
    int phaseCount;
    bool boss;              // arena boss: bigger sprite, room seal, banked defeat
    const char* customBehavior; // gamecode "custom:<name>" behavior, 0 = none
    const DropEntry* drops; // per-enemy drop table, 0 = engine default
    int dropCount;
};

struct EnemySpawn
{
    int tileX;
    int tileY;
    int enemy;
};

// Object interaction vocabulary (engine-owned; a game's object types
// compose these in resources/objects/types.json). The engine branches on
// these flags, never on type identity, so new game-defined types get the
// composed behavior for free. Interactions marked with the aux field they
// consume on spawns (see ObjectSpawn below).
constexpr unsigned OBJINT_SOLID = 0x1;         // blocks movement while grounded
constexpr unsigned OBJINT_FLOOR = 0x2;         // walkable plate, drawn under entities
constexpr unsigned OBJINT_LIFTABLE = 0x4;      // action button hoists it overhead
constexpr unsigned OBJINT_CUT = 0x8;           // sword swings smash it (grass-style)
constexpr unsigned OBJINT_DASH_SMASH = 0x10;   // pegasus dash shatters it
constexpr unsigned OBJINT_PUSHABLE = 0x20;     // sustained push slides it one tile
constexpr unsigned OBJINT_SINKS = 0x40;        // push-block sinks onto latch switches
constexpr unsigned OBJINT_WEIGHT = 0x80;       // counts as weight on floor switches
constexpr unsigned OBJINT_SWITCH = 0x100;      // floor plate: aux1 hold/latch, aux2 link
constexpr unsigned OBJINT_BARRIER = 0x200;     // linked gate: aux2 link, aux3 color
constexpr unsigned OBJINT_READABLE = 0x400;    // sign: aux1 dialogue index
constexpr unsigned OBJINT_CHEST = 0x800;       // container: aux1 kind, aux2/aux3 grant
constexpr unsigned OBJINT_TORCH = 0x1000;      // lightable post: aux2 link
constexpr unsigned OBJINT_WARP = 0x2000;       // warp pad: aux1 room, aux2/aux3 tile
constexpr unsigned OBJINT_CRYSTAL = 0x4000;    // sword strikes toggle the crystal state
constexpr unsigned OBJINT_LEAVES = 0x8000;     // smashes into leaves instead of shards
constexpr unsigned OBJINT_TWO_FRAME = 0x10000; // two-frame sheet (frame 2 once opened)
constexpr unsigned OBJINT_HIDES = 0x20000;    // hides an enemy (aux1) until lifted

struct ObjectConfig
{
    const char* name;
    const char* image;  // single 32x32 frame
    unsigned interactions;  // OBJINT_* composition (the engine's behavior seam)
};

// Door rule vocabulary: how a door kind (game-defined id) behaves. The
// engine consults this table; kinds are data, not code branches.
struct DoorRule
{
    bool startsOpen;      // always passable (cosmetic when open)
    bool pushOpen;        // a push from either side opens it, stays open
    int keyCost;          // 0 = none, 1 = small key, 2 = big key
    bool bombable;        // a blast at the wall midpoint blows it open
    bool liveEnemiesDead; // open only while no enemies live in the room
};

// Sentinel for "no door on this edge" (engine-level; kind ids are game
// data and resolve through DOOR_RULES).
constexpr int DOOR_NONE = -1;

// Chest kind rule: what opening a chest of this kind costs.
struct ChestRule
{
    int keyCost;  // 0 = none, 1 = small key, 2 = big key
};

// Grant effects (trigger-vocabulary "grant" action, e.g. chest payloads).
constexpr int GRANTEFFECT_GIVE_ITEM = 0;
constexpr int GRANTEFFECT_ADD_RUPEES = 1;
constexpr int GRANTEFFECT_ADD_ARROWS = 2;
constexpr int GRANTEFFECT_ADD_BOMBS = 3;
constexpr int GRANTEFFECT_ADD_MAGIC = 4;
constexpr int GRANTEFFECT_ADD_HEARTS = 5;   // heart container: max +1 heart
constexpr int GRANTEFFECT_ADD_SMALL_KEY = 6;
constexpr int GRANTEFFECT_ADD_BIG_KEY = 7;

// Grant rule: the engine applies `effect` (with param = item index or
// amount), shows the item-get rise with `pickupFrame` (pickups sheet
// frame, -1 = medallion), and describes it with `text` (%s = item label,
// %d = amount).
struct GrantRule
{
    int effect;
    int pickupFrame;
    const char* text;
};

struct ObjectSpawn
{
    int tileX;
    int tileY;
    int type;
    // Aux payloads; which apply depends on the type's interactions:
    int aux1;  // switch: 0 hold / 1 latch; readable: dialogue; chest: kind; hides: enemy; warp: room
    int aux2;  // switch/barrier/torch: link group; chest: grant kind; warp: tile x
    int aux3;  // barrier: color; chest: grant item index or amount; warp: tile y
};

// NPCs: character-driven townsfolk. Idle NPCs turn to face the hero when
// nearby; wanderers stroll with pauses; path-followers walk a loop of tile
// waypoints (path beats wander). Talking opens their dialogue. Spawn
// conditions gate presence on world flags (story state).
struct NpcSpawn
{
    int tileX;
    int tileY;
    int character;     // CHARACTER index
    int dialogue;      // DIALOGUE index, -1 = silent
    bool wander;       // idle/stroll when false/true
    const short* path;   // tile waypoints as x,y pairs, 0 = none
    int pathCount;       // waypoint count (pairs), 0 = none
    int ifFlag;        // only spawned when the flag is set (-1 = always)
    int unlessFlag;    // ...or when the flag is NOT set
};

// Placed pickups (room "pickups" list): world items lying around, spawned
// fresh on every room entry like LttP field hearts. kind = pickups-sheet frame.
struct PickupSpawn
{
    int tileX;
    int tileY;
    int kind;
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
    const PickupSpawn* pickups;  // placed world items (hearts in grass...)
    int pickupCount;
    bool dark;       // darkness mask: lantern + lit torches reveal
    int doorN;      // door kind id on each edge (-1 = no door)
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
    unsigned char heal;     // 1 = fairy-fountain water: refills hearts/magic
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

// Frame delta with a safety clamp. Every dt-driven system already scales by
// the real delta (so gameplay speed is framerate independent); the clamp
// caps pathological one-frame deltas (window drags, alt-tabs, debugger
// pauses) so nothing teleports across the map. One frame of gameplay never
// advances more than 1/20s this way.
inline float frameDt()
{
    float dt = GetFrameTime();
    return (dt > 0.05f) ? 0.05f : dt;
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

// Item use-action vocabulary (engine-owned; a game's items compose these
// in resources/items/*.json). The engine interprets these; the per-item
// switch is gone.
constexpr int ITEM_USE_NONE = 0;
constexpr int ITEM_USE_BOOMERANG = 1;  // boomerang flight: arc out, homing return
constexpr int ITEM_USE_ARROW = 2;      // arrow projectile
constexpr int ITEM_USE_BOMB = 3;       // placed bomb at the hero's feet
constexpr int ITEM_USE_BOLT = 4;       // element bolt (fire/ice rod)

// Projectile elements (rods, future medallions).
constexpr int ELEMENT_NONE = 0;
constexpr int ELEMENT_FIRE = 1;  // burns: kills grunts, lights torches, thaws+shatters
constexpr int ELEMENT_ICE = 2;   // freezes enemies solid

// Item configuration, baked per game from resources/items/<name>.json.
// The use-action composes the vocabulary above with costs and an sfx cue;
// passives are always-on effects (the lantern's dark-room light). An item
// with useKind ITEM_USE_NONE is passive-only (using it plays the error cue).
struct ItemConfig
{
    const char* name;
    const char* label;  // subscreen display label
    int useKind;        // ITEM_USE_*
    int element;        // ELEMENT_* (bolts)
    int magicCost;      // half-units consumed per use (0 = none)
    int bombCost;       // bombs consumed per use (0 = none)
    int arrowCost;       // arrows consumed per use (0 = none)
    bool arrowRupeeFallback; // dry quiver consumes a rupee instead (LttP bow)
    const char* useSfx;  // audio file played on a successful use, 0 = none
    bool hasLight;       // passive: light radius in dark rooms
    int lightRadius;     // px
};

// Inventory: quantities + owned items + the equipped item (the item
// button runs it). Health lives on the hero; everything else lives here.
// Process-lifetime until the save system lands.
struct Inventory
{
    static const int MAX_ITEMS = 16;

    int equippedItem;    // game-defined item id or -1 (empty hands)
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

// Weighted roll over a content drop table (per-enemy tables baked by the
// generator; the engine default above applies when a table is absent).
inline int rollDrop(Rng& rng, const DropEntry* table, int count)
{
    if (!table || count <= 0) return rollDrop(rng);
    int total = 0;
    for (int i = 0; i < count; i++) total += table[i].weight;
    if (total <= 0) return PICKUP_NONE;
    int roll = rng.range(total);
    for (int i = 0; i < count; i++)
    {
        roll -= table[i].weight;
        if (roll < 0) return table[i].kind;
    }
    return PICKUP_NONE;
}
