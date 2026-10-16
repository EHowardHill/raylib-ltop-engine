#include "enemy.h"
#include "room.h"
#include "gamecode.h"

#include <math.h>

static const float ENEMY_ANIM_FPS = 6.0f;
static const float HURT_DURATION = 0.22f;
static const float KNOCK_SPEED = 160.0f;
static const float STUN_DURATION = 0.6f;
static const float AGITATE_DURATION = 3.0f;
static const float AGITATE_SPEED_SCALE = 1.8f;
static const float SOLDIER_CHARGE_SPEED = 2.2f;
static const float BOX_X = 8.0f;
static const float BOX_Y = 12.0f;
static const float BOX_W = 16.0f;
static const float BOX_H = 16.0f;
// Mover feel-constants (engine vocabulary defaults; the walk segment
// length is content, the beat lengths are feel).
static const float DEFAULT_WALK_SEGMENT = 2.0f;
static const float PRE_SHOT_PAUSE = 0.5f;
static const float BUMP_PAUSE = 0.4f;
static const float IDLE_PAUSE = 0.3f;

Enemy::Enemy()
    : cfg(0), sheet(), x(0.0f), y(0.0f), facing(DIR_DOWN), mode(ENEMY_IDLE),
      modeTime(0.0f), timer(0.0f), hp(0), hits(0), knockX(0.0f), knockY(0.0f),
      lastSwing(-1), shotPending_(false),
      shotDirX(0.0f), shotDirY(0.0f), animTime(0.0f), agitateTimer(0.0f),
      trailCount(0), trailSpacing(0.0f), trailDist(0.0f),
      lastSampleX(0.0f), lastSampleY(0.0f),
      weakX(0.0f), weakY(0.0f), weakW(0.0f), weakH(0.0f),
      harmless(false), customFn(0)
{
    for (int i = 0; i < TRAIL_MAX; i++)
    {
        trailX[i] = 0.0f;
        trailY[i] = 0.0f;
    }
    for (int i = 0; i < 4; i++) scratch_[i] = 0.0f;
}

void Enemy::spawn(const EnemyConfig& config, Texture2D spriteSheet, int tileX, int tileY)
{
    cfg = &config;
    sheet = spriteSheet;
    x = (float)(tileX * TILE_SIZE);
    y = (float)(tileY * TILE_SIZE);
    facing = DIR_DOWN;
    hp = config.hp;
    hits = 0;
    lastSwing = -1;
    shotPending_ = false;
    knockX = 0.0f;
    knockY = 0.0f;
    animTime = 0.0f;
    timer = 0.0f;
    agitateTimer = 0.0f;
    trailCount = 0;
    trailSpacing = 0.0f;
    trailDist = 0.0f;
    lastSampleX = x;
    lastSampleY = y;
    for (int i = 0; i < TRAIL_MAX; i++)
    {
        trailX[i] = x;
        trailY[i] = y;
    }
    weakW = 0.0f;
    weakH = 0.0f;
    harmless = false;
    for (int i = 0; i < 4; i++) scratch_[i] = 0.0f;
    // Layer-4 seam: game-code behaviors resolve by name at spawn (bake-time
    // linked registry; missing names degrade to the built-in components).
    customFn = config.customBehavior ? gamecodeFindEnemyBehavior(config.customBehavior) : 0;
    setMode(ENEMY_IDLE);
}

void Enemy::setMode(Mode next)
{
    mode = next;
    modeTime = 0.0f;
}

void Enemy::faceVector(float dx, float dy)
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

void Enemy::pickDirection(Rng& rng)
{
    facing = (Direction)rng.range(DIR_COUNT);
}

