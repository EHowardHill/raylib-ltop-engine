#include "hero.h"
#include "room.h"

#include <math.h>
#include <string.h>

static const float WALK_SPEED = 96.0f;
static const float MOVE_ANIM_FPS = 12.0f;
static const float STAND_ANIM_FPS = 2.0f;
static const float STEP_DISTANCE = 32.0f;

static const float BOX_X = 6.0f;
static const float BOX_Y = 18.0f;
static const float BOX_W = 20.0f;
static const float BOX_H = 12.0f;

static const int START_HEARTS = 3;

static const float ATTACK_DURATION = 0.30f;
static const int ATTACK_FRAMES = 3;
static const float ATTACK_REACH = 26.0f;

static const float HURT_DURATION = 0.28f;
static const float KNOCK_SPEED = 200.0f;
static const float INVULN_DURATION = 1.0f;

static const float FALL_DURATION = 0.45f;
static const int FALL_DAMAGE = 1;  // half-hearts

// Deep water: flinch dip, splash rings, then a hop back onto the bank
// (no damage; drowning without flippers comes with the swimming system).
static const float SPLASH_DURATION = 0.50f;
static const float SPLASH_DIP = 0.18f;     // dip-into-water phase before the hop
static const float SPLASH_HOP = 14.0f;     // hop arc height, px

// Shallow water wading.
static const float WADE_SPEED_SCALE = 0.55f;

// Raised shield (held button): a guarded walk is a slow walk.
static const float GUARD_SPEED_SCALE = 0.55f;

// Deep-water swimming (flippers): each stroke is a speed kick that glides
// against water drag; holding a direction re-strokes once the glide slows.
static const float SWIM_STROKE_SPEED = 150.0f;  // px/s kick per stroke
static const float SWIM_DRAG = 130.0f;          // px/s^2 water drag
static const float SWIM_RESTROKE = 36.0f;       // px/s: slow enough to kick again
static const float SWIM_BOB_FREQ = 5.0f;        // float bob, Hz
static const float SWIM_BOB_AMP = 1.5f;         // float bob, px

// Ice: input accelerates a momentum vector, friction is a slow decay.
static const float ICE_ACCEL = 420.0f;     // px/s^2
static const float ICE_FRICTION = 90.0f;  // px/s^2 decay while coasting

// Ledge vault: two tiles of travel (over the ledge tile itself), an arc,
// then a landing beat with dust rings at the feet.
static const float HOP_ARC = 0.34f;
static const float HOP_LAND = 0.24f;
static const float HOP_HEIGHT = 16.0f;   // arc apex, px
static const float HOP_TRAVEL = 2.0f * TILE_SIZE;

// Hoisting a liftable object: brief pause, then the carrying mode takes
// over (the object rides overhead; main positions it).
const float Hero::LIFT_DURATION = 0.30f;

// Grab-and-pull: one tile of dragged travel, a strained backward glide.
// Matches the object-slide duration in main so the pair stays in step.
static const float PULL_DURATION = 0.22f;

// Hammer (use-action item): windup, then the quake lands, then recover.
const float Hero::HAMMER_WINDUP = 0.18f;
const float Hero::HAMMER_DURATION = 0.42f;
static const float HAMMER_REACH = 30.0f;

// Shovel (use-action item): a stab into the tile ahead.
const float Hero::DIG_WINDUP = 0.18f;
const float Hero::DIG_DURATION = 0.40f;

// Hookshot yank: the reel-in glide toward a latched target (constant
// speed; the duration derives from the distance).
static const float HOOK_PULL_SPEED = 420.0f;  // px/s

// Magic powder: windup, then the puff, then a recover beat.
const float Hero::SPRINKLE_WINDUP = 0.14f;
const float Hero::SPRINKLE_DURATION = 0.34f;

// Bug net: windup, then the swipe (main resolves catches/agitation),
// then a recover beat — the same action shape as the powder flick.
const float Hero::NET_WINDUP = 0.14f;
const float Hero::NET_DURATION = 0.34f;

static const float SPIN_CHARGE = 0.45f;   // held time before a release spins
static const float SPIN_DURATION = 0.50f;
static const float SPIN_RADIUS = 44.0f;   // 360-degree hitbox half-extent

static const float DASH_SPEED = 280.0f;
static const float DASH_DURATION = 0.42f;
static const float CRASH_DURATION = 0.65f;

static const char* SWORD_SHEET = "assets/characters/link_sword.png";
// Hero tool sprites (hammer/shovel/net/shield): 32x32 frames pointing
// up, drawn rotated around the handle-base pivot so one frame per tool
// serves every facing. Missing art falls back to the code-drawn tools.
static const char* TOOL_SHEET = "assets/effects/tools.png";
static const int TOOL_FRAME_HAMMER = 0;
static const int TOOL_FRAME_SHOVEL = 1;
static const int TOOL_FRAME_NET = 2;
static const int TOOL_FRAME_SHIELD = 3;
// Sfx names (assets/audio/): the hero's cue set decodes lazily through
// the shared cache — no per-room re-decode, nothing loads until heard.
static const char* SWORD_SFX = "fighter sword 1.wav";
static const char* HURT_SFX = "link hurt.wav";
static const char* FALL_SFX = "link falls.wav";
static const char* SPIN_CHARGE_SFX = "spin attack charge.wav";
static const char* SPIN_SFX = "spin attack.wav";
static const char* DASH_SFX = "pegasus boots.wav";
static const char* CRASH_SFX = "ram.wav";
static const char* DIE_SFX = "link dies.wav";
static const char* SPLASH_SFX = "fall in water.wav";
static const char* WADE_SFX = "walk water.wav";
static const char* SWIM_SFX = "swim.wav";
static const char* LAND_SFX = "land.wav";
static const char* LIFT_SFX = "lift.wav";

Hero::Hero()
    : cfg(0), sheet(), swordSheet(),
      swordSfx({ SWORD_SFX }), hurtSfx({ HURT_SFX }), fallSfx({ FALL_SFX }),
      spinChargeSfx({ SPIN_CHARGE_SFX }), spinSfx({ SPIN_SFX }),
      dashSfx({ DASH_SFX }), crashSfx({ CRASH_SFX }), dieSfx({ DIE_SFX }),
      splashSfx({ SPLASH_SFX }), wadeSfx({ WADE_SFX }), swimSfx({ SWIM_SFX }),
      landSfx({ LAND_SFX }), liftSfx({ LIFT_SFX }), sfxLoaded(false),
      x(0.0f), y(0.0f), facing(DIR_DOWN), moving(false),
      animTime(0.0f), stepDist(0.0f), state_(HERO_IDLE), stateTime(0.0f),
      invulnTime(0.0f), health_(START_HEARTS * 2), maxHealth_(START_HEARTS * 2),
      knockX(0.0f), knockY(0.0f), swordHeldTime(0.0f), chargeCuePlayed(false),
      dashX(0.0f), dashY(0.0f), swing_(0), safeTileX(-1), safeTileY(-1),
      velX(0.0f), velY(0.0f), splashX(0.0f), splashY(0.0f), wading_(false),
      fallFromX(0.0f), fallFromY(0.0f), fallCenterX(0.0f), fallCenterY(0.0f),
      fallRedirectRoom(-1), fallRedirectX(0), fallRedirectY(0),
      fallRedirectPending(false),
      canSwim_(false), strokeTime(0.0f), riding_(false),
      protected_(false), cloaked_(false),
      inputX(0.0f), inputY(0.0f),
      hopStartX(0.0f), hopStartY(0.0f), hopTargetX(0.0f), hopTargetY(0.0f),
      hopLanded(false), carrying_(false), guarding_(false),
      pullActive(false), pullT(0.0f),
      pullFromX(0.0f), pullFromY(0.0f), pullToX(0.0f), pullToY(0.0f),
      hookPullActive(false), hookPullT(0.0f), hookPullDuration(0.0f),
      hookFromX(0.0f), hookFromY(0.0f), hookToX(0.0f), hookToY(0.0f),
      swordEnabled_(true)
{
}

void Hero::load(const CharacterConfig& config)
{
    unload();
    cfg = &config;
    sheet = LoadTexture(config.image);
    SetTextureFilter(sheet, TEXTURE_FILTER_POINT);

    swordSheet = LoadTexture(SWORD_SHEET);
    SetTextureFilter(swordSheet, TEXTURE_FILTER_POINT);

    toolSheet = LoadTexture(TOOL_SHEET);
    SetTextureFilter(toolSheet, TEXTURE_FILTER_POINT);

    // Sfx stay lazily cached (see the cue set above) — a room switch
    // never re-decodes them.
    sfxLoaded = true;
}

