#pragma once

#include "engine.h"

// Inventory query for "if_item"/"unless_item" line conditions: the game
// installs this at boot (dialogue stays decoupled from the inventory).
extern bool (*gDialogueItemOwned)(int item);

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
    bool takeGiveRequest(int& item);  // "give" lines queue one grant per line

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
    int giveItem;
    bool givePending;
    int warpRoom;
    int warpTileX;
    int warpTileY;
    bool warpPending;
};