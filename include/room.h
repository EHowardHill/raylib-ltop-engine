#pragma once

#include "engine.h"

// Extra solidity: registered by the game (e.g. ground-state objects) so
// entities and projectiles collide with dynamic things as if they were
// tile colliders. Return true when the box hits something.
typedef bool (*SolidExtraFn)(Rectangle box, void* context);

class Room
{
public:
    Room();

    void load(const RoomConfig& config);
    void unload();

    const RoomConfig& config() const { return *cfg; }

    bool solidBox(Rectangle box) const;
    // Tile colliders + map bounds only (ignores dynamic object solids);
    // used for block-push destinations and projectile free checks.
    bool solidTilesBox(Rectangle box) const;
    // Dynamic solids (pots, bushes...) on top of the tile colliders.
    void setSolidExtras(SolidExtraFn fn, void* context) { solidExtra = fn; solidExtraCtx = context; }
    // Behavior metadata for the tile at (tileX, tileY), combining the decor
    // and collider layers (0,0 when out of bounds).
    TileMeta tileMetaAt(int tileX, int tileY) const;
    // Sword-cut the tile at (tileX, tileY): if it is cuttable, replaces it
    // with its cut_result tile (runtime decor layer only, not baked data)
    // and returns true.
    bool cutTileAt(int tileX, int tileY);
    // Bomb-blast the tile at (tileX, tileY): if it is bombable, replaces it
    // with its bomb_result tile (255 = empty) on the runtime decor copy.
    bool bombTileAt(int tileX, int tileY);
    void draw(Texture2D tileset, const Camera2D& camera) const;
    // Decor-above layer (tree tops, archways): camera-culled like draw(),
    // but painted over the entities; call after they render.
    void drawUpper(Texture2D tileset, const Camera2D& camera) const;

    const Sound* walkSfx() const;

private:
    const RoomConfig* cfg;
    Sound walkSound;
    bool hasWalkSound;
    SolidExtraFn solidExtra;
    void* solidExtraCtx;
    // Runtime-mutable copies of the baked layers (grass cutting, bomb
    // blasts); reload resets both.
    unsigned short* decorState;
    unsigned short* colliderState;
};
