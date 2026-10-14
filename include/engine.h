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
    int giveItem;           // item granted when chosen, -1 = none (after cost)
    int costRupees;         // price checked + paid on selection, 0 = free
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
    // Cutscene vocabulary: beat holds, scripted camera, letterbox, actor
    // blocking. The engine interprets; scripts compose them like any
    // other shown-effect (they queue on the line and drain in the game).
    float waitSeconds;      // 0 = button-advance; >0 = auto-advance once
                             // the reveal completes (a silent beat when
                             // the line carries no text)
    int camAction;          // 0 = none, 1 = pan to tile, 2 = back to hero
    int camTileX;
    int camTileY;
    float camSeconds;       // pan duration
    int letterbox;          // -1 = leave as-is, 0 = bars off, 1 = bars on
    int moveActor;          // -1 = none, -2 = the hero, else room npc index
    int moveTileX;
    int moveTileY;
    float moveSeconds;      // blocking glide duration
};

struct Dialogue
{
    const char* name;
    int lineCount;
    const DialogueLine* lines;
    bool credits;        // endgame roll: centered lines over black that
                         // auto-advance (a wait on a line overrides the
                         // default beat)
    int nextDialogue;    // chain into this dialogue when it runs out,
                          // -1 = none (a warp line still wins over it)
};