// Hazards (pit holes, deep water) aren't solid for tile-collision but
// are for enemies: they bump at the lip instead of hovering over the
// hole or floating on the sea. (Water natives like the zora stand still
// in it — their behavior never slides.)
static bool enemyHazardAt(const Room& room, const Rectangle& box)
{
    int x0 = (int)(box.x / TILE_SIZE);
    int y0 = (int)(box.y / TILE_SIZE);
    int x1 = (int)((box.x + box.width - 0.01f) / TILE_SIZE);
    int y1 = (int)((box.y + box.height - 0.01f) / TILE_SIZE);
    for (int ty = y0; ty <= y1; ty++)
        for (int tx = x0; tx <= x1; tx++)
        {
            TileMeta meta = room.tileMetaAt(tx, ty);
            if (meta.flags & (TILE_FLAG_PIT | TILE_FLAG_WATER_DEEP)) return true;
        }
    return false;
}

bool Enemy::slide(const Room& room, float dx, float dy)
{
    // Axis-separated tile collision (mirrors the hero), returns whether the
    // full move happened (blocked = false).
    const RoomConfig& rc = room.config();
    float maxX = (float)(rc.mapWidth * TILE_SIZE) - (float)TILE_SIZE;
    float maxY = (float)(rc.mapHeight * TILE_SIZE) - (float)TILE_SIZE;

    float nx = x;
    float ny = y;
    bool blocked = false;

    if (dx != 0.0f)
    {
        Rectangle box = { nx + dx + BOX_X, y + BOX_Y, BOX_W, BOX_H };
        if (!room.solidBox(box) && !enemyHazardAt(room, box)) nx += dx;
        else blocked = true;
    }
    if (dy != 0.0f)
    {
        Rectangle box = { nx + BOX_X, ny + dy + BOX_Y, BOX_W, BOX_H };
        if (!room.solidBox(box) && !enemyHazardAt(room, box)) ny += dy;
        else blocked = true;
    }

    if (nx < 0.0f) { nx = 0.0f; blocked = true; }
    if (ny < 0.0f) { ny = 0.0f; blocked = true; }
    if (nx > maxX) { nx = maxX; blocked = true; }
    if (ny > maxY) { ny = maxY; blocked = true; }

    x = nx;
    y = ny;
    return !blocked;
}

Rectangle Enemy::box() const
{
    if (cfg && cfg->boss)
    {
        // The full 1.5x sprite (48x48 centered on the tile): swings that
        // clip the big sprite connect — a charging boss that overlaps the
        // hero's tile stays reachable by the sword, like LttP.
        return Rectangle{ x - 8.0f, y - 8.0f, 48.0f, 48.0f };
    }
    return Rectangle{ x + BOX_X, y + BOX_Y, BOX_W, BOX_H };
}

Vector2 Enemy::center() const
{
    return Vector2{ x + TILE_SIZE * 0.5f, y + TILE_SIZE * 0.5f };
}

void Enemy::stun(float seconds)
{
    if (!alive()) return;
    setMode(ENEMY_STUN);
    timer = seconds > 0.0f ? seconds : STUN_DURATION;
}

void Enemy::agitate(float seconds)
{
    if (!alive()) return;
    agitateTimer = seconds > 0.0f ? seconds : AGITATE_DURATION;
    // A jostled enemy snaps out of a stun into its dither.
    if (mode == ENEMY_STUN) setMode(ENEMY_PAUSE);
}

void Enemy::enableTrail(int count, float spacing)
{
    if (count > TRAIL_MAX) count = TRAIL_MAX;
    if (count < 1) count = 1;
    trailCount = count;
    trailSpacing = spacing > 1.0f ? spacing : 1.0f;
    // Seed the whole trail at the current spot so it grows out behind
    // the enemy instead of streaming in from (0,0).
    for (int i = 0; i < TRAIL_MAX; i++)
    {
        trailX[i] = x;
        trailY[i] = y;
    }
    trailDist = 0.0f;
    lastSampleX = x;
    lastSampleY = y;
}

bool Enemy::tailPosition(float& outX, float& outY) const
{
    if (trailCount <= 0) return false;
    outX = trailX[0];
    outY = trailY[0];
    return true;
}

