#pragma once

#include "engine.h"

class Room;

class Hero
{
public:
    Hero();

    void load(const CharacterConfig& config);
    void unload();
    void place(int tileX, int tileY);
    void update(const Room& room, float dt);
    void draw() const;

    void tileUnder(int& tileX, int& tileY) const;
    Vector2 center() const;

private:
    const CharacterConfig* cfg;
    Texture2D sheet;
    float x;
    float y;
    Direction facing;
    bool moving;
    float animTime;
    float stepDist;
};
