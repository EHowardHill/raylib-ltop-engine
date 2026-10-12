#include "enemy.h"
#include "room.h"

#include <math.h>

static const float ENEMY_ANIM_FPS = 6.0f;
static const float HURT_DURATION = 0.22f;
static const float KNOCK_SPEED = 160.0f;
static const float STUN_DURATION = 0.6f;
static const float SOLDIER_CHARGE_SPEED = 2.2f;
static const float BOX_X = 8.0f;
static const float BOX_Y = 12.0f;
static const float BOX_W = 16.0f;
static const float BOX_H = 16.0f;

Enemy::Enemy()
    : cfg(0), sheet(), x(0.0f), y(0.0f), facing(DIR_DOWN), mode(ENEMY_IDLE),
      modeTime(0.0f), timer(0.0f), hp(0), knockX(0.0f), knockY(0.0f),
      lastSwing(-1), shotPending_(false),
      shotDirX(0.0f), shotDirY(0.0f), animTime(0.0f)
{
}

void Enemy::spawn(const EnemyConfig& config, Texture2D spriteSheet, int tileX, int tileY)
{
    cfg = &config;
    sheet = spriteSheet;
    x = (float)(tileX * TILE_SIZE);
    y = (float)(tileY * TILE_SIZE);
    facing = DIR_DOWN;
    hp = config.hp;
    lastSwing = -1;
    shotPending_ = false;
    knockX = 0.0f;
    knockY = 0.0f;
    animTime = 0.0f;
    timer = 0.0f;
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
        if (!room.solidBox(box)) nx += dx;
        else blocked = true;
    }
    if (dy != 0.0f)
    {
        Rectangle box = { nx + BOX_X, ny + dy + BOX_Y, BOX_W, BOX_H };
        if (!room.solidBox(box)) ny += dy;
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
        // 1.5x sprite centered on the tile: a bigger target.
        return Rectangle{ x + 4.0f, y + 4.0f, 24.0f, 24.0f };
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

void Enemy::freeze(float seconds)
{
    if (!alive()) return;
    setMode(ENEMY_FROZEN);
    timer = seconds > 0.0f ? seconds : STUN_DURATION * 3.0f;
    knockX = 0.0f;
    knockY = 0.0f;
}

bool Enemy::tryHit(Rectangle attackBox, int swingId, Vector2 knockDir)
{
    if (!alive()) return false;

    // Frozen: any hit shatters the ice (and the enemy) instantly.
    if (mode == ENEMY_FROZEN)
    {
        if (swingId == lastSwing) return false;
        lastSwing = swingId;
        hp = 0;
        return true;
    }

    Rectangle enemyBox = box();
    if (attackBox.width <= 0.0f || attackBox.height <= 0.0f) return false;
    if (attackBox.x > enemyBox.x + enemyBox.width) return false;
    if (attackBox.x + attackBox.width < enemyBox.x) return false;
    if (attackBox.y > enemyBox.y + enemyBox.height) return false;
    if (attackBox.y + attackBox.height < enemyBox.y) return false;

    // Single hit per swing.
    if (swingId == lastSwing) return false;
    lastSwing = swingId;

    hp--;
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
        // Soldiers run the hero down; hitting a wall stuns them.
        // Arena bosses charge too, in phases: they speed up and shorten
        // their recovery as their health drops.
        Vector2 c = center();
        float dx = heroCenter.x - c.x;
        float dy = heroCenter.y - c.y;
        float len = sqrtf(dx * dx + dy * dy);
        float speedScale = SOLDIER_CHARGE_SPEED;
        if (cfg->behavior == BEHAVIOR_BOSS)
        {
            float hpRatio = (cfg->hp > 0) ? (float)hp / (float)cfg->hp : 1.0f;
            if (hpRatio <= 0.34f) speedScale = 3.4f;
            else if (hpRatio <= 0.67f) speedScale = 2.6f;
        }
        if (len > 1.0f)
        {
            dx /= len;
            dy /= len;
            faceVector(dx, dy);
            float speed = (float)cfg->speed * speedScale;
            if (!slide(room, dx * speed * dt, dy * speed * dt))
            {
                stun(0.5f);
                break;
            }
        }
        // Lost sight: back to patrol.
        if (len > (float)cfg->sight * 1.5f) setMode(ENEMY_IDLE);
        break;
    }

    case ENEMY_WALK:
    {
        float speed = (float)cfg->speed;
        float dx = (facing == DIR_LEFT) ? -speed : (facing == DIR_RIGHT) ? speed : 0.0f;
        float dy = (facing == DIR_UP) ? -speed : (facing == DIR_DOWN) ? speed : 0.0f;
        bool moved = slide(room, dx * dt, dy * dt);

        if (timer > 0.0f) timer -= dt;

        if (cfg->behavior == BEHAVIOR_OCTOROK && timer <= 0.0f)
        {
            setMode(ENEMY_PAUSE);
            timer = 0.5f;
            break;
        }

        if (!moved)
        {
            // Bump into something: pause, then turn.
            setMode(ENEMY_PAUSE);
            timer = 0.4f;
        }
        else if (modeTime >= 2.0f && cfg->behavior != BEHAVIOR_OCTOROK)
        {
            setMode(ENEMY_IDLE);
            timer = 0.3f;
        }
        else if (cfg->behavior == BEHAVIOR_OCTOROK)
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

        if (cfg->behavior == BEHAVIOR_SOLDIER || cfg->behavior == BEHAVIOR_BOSS)
        {
            // Notice the hero and charge.
            Vector2 c = center();
            float dx = heroCenter.x - c.x;
            float dy = heroCenter.y - c.y;
            if (sqrtf(dx * dx + dy * dy) <= (float)cfg->sight)
            {
                setMode(ENEMY_CHARGE);
                break;
            }
        }

        if (cfg->behavior == BEHAVIOR_OCTOROK && cfg->shootPeriod > 0 && rng.range(cfg->shootPeriod) == 0)
        {
            // Spit a rock toward the facing direction.
            shotDirX = (facing == DIR_LEFT) ? -1.0f : (facing == DIR_RIGHT) ? 1.0f : 0.0f;
            shotDirY = (facing == DIR_UP) ? -1.0f : (facing == DIR_DOWN) ? 1.0f : 0.0f;
            shotPending_ = true;
        }

        pickDirection(rng);
        setMode(ENEMY_WALK);
        timer = (cfg->behavior == BEHAVIOR_OCTOROK) ? (float)cfg->shootPeriod : 0.0f;
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

    const DirectionalFrames* frames = (mode == ENEMY_WALK || mode == ENEMY_CHARGE)
                                          ? &cfg->move[facing] : &cfg->stand[facing];
    if (frames->count <= 0) frames = &cfg->stand[facing];
    if (frames->count <= 0) return;

    int frame = frames->frames[(int)(animTime * ENEMY_ANIM_FPS) % frames->count];
    int columns = cfg->sheetColumns;
    if (columns <= 0) columns = 1;
    // Ice block overlay while frozen.
    if (mode == ENEMY_FROZEN)
    {
        Rectangle src = { (float)((frame % columns) * TILE_SIZE),
                         (float)((frame / columns) * TILE_SIZE),
                         (float)TILE_SIZE, (float)TILE_SIZE };
        Vector2 pos = { x, y };
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
    drawFrame(frame);
}