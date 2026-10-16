#pragma once

#include "engine.h"
#include "sfx.h"

// Inventory query for "if_item"/"unless_item" line conditions: the game
// installs this at boot (dialogue stays decoupled from the inventory).
extern bool (*gDialogueItemOwned)(int item);

// Shop seam: choices may cost rupees ("cost"). The game installs this at
// boot; it returns true when the wallet covered the amount (and deducts
// it), false when the pick can't be afforded (nothing deducted).
extern bool (*gDialogueTryPayRupees)(int rupees);

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

    // The game draws the scene picture (the "image" cue); this tells the
    // box a full-bleed backdrop is already up, so the credits roll skips
    // its own black (the picture is the backdrop — the storybook roll).
    void setSceneBackdrop(bool up) { sceneBackdrop = up; }

    // Scripting drains (the game polls these once per frame after update).
    // Returns true/pointer once per queued request.
    const char* takeMusicRequest();   // 0 = none; "" = stop the bgm
    bool takeWarpRequest(int& room, int& tileX, int& tileY);
    bool takeGiveRequest(int& item);  // "give" lines queue one grant per line
    // Cutscene drains: camera pan/return, letterbox state, actor blocking,
    // and the end-of-dialogue chain into the next script.
    bool takeCameraRequest(int& action, int& tileX, int& tileY, float& seconds);
    bool takeLetterboxRequest(int& state);
    bool takeMoveRequest(int& actor, int& tileX, int& tileY, float& seconds);
    bool takeFaceRequest(int& actor, int& dir);
    bool takeShowRequest(int& actor);
    bool takeHideRequest(int& actor);
    bool takeFadeRequest(int& to, float& seconds);
    bool takeNextRequest(int& dialogue);
    // Image scene cue: -2 = clear the picture, else a SCENES index.
    bool takeImageRequest(int& scene);

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
    bool fontOwned;  // the default font is borrowed, a loaded one is ours
    Texture2D border;
    Texture2D cursor;  // ui/cursor.png: 0 = list arrow, 1 = continue arrow
    // Letter cues decode lazily through the shared cache (sfx.h).
    LazySound letterSfx;
    LazySound finishSfx;
    LazySound errorSfx;  // a pick the wallet can't afford (shop choices)

    const Dialogue* current;
    int lineIndex;
    int totalChars;
    float reveal;
    Row rows[8];
    int rowCount;
    int rowOffset;  // first wrapped row of the visible page (long lines
                    // paginate: three rows draw, action turns the page)

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

    // Cutscene cue queues (one of each per shown line, like music/warp).
    bool camPending;
    int camAction;
    int camTileX;
    int camTileY;
    float camSeconds;
    bool letterboxPending;
    int letterboxState;
    bool movePending;
    int moveActor;
    int moveTileX;
    int moveTileY;
    float moveSeconds;
    bool facePending;
    int faceActor;
    int faceDir;
    bool showPending;
    int showActor;
    bool hidePending;
    int hideActor;
    bool fadePending;
    int fadeTo;
    float fadeSeconds;
    bool imagePending;
    int imageScene;
    bool sceneBackdrop;  // set per-frame by the game (see setSceneBackdrop)
    int nextDialogue;
    bool nextPending;
    float waitTimer;  // auto-advance beat: counts past the full reveal
};