#pragma once

#include "engine.h"

class DialogueBox
{
public:
    DialogueBox();

    void init();
    void shutdown();

    void open(const Dialogue& dialogue);
    void close() { current = 0; }  // drop the box without side effects
    void update();
    void draw() const;

    bool isActive() const { return current != 0; }

    // Scripting drains (the game polls these once per frame after update).
    // Returns true/pointer once per queued request.
    const char* takeMusicRequest();   // 0 = none; "" = stop the bgm
    bool takeWarpRequest(int& room, int& tileX, int& tileY);

private:
    struct Row
    {
        int offset;
        char text[256];
    };

    float scale() const;
    float fontSize() const;
    float boxWidthVirtual() const;
    void startLine();
    void wrapLine(const char* text, float maxWidth);
    void advanceLine(bool useGoto);
    void applyFlags(const int* set, int setCount, const int* clear, int clearCount);
    void drawChoices(Rectangle box) const;

    Font font;
    Texture2D border;
    Sound letterSfx;
    Sound finishSfx;

    const Dialogue* current;
    int lineIndex;
    int totalChars;
    float reveal;
    Row rows[8];
    int rowCount;

    bool choiceActive;
    int choiceCursor;

    char musicRequest[128];
    bool musicPending;
    int warpRoom;
    int warpTileX;
    int warpTileY;
    bool warpPending;
};