struct Encounter
{
    int tileX;
    int tileY;
    int dialogue;
    int id;
    bool repeat;
    int ifFlag;      // story-flag conditions: fires only while they hold
    int unlessFlag;  // (a condition that fails never consumes the shot)
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
    bool miniBoss;          // mid-fight: seals the room + health bar, room-local death
    bool strikeImmune;      // direct hero strikes (sword/spin/dash/hammer)
                            // bounce off — only projectile hits land
    bool catchable;         // net-catchable critter (bees): the bug net
                            // bottles it instead of agitating it
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
constexpr unsigned OBJINT_PULLABLE = 0x40000; // grabbable + pullable (action button grabs;
                                              // walking away drags it one tile)
constexpr unsigned OBJINT_HAMMER_SMASH = 0x80000; // hammer pounds shatter it
constexpr unsigned OBJINT_HEAVY = 0x100000;    // liftable only with a lift passive item
                                              // (gloves); too heavy to hoist bare-handed
constexpr unsigned OBJINT_LEVER = 0x200000;   // pull-handle: grabbing and pulling flips it
                                              // (aux2 = barrier link group it toggles)
constexpr unsigned OBJINT_PLATFORM = 0x400000; // moving platform: glides a tile-waypoint
                                              // rail, carrying whoever stands on it
constexpr unsigned OBJINT_HOOKABLE = 0x800000; // hookshot latch: the chain grabs it
                                              // and reels the hero in
constexpr unsigned OBJINT_POWDER = 0x1000000; // magic powder awakens it (aux2 = the
                                              // world flag the sprinkle sets)
constexpr unsigned OBJINT_STAIR = 0x2000000;  // staircase: step-on warp down/up a
                                              // room layer, with the stair cue
                                              // (aux payloads like warp: room,
                                              // tile x, tile y)

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
constexpr int GRANTEFFECT_SET_FLAG = 8;     // story flag set (param = flag index)
constexpr int GRANTEFFECT_GIVE_BOTTLE = 9;  // bottle slot (param = starting contents)

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
    // Platform rail (OBJINT_PLATFORM): tile waypoints as x,y pairs,
    // glided in a loop (0 = stays put).
    const short* path;
    int pathCount;
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

// Room weather vocabulary (room "weather" data): purely presentational
// layers drawn over the world (screen space), on top of any darkness/tint.
constexpr int WEATHER_NONE = 0;
constexpr int WEATHER_RAIN = 1;    // falling streaks over the screen
constexpr int WEATHER_STORM = 2;   // rain + lightning flashes + thunder
constexpr int WEATHER_FOG = 3;     // drifting fog bands

struct RoomConfig
{
    const char* name;
    int hero;
    bool hasSpawn;
    int spawnTileX;
    int spawnTileY;
    const char* music;
    const char* walkSfx;
    const char* ambient;   // streamed, looping room ambience (birds, rain)
    const char* dangerMusic; // alternate track crossfaded in while an
                             // arena fight (boss/mini-boss) lives
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
    const PickupSpawn* buried;  // buried treasure: dug up with the shovel
    int buriedCount;
    bool hasBoss;    // compass marker: the room spawns a boss
    bool hasChest;   // compass marker: the room holds a chest
    int bossFlag;    // story flag set when the room's boss falls, -1 = none
    bool dark;       // darkness mask: lantern + lit torches reveal
    int weather;      // WEATHER_* overlay (rain, storm, fog)
    int tintR;        // ambient tint, multiplied over the world (255 = none)
    int tintG;
    int tintB;
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
    // Dungeon treatment: defeated spawns stay dead across re-entries
    // (session-lifetime), while overworld rooms respawn on every visit.
    bool dungeon;
    // Pit-landing: falling into a pit tile warps here (room index + tile)
    // instead of the safe-tile respawn, -1 = the usual respawn.
    int pitRoom;
    int pitTileX;
    int pitTileY;
    // Parallel-world pair (the mirror): using the mirror warps between
    // paired rooms at the hero's tile. Declared on either side; the
    // generator bakes the reciprocal. -1 = no pair.
    int mirrorRoom;
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
    unsigned short flags;
    unsigned char damage;      // half-hearts, when TILE_FLAG_DAMAGE is set
    unsigned char cutResult;  // tile id left behind when cut, 0 = empty
    unsigned char bombResult; // tile id left behind when bombed, 255 = empty
    signed char pushX;      // px/s conveyor/current/wind drift
    signed char pushY;
    unsigned char ledgeDir; // Direction when TILE_FLAG_LEDGE is set
    unsigned char heal;     // 1 = fairy-fountain water: refills hearts/magic
};

constexpr unsigned short TILE_FLAG_PIT = 0x01;      // hole: hero falls in
constexpr unsigned short TILE_FLAG_DAMAGE = 0x02;   // hazard: hurts on contact
constexpr unsigned short TILE_FLAG_CUTTABLE = 0x04;  // grass: sword cuts it
constexpr unsigned short TILE_FLAG_WATER_DEEP = 0x08;    // deep water: splash + bounce back to the bank
constexpr unsigned short TILE_FLAG_WATER_SHALLOW = 0x10; // shallow water: wading
constexpr unsigned short TILE_FLAG_ICE = 0x20;           // ice: slippery momentum
constexpr unsigned short TILE_FLAG_LEDGE = 0x40;         // ledge: vault over it along ledgeDir
constexpr unsigned short TILE_FLAG_BOMBABLE = 0x80;       // cracked wall/floor: bombs blast it open
constexpr unsigned short TILE_FLAG_DIGGABLE = 0x100;      // soft soil: the shovel turns it over

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
constexpr InputBinding BINDING_SHIELD = { KEY_LEFT_SHIFT, GAMEPAD_BUTTON_LEFT_TRIGGER_1 };
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
constexpr int PICKUP_FAIRY = 8;  // full heal + magic refill, on the wing
constexpr int PICKUP_KEY = 9;    // a small key lying in the world

// Item use-action vocabulary (engine-owned; a game's items compose these
// in resources/items/*.json). The engine interprets these; the per-item
// switch is gone.
constexpr int ITEM_USE_NONE = 0;
constexpr int ITEM_USE_BOOMERANG = 1;  // boomerang flight: arc out, homing return
constexpr int ITEM_USE_ARROW = 2;      // arrow projectile
constexpr int ITEM_USE_BOMB = 3;       // placed bomb at the hero's feet
constexpr int ITEM_USE_BOLT = 4;       // element bolt (fire/ice rod)
constexpr int ITEM_USE_HAMMER = 5;     // hero pound: windup + quake in the facing
constexpr int ITEM_USE_SHOVEL = 6;     // hero dig: turns over diggable ground ahead
constexpr int ITEM_USE_HOOKSHOT = 7;   // chained hook: latches, stuns, fetches
constexpr int ITEM_USE_MEDALLION = 8;  // screen-wide element burst (element decides)
constexpr int ITEM_USE_BYRNA = 9;      // toggle aura: invincible while magic drains
constexpr int ITEM_USE_CAPE = 10;      // toggle cloak: invisible + intangible while magic drains
constexpr int ITEM_USE_SOMARIA = 11;   // toggle block: spawns/removes the linked block type
constexpr int ITEM_USE_POWDER = 12;    // sprinkle: stuns, lights torches, awakens shrines
constexpr int ITEM_USE_FLUTE = 13;     // bird-summon fast travel to visited rooms
constexpr int ITEM_USE_NET = 14;       // bug net: catches fairies/bees, agitates enemies
constexpr int ITEM_USE_BOTTLE = 15;    // bottle: fill at water, use the carried contents
constexpr int ITEM_USE_MIRROR = 16;    // reflection: warp to this room's
                                       // parallel-world pair (dark world)

// Projectile elements (rods, future medallions).
constexpr int ELEMENT_NONE = 0;
constexpr int ELEMENT_FIRE = 1;  // burns: kills grunts, lights torches, thaws+shatters
constexpr int ELEMENT_ICE = 2;   // freezes enemies solid

// Item configuration, baked per game from resources/items/<name>.json.
// The use-action composes the vocabulary above with costs and an sfx cue;
// passives are always-on effects (the lantern's dark-room light, the
// flippers' deep-water swimming). An item with useKind ITEM_USE_NONE is
// passive-only (using it plays the error cue).
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
    bool hasSwim;        // passive: deep water is swimmable (flippers)
    bool hasLift;        // passive: heavy objects are liftable (gloves)
    bool hasCompass;     // passive: the map marks chest/boss rooms
    // World-flag upgrade (silver arrows, magical boomerang): while the
    // flag is set, the use-action runs with the upgraded stats.
    int upgradeFlag;     // world flag index, -1 = none
    int upgradeSpeed;    // px/s while upgraded (0 = keep the default)
    int upgradeDamage;   // hits per strike while upgraded (0 = default)
    // Spawned object (Somaria-style block use-actions): the object type
    // the use-action conjures/removes, -1 = none.
    int spawnObject;
};

