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

Hero::Hero()
    : cfg(0), sheet(), x(0.0f), y(0.0f), facing(DIR_DOWN), moving(false),
      animTime(0.0f), stepDist(0.0f)
{
}

void Hero::load(const CharacterConfig& config)
{
    unload();
    cfg = &config;
    sheet = LoadTexture(config.image);
    SetTextureFilter(sheet, TEXTURE_FILTER_POINT);
}

void Hero::unload()
{
    if (cfg && sheet.id != 0) UnloadTexture(sheet);
    sheet = Texture2D();
    cfg = 0;
}

void Hero::place(int tileX, int tileY)
{
    x = (float)(tileX * TILE_SIZE);
    y = (float)(tileY * TILE_SIZE);
}

void Hero::update(const Room& room, float dt)
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

    // Diagonal movement: same overall speed as cardinal movement,
    // and a single step accumulator so the walk sfx never doubles up.
    const float diagonalScale = (dx != 0.0f && dy != 0.0f) ? 0.70710678f : 1.0f;

    const RoomConfig& cfg = room.config();
    const float maxX = (float)(cfg.mapWidth * TILE_SIZE) - (float)TILE_SIZE;
    const float maxY = (float)(cfg.mapHeight * TILE_SIZE) - (float)TILE_SIZE;

    float nx = x;
    float ny = y;

    if (dx != 0.0f)
    {
        float step = dx * WALK_SPEED * dt * diagonalScale;
        Rectangle box = { nx + step + BOX_X, y + BOX_Y, BOX_W, BOX_H };
        if (!room.solidBox(box)) nx += step;
    }
    if (dy != 0.0f)
    {
        float step = dy * WALK_SPEED * dt * diagonalScale;
        Rectangle box = { nx + BOX_X, ny + step + BOX_Y, BOX_W, BOX_H };
        if (!room.solidBox(box)) ny += step;
    }

    if (nx < 0.0f) nx = 0.0f;
    if (ny < 0.0f) ny = 0.0f;
    if (nx > maxX) nx = maxX;
    if (ny > maxY) ny = maxY;

    bool moved = (nx != x) || (ny != y);
    if (moved)
    {
        stepDist += sqrtf((nx - x) * (nx - x) + (ny - y) * (ny - y));
        if (stepDist >= STEP_DISTANCE)
        {
            stepDist -= STEP_DISTANCE;
            const Sound* sfx = room.walkSfx();
            if (sfx) PlaySound(*sfx);
        }
    }

    x = nx;
    y = ny;

    moving = (dx != 0.0f) || (dy != 0.0f);
    if (dx != 0.0f) facing = (dx < 0.0f) ? DIR_LEFT : DIR_RIGHT;
    else if (dy != 0.0f) facing = (dy < 0.0f) ? DIR_UP : DIR_DOWN;

    animTime += dt;
}

void Hero::draw() const
{
    if (!cfg) return;

    const DirectionalFrames* frames = moving ? &cfg->move[facing] : &cfg->stand[facing];
    if (frames->count <= 0)
        frames = moving ? &cfg->stand[facing] : &cfg->move[facing];
    if (frames->count <= 0) return;

    float fps = moving ? MOVE_ANIM_FPS : STAND_ANIM_FPS;
    int index = (int)(animTime * fps);
    if (index < 0) index = 0;
    index = frames->frames[index % frames->count];

    int columns = cfg->sheetColumns;
    if (columns <= 0) columns = 1;
    if (index < 0) index = 0;
    if (index >= cfg->frameCount && cfg->frameCount > 0) index = cfg->frameCount - 1;

    Rectangle src = { (float)((index % columns) * TILE_SIZE),
                     (float)((index / columns) * TILE_SIZE),
                     (float)TILE_SIZE, (float)TILE_SIZE };
    Vector2 pos = { x, y };
    DrawTextureRec(sheet, src, pos, WHITE);
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