void Enemy::setWeakPointAt(float worldX, float worldY, float w, float h)
{
    weakX = worldX;
    weakY = worldY;
    weakW = w;
    weakH = h;
}

void Enemy::freeze(float seconds)
{
    if (!alive()) return;
    setMode(ENEMY_FROZEN);
    timer = seconds > 0.0f ? seconds : STUN_DURATION * 3.0f;
    knockX = 0.0f;
    knockY = 0.0f;
}

void Enemy::bounce(Vector2 dir)
{
    if (!cfg || !alive()) return;
    // Bosses keep their patterns; frozen enemies hold their freeze
    // (they don't chase, so the pin scenario doesn't apply).
    if (isBoss() || mode == ENEMY_FROZEN) return;
    float len = sqrtf(dir.x * dir.x + dir.y * dir.y);
    if (len <= 0.0001f) return;
    knockX = dir.x / len;
    knockY = dir.y / len;
    setMode(ENEMY_HURT);
}

bool Enemy::tryHit(Rectangle attackBox, int swingId, Vector2 knockDir,
                    int damage)
{
    if (!alive()) return false;

    // Strike-immune (reflectable-magic bosses): direct hero actions —
    // sword/spin/dash/hammer, which swing the hero's positive action
    // counter — bounce off. Only projectile hits (negative unique ids)
    // land.
    if (cfg->strikeImmune && swingId >= 0) return false;

    Rectangle enemyBox = box();
    if (attackBox.width <= 0.0f || attackBox.height <= 0.0f) return false;
    if (attackBox.x > enemyBox.x + enemyBox.width) return false;
    if (attackBox.x + attackBox.width < enemyBox.x) return false;
    if (attackBox.y > enemyBox.y + enemyBox.height) return false;
    if (attackBox.y + attackBox.height < enemyBox.y) return false;

    // Frozen: any hit that actually lands shatters the ice (and the
    // enemy) instantly — overlap verified above, so a fresh swing can't
    // freeze-shatter the whole room from across the map.
    if (mode == ENEMY_FROZEN)
    {
        if (swingId == lastSwing) return false;
        lastSwing = swingId;
        hits++;
        hp = 0;
        return true;
    }

    // Weak point (a behavior-armed tail): strikes landing outside the
    // sub-box bounce off harmlessly — one bounce per swing id, no
    // damage, no hurt flinch.
    if (weakW > 0.0f && weakH > 0.0f)
    {
        Rectangle weak = { weakX - weakW * 0.5f, weakY - weakH * 0.5f,
                           weakW, weakH };
        bool inWeak = !(attackBox.x > weak.x + weak.width
                        || attackBox.x + attackBox.width < weak.x
                        || attackBox.y > weak.y + weak.height
                        || attackBox.y + attackBox.height < weak.y);
        if (!inWeak)
        {
            lastSwing = swingId;
            return false;
        }
    }

    // Single hit per swing.
    if (swingId == lastSwing) return false;
    lastSwing = swingId;
    hits++;

    hp -= (damage > 0) ? damage : 1;
    if (hp <= 0)
    {
        return true;
    }

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
    setMode(ENEMY_HURT);
    return false;
}

bool Enemy::popShot(float& dirX, float& dirY)
{
    if (!shotPending_) return false;
    shotPending_ = false;
    dirX = shotDirX;
    dirY = shotDirY;
    return true;
}

