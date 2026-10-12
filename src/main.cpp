#include "engine.h"
#include "assets.h"
#include "room.h"
#include "hero.h"
#include "dialogue.h"

#include <stdio.h>
#include <string.h>

#if defined(PLATFORM_WEB)
    #include <emscripten/emscripten.h>
#endif

static Room room;
static Hero hero;
static DialogueBox dialogue;

static Music music = {};
static bool musicLoaded = false;
static char musicFile[512] = "";

static Texture2D tilesetTexture = {};

static int prevTileX = -1;
static int prevTileY = -1;
static Camera2D camera = {};

// One-shot encounter state. Global memory: survives room (re)loads and is
// only reset when the game process starts.
static bool* encounterFired = 0;

static bool encounterAvailable(const Encounter& encounter)
{
    if (encounter.repeat) return true;
    if (encounter.id < 0 || encounter.id >= ENCOUNTER_TOTAL) return true;
    return !encounterFired[encounter.id];
}

static void fireEncounter(const Encounter& encounter)
{
    if (!encounter.repeat && encounter.id >= 0 && encounter.id < ENCOUNTER_TOTAL)
        encounterFired[encounter.id] = true;
}

static void startMusic(const char* file)
{
    char path[600];
    if (file && file[0])
    {
        snprintf(path, sizeof(path), "assets/music/%s", file);
    }
    else
    {
        path[0] = '\0';
    }

    if (path[0] == '\0')
    {
        if (musicLoaded)
        {
            StopMusicStream(music);
            UnloadMusicStream(music);
            musicLoaded = false;
        }
        musicFile[0] = '\0';
        return;
    }

    if (musicLoaded && strcmp(musicFile, path) == 0) return;

    if (musicLoaded)
    {
        StopMusicStream(music);
        UnloadMusicStream(music);
    }

    music = LoadMusicStream(path);
    music.looping = true;
    strcpy(musicFile, path);
    musicLoaded = true;
    PlayMusicStream(music);
}

static void loadRoom(int index)
{
    const RoomConfig& cfg = ROOMS[index];

    room.unload();
    room.load(cfg);

    if (cfg.hero >= 0 && cfg.hero < CHARACTER_COUNT)
        hero.load(CHARACTERS[cfg.hero]);

    int spawnX = cfg.hasSpawn ? cfg.spawnTileX : 0;
    int spawnY = cfg.hasSpawn ? cfg.spawnTileY : 0;
    hero.place(spawnX, spawnY);

    startMusic(cfg.music);

    prevTileX = -1;
    prevTileY = -1;
}

static void updateEncounters()
{
    const RoomConfig& cfg = room.config();
    int tileX = 0;
    int tileY = 0;
    hero.tileUnder(tileX, tileY);

    if (tileX != prevTileX || tileY != prevTileY)
    {
        for (int i = 0; i < cfg.instantCount; i++)
        {
            const Encounter& e = cfg.instant[i];
            if (e.tileX == tileX && e.tileY == tileY && e.dialogue >= 0 && e.dialogue < DIALOGUE_COUNT
                && encounterAvailable(e))
            {
                fireEncounter(e);
                dialogue.open(DIALOGUES[e.dialogue]);
            }
        }
        prevTileX = tileX;
        prevTileY = tileY;
    }

    if (actionPressed())
    {
        for (int i = 0; i < cfg.interactiveCount; i++)
        {
            const Encounter& e = cfg.interactive[i];
            if (e.tileX == tileX && e.tileY == tileY && e.dialogue >= 0 && e.dialogue < DIALOGUE_COUNT
                && encounterAvailable(e))
            {
                fireEncounter(e);
                dialogue.open(DIALOGUES[e.dialogue]);
                break;
            }
        }
    }
}

static void updateAndDraw(void)
{
    if (musicLoaded) UpdateMusicStream(music);

    if (dialogue.isActive())
    {
        dialogue.update();
    }
    else
    {
        hero.update(room, GetFrameTime());
        updateEncounters();
    }

    float scale = virtualScale();
    camera.zoom = scale;
    camera.offset = Vector2{ (float)GetScreenWidth() * 0.5f, (float)GetScreenHeight() * 0.5f };

    float viewWidth = (float)GetScreenWidth() / scale;
    float viewHeight = VIRTUAL_HEIGHT;
    float mapWidth = (float)(room.config().mapWidth * TILE_SIZE);
    float mapHeight = (float)(room.config().mapHeight * TILE_SIZE);

    Vector2 center = hero.center();
    float camX = (mapWidth <= viewWidth) ? mapWidth * 0.5f
                                         : clampf(center.x, viewWidth * 0.5f, mapWidth - viewWidth * 0.5f);
    float camY = (mapHeight <= viewHeight) ? mapHeight * 0.5f
                                           : clampf(center.y, viewHeight * 0.5f, mapHeight - viewHeight * 0.5f);
    camera.target = Vector2{ camX, camY };

    BeginDrawing();
    ClearBackground(BLACK);

    BeginMode2D(camera);
    room.draw(tilesetTexture, camera);
    hero.draw();
    EndMode2D();

    dialogue.draw();

    EndDrawing();
}

int main(void)
{
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(896, 448, "LttP Engine");

    InitAudioDevice();
    SetTargetFPS(60);

    tilesetTexture = LoadTexture(TILESET.image);
    SetTextureFilter(tilesetTexture, TEXTURE_FILTER_POINT);

    dialogue.init();

    if (ENCOUNTER_TOTAL > 0)
    {
        encounterFired = new bool[ENCOUNTER_TOTAL];
        for (int i = 0; i < ENCOUNTER_TOTAL; i++) encounterFired[i] = false;
    }

    if (ROOM_COUNT > 0) loadRoom(0);

    camera.rotation = 0.0f;

#if defined(PLATFORM_WEB)
    emscripten_set_main_loop(updateAndDraw, 0, 1);
#else
    while (!WindowShouldClose()) updateAndDraw();

    startMusic(NULL);
    delete[] encounterFired;
    encounterFired = 0;
    hero.unload();
    room.unload();
    dialogue.shutdown();
    UnloadTexture(tilesetTexture);

    CloseAudioDevice();
    CloseWindow();
#endif
    return 0;
}