void Hero::unload()
{
    if (cfg && sheet.id != 0) UnloadTexture(sheet);
    if (swordSheet.id != 0) UnloadTexture(swordSheet);
    if (toolSheet.id != 0) UnloadTexture(toolSheet);
    sfxLoaded = false;
    sheet = Texture2D();
    swordSheet = Texture2D();
    toolSheet = Texture2D();
    cfg = 0;
}

void Hero::place(int tileX, int tileY)
{
    x = (float)(tileX * TILE_SIZE);
    y = (float)(tileY * TILE_SIZE);
    safeTileX = tileX;
    safeTileY = tileY;
    velX = 0.0f;
    velY = 0.0f;
    wading_ = false;
    carrying_ = false;
    riding_ = false;
    setState(HERO_IDLE);
    animTime = 0.0f;
    stepDist = 0.0f;
    invulnTime = 0.0f;
}

void Hero::setFallRedirect(int room, int tileX, int tileY)
{
    fallRedirectRoom = room;
    fallRedirectX = tileX;
    fallRedirectY = tileY;
    fallRedirectPending = false;
}

bool Hero::takeFallRedirect(int& room, int& tileX, int& tileY)
{
    if (!fallRedirectPending) return false;
    fallRedirectPending = false;
    room = fallRedirectRoom;
    tileX = fallRedirectX;
    tileY = fallRedirectY;
    return true;
}

void Hero::setState(HeroState next)
{
    state_ = next;
    stateTime = 0.0f;
    // The shield only stays raised while the hero is free to move.
    if (next != HERO_IDLE && next != HERO_WALK) guarding_ = false;
}

void Hero::tileUnder(int& tileX, int& tileY) const
{
    tileX = (int)((x + BOX_X + BOX_W * 0.5f) / TILE_SIZE);
    tileY = (int)((y + BOX_Y + BOX_H * 0.5f) / TILE_SIZE);
}

Vector2 Hero::center() const
{
    Vector2 c = { x + TILE_SIZE * 0.5f, y + TILE_SIZE * 0.5f };
    return c;
}

// Held movement direction, keyboard + dpad/stick (cardinal, normalized
// to unit steps; diagonals keep both axes and the callers scale them).
static void readMoveInput(float& dx, float& dy)
{
    dx = 0.0f;
    dy = 0.0f;
    if (IsKeyDown(KEY_LEFT)) dx -= 1.0f;
    if (IsKeyDown(KEY_RIGHT)) dx += 1.0f;
    if (IsKeyDown(KEY_UP)) dy -= 1.0f;
    if (IsKeyDown(KEY_DOWN)) dy += 1.0f;

    for (int i = 0; i < 4; i++)
    {
        if (!IsGamepadAvailable(i)) continue;
        float ax = GetGamepadAxisMovement(i, GAMEPAD_AXIS_LEFT_X);
        float ay = GetGamepadAxisMovement(i, GAMEPAD_AXIS_LEFT_Y);
        if (ax < -0.25f || IsGamepadButtonDown(i, GAMEPAD_BUTTON_LEFT_FACE_LEFT)) dx -= 1.0f;
        if (ax > 0.25f || IsGamepadButtonDown(i, GAMEPAD_BUTTON_LEFT_FACE_RIGHT)) dx += 1.0f;
        if (ay < -0.25f || IsGamepadButtonDown(i, GAMEPAD_BUTTON_LEFT_FACE_UP)) dy -= 1.0f;
        if (ay > 0.25f || IsGamepadButtonDown(i, GAMEPAD_BUTTON_LEFT_FACE_DOWN)) dy += 1.0f;
    }

    if (dx < 0.0f) dx = -1.0f;
    if (dx > 0.0f) dx = 1.0f;
    if (dy < 0.0f) dy = -1.0f;
    if (dy > 0.0f) dy = 1.0f;
}

bool Hero::slideWithCollision(const Room& room, float dx, float dy)
{
    // Axis-separated move so wall sliding keeps working during knockback.
    // Returns whether the full move happened (dash uses a block as a crash).
    const RoomConfig& rc = room.config();
    float maxX = (float)(rc.mapWidth * TILE_SIZE) - (float)TILE_SIZE;
    float maxY = (float)(rc.mapHeight * TILE_SIZE) - (float)TILE_SIZE;

    float nx = x;
    float ny = y;
    bool full = true;

    if (dx != 0.0f)
    {
        Rectangle box = { nx + dx + BOX_X, y + BOX_Y, BOX_W, BOX_H };
        if (!room.solidBox(box)) nx += dx;
        else full = false;
    }
    if (dy != 0.0f)
    {
        Rectangle box = { nx + BOX_X, ny + dy + BOX_Y, BOX_W, BOX_H };
        if (!room.solidBox(box)) ny += dy;
        else full = false;
    }

    if (nx < 0.0f) { nx = 0.0f; full = false; }
    if (ny < 0.0f) { ny = 0.0f; full = false; }
    if (nx > maxX) { nx = maxX; full = false; }
    if (ny > maxY) { ny = maxY; full = false; }

    x = nx;
    y = ny;
    return full;
}

void Hero::faceVector(float dx, float dy)
{
    if (fabsf(dx) >= fabsf(dy))
    {
        if (dx > 0.0001f) facing = DIR_RIGHT;
        else if (dx < -0.0001f) facing = DIR_LEFT;
    }
    else
    {
        if (dy > 0.0001f) facing = DIR_DOWN;
        else if (dy < -0.0001f) facing = DIR_UP;
    }
}

void Hero::updateFootsteps(const Room& room, float movedDist, const char* overrideSfx)
{
    if (movedDist <= 0.0f) return;
    stepDist += movedDist;
    if (stepDist >= STEP_DISTANCE)
    {
        stepDist -= STEP_DISTANCE;
        // Footsteps cue by name through the shared lazy cache — no
        // per-room decode, nothing loads until the hero walks.
        const char* name = overrideSfx ? overrideSfx : room.walkSfx();
        if (name && name[0]) playNamedSfx(name);
    }
}

void Hero::startFall()
{
    setState(HERO_FALL);
    moving = false;
    // Glide into the hole's center while shrinking: the fall reads as
    // "dropping in", not "shrinking on the spot".
    int tileX = 0;
    int tileY = 0;
    tileUnder(tileX, tileY);
    fallFromX = x;
    fallFromY = y;
    fallCenterX = (float)(tileX * TILE_SIZE);
    fallCenterY = (float)(tileY * TILE_SIZE);
    if (sfxLoaded) PlaySound(fallSfx);
}

void Hero::startSplash()
{
    setState(HERO_SPLASH);
    moving = false;
    velX = 0.0f;
    velY = 0.0f;
    wading_ = false;
    splashX = x;
    splashY = y;
    if (sfxLoaded) PlaySound(splashSfx);
}

void Hero::startSwim()
{
    setState(HERO_SWIM);
    moving = false;
    velX = 0.0f;
    velY = 0.0f;
    wading_ = false;
    swordHeldTime = 0.0f;
    chargeCuePlayed = false;
    strokeTime = 0.0f;
    if (sfxLoaded) PlaySound(splashSfx);
}

void Hero::startLift()
{
    setState(HERO_LIFT);
    moving = false;
    velX = 0.0f;
    velY = 0.0f;
    swordHeldTime = 0.0f;
    chargeCuePlayed = false;
    if (sfxLoaded) PlaySound(liftSfx);
}

void Hero::startGrab()
{
    setState(HERO_GRAB);
    moving = false;
    velX = 0.0f;
    velY = 0.0f;
    swordHeldTime = 0.0f;
    chargeCuePlayed = false;
    pullActive = false;
    pullT = 0.0f;
}

void Hero::startPull(float dx, float dy)
{
    // One-tile backward glide away from the grabbed object (the object
    // slides the same delta in step, driven by main).
    pullActive = true;
    pullT = 0.0f;
    pullFromX = x;
    pullFromY = y;
    pullToX = x + dx * TILE_SIZE;
    pullToY = y + dy * TILE_SIZE;
    moving = true;
}

void Hero::startHammer()
{
    setState(HERO_HAMMER);
    swing_++;  // pound hits dedupe by the action id, like sword swings
    moving = false;
    swordHeldTime = 0.0f;
    chargeCuePlayed = false;
}

void Hero::startDig()
{
    setState(HERO_DIG);
    swing_++;  // one dig turn per action id (main dedupes on it)
    moving = false;
    swordHeldTime = 0.0f;
    chargeCuePlayed = false;
}