void Enemy::update(const Room& room, Vector2 heroCenter, Rng& rng, float dt)
{
    if (!cfg || !alive()) return;

    modeTime += dt;
    animTime += dt;
    if (agitateTimer > 0.0f) agitateTimer -= dt;

    // Segmented trail: distance-based sampling — the trail only moves
    // as far as the body does, so segments trail naturally.
    if (trailCount > 0)
    {
        float dx = x - lastSampleX;
        float dy = y - lastSampleY;
        trailDist += sqrtf(dx * dx + dy * dy);
        lastSampleX = x;
        lastSampleY = y;
        while (trailDist >= trailSpacing)
        {
            trailDist -= trailSpacing;
            for (int i = 0; i + 1 < trailCount; i++)
            {
                trailX[i] = trailX[i + 1];
                trailY[i] = trailY[i + 1];
            }
            trailX[trailCount - 1] = x;
            trailY[trailCount - 1] = y;
        }
    }

    // Layer-4 seam: a registered custom behavior owns the whole AI in the
    // AI states. The engine keeps handling hurt/stun/frozen recovery (and
    // the built-in component AI below degrades gracefully if a registered
    // name was not found).
    if (customFn
        && (mode == ENEMY_IDLE || mode == ENEMY_WALK
            || mode == ENEMY_PAUSE || mode == ENEMY_CHARGE))
    {
        customFn(*this, room, heroCenter, rng, dt);
        return;
    }

    switch (mode)
    {
    case ENEMY_HURT:
    {
        float decay = (HURT_DURATION - modeTime) / HURT_DURATION;
        if (decay > 0.0f)
        {
            float move = KNOCK_SPEED * decay * dt;
            slide(room, knockX * move, knockY * move);
        }
        if (modeTime >= HURT_DURATION)
        {
            knockX = 0.0f;
            knockY = 0.0f;
            setMode(ENEMY_PAUSE);
            timer = 0.4f;
        }
        break;
    }

    case ENEMY_STUN:
        // Frozen (boomerang hit, wall crash): timer counts down to release.
        if (timer > 0.0f) timer -= dt;
        if (timer <= 0.0f)
        {
            setMode(ENEMY_IDLE);
            timer = 0.2f;
        }
        break;

    case ENEMY_FROZEN:
        // Ice block: completely inert until it thaws.
        if (timer > 0.0f) timer -= dt;
        if (timer <= 0.0f)
        {
            setMode(ENEMY_PAUSE);
            timer = 0.3f;
        }
        break;

    case ENEMY_CHARGE:
    {
        // Chargers run the hero down (component: chargeSight > 0); hitting
        // a wall stuns them when the component asks for it. Boss phase
        // tables (component: phases) speed the charge up as health drops —
        // the first threshold matched wins, so the generator emits them
        // in ascending hpBelow order.
        Vector2 c = center();
        float dx = heroCenter.x - c.x;
        float dy = heroCenter.y - c.y;
        float len = sqrtf(dx * dx + dy * dy);
        float speedScale = (cfg->chargeSpeedScale > 0.0f)
                               ? cfg->chargeSpeedScale : SOLDIER_CHARGE_SPEED;
        if (cfg->phaseCount > 0 && cfg->phases)
        {
            float hpRatio = (cfg->hp > 0) ? (float)hp / (float)cfg->hp : 1.0f;
            for (int i = 0; i < cfg->phaseCount; i++)
            {
                if (hpRatio <= cfg->phases[i].hpBelow)
                {
                    speedScale = cfg->phases[i].speedScale;
                    break;
                }
            }
        }
        if (len > 1.0f)
        {
            dx /= len;
            dy /= len;
            faceVector(dx, dy);
            float speed = (float)cfg->speed * speedScale;
            if (agitateTimer > 0.0f) speed *= AGITATE_SPEED_SCALE;
            if (!slide(room, dx * speed * dt, dy * speed * dt))
            {
                if (cfg->chargeStunOnWall) stun(0.5f);
                break;
            }
        }
        // Lost sight: back to patrol.
        if (len > cfg->chargeSight * 1.5f) setMode(ENEMY_IDLE);
        break;
    }

    case ENEMY_WALK:
    {
        float speed = (float)cfg->speed;
        if (agitateTimer > 0.0f) speed *= AGITATE_SPEED_SCALE;
        float dx = (facing == DIR_LEFT) ? -speed : (facing == DIR_RIGHT) ? speed : 0.0f;
        float dy = (facing == DIR_UP) ? -speed : (facing == DIR_DOWN) ? speed : 0.0f;
        bool moved = slide(room, dx * dt, dy * dt);

        if (timer > 0.0f) timer -= dt;

        // Agitated (bug-net swish): dither about, changing course often.
        if (agitateTimer > 0.0f && rng.range(24) == 0) pickDirection(rng);

        // Shooter cadence (component: shootPeriod > 0): the walk leg lasts
        // one cadence, then a pre-shot pause.
        if (cfg->shootPeriod > 0 && timer <= 0.0f)
        {
            setMode(ENEMY_PAUSE);
            timer = PRE_SHOT_PAUSE;
            break;
        }

        if (!moved)
        {
            // Bump into something: pause, then turn.
            setMode(ENEMY_PAUSE);
            timer = BUMP_PAUSE;
        }
        else if (modeTime >= (cfg->walkSegment > 0.0f ? cfg->walkSegment : DEFAULT_WALK_SEGMENT)
                 && cfg->shootPeriod <= 0)
        {
            setMode(ENEMY_IDLE);
            timer = IDLE_PAUSE;
        }
        else if (cfg->turnInPlace)
        {
            // Occasionally change course even in the open.
            if (rng.range(100) == 0) pickDirection(rng);
        }
        break;
    }

    case ENEMY_IDLE:
    case ENEMY_PAUSE:
    default:
        if (timer > 0.0f) timer -= dt;
        if (timer > 0.0f) break;

        // Charger (component: chargeSight): notice the hero and charge.
        if (cfg->chargeSight > 0.0f)
        {
            Vector2 c = center();
            float dx = heroCenter.x - c.x;
            float dy = heroCenter.y - c.y;
            if (sqrtf(dx * dx + dy * dy) <= cfg->chargeSight)
            {
                setMode(ENEMY_CHARGE);
                break;
            }
        }

        // Shooter (component: shootPeriod): the idle beat spits one rock
        // toward the facing, every cycle — the walk leg lasts
        // shootPeriod seconds, so the number IS the spit cadence (the
        // leg plus this idle pause), not a roll of the dice.
        if (cfg->shootPeriod > 0)
        {
            shotDirX = (facing == DIR_LEFT) ? -1.0f : (facing == DIR_RIGHT) ? 1.0f : 0.0f;
            shotDirY = (facing == DIR_UP) ? -1.0f : (facing == DIR_DOWN) ? 1.0f : 0.0f;
            shotPending_ = true;
        }

        pickDirection(rng);
        setMode(ENEMY_WALK);
        // Shooter walk legs last one cadence; other movers walk until
        // their segment ends.
        timer = (cfg->shootPeriod > 0) ? (float)cfg->shootPeriod : 0.0f;
        break;
    }
}

