#include "dialogue.h"

#include <string.h>

static const int MAX_ROWS = 8;
static const int LINE_HEIGHT = 16;
static const int DRAW_FONT_BASE = 64;
static const int DRAW_FONT_VIRTUAL = 16;
static const float BOX_HEIGHT = 72.0f;
static const float BOX_MARGIN_BOTTOM = 8.0f;
static const float BOX_MARGIN_SIDE = 8.0f;
static const float BOX_MAX_WIDTH = 224.0f;
static const float BOX_MIN_WIDTH = 128.0f;
static const float TEXT_PAD_X = 6.0f;
static const float TEXT_PAD_Y = 5.0f;
static const float CHARS_PER_SECOND = 30.0f;

DialogueBox::DialogueBox()
    : font(), border(), letterSfx(), finishSfx(),
      current(0), lineIndex(0), totalChars(0), reveal(0.0f), rowCount(0)
{
    for (int i = 0; i < MAX_ROWS; i++) rows[i].offset = 0;
}

void DialogueBox::init()
{
    font = LoadFontEx("assets/fonts/dialogue.otf", DRAW_FONT_BASE, NULL, 0);
    border = LoadTexture("assets/ui/border.png");
    SetTextureFilter(border, TEXTURE_FILTER_POINT);
    letterSfx = LoadSound("assets/audio/message.wav");
    finishSfx = LoadSound("assets/audio/message_finish.wav");
}

void DialogueBox::shutdown()
{
    UnloadFont(font);
    UnloadTexture(border);
    UnloadSound(letterSfx);
    UnloadSound(finishSfx);
    current = 0;
}

float DialogueBox::scale() const
{
    return virtualScale();
}

float DialogueBox::fontSize() const
{
    return (float)DRAW_FONT_VIRTUAL * scale();
}

float DialogueBox::boxWidthVirtual() const
{
    float viewWidth = (float)GetScreenWidth() / scale();
    float boxWidth = viewWidth - BOX_MARGIN_SIDE * 2.0f;
    if (boxWidth > BOX_MAX_WIDTH) boxWidth = BOX_MAX_WIDTH;
    if (boxWidth < BOX_MIN_WIDTH) boxWidth = BOX_MIN_WIDTH;
    return boxWidth;
}

void DialogueBox::open(const Dialogue& dialogue)
{
    current = &dialogue;
    lineIndex = 0;
    startLine();
}

void DialogueBox::startLine()
{
    reveal = 0.0f;
    if (!current || lineIndex < 0 || lineIndex >= current->lineCount)
    {
        totalChars = 0;
        rowCount = 0;
        return;
    }
    const char* text = current->lines[lineIndex];
    if (!text) text = "";

    float patch = (float)(border.width / 3);
    float textWidth = boxWidthVirtual() - 2.0f * (patch + TEXT_PAD_X);
    wrapLine(text, textWidth);
}

void DialogueBox::wrapLine(const char* text, float maxWidth)
{
    rowCount = 0;
    totalChars = (int)strlen(text);

    if (!text[0])
    {
        rows[0].offset = 0;
        rows[0].text[0] = '\0';
        rowCount = 1;
        return;
    }

    int i = 0;
    while (text[i] && rowCount < MAX_ROWS)
    {
        Row& row = rows[rowCount];
        row.offset = i;
        row.text[0] = '\0';

        int len = 0;
        while (text[i])
        {
            int wordStart = i;
            int wordLen = 0;
            while (text[wordStart + wordLen] && text[wordStart + wordLen] != ' ') wordLen++;

            char candidate[256];
            int candLen = len;
            if (len > 0)
            {
                memcpy(candidate, row.text, (size_t)len);
                candidate[candLen++] = ' ';
            }
            if (candLen + wordLen >= 255) break;
            memcpy(candidate + candLen, text + wordStart, (size_t)wordLen);
            candLen += wordLen;
            candidate[candLen] = '\0';

            float width = MeasureTextEx(font, candidate, fontSize(), scale()).x / scale();
            if (len > 0 && width > maxWidth) break;

            memcpy(row.text, candidate, (size_t)candLen);
            len = candLen;
            row.text[len] = '\0';

            i = wordStart + wordLen;
            while (text[i] == ' ') i++;
        }

        rowCount++;
    }
}

void DialogueBox::update()
{
    if (!current) return;

    const char* line = (lineIndex < current->lineCount) ? current->lines[lineIndex] : "";
    if (!line) line = "";

    if (reveal < (float)totalChars)
    {
        int previous = (int)reveal;
        reveal += CHARS_PER_SECOND * GetFrameTime();
        if (reveal > (float)totalChars) reveal = (float)totalChars;

        int now = (int)reveal;
        if (now > previous && line[now - 1] != ' ') PlaySound(letterSfx);
        if (reveal >= (float)totalChars) PlaySound(finishSfx);
    }

    if (actionPressed())
    {
        if (reveal < (float)totalChars)
        {
            reveal = (float)totalChars;
            PlaySound(finishSfx);
        }
        else
        {
            lineIndex++;
            if (lineIndex >= current->lineCount) current = 0;
            else startLine();
        }
    }
}

void DialogueBox::draw() const
{
    if (!current) return;

    float sc = scale();
    float viewWidth = (float)GetScreenWidth() / sc;
    float boxWidth = boxWidthVirtual();

    Rectangle box = { (viewWidth - boxWidth) * 0.5f,
                      VIRTUAL_HEIGHT - BOX_HEIGHT - BOX_MARGIN_BOTTOM,
                      boxWidth, BOX_HEIGHT };

    int patch = border.width / 3;
    if (patch < 1) patch = 1;

    NPatchInfo npatch;
    npatch.source = Rectangle{ 0.0f, 0.0f, (float)border.width, (float)border.height };
    npatch.left = patch;
    npatch.top = patch;
    npatch.right = patch;
    npatch.bottom = patch;
    npatch.layout = NPATCH_NINE_PATCH;

    Rectangle dest = { box.x * sc, box.y * sc, box.width * sc, box.height * sc };
    Vector2 origin = { 0.0f, 0.0f };
    DrawTextureNPatch(border, npatch, dest, origin, 0.0f, WHITE);

    float textSize = fontSize();
    float spacing = sc;
    float left = (box.x + (float)patch + TEXT_PAD_X) * sc;
    float top = (box.y + (float)patch + TEXT_PAD_Y) * sc;

    int revealed = (int)reveal;
    for (int j = 0; j < rowCount; j++)
    {
        int rowLen = (int)strlen(rows[j].text);
        int count = revealed - rows[j].offset;
        if (count <= 0) break;
        if (count > rowLen) count = rowLen;

        char tmp[256];
        memcpy(tmp, rows[j].text, (size_t)count);
        tmp[count] = '\0';

        Vector2 pos = { left, top + (float)(j * LINE_HEIGHT) * sc };
        DrawTextEx(font, tmp, pos, textSize, spacing, RAYWHITE);
    }
}