void Hero::startHookshot()
{
    setState(HERO_HOOKSHOT);
    moving = false;
    swordHeldTime = 0.0f;
    chargeCuePlayed = false;
    hookPullActive = false;
    hookPullT = 0.0f;
}

void Hero::startHookPull(float targetX, float targetY)
{
    // Yank: reel the hero in to the latched target, gliding free of
    // collision (and of tile hazards — the hookshot crosses pits/water).
    float dist = sqrtf((targetX - x) * (targetX - x) + (targetY - y) * (targetY - y));
    hookPullActive = true;
    hookPullT = 0.0f;
    hookPullDuration = dist / HOOK_PULL_SPEED;
    if (hookPullDuration < 0.05f) hookPullDuration = 0.05f;
    hookFromX = x;
    hookFromY = y;
    hookToX = targetX;
    hookToY = targetY;
    moving = true;
}

void Hero::endHookshot()
{
    // The chain resolved (retracted home); only a non-pulling hookshot
    // state resolves — a yank in progress lands first.
    if (state_ == HERO_HOOKSHOT && !hookPullActive) setState(HERO_IDLE);
}

void Hero::startSprinkle()
{
    setState(HERO_SPRINKLE);
    swing_++;  // one sprinkle per action id (main dedupes on it)
    moving = false;
    swordHeldTime = 0.0f;
    chargeCuePlayed = false;
}

void Hero::startNet()
{
    setState(HERO_NET);
    swing_++;  // one swipe per action id (main dedupes on it)
    moving = false;
    swordHeldTime = 0.0f;
    chargeCuePlayed = false;
}

void Hero::startHop(float dx, float dy, const Room& room)
{
    // Vault over a ledge: two tiles of travel in the press direction. The
    // landing spot has to be free; otherwise the hero stays put (the ledge
    // keeps blocking).
    float targetX = x + dx * HOP_TRAVEL;
    float targetY = y + dy * HOP_TRAVEL;
    Rectangle landing = { targetX + BOX_X, targetY + BOX_Y, BOX_W, BOX_H };
    if (room.solidBox(landing)) return;

    setState(HERO_HOP);
    moving = false;
    velX = 0.0f;
    velY = 0.0f;
    wading_ = false;
    hopStartX = x;
    hopStartY = y;
    hopTargetX = targetX;
    hopTargetY = targetY;
    hopLanded = false;
    faceVector(dx, dy);
}

void Hero::startDeath()
{
    setState(HERO_DEAD);
    moving = false;
    swordHeldTime = 0.0f;
    chargeCuePlayed = false;
    if (sfxLoaded) PlaySound(dieSfx);
}

void Hero::startSpin()
{
    setState(HERO_SPIN);
    swing_++;
    moving = false;
    swordHeldTime = 0.0f;
    chargeCuePlayed = false;
    if (sfxLoaded) PlaySound(spinSfx);
}

void Hero::startDash(float dx, float dy)
{
    setState(HERO_DASH);
    swing_++;  // dash hits are deduped by the action id, like swings
    moving = true;
    float len = sqrtf(dx * dx + dy * dy);
    if (len > 0.0001f)
    {
        dashX = dx / len;
        dashY = dy / len;
    }
    else
    {
        dashX = 0.0f;
        dashY = 1.0f;
    }
    faceVector(dashX, dashY);
    if (sfxLoaded) PlaySound(dashSfx);
}

void Hero::revive()
{
    health_ = maxHealth_;
    invulnTime = 1.0f;
    swordHeldTime = 0.0f;
    chargeCuePlayed = false;
    carrying_ = false;
    setState(HERO_IDLE);
}

void Hero::heal(int halfHearts)
{
    health_ += halfHearts;
    if (health_ > maxHealth_) health_ = maxHealth_;
}

void Hero::addMaxHearts(int halfHearts)
{
    maxHealth_ += halfHearts;
    health_ = maxHealth_;
}

void Hero::setHealth(int halfHearts, int maxHalfHearts)
{
    if (maxHalfHearts <= 0) maxHalfHearts = START_HEARTS * 2;
    maxHealth_ = maxHalfHearts;
    health_ = halfHearts;
    if (health_ < 0) health_ = 0;
    if (health_ > maxHealth_) health_ = maxHealth_;
}

void Hero::hurt(int halfHearts, Vector2 knockDir)
{
    if (invulnTime > 0.0f || state_ == HERO_FALL || state_ == HERO_SPLASH
        || state_ == HERO_HOP || state_ == HERO_DEAD || protected_) return;

    health_ -= halfHearts;
    if (health_ < 0) health_ = 0;

    float len = sqrtf(knockDir.x * knockDir.x + knockDir.y * knockDir.y);
    if (len > 0.0001f)
    {
        knockX = knockDir.x / len;
        knockY = knockDir.y / len;
    }
    else
    {
        knockX = 0.0f;
        knockY = -1.0f;
    }

    invulnTime = INVULN_DURATION;
    setState(HERO_HURT);
    moving = false;
    if (sfxLoaded) PlaySound(hurtSfx);
}

Rectangle Hero::attackBox() const
{
    if (state_ == HERO_SPIN)
    {
        // Full-circle hitbox around the hero.
        return Rectangle{ x + TILE_SIZE * 0.5f - SPIN_RADIUS,
                         y + TILE_SIZE * 0.5f - SPIN_RADIUS,
                         SPIN_RADIUS * 2.0f, SPIN_RADIUS * 2.0f };
    }
    if (state_ == HERO_HAMMER)
    {
        // The pound lands only after the windup: no hitbox while the
        // mallet is still raised. A touch wider than the sword arc.
        if (stateTime < HAMMER_WINDUP) return Rectangle{ 0.0f, 0.0f, 0.0f, 0.0f };
        switch (facing)
        {
        case DIR_DOWN:  return Rectangle{ x + 2.0f, y + TILE_SIZE, TILE_SIZE - 4.0f, HAMMER_REACH };
        case DIR_UP:    return Rectangle{ x + 2.0f, y - HAMMER_REACH, TILE_SIZE - 4.0f, HAMMER_REACH };
        case DIR_RIGHT: return Rectangle{ x + TILE_SIZE, y + 2.0f, HAMMER_REACH, TILE_SIZE - 4.0f };
        case DIR_LEFT: return Rectangle{ x - HAMMER_REACH, y + 2.0f, HAMMER_REACH, TILE_SIZE - 4.0f };
        default: return Rectangle{ 0.0f, 0.0f, 0.0f, 0.0f };
        }
    }
    if (state_ == HERO_SPRINKLE || state_ == HERO_NET)
    {
        // Powder flick / bug-net swipe: the effect lands only after the
        // windup, in the facing (no hitbox while the hand is still back).
        float windup = (state_ == HERO_NET) ? NET_WINDUP : SPRINKLE_WINDUP;
        if (stateTime < windup) return Rectangle{ 0.0f, 0.0f, 0.0f, 0.0f };
        switch (facing)
        {
        case DIR_DOWN:  return Rectangle{ x + 2.0f, y + TILE_SIZE, TILE_SIZE - 4.0f, ATTACK_REACH };
        case DIR_UP:    return Rectangle{ x + 2.0f, y - ATTACK_REACH, TILE_SIZE - 4.0f, ATTACK_REACH };
        case DIR_RIGHT: return Rectangle{ x + TILE_SIZE, y + 2.0f, ATTACK_REACH, TILE_SIZE - 4.0f };
        case DIR_LEFT:  return Rectangle{ x - ATTACK_REACH, y + 2.0f, ATTACK_REACH, TILE_SIZE - 4.0f };
        default: return Rectangle{ 0.0f, 0.0f, 0.0f, 0.0f };
        }
    }
    if (state_ != HERO_ATTACK) return Rectangle{ 0.0f, 0.0f, 0.0f, 0.0f };

    switch (facing)
    {
    case DIR_DOWN:  return Rectangle{ x + 2.0f, y + TILE_SIZE, TILE_SIZE - 4.0f, ATTACK_REACH };
    case DIR_UP:    return Rectangle{ x + 2.0f, y - ATTACK_REACH, TILE_SIZE - 4.0f, ATTACK_REACH };
    case DIR_RIGHT: return Rectangle{ x + TILE_SIZE, y + 2.0f, ATTACK_REACH, TILE_SIZE - 4.0f };
    case DIR_LEFT: return Rectangle{ x - ATTACK_REACH, y + 2.0f, ATTACK_REACH, TILE_SIZE - 4.0f };
    default: return Rectangle{ 0.0f, 0.0f, 0.0f, 0.0f };
    }
}

