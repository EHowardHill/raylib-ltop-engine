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

static const float SPIN_CHARGE = 0.45f;   // held time before a release spins
static const float SPIN_DURATION = 0.50f;
static const float SPIN_RADIUS = 44.0f;   // 360-degree hitbox half-extent

static const float DASH_SPEED = 280.0f;
static const float DASH_DURATION = 0.42f;
static const float CRASH_DURATION = 0.65f;

static const char* SWORD_SHEET = "assets/characters/link_sword.png";
static const char* SWORD_SFX = "assets/audio/fighter sword 1.wav";
static const char* HURT_SFX = "assets/audio/link hurt.wav";
static const char* FALL_SFX = "assets/audio/link falls.wav";
static const char* SPIN_CHARGE_SFX = "assets/audio/spin attack charge.wav";
static const char* SPIN_SFX = "assets/audio/spin attack.wav";
static const char* DASH_SFX = "assets/audio/pegasus boots.wav";
static const char* CRASH_SFX = "assets/audio/ram.wav";
static const char* DIE_SFX = "assets/audio/link dies.wav";
static const char* SPLASH_SFX = "assets/audio/fall in water.wav";
static const char* WADE_SFX = "assets/audio/walk water.wav";
static const char* LAND_SFX = "assets/audio/land.wav";
static const char* LIFT_SFX = "assets/audio/lift.wav";

Hero::Hero()
    : cfg(0), sheet(), swordSheet(), swordSfx(), hurtSfx(), fallSfx(),
      spinChargeSfx(), spinSfx(), dashSfx(), crashSfx(), dieSfx(),
      splashSfx(), wadeSfx(), landSfx(), liftSfx(), sfxLoaded(false),
      x(0.0f), y(0.0f), facing(DIR_DOWN), moving(false),
      animTime(0.0f), stepDist(0.0f), state_(HERO_IDLE), stateTime(0.0f),
      invulnTime(0.0f), health_(START_HEARTS * 2), maxHealth_(START_HEARTS * 2),
      knockX(0.0f), knockY(0.0f), swordHeldTime(0.0f), chargeCuePlayed(false),
      dashX(0.0f), dashY(0.0f), swing_(0), safeTileX(-1), safeTileY(-1),
      velX(0.0f), velY(0.0f), splashX(0.0f), splashY(0.0f), wading_(false),
      inputX(0.0f), inputY(0.0f),
      hopStartX(0.0f), hopStartY(0.0f), hopTargetX(0.0f), hopTargetY(0.0f),
      hopLanded(false), carrying_(false)
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

    swordSfx = LoadSound(SWORD_SFX);
    hurtSfx = LoadSound(HURT_SFX);
    fallSfx = LoadSound(FALL_SFX);
    spinChargeSfx = LoadSound(SPIN_CHARGE_SFX);
    spinSfx = LoadSound(SPIN_SFX);
    dashSfx = LoadSound(DASH_SFX);
    crashSfx = LoadSound(CRASH_SFX);
    dieSfx = LoadSound(DIE_SFX);
    splashSfx = LoadSound(SPLASH_SFX);
    wadeSfx = LoadSound(WADE_SFX);
    landSfx = LoadSound(LAND_SFX);
    liftSfx = LoadSound(LIFT_SFX);
    sfxLoaded = true;
}

void Hero::unload()
{
    if (cfg && sheet.id != 0) UnloadTexture(sheet);
    if (swordSheet.id != 0) UnloadTexture(swordSheet);
    if (sfxLoaded)
    {
        UnloadSound(swordSfx);
        UnloadSound(hurtSfx);
        UnloadSound(fallSfx);
        UnloadSound(spinChargeSfx);
        UnloadSound(spinSfx);
        UnloadSound(dashSfx);
        UnloadSound(crashSfx);
        UnloadSound(dieSfx);
        UnloadSound(splashSfx);
        UnloadSound(wadeSfx);
        UnloadSound(landSfx);
        UnloadSound(liftSfx);
        sfxLoaded = false;
    }
    sheet = Texture2D();
    swordSheet = Texture2D();
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
    setState(HERO_IDLE);
    animTime = 0.0f;
    stepDist = 0.0f;
    invulnTime = 0.0f;
}

