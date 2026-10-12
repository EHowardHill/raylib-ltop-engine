#include "room.h"
#include "assets.h"

#include <assert.h>
#include <stdio.h>

Room::Room() : cfg(0), walkSound(), hasWalkSound(false), solidExtra(0),
    solidExtraCtx(0), decorState(0), colliderState(0)
{
}

void Room::load(const RoomConfig& config)
{
    unload();
    cfg = &config;
    if (config.walkSfx && config.walkSfx[0])
    {
        char path[512];
        snprintf(path, sizeof(path), "assets/audio/%s", config.walkSfx);
        walkSound = LoadSound(path);
        hasWalkSound = true;
    }
    int tiles = config.mapWidth * config.mapHeight;
    if (tiles > 0 && config.decor)
    {
        decorState = new unsigned short[tiles];
        for (int i = 0; i < tiles; i++) decorState[i] = config.decor[i];
    }
    if (tiles > 0 && config.colliders)
    {
        colliderState = new unsigned short[tiles];
        for (int i = 0; i < tiles; i++) colliderState[i] = config.colliders[i];
    }
}

void Room::unload()
{
    if (hasWalkSound)
    {
        UnloadSound(walkSound);
        hasWalkSound = false;
    }
    delete[] decorState;
    decorState = 0;
    delete[] colliderState;
    colliderState = 0;
    cfg = 0;
}

const Sound* Room::walkSfx() const
{
    return hasWalkSound ? &walkSound : 0;
}

bool Room::solidTilesBox(Rectangle box) const
{
    if (!cfg) return true;

    int x0 = (int)((box.x) / TILE_SIZE);
    int y0 = (int)((box.y) / TILE_SIZE);
    int x1 = (int)((box.x + box.width - 0.01f) / TILE_SIZE);
    int y1 = (int)((box.y + box.height - 0.01f) / TILE_SIZE);
    const unsigned short* colliders = colliderState ? colliderState : cfg->colliders;

    for (int ty = y0; ty <= y1; ty++)
    {
        for (int tx = x0; tx <= x1; tx++)
        {
            if (tx < 0 || ty < 0 || tx >= cfg->mapWidth || ty >= cfg->mapHeight) return true;
            if (colliders[ty * cfg->mapWidth + tx] != 0) return true;
        }
    }

    return false;
}

bool Room::solidBox(Rectangle box) const
{
    if (solidTilesBox(box)) return true;

    // Dynamic solids (ground-state objects) registered by the game.
    if (solidExtra && solidExtra(box, solidExtraCtx)) return true;

    return false;
}

TileMeta Room::tileMetaAt(int tileX, int tileY) const
{
    TileMeta meta = { 0, 0, 0, 255, 0, 0, 0 };
    if (!cfg || TILE_META_COUNT <= 0) return meta;
    if (tileX < 0 || tileY < 0 || tileX >= cfg->mapWidth || tileY >= cfg->mapHeight) return meta;

    int decorId = decorState ? decorState[tileY * cfg->mapWidth + tileX]
                             : cfg->decor[tileY * cfg->mapWidth + tileX];
    int ids[2] = { decorId,
                  (colliderState ? colliderState : cfg->colliders)[tileY * cfg->mapWidth + tileX] };
    for (int i = 0; i < 2; i++)
    {
        int id = ids[i];
        if (id <= 0) continue;             // empty tile
        assert(id <= TILE_META_COUNT);     // generator validates layer ids
        TileMeta tile = TILE_META[id - 1];
        meta.flags |= tile.flags;
        if (tile.damage > meta.damage) meta.damage = tile.damage;
        if (tile.cutResult > 0) meta.cutResult = tile.cutResult;
        // Drift stacks across layers, clamped to the field range.
        int pushX = meta.pushX + tile.pushX;
        int pushY = meta.pushY + tile.pushY;
        if (pushX > 127) pushX = 127;
        if (pushX < -127) pushX = -127;
        if (pushY > 127) pushY = 127;
        if (pushY < -127) pushY = -127;
        meta.pushX = (signed char)pushX;
        meta.pushY = (signed char)pushY;
        if (tile.ledgeDir != 0 && meta.ledgeDir == 0) meta.ledgeDir = tile.ledgeDir;
    }
    return meta;
}

bool Room::cutTileAt(int tileX, int tileY)
{
    if (!cfg || !decorState) return false;
    if (tileX < 0 || tileY < 0 || tileX >= cfg->mapWidth || tileY >= cfg->mapHeight) return false;

    TileMeta meta = tileMetaAt(tileX, tileY);
    if (!(meta.flags & TILE_FLAG_CUTTABLE)) return false;

    decorState[tileY * cfg->mapWidth + tileX] = (unsigned short)meta.cutResult;
    return true;
}