Rectangle Hero::box() const
{
    return Rectangle{ x + BOX_X, y + BOX_Y, BOX_W, BOX_H };
}

void Hero::advanceWalkCycle(const Room& room, float dt, float movedDist)
{
    // The glide drives the hero forward: stride in (walk frames), not
    // idle-glide — the next update() settles back to idle on landing.
    // Anything but the walking look keeps its own state animation.
    if (movedDist > 0.0f && state_ == HERO_IDLE) setState(HERO_WALK);
    if (state_ != HERO_WALK && state_ != HERO_IDLE) return;
    animTime += dt;
    updateFootsteps(room, movedDist);
}

void Hero::update(const Room& room, float dt)
{
    stateTime += dt;
    animTime += dt;
    if (invulnTime > 0.0f) invulnTime -= dt;
    inputX = 0.0f;
    inputY = 0.0f;

    switch (state_)
    {
    case HERO_ATTACK:
        // Locked in the swing; walking is canceled for its duration.
        // Holding through the swing charges a spin (released after).
        if (stateTime >= ATTACK_DURATION)
        {
            if (swordHeldTime >= SPIN_CHARGE) startSpin();
            else setState(HERO_IDLE);
        }
        break;

    case HERO_SPIN:
        // Full-circle swing; input locked until it resolves.
        if (stateTime >= SPIN_DURATION) setState(HERO_IDLE);
        break;

    case HERO_DASH:
    {
        // Pegasus boots: fixed-direction burst; hitting a wall crashes
        // into a stunned stagger.
        float dist = DASH_SPEED * dt;
        bool full = slideWithCollision(room, dashX * dist, dashY * dist);
        updateFootsteps(room, full ? dist : dist * 0.5f);
        if (!full)
        {
            setState(HERO_CRASH);
            moving = false;
            if (sfxLoaded) PlaySound(crashSfx);
        }
        else if (stateTime >= DASH_DURATION)
        {
            setState(HERO_IDLE);
        }
        break;
    }

    case HERO_CRASH:
        // Wall impact stagger: brief input lockout, no damage.
        if (stateTime >= CRASH_DURATION)
        {
            setState(HERO_IDLE);
            moving = false;
        }
        break;

    case HERO_DEAD:
        // Death sequence plays out; main owns the game-over overlay and
        // the continue flow (revive + room reload).
        break;

    case HERO_HURT:
    {
        // Directional recoil: pushed along the knock vector, decaying to a
        // stop, still colliding with walls.
        float decay = (HURT_DURATION - stateTime) / HURT_DURATION;
        if (decay > 0.0f)
        {
            float move = KNOCK_SPEED * decay * dt;
            slideWithCollision(room, knockX * move, knockY * move);
        }
        // Knocked onto a hazard mid-flinch: hand over immediately —
        // splash (or dive, with flippers) at the bank, fall into the
        // hole — instead of standing on water for the rest of the
        // knockback.
        {
            int tileX = 0;
            int tileY = 0;
            tileUnder(tileX, tileY);
            TileMeta meta = room.tileMetaAt(tileX, tileY);
            if (meta.flags & TILE_FLAG_WATER_DEEP)
            {
                if (canSwim_) startSwim();
                else startSplash();
                break;
            }
            if (meta.flags & TILE_FLAG_PIT)
            {
                startFall();
                break;
            }
        }
        if (stateTime >= HURT_DURATION)
        {
            knockX = 0.0f;
            knockY = 0.0f;
            if (health_ <= 0) startDeath();
            else setState(HERO_IDLE);
        }
        break;
    }

    case HERO_FALL:
        // Frozen over the hole, gliding into its center while shrinking.
        // On completion: damage, then respawn on the last safe tile stood
        // on (or die at 0 hearts) — unless the room's pit-landing redirect
        // is armed, in which case main warps the hero to the room below
        // instead.
        {
            float t = stateTime / FALL_DURATION;
            if (t > 1.0f) t = 1.0f;
            x = fallFromX + (fallCenterX - fallFromX) * t;
            y = fallFromY + (fallCenterY - fallFromY) * t;
        }
        if (stateTime >= FALL_DURATION)
        {
            health_ -= FALL_DAMAGE;
            if (health_ < 0) health_ = 0;
            if (health_ <= 0) startDeath();
            else if (fallRedirectRoom >= 0)
            {
                fallRedirectPending = true;
                setState(HERO_IDLE);  // main drains the warp this frame
            }
            else place(safeTileX, safeTileY);
        }
        break;

    case HERO_SPLASH:
        // Deep water without flippers: flinch dip + splash rings at the
        // waterline, then hop back onto the last safe tile. No damage.
        if (stateTime >= SPLASH_DURATION)
        {
            place(safeTileX, safeTileY);
        }
        break;

    case HERO_HOP:
        // Arc over the ledge tile, then a landing beat with dust rings.
        if (stateTime < HOP_ARC)
        {
            float t = stateTime / HOP_ARC;
            x = hopStartX + (hopTargetX - hopStartX) * t;
            y = hopStartY + (hopTargetY - hopStartY) * t;
        }
        else if (!hopLanded)
        {
            hopLanded = true;
            x = hopTargetX;
            y = hopTargetY;
            moving = false;
            if (sfxLoaded) PlaySound(landSfx);
        }
        else if (stateTime >= HOP_ARC + HOP_LAND)
        {
            setState(HERO_IDLE);
        }
        break;

    case HERO_LIFT:
        // Hoist pause; resolves into the carrying mode (main keeps the
        // object glued overhead from here on).
        if (stateTime >= LIFT_DURATION)
        {
            carrying_ = true;
            moving = false;
            setState(HERO_IDLE);
        }
        break;

    case HERO_GRAB:
        // Holding a grabbable object: the pull in progress glides the
        // hero one tile backward (main slides the object in step, having
        // checked both paths). Otherwise the hero digs in facing the
        // object — main reads inputDir() for the pull; letting go of the
        // action button releases the grab.
        if (pullActive)
        {
            pullT += dt;
            float t = pullT / PULL_DURATION;
            if (t >= 1.0f)
            {
                x = pullToX;
                y = pullToY;
                pullActive = false;
                moving = false;
            }
            else
            {
                x = pullFromX + (pullToX - pullFromX) * t;
                y = pullFromY + (pullToY - pullFromY) * t;
            }
        }
        else
        {
            readMoveInput(inputX, inputY);
            if (!bindingHeld(BINDING_ACTION)) setState(HERO_IDLE);
        }
        break;

    case HERO_HAMMER:
        // Windup, then the pound (main lands the quake effects once
        // HAMMER_WINDUP elapses), then a recover beat.
        if (stateTime >= HAMMER_DURATION) setState(HERO_IDLE);
        break;

    case HERO_DIG:
        // Windup, then the dig (main turns the tile ahead over once
        // DIG_WINDUP elapses), then a recover beat.
        if (stateTime >= DIG_DURATION) setState(HERO_IDLE);
        break;

    case HERO_HOOKSHOT:
        // Locked while the chain flies (main owns the head); a latch yank
        // glides the hero to the target, free of collision and hazards.
        if (hookPullActive)
        {
            hookPullT += dt;
            float t = hookPullT / hookPullDuration;
            if (t >= 1.0f)
            {
                x = hookToX;
                y = hookToY;
                hookPullActive = false;
                moving = false;
                setState(HERO_IDLE);
            }
            else
            {
                x = hookFromX + (hookToX - hookFromX) * t;
                y = hookFromY + (hookToY - hookFromY) * t;
            }
        }
        break;

    case HERO_SPRINKLE:
        // Windup, then the puff (main resolves the powder), then recover.
        if (stateTime >= SPRINKLE_DURATION) setState(HERO_IDLE);
        break;

    case HERO_NET:
        // Windup, then the swipe (main resolves the net), then recover.
        if (stateTime >= NET_DURATION) setState(HERO_IDLE);
        break;

    case HERO_SWIM:
    {
        // Stroke-glide swimming (flippers): a direction press kicks a
        // stroke, the glide decays with water drag, and holding the
        // direction re-strokes once the glide slows. The hero stays in
        // the water until it glides onto a tile that isn't deep — the
        // bank — then stands up (wading or walking from there).
        float dx = 0.0f;
        float dy = 0.0f;
        readMoveInput(dx, dy);
        inputX = dx;
        inputY = dy;

        float speed = sqrtf(velX * velX + velY * velY);
        if ((dx != 0.0f || dy != 0.0f) && speed < SWIM_RESTROKE)
        {
            float len = sqrtf(dx * dx + dy * dy);
            velX = dx / len * SWIM_STROKE_SPEED;
            velY = dy / len * SWIM_STROKE_SPEED;
            faceVector(dx, dy);
            strokeTime = 0.0f;
            if (sfxLoaded) PlaySound(swimSfx);
        }

        // Water drag on the glide.
        speed = sqrtf(velX * velX + velY * velY);
        if (speed > 0.0f)
        {
            float drop = SWIM_DRAG * dt;
            if (speed <= drop)
            {
                velX = 0.0f;
                velY = 0.0f;
            }
            else
            {
                velX *= (speed - drop) / speed;
                velY *= (speed - drop) / speed;
            }
        }

        float beforeX = x;
        float beforeY = y;
        slideWithCollision(room, velX * dt, velY * dt);
        if (fabsf(x - beforeX) < 0.0001f) velX = 0.0f;
        if (fabsf(y - beforeY) < 0.0001f) velY = 0.0f;
        strokeTime += dt;

        moving = sqrtf(velX * velX + velY * velY) > 8.0f;

        // Bank check: only deep water keeps the hero swimming.
        int tileX = 0;
        int tileY = 0;
        tileUnder(tileX, tileY);
        TileMeta meta = room.tileMetaAt(tileX, tileY);
        if (!(meta.flags & TILE_FLAG_WATER_DEEP))
        {
            setState(HERO_IDLE);
            moving = false;
            velX = 0.0f;
            velY = 0.0f;
        }
        break;
    }

    case HERO_IDLE:
    case HERO_WALK:
    default:
    {
        float dx = 0.0f;
        float dy = 0.0f;
        readMoveInput(dx, dy);
        inputX = dx;
        inputY = dy;

        // Active shield (held button): raised while free to move, the
        // walk slows and the projectile block arc widens (main reads
        // guarding() when it resolves rocks).
        guarding_ = !carrying_ && bindingHeld(BINDING_SHIELD);

        // Carrying locks out the offensive actions: the action button
        // (main) throws the held object instead.
        if (carrying_)
        {
            swordHeldTime = 0.0f;
            chargeCuePlayed = false;
        }
        else
        {
            // Spin charge: holding the sword button winds up (cue + weapon
            // shake); releasing it after the threshold spins. Bladeless
            // heroes (the sword is item-gated, as data) never wind up.
            if (swordEnabled_ && bindingHeld(BINDING_SWORD))
            {
                swordHeldTime += dt;
                if (swordHeldTime >= SPIN_CHARGE && !chargeCuePlayed)
                {
                    chargeCuePlayed = true;
                    if (sfxLoaded) PlaySound(spinChargeSfx);
                }
            }
            else
            {
                if (swordHeldTime >= SPIN_CHARGE)
                {
                    startSpin();
                    break;
                }
                swordHeldTime = 0.0f;
                chargeCuePlayed = false;
            }

            // Pegasus boots: dash in the held direction (needs a direction).
            if (dashPressed() && (dx != 0.0f || dy != 0.0f))
            {
                startDash(dx, dy);
                break;
            }

            // Sword swings cancel walking entirely (and need the blade).
            if (swordEnabled_ && swordPressed())
            {
                setState(HERO_ATTACK);
                swing_++;
                moving = false;
                swordHeldTime = 0.0f;
                chargeCuePlayed = false;
                if (sfxLoaded) PlaySound(swordSfx);
                break;
            }
        }

        // The tile under the hero's feet decides how it moves: shallow
        // water slows it to a wade with splashing steps, ice keeps it
        // sliding on momentum.
        int tileX = 0;
        int tileY = 0;
        tileUnder(tileX, tileY);
        TileMeta meta = room.tileMetaAt(tileX, tileY);
        bool onIce = (meta.flags & TILE_FLAG_ICE) != 0;
        wading_ = (meta.flags & TILE_FLAG_WATER_SHALLOW) != 0;
        const char* stepSfx = wading_ ? WADE_SFX : 0;

        float beforeX = x;
        float beforeY = y;

        if (onIce)
        {
            // Momentum movement: input accelerates the velocity, releasing
            // the stick coasts with weak friction, walls kill the axis.
            velX += dx * ICE_ACCEL * dt;
            velY += dy * ICE_ACCEL * dt;
            if (dx == 0.0f && dy == 0.0f)
            {
                float speed = sqrtf(velX * velX + velY * velY);
                float drop = ICE_FRICTION * dt;
                if (speed <= drop)
                {
                    velX = 0.0f;
                    velY = 0.0f;
                }
                else
                {
                    velX *= (speed - drop) / speed;
                    velY *= (speed - drop) / speed;
                }
            }
            float speed = sqrtf(velX * velX + velY * velY);
            if (speed > WALK_SPEED)
            {
                velX *= WALK_SPEED / speed;
                velY *= WALK_SPEED / speed;
            }
            slideWithCollision(room, velX * dt, velY * dt);
            if (fabsf(x - beforeX) < 0.0001f) velX = 0.0f;
            if (fabsf(y - beforeY) < 0.0001f) velY = 0.0f;
        }
        else
        {
            // Direct movement, diagonals at the same overall speed, wading
            // and the raised shield slower than dry land. A single step
            // accumulator keeps the walk sfx from doubling up.
            velX = 0.0f;
            velY = 0.0f;
            const float diagonalScale = (dx != 0.0f && dy != 0.0f) ? 0.70710678f : 1.0f;
            float speedScale = wading_ ? WADE_SPEED_SCALE : 1.0f;
            if (guarding_) speedScale *= GUARD_SPEED_SCALE;
            slideWithCollision(room,
                               dx * WALK_SPEED * speedScale * dt * diagonalScale,
                               dy * WALK_SPEED * speedScale * dt * diagonalScale);
        }

        float dist = sqrtf((x - beforeX) * (x - beforeX) + (y - beforeY) * (y - beforeY));
        updateFootsteps(room, dist, stepSfx);

        // Conveyor/current drift: the tile underfoot carries the hero (not
        // counted as steps — it isn't walking).
        if (meta.pushX != 0 || meta.pushY != 0)
        {
            slideWithCollision(room, meta.pushX * dt, meta.pushY * dt);
        }

        // Ledge vault: pressing into a ledge tile along its facing hops
        // over it (one-way; from every other side it just blocks).
        if ((dx != 0.0f) != (dy != 0.0f))
        {
            float probeX = x + BOX_X + BOX_W * 0.5f + dx * 20.0f;
            float probeY = y + BOX_Y + BOX_H * 0.5f + dy * 20.0f;
            TileMeta ahead = room.tileMetaAt((int)(probeX / TILE_SIZE),
                                             (int)(probeY / TILE_SIZE));
            if (ahead.flags & TILE_FLAG_LEDGE)
            {
                Direction press;
                if (dy > 0.0f) press = DIR_DOWN;
                else if (dy < 0.0f) press = DIR_UP;
                else if (dx > 0.0f) press = DIR_RIGHT;
                else press = DIR_LEFT;
                if ((int)press == (int)ahead.ledgeDir)
                {
                    startHop(dx, dy, room);
                    break;
                }
            }
        }

        moving = (dx != 0.0f) || (dy != 0.0f)
                 || sqrtf(velX * velX + velY * velY) > 24.0f;
        if (dx != 0.0f) facing = (dx < 0.0f) ? DIR_LEFT : DIR_RIGHT;
        else if (dy != 0.0f) facing = (dy < 0.0f) ? DIR_UP : DIR_DOWN;

        HeroState walkState = moving ? HERO_WALK : HERO_IDLE;
        if (state_ != walkState) setState(walkState);

        // Tile hazards are checked against the tile under the hero's feet,
        // recomputed after the move (it may have crossed a tile boundary).
        // Riding a moving platform, the platform is the ground underfoot —
        // water below never gets a chance to splash.
        tileUnder(tileX, tileY);
        meta = room.tileMetaAt(tileX, tileY);

        if (riding_)
        {
            // No hazards and no safe-tile bookkeeping while carried along.
        }
        else if (meta.flags & TILE_FLAG_WATER_DEEP)
        {
            if (canSwim_) startSwim();
            else startSplash();
        }
        else if (meta.flags & TILE_FLAG_PIT)
        {
            startFall();
        }
        else
        {
            safeTileX = tileX;
            safeTileY = tileY;
            if ((meta.flags & TILE_FLAG_DAMAGE) && meta.damage > 0)
            {
                hurt(meta.damage, Vector2{ -dx, -dy });
            }
        }
        break;
    }
    }
}

