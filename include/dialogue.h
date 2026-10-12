#pragma once

#include "engine.h"

class DialogueBox
{
public:
    DialogueBox();

    void init();
    void shutdown();

    void open(const Dialogue& dialogue);
    void update();
    void draw() const;

    bool isActive() const { return current != 0; }

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
};