void Enemy::drawFrame(int frame) const
{
    int columns = cfg->sheetColumns;
    if (columns <= 0) columns = 1;
    if (frame < 0) frame = 0;
    if (frame >= cfg->frameCount && cfg->frameCount > 0) frame = cfg->frameCount - 1;

    Rectangle src = { (float)((frame % columns) * TILE_SIZE),
                     (float)((frame / columns) * TILE_SIZE),
                     (float)TILE_SIZE, (float)TILE_SIZE };
    if (cfg->boss)
    {
        // 1.5x sprite centered on the tile.
        Rectangle dest = { x + (float)TILE_SIZE * 0.5f - (float)TILE_SIZE * 0.75f,
                           y + (float)TILE_SIZE * 0.5f - (float)TILE_SIZE * 0.75f,
                           (float)TILE_SIZE * 1.5f, (float)TILE_SIZE * 1.5f };
        Vector2 origin = { 0.0f, 0.0f };
        DrawTexturePro(sheet, src, dest, origin, 0.0f, WHITE);
        return;
    }
    Vector2 pos = { x, y };
    DrawTextureRec(sheet, src, pos, WHITE);
}

void Enemy::draw() const
{
    if (!cfg || !alive()) return;

    // Segmented trail (Moldorm-style worms): the body's own sprite
    // shrinks down the chain — the smallest segment is the oldest (the
    // tail, the weak point the behavior arms), growing toward the body.
    if (trailCount > 0)
    {
        const DirectionalFrames* bodyFrames = &cfg->move[facing];
        if (bodyFrames->count <= 0) bodyFrames = &cfg->stand[facing];
        int frame = bodyFrames->count > 0 ? bodyFrames->frames[0] : 0;
        int columns = cfg->sheetColumns;
        if (columns <= 0) columns = 1;
        if (frame < 0) frame = 0;
        if (frame >= cfg->frameCount && cfg->frameCount > 0) frame = cfg->frameCount - 1;
        Rectangle src = { (float)((frame % columns) * TILE_SIZE),
                          (float)((frame / columns) * TILE_SIZE),
                          (float)TILE_SIZE, (float)TILE_SIZE };
        float bodyScale = cfg->boss ? 1.5f : 1.0f;
        Vector2 origin = { 0.0f, 0.0f };
        for (int i = 0; i < trailCount; i++)
        {
            float t = (trailCount > 1) ? (float)i / (float)(trailCount - 1) : 1.0f;
            float w = (float)TILE_SIZE * (0.4f + t * 0.5f) * bodyScale;
            Rectangle dest = { trailX[i] + (float)TILE_SIZE * 0.5f - w * 0.5f,
                               trailY[i] + (float)TILE_SIZE * 0.5f - w * 0.5f,
                               w, w };
            DrawTexturePro(sheet, src, dest, origin, 0.0f, WHITE);
        }
    }

    const DirectionalFrames* frames = (mode == ENEMY_WALK || mode == ENEMY_CHARGE)
                                          ? &cfg->move[facing] : &cfg->stand[facing];
    if (frames->count <= 0) frames = &cfg->stand[facing];
    if (frames->count <= 0) return;

    int frame = frames->frames[(int)(animTime * ENEMY_ANIM_FPS) % frames->count];
    int columns = cfg->sheetColumns;
    if (columns <= 0) columns = 1;
    // Ice block overlay while frozen (bosses keep their 1.5x scale).
    if (mode == ENEMY_FROZEN)
    {
        Rectangle src = { (float)((frame % columns) * TILE_SIZE),
                         (float)((frame / columns) * TILE_SIZE),
                         (float)TILE_SIZE, (float)TILE_SIZE };
        Vector2 pos = { x, y };
        if (cfg->boss)
        {
            Rectangle dest = { x + (float)TILE_SIZE * 0.5f - (float)TILE_SIZE * 0.75f,
                               y + (float)TILE_SIZE * 0.5f - (float)TILE_SIZE * 0.75f,
                               (float)TILE_SIZE * 1.5f, (float)TILE_SIZE * 1.5f };
            Vector2 origin = { 0.0f, 0.0f };
            DrawTexturePro(sheet, src, dest, origin, 0.0f, Color{ 120, 190, 255, 255 });
            DrawRectangle((int)(x - 8.0f) + 6, (int)(y - 8.0f) + 6,
                          (int)(TILE_SIZE * 1.5f) - 12, (int)(TILE_SIZE * 1.5f) - 12,
                          Color{ 150, 205, 255, 90 });
            return;
        }
        DrawTextureRec(sheet, src, pos, Color{ 120, 190, 255, 255 });
        DrawRectangle((int)x + 4, (int)y + 4, TILE_SIZE - 8, TILE_SIZE - 8,
                      Color{ 150, 205, 255, 90 });
        return;
    }
    // Hit flash while recoiling.
    if (mode == ENEMY_HURT && ((int)(modeTime * 30.0f) % 2 == 0))
    {
        if (frame >= 0 && (cfg->frameCount <= 0 || frame < cfg->frameCount))
        {
            Rectangle src = { (float)((frame % columns) * TILE_SIZE),
                             (float)((frame / columns) * TILE_SIZE),
                             (float)TILE_SIZE, (float)TILE_SIZE };
            if (cfg->boss)
            {
                Rectangle dest = { x + (float)TILE_SIZE * 0.5f - (float)TILE_SIZE * 0.75f,
                                   y + (float)TILE_SIZE * 0.5f - (float)TILE_SIZE * 0.75f,
                                   (float)TILE_SIZE * 1.5f, (float)TILE_SIZE * 1.5f };
                Vector2 origin = { 0.0f, 0.0f };
                DrawTexturePro(sheet, src, dest, origin, 0.0f, Color{ 255, 96, 96, 255 });
                return;
            }
            Vector2 pos = { x, y };
            DrawTextureRec(sheet, src, pos, Color{ 255, 96, 96, 255 });
            return;
        }
    }
    // Agitated (bug-net swish): a buzzy yellow flicker while it dithers
    // (bosses keep their 1.5x scale).
    if (agitateTimer > 0.0f && ((int)(animTime * 24.0f) % 2 == 0)
        && frame >= 0 && (cfg->frameCount <= 0 || frame < cfg->frameCount))
    {
        Rectangle src = { (float)((frame % columns) * TILE_SIZE),
                         (float)((frame / columns) * TILE_SIZE),
                         (float)TILE_SIZE, (float)TILE_SIZE };
        if (cfg->boss)
        {
            Rectangle dest = { x + (float)TILE_SIZE * 0.5f - (float)TILE_SIZE * 0.75f,
                               y + (float)TILE_SIZE * 0.5f - (float)TILE_SIZE * 0.75f,
                               (float)TILE_SIZE * 1.5f, (float)TILE_SIZE * 1.5f };
            Vector2 origin = { 0.0f, 0.0f };
            DrawTexturePro(sheet, src, dest, origin, 0.0f, Color{ 255, 230, 120, 255 });
            return;
        }
        Vector2 pos = { x, y };
        DrawTextureRec(sheet, src, pos, Color{ 255, 230, 120, 255 });
        return;
    }
    drawFrame(frame);
}