// Reflection squash: how tall the mirrored copy is vs. the sprite.
static const float REFLECT_SQUASH = 0.62f;

// Animation channel for the current state (see hero.h): the optional
// character blocks when the sheet carries them, the stand/move pair (with
// its own cross-fallback) otherwise.
const DirectionalFrames* Hero::animFrames() const
{
    if (!cfg) return 0;
    const bool walking = (state_ == HERO_WALK || state_ == HERO_DASH);
    const DirectionalFrames* primary = 0;
    const DirectionalFrames* secondary = 0;
    switch (state_)
    {
    case HERO_SWIM:
        primary = &cfg->swim[facing];
        secondary = &cfg->move[facing];
        break;
    case HERO_ATTACK:
    case HERO_SPIN:
        primary = &cfg->attack[facing];
        secondary = &cfg->stand[facing];
        break;
    case HERO_HURT:
        primary = &cfg->hurt[facing];
        secondary = &cfg->stand[facing];
        break;
    case HERO_LIFT:
        primary = &cfg->lift[facing];
        secondary = &cfg->stand[facing];
        break;
    default:
        if (carrying_)
        {
            primary = &cfg->lift[facing];
            secondary = walking ? &cfg->move[facing] : &cfg->stand[facing];
        }
        else
        {
            primary = walking ? &cfg->move[facing] : &cfg->stand[facing];
            secondary = walking ? &cfg->stand[facing] : &cfg->move[facing];
        }
        break;
    }
    if (primary->count > 0) return primary;
    if (secondary->count > 0) return secondary;
    return &cfg->stand[facing];
}

