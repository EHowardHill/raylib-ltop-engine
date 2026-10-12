#include "room.h"
#include "assets.h"

#include <stdio.h>

Room::Room() : cfg(0), walkSound(), hasWalkSound(false)
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
}

void Room::unload()
{
    if (hasWalkSound)
    {
        UnloadSound(walkSound);
        hasWalkSound = false;
    }
    cfg = 0;
}

const Sound* Room::walkSfx() const
{
    return hasWalkSound ? &walkSound : 0;
}

bool Room::solidBox(Rectangle box) const
{
    if (!cfg) return true;

    int x0 = (int)((box.x) / TILE_SIZE);
    int y0 = (int)((box.y) / TILE_SIZE);
    int x1 = (int)((box.x + box.width - 0.01f) / TILE_SIZE);
    int y1 = (int)((box.y + box.height - 0.01f) / TILE_SIZE);

    for (int ty = y0; ty <= y1; ty++)
    {
        for (int tx = x0; tx <= x1; tx++)
        {
            if (tx < 0 || ty < 0 || tx >= cfg->mapWidth || ty >= cfg->mapHeight) return true;
            if (cfg->colliders[ty * cfg->mapWidth + tx] != 0) return true;
        }
    }
    return false;
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

    drawLayer(cfg, cfg->decor, columns, tileset, x0, y0, x1, y1);
    drawLayer(cfg, cfg->colliders, columns, tileset, x0, y0, x1, y1);
}
