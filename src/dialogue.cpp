#include "dialogue.h"
#include "assets.h"

#include <stdio.h>
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

// Lazy sfx cache for scripted cues (name -> loaded Sound).
static const int MAX_SCRIPT_SFX = 16;
struct ScriptSfx
{
    char name[96];
    Sound sound;
    bool used;
};
static ScriptSfx scriptSfx[MAX_SCRIPT_SFX] = {};

static Sound loadScriptSfx(const char* name)
{
    for (int i = 0; i < MAX_SCRIPT_SFX; i++)
    {
        if (scriptSfx[i].used && strcmp(scriptSfx[i].name, name) == 0)
            return scriptSfx[i].sound;
    }
    for (int i = 0; i < MAX_SCRIPT_SFX; i++)
    {
        if (scriptSfx[i].used) continue;
        char path[128];
        snprintf(path, sizeof(path), "assets/audio/%s", name);
        scriptSfx[i].sound = LoadSound(path);
        scriptSfx[i].used = true;
        snprintf(scriptSfx[i].name, sizeof(scriptSfx[i].name), "%s", name);
        return scriptSfx[i].sound;
    }
    // Cache full: reuse slot 0 (playing a wrong cue beats crashing).
    return scriptSfx[0].sound;
}

static void unloadScriptSfx()
{
    for (int i = 0; i < MAX_SCRIPT_SFX; i++)
    {
        if (scriptSfx[i].used)
        {
            UnloadSound(scriptSfx[i].sound);
            scriptSfx[i].used = false;
            scriptSfx[i].name[0] = '\0';
        }
    }
}

// Cursor up/down for the choice list (keyboard + gamepad dpad).
static bool cursorUpPressed()
{
    if (IsKeyPressed(KEY_UP)) return true;
    for (int i = 0; i < 4; i++)
        if (IsGamepadAvailable(i) && IsGamepadButtonPressed(i, GAMEPAD_BUTTON_LEFT_FACE_UP)) return true;
    return false;
}

static bool cursorDownPressed()
{
    if (IsKeyPressed(KEY_DOWN)) return true;
    for (int i = 0; i < 4; i++)
        if (IsGamepadAvailable(i) && IsGamepadButtonPressed(i, GAMEPAD_BUTTON_LEFT_FACE_DOWN)) return true;
    return false;
}

DialogueBox::DialogueBox()
    : font(), border(), letterSfx(), finishSfx(),
      current(0), lineIndex(0), totalChars(0), reveal(0.0f), rowCount(0),
      choiceActive(false), choiceCursor(0),
      musicRequest(), musicPending(false),
      warpRoom(-1), warpTileX(0), warpTileY(0), warpPending(false)
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
    unloadScriptSfx();
    current = 0;
    musicPending = false;
    warpPending = false;
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

void DialogueBox::applyFlags(const int* set, int setCount, const int* clear, int clearCount)
{
    if (WORLD_FLAG_COUNT <= 0) return;
    for (int i = 0; i < setCount; i++)
        if (set[i] >= 0 && set[i] < WORLD_FLAG_COUNT) gWorldFlags[set[i]] = true;
    for (int i = 0; i < clearCount; i++)
        if (clear[i] >= 0 && clear[i] < WORLD_FLAG_COUNT) gWorldFlags[clear[i]] = false;
}

void DialogueBox::open(const Dialogue& dialogue)
{
    current = &dialogue;
    lineIndex = 0;
    choiceActive = false;
    choiceCursor = 0;
    musicPending = false;
    warpPending = false;
    startLine();
}