void Hero::drawReflection() const
{
    // Only the plain standing/walking hero reflects; the special-state
    // sprites (falling, splashing, hopping, swimming, dying) don't.
    if (!cfg) return;
    if (state_ == HERO_FALL || state_ == HERO_SPLASH || state_ == HERO_HOP
        || state_ == HERO_SWIM || state_ == HERO_DEAD)
        return;

    const DirectionalFrames* frames = (state_ == HERO_WALK || state_ == HERO_DASH)
                                          ? &cfg->move[facing] : &cfg->stand[facing];
    if (frames->count <= 0)
        frames = (state_ == HERO_WALK || state_ == HERO_DASH)
                     ? &cfg->stand[facing] : &cfg->move[facing];
    if (frames->count <= 0) return;

    float fps = (state_ == HERO_WALK || state_ == HERO_DASH) ? MOVE_ANIM_FPS : STAND_ANIM_FPS;
    int index = (int)(animTime * fps);
    if (index < 0) index = 0;
    int frame = frames->frames[index % frames->count];

    int columns = cfg->sheetColumns;
    if (columns <= 0) columns = 1;
    if (frame < 0) frame = 0;
    if (frame >= cfg->frameCount && cfg->frameCount > 0) frame = cfg->frameCount - 1;

    // Flipped (negative source height) below the feet, squashed, tinted
    // like the water it stands in.
    Rectangle src = { (float)((frame % columns) * TILE_SIZE),
                     (float)((frame / columns) * TILE_SIZE),
                     (float)TILE_SIZE, -(float)TILE_SIZE };
    Rectangle dest = { x, y + (float)TILE_SIZE,
                       (float)TILE_SIZE, (float)TILE_SIZE * REFLECT_SQUASH };
    Vector2 origin = { 0.0f, 0.0f };
    DrawTexturePro(sheet, src, dest, origin, 0.0f, Color{ 175, 200, 240, 100 });
}

// Tool sprites rotate around the handle base (the hands); facing maps
// to the base angle of an up-pointing tool.
static float toolFacingAngle(Direction facing)
{
    switch (facing)
    {
    case DIR_UP: return 0.0f;
    case DIR_RIGHT: return 90.0f;
    case DIR_DOWN: return 180.0f;
    default: return 270.0f;
    }
}

void Hero::drawTool(int frame, float px, float py, float angle,
                    bool centered) const
{
    if (toolSheet.id == 0) return;
    Rectangle src = { (float)(frame * TILE_SIZE), 0.0f,
                     (float)TILE_SIZE, (float)TILE_SIZE };
    Rectangle dest = { px, py, (float)TILE_SIZE, (float)TILE_SIZE };
    Vector2 origin = centered ? Vector2{ TILE_SIZE * 0.5f, TILE_SIZE * 0.5f }
                              : Vector2{ 16.0f, 28.0f };
    DrawTexturePro(toolSheet, src, dest, origin, angle, WHITE);
}

void Hero::drawFrame(int frame, float scaleX, float scaleY, float yOff) const
{
    if (!cfg) return;

    int columns = cfg->sheetColumns;
    if (columns <= 0) columns = 1;
    if (frame < 0) frame = 0;
    if (frame >= cfg->frameCount && cfg->frameCount > 0) frame = cfg->frameCount - 1;

    Rectangle src = { (float)((frame % columns) * TILE_SIZE),
                     (float)((frame / columns) * TILE_SIZE),
                     (float)TILE_SIZE, (float)TILE_SIZE };

    float w = (float)TILE_SIZE * scaleX;
    float h = (float)TILE_SIZE * scaleY;
    Rectangle dest = { x + (float)TILE_SIZE * 0.5f - w * 0.5f,
                       y + (float)TILE_SIZE * 0.5f - h * 0.5f + yOff, w, h };
    Vector2 origin = { 0.0f, 0.0f };
    DrawTexturePro(sheet, src, dest, origin, 0.0f, WHITE);
}