void Hero::setState(HeroState next)
{
    state_ = next;
    stateTime = 0.0f;
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

void Hero::updateFootsteps(const Room& room, float movedDist, const Sound* overrideSfx)
{
    if (movedDist <= 0.0f) return;
    stepDist += movedDist;
    if (stepDist >= STEP_DISTANCE)
    {
        stepDist -= STEP_DISTANCE;
        const Sound* sfx = overrideSfx ? overrideSfx : room.walkSfx();
        if (sfx) PlaySound(*sfx);
    }
}

void Hero::startFall()
{
    setState(HERO_FALL);
    moving = false;
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
        || state_ == HERO_HOP || state_ == HERO_DEAD) return;

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
        // Frozen over the hole, shrinking into it. On completion: damage,
        // then respawn on the last safe tile stood on (or die at 0 hearts).
        if (stateTime >= FALL_DURATION)
        {
            health_ -= FALL_DAMAGE;
            if (health_ < 0) health_ = 0;
            if (health_ <= 0) startDeath();
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

    case HERO_IDLE:
    case HERO_WALK:
    default:
    {
        float dx = 0.0f;
        float dy = 0.0f;

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
        inputX = dx;
        inputY = dy;

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
            // shake); releasing it after the threshold spins.
            if (bindingHeld(BINDING_SWORD))
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

            // Sword swings cancel walking entirely.
            if (swordPressed())
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
        const Sound* stepSfx = (wading_ && sfxLoaded) ? &wadeSfx : 0;

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
            // slower than dry land. A single step accumulator keeps the
            // walk sfx from doubling up.
            velX = 0.0f;
            velY = 0.0f;
            const float diagonalScale = (dx != 0.0f && dy != 0.0f) ? 0.70710678f : 1.0f;
            const float speedScale = wading_ ? WADE_SPEED_SCALE : 1.0f;
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
        tileUnder(tileX, tileY);
        meta = room.tileMetaAt(tileX, tileY);

        if (meta.flags & TILE_FLAG_WATER_DEEP)
        {
            startSplash();
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

void Hero::drawFrame(int frame, float scaleX, float scaleY) const
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
                       y + (float)TILE_SIZE * 0.5f - h * 0.5f, w, h };
    Vector2 origin = { 0.0f, 0.0f };
    DrawTexturePro(sheet, src, dest, origin, 0.0f, WHITE);
}

void Hero::draw() const
{
    if (!cfg) return;

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
        Rectangle dest = { x + (float)TILE_SIZE * 0.5f - w * 0.5f,
                           y + (float)TILE_SIZE * 0.5f - w * 0.5f, w, w };
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

    if (state_ == HERO_SPIN && swordSheet.id != 0)
    {
        // Spin: the slash arc whirls around the hero (two rotations).
        Rectangle src = { 0.0f, 0.0f, 64.0f, 64.0f };
        Vector2 centerPos = { x + (float)TILE_SIZE * 0.5f, y + (float)TILE_SIZE * 0.5f };
        Rectangle dest = { centerPos.x - 32.0f, centerPos.y - 32.0f, 64.0f, 64.0f };
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

    const DirectionalFrames* frames = (state_ == HERO_WALK || state_ == HERO_DASH) ? &cfg->move[facing] : &cfg->stand[facing];
    if (frames->count <= 0)
        frames = (state_ == HERO_WALK || state_ == HERO_DASH) ? &cfg->stand[facing] : &cfg->move[facing];
    if (frames->count <= 0) return;

    float fps = (state_ == HERO_WALK || state_ == HERO_DASH) ? MOVE_ANIM_FPS : STAND_ANIM_FPS;
    int index = (int)(animTime * fps);
    if (index < 0) index = 0;
    index = frames->frames[index % frames->count];

    drawFrame(index, 1.0f, 1.0f);
}