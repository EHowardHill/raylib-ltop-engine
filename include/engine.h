#pragma once

#include "raylib.h"

constexpr int TILE_SIZE = 32;
constexpr float VIRTUAL_HEIGHT = 224.0f;

enum Direction
{
    DIR_DOWN = 0,
    DIR_RIGHT = 1,
    DIR_LEFT = 2,
    DIR_UP = 3,
    DIR_COUNT = 4
};

struct DirectionalFrames
{
    int count;
    const int* frames;
};

struct CharacterConfig
{
    const char* name;
    const char* image;
    int sheetColumns;
    int frameCount;
    DirectionalFrames stand[DIR_COUNT];
    DirectionalFrames move[DIR_COUNT];
};

struct Dialogue
{
    const char* name;
    int lineCount;
    const char* const* lines;
};

struct Encounter
{
    int tileX;
    int tileY;
    int dialogue;
    int id;
    bool repeat;
};

struct RoomConfig
{
    const char* name;
    int hero;
    bool hasSpawn;
    int spawnTileX;
    int spawnTileY;
    const char* music;
    const char* walkSfx;
    const Encounter* instant;
    int instantCount;
    const Encounter* interactive;
    int interactiveCount;
    int mapWidth;
    int mapHeight;
    const unsigned short* decor;
    const unsigned short* colliders;
};

struct TilesetConfig
{
    const char* image;
    int firstGid;
    int columns;
    int tileCount;
    int tileWidth;
    int tileHeight;
};

inline float virtualScale()
{
    return (float)GetScreenHeight() / VIRTUAL_HEIGHT;
}

inline float clampf(float value, float low, float high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

constexpr int ACTION_KEY = KEY_Z;

inline bool actionPressed()
{
    if (IsKeyPressed(ACTION_KEY)) return true;
    for (int i = 0; i < 4; i++)
    {
        if (IsGamepadAvailable(i) && IsGamepadButtonPressed(i, GAMEPAD_BUTTON_RIGHT_FACE_DOWN)) return true;
    }
    return false;
}
