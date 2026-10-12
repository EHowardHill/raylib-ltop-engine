#pragma once

#include "engine.h"

class Room
{
public:
    Room();

    void load(const RoomConfig& config);
    void unload();

    const RoomConfig& config() const { return *cfg; }

    bool solidBox(Rectangle box) const;
    void draw(Texture2D tileset, const Camera2D& camera) const;

    const Sound* walkSfx() const;

private:
    const RoomConfig* cfg;
    Sound walkSound;
    bool hasWalkSound;
};