void DialogueBox::startLine()
{
    reveal = 0.0f;
    rowCount = 0;
    totalChars = 0;
    if (!current) return;

    // Skip lines whose conditions fail (skipped lines execute nothing).
    while (lineIndex >= 0 && lineIndex < current->lineCount)
    {
        const DialogueLine& line = current->lines[lineIndex];
        if (line.ifFlag >= 0 && !gWorldFlags[line.ifFlag]) { lineIndex++; continue; }
        if (line.unlessFlag >= 0 && gWorldFlags[line.unlessFlag]) { lineIndex++; continue; }

        // Shown-effects: world flags, sfx cue, music switch.
        applyFlags(line.setFlags, line.setCount, line.clearFlags, line.clearCount);
        if (line.sfx) PlaySound(loadScriptSfx(line.sfx));
        if (line.music)
        {
            snprintf(musicRequest, sizeof(musicRequest), "%s", line.music);
            musicPending = true;
        }

        choiceActive = false;
        choiceCursor = 0;

        const char* text = line.text ? line.text : "";
        float patch = (float)(border.width / 3);
        float textWidth = boxWidthVirtual() - 2.0f * (patch + TEXT_PAD_X);
        wrapLine(text, textWidth);
        return;
    }

    // Ran off the end: the dialogue closes.
    current = 0;
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

void DialogueBox::advanceLine(bool useGoto)
{
    if (!current) return;
    const DialogueLine& line = current->lines[lineIndex];

    // A warp line ends the dialogue and queues the warp.
    if (line.warpRoom >= 0)
    {
        warpRoom = line.warpRoom;
        warpTileX = line.warpTileX;
        warpTileY = line.warpTileY;
        warpPending = true;
        current = 0;
        return;
    }

    if (useGoto && line.gotoLine >= 0) lineIndex = line.gotoLine;
    else lineIndex++;
    startLine();
}

void DialogueBox::update()
{
    if (!current) return;

    if (lineIndex < 0 || lineIndex >= current->lineCount)
    {
        current = 0;
        return;
    }
    const DialogueLine& line = current->lines[lineIndex];
    const char* text = line.text ? line.text : "";

    if (reveal < (float)totalChars)
    {
        int previous = (int)reveal;
        reveal += CHARS_PER_SECOND * GetFrameTime();
        if (reveal > (float)totalChars) reveal = (float)totalChars;

        int now = (int)reveal;
        if (now > previous && text[now - 1] != ' ') PlaySound(letterSfx);
        if (reveal >= (float)totalChars) PlaySound(finishSfx);
    }

    if (choiceActive)
    {
        // Cursor over the option list; action selects.
        if (cursorUpPressed())
        {
            choiceCursor--;
            if (choiceCursor < 0) choiceCursor = line.choiceCount - 1;
        }
        if (cursorDownPressed())
        {
            choiceCursor++;
            if (choiceCursor >= line.choiceCount) choiceCursor = 0;
        }

        if (actionPressed() && line.choices)
        {
            const DialogueChoice& choice = line.choices[choiceCursor];
            applyFlags(choice.setFlags, choice.setCount, choice.clearFlags, choice.clearCount);
            if (choice.sfx) PlaySound(loadScriptSfx(choice.sfx));
            choiceActive = false;

            // The chosen option decides where the dialogue goes (the line's
            // own goto is skipped while branching).
            if (choice.gotoLine >= 0) lineIndex = choice.gotoLine;
            else lineIndex++;
            startLine();
        }
        return;
    }

    if (actionPressed())
    {
        if (reveal < (float)totalChars)
        {
            reveal = (float)totalChars;
            PlaySound(finishSfx);
        }
        else if (line.choiceCount > 0 && line.choices)
        {
            // First press opens the choice list.
            choiceActive = true;
            choiceCursor = 0;
        }
        else
        {
            advanceLine(true);
        }
    }
}

const char* DialogueBox::takeMusicRequest()
{
    if (!musicPending) return 0;
    musicPending = false;
    return musicRequest;
}

bool DialogueBox::takeWarpRequest(int& room, int& tileX, int& tileY)
{
    if (!warpPending) return false;
    warpPending = false;
    room = warpRoom;
    tileX = warpTileX;
    tileY = warpTileY;
    return true;
}

void DialogueBox::drawChoices(Rectangle box) const
{
    if (!current || !choiceActive) return;
    const DialogueLine& line = current->lines[lineIndex];
    if (line.choiceCount <= 0 || !line.choices) return;

    float sc = scale();
    float textSize = fontSize();
    float spacing = sc;
    int patch = border.width / 3;
    if (patch < 1) patch = 1;

    // Options stack in the bottom-right corner of the box (LttP style).
    float top = (box.y + box.height - (float)patch - TEXT_PAD_Y
                 - (float)(line.choiceCount * LINE_HEIGHT)) * sc;
    float right = (box.x + box.width - (float)patch - TEXT_PAD_X) * sc;
    float cursorX = right - 14.0f * sc;

    for (int i = 0; i < line.choiceCount; i++)
    {
        const char* option = line.choices[i].text ? line.choices[i].text : "";
        float width = MeasureTextEx(font, option, textSize, spacing).x;
        Vector2 pos = { right - width, top + (float)(i * LINE_HEIGHT) * sc };
        DrawTextEx(font, option, pos, textSize, spacing, RAYWHITE);

        if (i == choiceCursor)
        {
            float cy = pos.y + textSize * 0.5f;
            Vector2 a = { cursorX, cy - 5.0f * sc };
            Vector2 b = { cursorX, cy + 5.0f * sc };
            Vector2 c2 = { cursorX + 8.0f * sc, cy };
            DrawTriangle(a, b, c2, RAYWHITE);
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

    // Waiting-state prompt: a blinking arrow once the line is fully typed
    // (skipped when the choice list takes over the box).
    if (reveal >= (float)totalChars && !choiceActive
        && lineIndex >= 0 && lineIndex < current->lineCount
        && current->lines[lineIndex].choiceCount <= 0
        && ((int)(GetTime() * 3.0f) % 2 == 0))
    {
        int patchArrow = border.width / 3;
        if (patchArrow < 1) patchArrow = 1;
        float ax = (box.x + box.width - (float)patchArrow - TEXT_PAD_X - 2.0f) * sc;
        float ay = (box.y + box.height - (float)patchArrow - TEXT_PAD_Y + 1.0f) * sc;
        Vector2 a = { ax, ay - 3.0f * sc };
        Vector2 b = { ax + 6.0f * sc, ay - 3.0f * sc };
        Vector2 c = { ax + 3.0f * sc, ay + 3.0f * sc };
        DrawTriangle(a, b, c, RAYWHITE);
    }

    drawChoices(box);
}