// Inventory: quantities + owned items + the equipped item (the item
// button runs it). Health lives on the hero; everything else lives here.
// Process-lifetime until the save system lands.
struct Inventory
{
    static const int MAX_ITEMS = 24;

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

    // Bottles (LttP): up to four slots, one thing each. The bottle item
    // uses the first filled slot (or fills the first empty one at water).
    static const int BOTTLE_MAX = 4;
    static const int BOTTLE_EMPTY = 0;
    static const int BOTTLE_WATER = 1;        // fairy-fountain water: 1 heart
    static const int BOTTLE_POTION_RED = 2;   // hearts refill
    static const int BOTTLE_POTION_GREEN = 3; // magic refill
    static const int BOTTLE_POTION_BLUE = 4;  // hearts + magic refill
    static const int BOTTLE_FAIRY = 5;        // releases: full refill, on the wing
    static const int BOTTLE_BEE = 6;         // releases: the bee simply leaves
    int bottles;                             // owned bottle slots (0..BOTTLE_MAX)
    int bottleContents[BOTTLE_MAX];

    Inventory()
        : equippedItem(-1), rupees(0), rupeeMax(99),
          arrows(0), arrowsMax(10), bombs(0), bombsMax(10),
          magic(0), magicMax(16), smallKeys(0), bigKeys(0), bottles(0)
    {
        for (int i = 0; i < MAX_ITEMS; i++) owned[i] = false;
        for (int i = 0; i < BOTTLE_MAX; i++) bottleContents[i] = BOTTLE_EMPTY;
    }

    void give(int item) { if (item >= 0 && item < MAX_ITEMS) owned[item] = true; }
    // Grants a bottle slot (with starting contents); first empty slot
    // wins, a full set ignores it. True when the bottle landed.
    bool giveBottle(int contents)
    {
        if (bottles >= BOTTLE_MAX) return false;
        bottles++;
        for (int i = 0; i < BOTTLE_MAX; i++)
        {
            if (bottleContents[i] == BOTTLE_EMPTY)
            {
                bottleContents[i] = contents;
                break;
            }
        }
        return true;
    }
    // First slot carrying something (contents out through the ref), -1 if
    // every bottle is empty.
    int firstFilledBottle(int& contents) const
    {
        for (int i = 0; i < BOTTLE_MAX; i++)
        {
            if (bottleContents[i] != BOTTLE_EMPTY)
            {
                contents = bottleContents[i];
                return i;
            }
        }
        contents = BOTTLE_EMPTY;
        return -1;
    }
    // First empty slot (-1 when all full) — filling water/catches.
    int firstEmptyBottle() const
    {
        for (int i = 0; i < BOTTLE_MAX; i++)
            if (bottleContents[i] == BOTTLE_EMPTY) return i;
        return -1;
    }
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