void Hero::draw() const
{
    if (!cfg) return;

    // Cloak (magic cape): a ghostly blink — mostly there, barely solid.
    if (cloaked_ && ((int)(GetTime() * 9.0f) % 4 == 3))
        return;

    // Invincibility flicker: skip every other blink while i-frames last.
    if (invulnTime > 0.0f && state_ != HERO_FALL && state_ != HERO_SPLASH
        && state_ != HERO_HOP && state_ != HERO_DEAD
        && ((int)(invulnTime * 30.0f) % 2 == 0))
        return;

    if (state_ == HERO_DEAD)
    {
        // Spin-collapse: rotate and shrink into the ground, then nothing.
        if (stateTime > 1.0f) return;
        int frame = cfg->stand[facing].count > 0 ? cfg->stand[facing].frames[0] : 0;
        int columns = cfg->sheetColumns;
        if (columns <= 0) columns = 1;
        if (frame >= cfg->frameCount && cfg->frameCount > 0) frame = cfg->frameCount - 1;
        Rectangle src = { (float)((frame % columns) * TILE_SIZE),
                         (float)((frame / columns) * TILE_SIZE),
                         (float)TILE_SIZE, (float)TILE_SIZE };
        float scale = 1.0f - stateTime;
        if (scale < 0.05f) return;
        float w = (float)TILE_SIZE * scale;
        // DrawTexturePro positions the drawn rect's top-left at
        // (dest.xy - origin): the pivot goes at the tile center.
        Rectangle dest = { x + (float)TILE_SIZE * 0.5f,
                           y + (float)TILE_SIZE * 0.5f, w, w };
        Vector2 origin = { w * 0.5f, w * 0.5f };
        DrawTexturePro(sheet, src, dest, origin, stateTime * 720.0f, WHITE);
        return;
    }

    if (state_ == HERO_FALL)
    {
        // Shrink into the hole.
        float t = stateTime / FALL_DURATION;
        if (t > 1.0f) t = 1.0f;
        float scale = 1.0f - t;
        if (scale <= 0.05f) return;
        int frame = cfg->stand[facing].count > 0 ? cfg->stand[facing].frames[0] : 0;
        drawFrame(frame, scale, scale);
        return;
    }

    if (state_ == HERO_SPLASH)
    {
        // Deep water: expanding splash rings on the surface while the hero
        // dips in, then a hop back onto the bank with a small arc.
        Vector2 c = { splashX + TILE_SIZE * 0.5f, splashY + TILE_SIZE * 0.7f };
        float t = stateTime / SPLASH_DURATION;
        for (int ring = 0; ring < 2; ring++)
        {
            float rt = t - ring * 0.12f;
            if (rt < 0.0f || rt > 0.8f) continue;
            float radius = 4.0f + rt * 26.0f;
            unsigned char alpha = (unsigned char)((1.0f - rt / 0.8f) * 200.0f);
            DrawCircleLines((int)c.x, (int)(c.y - rt * 4.0f), radius,
                            Color{ 130, 175, 255, alpha });
        }

        float hx = splashX;
        float hy = splashY;
        float scale = 1.0f;
        float hop = 0.0f;
        if (stateTime < SPLASH_DIP)
        {
            float dt2 = stateTime / SPLASH_DIP;
            scale = 1.0f - dt2 * 0.25f;
            hy += dt2 * 5.0f;
        }
        else
        {
            float t2 = (stateTime - SPLASH_DIP) / (SPLASH_DURATION - SPLASH_DIP);
            if (t2 > 1.0f) t2 = 1.0f;
            hx = splashX + ((float)(safeTileX * TILE_SIZE) - splashX) * t2;
            hy = splashY + ((float)(safeTileY * TILE_SIZE) - splashY) * t2;
            hop = sinf(t2 * PI) * SPLASH_HOP;
            scale = 0.75f + 0.25f * t2;
        }

        int frame = cfg->stand[facing].count > 0 ? cfg->stand[facing].frames[0] : 0;
        int columns = cfg->sheetColumns;
        if (columns <= 0) columns = 1;
        if (frame >= cfg->frameCount && cfg->frameCount > 0) frame = cfg->frameCount - 1;
        Rectangle src = { (float)((frame % columns) * TILE_SIZE),
                         (float)((frame / columns) * TILE_SIZE),
                         (float)TILE_SIZE, (float)TILE_SIZE };
        float w = (float)TILE_SIZE * scale;
        float h = (float)TILE_SIZE * scale;
        Rectangle dest = { hx + (float)TILE_SIZE * 0.5f - w * 0.5f,
                          hy + (float)TILE_SIZE * 0.5f - h * 0.5f - hop, w, h };
        Vector2 origin = { 0.0f, 0.0f };
        DrawTexturePro(sheet, src, dest, origin, 0.0f, WHITE);
        return;
    }

    if (state_ == HERO_HOP)
    {
        // Arc over the ledge with a ground shadow, then landing dust rings.
        float t = stateTime / HOP_ARC;
        float hop = 0.0f;
        if (t < 1.0f) hop = sinf(PI * t) * HOP_HEIGHT;

        float shadowScale = 1.0f - (hop / HOP_HEIGHT) * 0.5f;
        DrawEllipse((int)(x + TILE_SIZE * 0.5f), (int)(y + TILE_SIZE - 2.0f),
                    10.0f * shadowScale, 4.0f * shadowScale,
                    Color{ 20, 20, 40, 100 });

        if (hopLanded)
        {
            float lt = (stateTime - HOP_ARC) / HOP_LAND;
            for (int ring = 0; ring < 2; ring++)
            {
                float rt = lt - ring * 0.15f;
                if (rt < 0.0f || rt > 1.0f) continue;
                float radius = 3.0f + rt * 15.0f;
                unsigned char alpha = (unsigned char)((1.0f - rt) * 180.0f);
                DrawCircleLines((int)(x + TILE_SIZE * 0.5f),
                                (int)(y + TILE_SIZE - 4.0f), radius,
                                Color{ 205, 205, 215, alpha });
            }
        }

        int frame = cfg->stand[facing].count > 0 ? cfg->stand[facing].frames[0] : 0;
        int columns = cfg->sheetColumns;
        if (columns <= 0) columns = 1;
        if (frame >= cfg->frameCount && cfg->frameCount > 0) frame = cfg->frameCount - 1;
        Rectangle src = { (float)((frame % columns) * TILE_SIZE),
                         (float)((frame / columns) * TILE_SIZE),
                         (float)TILE_SIZE, (float)TILE_SIZE };
        Rectangle dest = { x, y - hop, (float)TILE_SIZE, (float)TILE_SIZE };
        Vector2 origin = { 0.0f, 0.0f };
        DrawTexturePro(sheet, src, dest, origin, 0.0f, WHITE);
        return;
    }

    if (state_ == HERO_SWIM)
    {
        // Floating on the surface: a slow bob plus ripple rings that mark
        // each stroke (no blob shadow — main skips it while swimming).
        const DirectionalFrames* frames = animFrames();
        if (!frames || frames->count <= 0) return;
        float fps = moving ? MOVE_ANIM_FPS : STAND_ANIM_FPS;
        int frame = frames->frames[(int)(animTime * fps) % frames->count];

        float bob = sinf(animTime * SWIM_BOB_FREQ * PI) * SWIM_BOB_AMP;
        Vector2 c = { x + TILE_SIZE * 0.5f, y + TILE_SIZE * 0.72f };
        for (int ring = 0; ring < 2; ring++)
        {
            float rt = strokeTime - ring * 0.14f;
            if (rt < 0.0f || rt > 0.7f) continue;
            float radius = 4.0f + rt * 24.0f;
            unsigned char alpha = (unsigned char)((1.0f - rt / 0.7f) * 150.0f);
            DrawCircleLines((int)c.x, (int)c.y, radius,
                            Color{ 150, 190, 255, alpha });
        }
        drawFrame(frame, 1.0f, 1.0f, bob);
        return;
    }

    if (state_ == HERO_HAMMER)
    {
        // Mallet: raised through the windup, slammed at the pound (the
        // quake ring stays code-drawn — it's FX, not chrome). The sprite
        // swings by rotation; without art the code-drawn mallet stands in.
        float cx = x + TILE_SIZE * 0.5f;
        float cy = y + TILE_SIZE * 0.5f;
        float fdx = (facing == DIR_LEFT) ? -1.0f : (facing == DIR_RIGHT) ? 1.0f : 0.0f;
        float fdy = (facing == DIR_UP) ? -1.0f : (facing == DIR_DOWN) ? 1.0f : 0.0f;
        float impactX = cx + fdx * 22.0f;
        float impactY = cy + fdy * 22.0f;

        if (toolSheet.id != 0)
        {
            float base = toolFacingAngle(facing);
            if (stateTime < HAMMER_WINDUP)
                drawTool(TOOL_FRAME_HAMMER, cx, cy, base - 65.0f);
            else
            {
                drawTool(TOOL_FRAME_HAMMER, cx, cy, base);
                float rt = (stateTime - HAMMER_WINDUP) / (HAMMER_DURATION - HAMMER_WINDUP);
                if (rt > 1.0f) rt = 1.0f;
                float radius = 4.0f + rt * 24.0f;
                unsigned char alpha = (unsigned char)((1.0f - rt) * 170.0f);
                DrawCircleLines((int)impactX, (int)impactY, radius,
                                Color{ 205, 205, 215, alpha });
            }
        }
        else if (stateTime < HAMMER_WINDUP)
        {
            // Raised: handle up from the hands, head above the hero.
            float raise = stateTime / HAMMER_WINDUP;
            float headY = y - 8.0f - raise * 4.0f;
            DrawLine((int)cx, (int)(cy - 4.0f), (int)cx, (int)(headY + 4.0f),
                     Color{ 120, 84, 52, 255 });
            DrawRectangle((int)(cx - 8.0f), (int)headY, 16, 8,
                          Color{ 148, 148, 160, 255 });
            DrawRectangle((int)(cx - 8.0f), (int)headY, 16, 3,
                          Color{ 196, 200, 210, 255 });
        }
        else
        {
            // Slammed at the impact point; the quake ring spreads.
            DrawLine((int)cx, (int)cy, (int)(impactX - fdx * 5.0f),
                     (int)(impactY - fdy * 5.0f), Color{ 120, 84, 52, 255 });
            if (fdy != 0.0f)
            {
                DrawRectangle((int)(impactX - 8.0f), (int)impactY, 16, 8,
                              Color{ 148, 148, 160, 255 });
                DrawRectangle((int)(impactX - 8.0f), (int)impactY, 16, 3,
                              Color{ 196, 200, 210, 255 });
            }
            else
            {
                DrawRectangle((int)impactX, (int)(impactY - 8.0f), 8, 16,
                              Color{ 148, 148, 160, 255 });
                DrawRectangle((int)impactX, (int)(impactY - 8.0f), 3, 16,
                              Color{ 196, 200, 210, 255 });
            }
            float rt = (stateTime - HAMMER_WINDUP) / (HAMMER_DURATION - HAMMER_WINDUP);
            if (rt > 1.0f) rt = 1.0f;
            float radius = 4.0f + rt * 24.0f;
            unsigned char alpha = (unsigned char)((1.0f - rt) * 170.0f);
            DrawCircleLines((int)impactX, (int)impactY, radius,
                            Color{ 205, 205, 215, alpha });
        }
    }

    if (state_ == HERO_DIG)
    {
        // Shovel: held up over the windup, then stabbed into the tile
        // ahead (rotation drives the sprite; the dirt burst is FX).
        float cx = x + TILE_SIZE * 0.5f;
        float cy = y + TILE_SIZE * 0.5f;
        float fdx = (facing == DIR_LEFT) ? -1.0f : (facing == DIR_RIGHT) ? 1.0f : 0.0f;
        float fdy = (facing == DIR_UP) ? -1.0f : (facing == DIR_DOWN) ? 1.0f : 0.0f;
        float tipX = cx + fdx * 20.0f;
        float tipY = cy + fdy * 20.0f;

        if (toolSheet.id != 0)
        {
            float base = toolFacingAngle(facing);
            if (stateTime < DIG_WINDUP)
                drawTool(TOOL_FRAME_SHOVEL, cx, cy, base - 55.0f);
            else
                drawTool(TOOL_FRAME_SHOVEL, cx, cy, base + 12.0f);
        }
        else if (stateTime < DIG_WINDUP)
        {
            // Held up: handle over the shoulder, blade above the hero.
            float raise = stateTime / DIG_WINDUP;
            float bladeY = y - 6.0f - raise * 3.0f;
            DrawLine((int)cx, (int)(cy - 6.0f), (int)cx, (int)(bladeY + 6.0f),
                     Color{ 120, 84, 52, 255 });
            DrawRectangle((int)(cx - 6.0f), (int)bladeY, 12, 7,
                          Color{ 176, 168, 152, 255 });
            DrawRectangle((int)(cx - 6.0f), (int)bladeY, 12, 2,
                          Color{ 214, 210, 198, 255 });
        }
        else
        {
            // Stabbed into the tile ahead; dirt sprays at the tip.
            DrawLine((int)cx, (int)cy, (int)(tipX - fdx * 4.0f),
                     (int)(tipY - fdy * 4.0f), Color{ 120, 84, 52, 255 });
            if (fdy != 0.0f)
            {
                DrawRectangle((int)(tipX - 6.0f), (int)tipY, 12, 7,
                              Color{ 176, 168, 152, 255 });
                DrawRectangle((int)(tipX - 6.0f), (int)tipY, 12, 2,
                              Color{ 214, 210, 198, 255 });
            }
            else
            {
                DrawRectangle((int)tipX, (int)(tipY - 6.0f), 7, 12,
                              Color{ 176, 168, 152, 255 });
                DrawRectangle((int)tipX, (int)(tipY - 6.0f), 2, 12,
                              Color{ 214, 210, 198, 255 });
            }
        }
    }

    if (state_ == HERO_NET)
    {
        // Bug net: one sprite swept in an arc through the facing — the
        // rotation carries the swish (the code-drawn hoop stands in
        // without art).
        float cx = x + TILE_SIZE * 0.5f;
        float cy = y + TILE_SIZE * 0.5f;
        float fdx = (facing == DIR_LEFT) ? -1.0f : (facing == DIR_RIGHT) ? 1.0f : 0.0f;
        float fdy = (facing == DIR_UP) ? -1.0f : (facing == DIR_DOWN) ? 1.0f : 0.0f;
        // The swing sweeps from rest to full reach and back.
        float t = stateTime / NET_DURATION;
        if (toolSheet.id != 0)
        {
            float base = toolFacingAngle(facing);
            float sweep = clampf(t, 0.0f, 1.0f);
            drawTool(TOOL_FRAME_NET, cx, cy, base - 55.0f + sweep * 110.0f);
        }
        else
        {
            float reach = 12.0f + 14.0f * sinf(PI * clampf(t, 0.0f, 1.0f));
            float netX = cx + fdx * (reach + 6.0f) - fdy * 2.0f;
            float netY = cy + fdy * (reach + 6.0f) - fdx * 2.0f;

            // Handle from the hands to the hoop.
            DrawLine((int)cx, (int)cy, (int)netX, (int)netY, Color{ 120, 84, 52, 255 });
            // Hoop + crosshatch mesh.
            DrawCircleLines((int)netX, (int)netY, 8, Color{ 196, 176, 120, 255 });
            for (int i = -1; i <= 1; i++)
            {
                DrawLine((int)(netX - 6.0f), (int)(netY + i * 4.0f),
                         (int)(netX + 6.0f), (int)(netY + i * 4.0f),
                         Color{ 226, 214, 170, 160 });
                DrawLine((int)(netX + i * 4.0f), (int)(netY - 6.0f),
                         (int)(netX + i * 4.0f), (int)(netY + 6.0f),
                         Color{ 226, 214, 170, 160 });
            }
        }
    }

    if (state_ == HERO_SPIN && swordSheet.id != 0)
    {
        // Spin: the slash arc whirls around the hero (two rotations).
        // The pivot (dest + origin pair) sits on the hero's center.
        Rectangle src = { 0.0f, 0.0f, 64.0f, 64.0f };
        Vector2 centerPos = { x + (float)TILE_SIZE * 0.5f, y + (float)TILE_SIZE * 0.5f };
        Rectangle dest = { centerPos.x, centerPos.y, 64.0f, 64.0f };
        Vector2 origin = { 32.0f, 32.0f };
        DrawTexturePro(swordSheet, src, dest, origin,
                        (stateTime / SPIN_DURATION) * 720.0f, WHITE);
    }
    else if (state_ == HERO_ATTACK && swordSheet.id != 0)
    {
        // Placeholder slash sheet: 64x64 frames, one column per direction
        // (down/right/left/up, matching Direction), 3 rows of sweep frames.
        int index = (int)(stateTime / (ATTACK_DURATION / (float)ATTACK_FRAMES));
        if (index < 0) index = 0;
        if (index >= ATTACK_FRAMES) index = ATTACK_FRAMES - 1;
        Rectangle src = { (float)(facing * 64), (float)(index * 64), 64.0f, 64.0f };
        Rectangle dest = { x - 16.0f, y - 16.0f, 64.0f, 64.0f };
        Vector2 origin = { 0.0f, 0.0f };
        DrawTexturePro(swordSheet, src, dest, origin, 0.0f, WHITE);
    }
    else if ((state_ == HERO_IDLE || state_ == HERO_WALK)
              && swordHeldTime >= 0.15f && swordSheet.id != 0)
    {
        // Charging a spin: weapon-shake feedback on the held sword.
        float jitter = swordHeldTime >= SPIN_CHARGE ? 2.0f : 1.0f;
        float ox = (((stateTime * 91.7f) - (float)(int)(stateTime * 91.7f)) * jitter - jitter * 0.5f);
        float oy = (((stateTime * 74.3f) - (float)(int)(stateTime * 74.3f)) * jitter - jitter * 0.5f);
        Rectangle src = { (float)(facing * 64), 0.0f, 64.0f, 64.0f };
        Rectangle dest = { x - 16.0f + ox, y - 16.0f + oy, 64.0f, 64.0f };
        Vector2 origin = { 0.0f, 0.0f };
        DrawTexturePro(swordSheet, src, dest, origin, 0.0f, WHITE);
    }

    const DirectionalFrames* frames = animFrames();
    if (!frames || frames->count <= 0) return;

    // Action channels animate at the walk cadence; the idle pair keeps
    // its slow breathing pace.
    float fps = (state_ == HERO_WALK || state_ == HERO_DASH
                 || state_ == HERO_ATTACK || state_ == HERO_SPIN
                 || (state_ == HERO_SWIM && moving))
                    ? MOVE_ANIM_FPS : STAND_ANIM_FPS;
    int index = (int)(animTime * fps);
    if (index < 0) index = 0;
    index = frames->frames[index % frames->count];

    drawFrame(index, 1.0f, 1.0f);

    // Raised shield pose: the carried shield sprite held toward the
    // facing edge (the code-drawn plate stands in without art).
    if (guarding_)
    {
        float cx;
        float cy;
        bool vertical;
        switch (facing)
        {
        case DIR_DOWN: cx = x + 24.0f; cy = y + 21.0f; vertical = true; break;
        case DIR_UP:   cx = x + 9.0f;  cy = y + 8.0f;  vertical = true; break;
        case DIR_RIGHT: cx = x + 26.0f; cy = y + 16.0f; vertical = false; break;
        default:       cx = x + 6.0f;  cy = y + 16.0f; vertical = false; break;
        }
        if (toolSheet.id != 0)
        {
            drawTool(TOOL_FRAME_SHIELD, cx, cy, 0.0f, true);
        }
        else
        {
            float rx = vertical ? 5.0f : 7.0f;
            float ry = vertical ? 7.0f : 5.0f;
            DrawEllipse((int)cx, (int)cy, rx + 1.0f, ry + 1.0f, Color{ 70, 74, 96, 255 });
            DrawEllipse((int)cx, (int)cy, rx, ry, Color{ 168, 176, 200, 255 });
            DrawEllipse((int)(cx - 1.0f), (int)(cy - 1.0f), rx * 0.45f, ry * 0.45f,
                        Color{ 214, 222, 240, 255 });
        }
    }
}