bool Room::bombTileAt(int tileX, int tileY)
{
    if (!cfg) return false;
    if (tileX < 0 || tileY < 0 || tileX >= cfg->mapWidth || tileY >= cfg->mapHeight) return false;

    TileMeta meta = tileMetaAt(tileX, tileY);
    if (!(meta.flags & TILE_FLAG_BOMBABLE)) return false;

    // 255 = blown open (empty); anything else is the debris tile left.
    // Blasts whichever layers hold the bombable tile (a cracked wall is a
    // collider; a cracked floor is decor).
    unsigned short result = (meta.bombResult >= 255) ? 0 : (unsigned short)meta.bombResult;
    int index = tileY * cfg->mapWidth + tileX;
    bool blasted = false;
    if (decorState && decorState[index] > 0 && decorState[index] <= TILE_META_COUNT
        && (TILE_META[decorState[index] - 1].flags & TILE_FLAG_BOMBABLE))
    {
        decorState[index] = result;
        blasted = true;
    }
    if (colliderState && colliderState[index] > 0 && colliderState[index] <= TILE_META_COUNT
        && (TILE_META[colliderState[index] - 1].flags & TILE_FLAG_BOMBABLE))
    {
        colliderState[index] = result;
        blasted = true;
    }
    return blasted;
}

static void drawLayer(const RoomConfig* cfg, const unsigned short* layer,
                      int columns, Texture2D tileset,
                      int x0, int y0, int x1, int y1)
{
    for (int ty = y0; ty <= y1; ty++)
    {
        for (int tx = x0; tx <= x1; tx++)
        {
            int id = layer[ty * cfg->mapWidth + tx];
            if (id == 0) continue;

            int frame = id - 1;
            Rectangle src = { (float)((frame % columns) * TILE_SIZE),
                             (float)((frame / columns) * TILE_SIZE),
                             (float)TILE_SIZE, (float)TILE_SIZE };
            Vector2 pos = { (float)(tx * TILE_SIZE), (float)(ty * TILE_SIZE) };
            DrawTextureRec(tileset, src, pos, WHITE);
        }
    }
}

void Room::draw(Texture2D tileset, const Camera2D& camera) const
{
    if (!cfg) return;

    float left = camera.target.x - camera.offset.x / camera.zoom;
    float top = camera.target.y - camera.offset.y / camera.zoom;
    float right = left + (float)GetScreenWidth() / camera.zoom;
    float bottom = top + (float)GetScreenHeight() / camera.zoom;

    int x0 = (int)(left / TILE_SIZE);
    int y0 = (int)(top / TILE_SIZE);
    int x1 = (int)(right / TILE_SIZE);
    int y1 = (int)(bottom / TILE_SIZE);

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > cfg->mapWidth - 1) x1 = cfg->mapWidth - 1;
    if (y1 > cfg->mapHeight - 1) y1 = cfg->mapHeight - 1;

    int columns = TILESET.columns;
    if (columns <= 0) columns = 1;

    const unsigned short* decor = decorState ? decorState : cfg->decor;
    drawLayer(cfg, decor, columns, tileset, x0, y0, x1, y1);
    const unsigned short* colliders = colliderState ? colliderState : cfg->colliders;
    drawLayer(cfg, colliders, columns, tileset, x0, y0, x1, y1);
}

void Room::drawUpper(Texture2D tileset, const Camera2D& camera) const
{
    if (!cfg || !cfg->upper) return;

    float left = camera.target.x - camera.offset.x / camera.zoom;
    float top = camera.target.y - camera.offset.y / camera.zoom;
    float right = left + (float)GetScreenWidth() / camera.zoom;
    float bottom = top + (float)GetScreenHeight() / camera.zoom;

    int x0 = (int)(left / TILE_SIZE);
    int y0 = (int)(top / TILE_SIZE);
    int x1 = (int)(right / TILE_SIZE);
    int y1 = (int)(bottom / TILE_SIZE);

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > cfg->mapWidth - 1) x1 = cfg->mapWidth - 1;
    if (y1 > cfg->mapHeight - 1) y1 = cfg->mapHeight - 1;

    int columns = TILESET.columns;
    if (columns <= 0) columns = 1;

    drawLayer(cfg, cfg->upper, columns, tileset, x0, y0, x1, y1);
}