void Enemy::drawReflection() const
{
    // Same frame as draw(), flipped and squashed below the feet, tinted
    // like the shallow water it stands in. Frozen blocks don't reflect.
    if (!cfg || !alive() || mode == ENEMY_FROZEN) return;

    const DirectionalFrames* frames = (mode == ENEMY_WALK || mode == ENEMY_CHARGE)
                                          ? &cfg->move[facing] : &cfg->stand[facing];
    if (frames->count <= 0) frames = &cfg->stand[facing];
    if (frames->count <= 0) return;

    int frame = frames->frames[(int)(animTime * ENEMY_ANIM_FPS) % frames->count];
    int columns = cfg->sheetColumns;
    if (columns <= 0) columns = 1;
    if (frame < 0) frame = 0;
    if (frame >= cfg->frameCount && cfg->frameCount > 0) frame = cfg->frameCount - 1;

    Rectangle src = { (float)((frame % columns) * TILE_SIZE),
                     (float)((frame / columns) * TILE_SIZE),
                     (float)TILE_SIZE, -(float)TILE_SIZE };
    Rectangle dest;
    if (cfg->boss)
    {
        // Mirror the 1.5x boss sprite around the tile bottom.
        dest = Rectangle{ x + (float)TILE_SIZE * 0.5f - (float)TILE_SIZE * 0.75f,
                         y + (float)TILE_SIZE * 1.5f,
                         (float)TILE_SIZE * 1.5f,
                         (float)TILE_SIZE * 1.5f * 0.62f };
    }
    else
    {
        dest = Rectangle{ x, y + (float)TILE_SIZE,
                          (float)TILE_SIZE, (float)TILE_SIZE * 0.62f };
    }
    Vector2 origin = { 0.0f, 0.0f };
    DrawTexturePro(sheet, src, dest, origin, 0.0f, Color{ 175, 200, 240, 100 });
}