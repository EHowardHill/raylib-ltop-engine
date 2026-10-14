#include "dialogue.h"
#include "assets.h"
#include "sfx.h"

#include <stdio.h>
#include <string.h>

static const int MAX_ROWS = 8;
// The 72px-tall box fits three 16px text lines plus the arrow row; extra
// wrapped rows stay in the buffer but never draw over the border.
static const int BOX_ROWS = 3;
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
// Credits roll: the default beat a centered line holds before the roll
// advances on its own (a line's "wait" overrides it).
static const float CREDITS_BEAT = 1.6f;

// Installed by the game at boot (see dialogue.h).
bool (*gDialogueItemOwned)(int item) = 0;
bool (*gDialogueTryPayRupees)(int rupees) = 0;

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
    : font(), fontOwned(false), border(), letterSfx(), finishSfx(), errorSfx(),
      current(0), lineIndex(0), totalChars(0), reveal(0.0f), rowCount(0),
      choiceActive(false), choiceCursor(0),
      musicRequest(), musicPending(false),
      giveItem(-1), givePending(false),
      warpRoom(-1), warpTileX(0), warpTileY(0), warpPending(false),
      camPending(false), camAction(0), camTileX(0), camTileY(0),
      camSeconds(0.0f), letterboxPending(false), letterboxState(-1),
      movePending(false), moveActor(-1), moveTileX(0), moveTileY(0),
      moveSeconds(0.0f), nextDialogue(-1), nextPending(false),
      waitTimer(0.0f)
{
    for (int i = 0; i < MAX_ROWS; i++) rows[i].offset = 0;
}

void DialogueBox::init()
{
    // The game's global font (resources/game.json "font"); games that
    // don't set one get the raylib default bitmap font instead of the
    // hard-coded file.
    if (GAME_FONT && GAME_FONT[0])
    {
        char path[600];
        snprintf(path, sizeof(path), "assets/%s", GAME_FONT);
        font = LoadFontEx(path, DRAW_FONT_BASE, NULL, 0);
        fontOwned = true;
    }
    else
    {
        font = GetFontDefault();
        fontOwned = false;
    }
    border = LoadTexture("assets/ui/border.png");
    SetTextureFilter(border, TEXTURE_FILTER_POINT);
    // Letter cues name their files; they decode on first play through the
    // shared lazy cache.
    letterSfx = { "message.wav" };
    finishSfx = { "message_finish.wav" };
    errorSfx = { "error.wav" };
}

void DialogueBox::shutdown()
{
    if (fontOwned) UnloadFont(font);
    fontOwned = false;
    UnloadTexture(border);
    shutdownNamedSfx();
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
    givePending = false;
    warpPending = false;
    camPending = false;
    letterboxPending = false;
    movePending = false;
    nextPending = false;
    startLine();
}

void DialogueBox::startLine()
{
    reveal = 0.0f;
    rowCount = 0;
    totalChars = 0;
    waitTimer = 0.0f;
    if (!current) return;

    // Skip lines whose conditions fail (skipped lines execute nothing).
    while (lineIndex >= 0 && lineIndex < current->lineCount)
    {
        const DialogueLine& line = current->lines[lineIndex];
        if (line.ifFlag >= 0 && !gWorldFlags[line.ifFlag]) { lineIndex++; continue; }
        if (line.unlessFlag >= 0 && gWorldFlags[line.unlessFlag]) { lineIndex++; continue; }
        if (line.ifItem >= 0 && !(gDialogueItemOwned && gDialogueItemOwned(line.ifItem)))
        {
            lineIndex++;
            continue;
        }
        if (line.unlessItem >= 0 && gDialogueItemOwned && gDialogueItemOwned(line.unlessItem))
        {
            lineIndex++;
            continue;
        }

        // Shown-effects: world flags, sfx cue, music switch, item grant,
        // and the cutscene cues (camera, bars, blocking).
        applyFlags(line.setFlags, line.setCount, line.clearFlags, line.clearCount);
        if (line.sfx) playNamedSfx(line.sfx);
        if (line.music)
        {
            snprintf(musicRequest, sizeof(musicRequest), "%s", line.music);
            musicPending = true;
        }
        if (line.giveItem >= 0)
        {
            giveItem = line.giveItem;
            givePending = true;
        }
        if (line.camAction != 0)
        {
            camAction = line.camAction;
            camTileX = line.camTileX;
            camTileY = line.camTileY;
            camSeconds = line.camSeconds;
            camPending = true;
        }
        if (line.letterbox >= 0)
        {
            letterboxState = line.letterbox;
            letterboxPending = true;
        }
        if (line.moveActor != -1)
        {
            moveActor = line.moveActor;
            moveTileX = line.moveTileX;
            moveTileY = line.moveTileY;
            moveSeconds = line.moveSeconds;
            movePending = true;
        }

        choiceActive = false;
        choiceCursor = 0;

        const char* text = line.text ? line.text : "";
        float patch = (float)(border.width / 3);
        float textWidth = boxWidthVirtual() - 2.0f * (patch + TEXT_PAD_X);
        wrapLine(text, textWidth);
        // A line with options shows its choice list right away — no empty
        // box beat between the prompt and the options (update still takes
        // one press to finish the reveal before a pick can land).
        if (line.choiceCount > 0 && line.choices)
        {
            choiceActive = true;
            choiceCursor = 0;
        }
        return;
    }

    // Ran off the end: the dialogue closes — or chains into the next
    // script when one is linked (a warp line still wins over the chain,
    // since it closes the box on its own path).
    if (current->nextDialogue >= 0 && current->nextDialogue < DIALOGUE_COUNT)
    {
        nextDialogue = current->nextDialogue;
        nextPending = true;
    }
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
        reveal += CHARS_PER_SECOND * frameDt();
        if (reveal > (float)totalChars) reveal = (float)totalChars;

        int now = (int)reveal;
        if (now > previous && text[now - 1] != ' ') PlaySound(letterSfx);
        if (reveal >= (float)totalChars) PlaySound(finishSfx);
    }
    else if (line.waitSeconds > 0.0f || current->credits)
    {
        // Cutscene beats and credits rolls advance on their own once the
        // line is fully revealed; the button can still hurry a beat along.
        float beat = line.waitSeconds > 0.0f ? line.waitSeconds : CREDITS_BEAT;
        waitTimer += frameDt();
        if (waitTimer >= beat && !choiceActive) advanceLine(true);
        return;
    }

    if (choiceActive)
    {
        // Cursor over the option list; action selects (after the prompt's
        // reveal finishes — a pick never cuts the typewriter short).
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
            if (reveal < (float)totalChars)
            {
                reveal = (float)totalChars;
                PlaySound(finishSfx);
                return;
            }

            const DialogueChoice& choice = line.choices[choiceCursor];

            // Shop picks ("cost"): the wallet covers it or the pick
            // rattles — nothing applied, the list stays up.
            if (choice.costRupees > 0
                && !(gDialogueTryPayRupees && gDialogueTryPayRupees(choice.costRupees)))
            {
                PlaySound(errorSfx);
                return;
            }

            applyFlags(choice.setFlags, choice.setCount, choice.clearFlags, choice.clearCount);
            if (choice.sfx) playNamedSfx(choice.sfx);
            // Purchases ("give") queue the same grant drain line gives
            // use; the game turns it into ownership + a toast.
            if (choice.giveItem >= 0)
            {
                giveItem = choice.giveItem;
                givePending = true;
            }
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

bool DialogueBox::takeGiveRequest(int& item)
{
    if (!givePending) return false;
    givePending = false;
    item = giveItem;
    return true;
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

bool DialogueBox::takeCameraRequest(int& action, int& tileX, int& tileY, float& seconds)
{
    if (!camPending) return false;
    camPending = false;
    action = camAction;
    tileX = camTileX;
    tileY = camTileY;
    seconds = camSeconds;
    return true;
}

bool DialogueBox::takeLetterboxRequest(int& state)
{
    if (!letterboxPending) return false;
    letterboxPending = false;
    state = letterboxState;
    return true;
}

bool DialogueBox::takeMoveRequest(int& actor, int& tileX, int& tileY, float& seconds)
{
    if (!movePending) return false;
    movePending = false;
    actor = moveActor;
    tileX = moveTileX;
    tileY = moveTileY;
    seconds = moveSeconds;
    return true;
}

bool DialogueBox::takeNextRequest(int& dialogue)
{
    if (!nextPending) return false;
    nextPending = false;
    dialogue = nextDialogue;
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

    // Options stack in the bottom-right corner of the box (LttP style),
    // right-aligned; the cursor points at the selected option from the
    // left, never over the text.
    float top = (box.y + box.height - (float)patch - TEXT_PAD_Y
                 - (float)(line.choiceCount * LINE_HEIGHT)) * sc;
    float right = (box.x + box.width - (float)patch - TEXT_PAD_X) * sc;

    for (int i = 0; i < line.choiceCount; i++)
    {
        const char* option = line.choices[i].text ? line.choices[i].text : "";
        float width = MeasureTextEx(font, option, textSize, spacing).x;
        Vector2 pos = { right - width, top + (float)(i * LINE_HEIGHT) * sc };
        DrawTextEx(font, option, pos, textSize, spacing, RAYWHITE);

        if (i == choiceCursor)
        {
            float cursorX = pos.x - 12.0f * sc;
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
    float textSize = fontSize();
    float spacing = sc;

    if (current->credits)
    {
        // Endgame roll: the world vanishes behind a black screen and the
        // lines center themselves, one beat at a time.
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), BLACK);
        float viewWidth = (float)GetScreenWidth() / sc;
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

            float width = MeasureTextEx(font, tmp, textSize, spacing).x;
            float y = VIRTUAL_HEIGHT * 0.42f
                      + (float)j * LINE_HEIGHT
                      - (float)rowCount * LINE_HEIGHT * 0.5f;
            Vector2 pos = { (viewWidth - width) * 0.5f, y * sc };
            DrawTextEx(font, tmp, pos, textSize, spacing, RAYWHITE);
        }
        return;
    }

    // Silent cutscene beats (no text, a timed wait): the cues play over
    // the world with no box in the way.
    if (lineIndex >= 0 && lineIndex < current->lineCount)
    {
        const DialogueLine& line = current->lines[lineIndex];
        const char* text = line.text ? line.text : "";
        if (!text[0] && line.waitSeconds > 0.0f) return;
    }

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

    float left = (box.x + (float)patch + TEXT_PAD_X) * sc;
    float top = (box.y + (float)patch + TEXT_PAD_Y) * sc;

    // Rows beyond the third would spill past the box border; the wrap
    // keeps them buffered, only the ones that fit draw.
    int drawnRows = rowCount < BOX_ROWS ? rowCount : BOX_ROWS;
    int revealed = (int)reveal;
    for (int j = 0; j < drawnRows; j++)
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
    // (skipped when the choice list takes over the box). It sits on its
    // own row below the last text line, right-aligned — never on top of
    // the text itself.
    if (reveal >= (float)totalChars && !choiceActive
        && lineIndex >= 0 && lineIndex < current->lineCount
        && current->lines[lineIndex].choiceCount <= 0
        && ((int)(GetTime() * 3.0f) % 2 == 0))
    {
        int patchArrow = border.width / 3;
        if (patchArrow < 1) patchArrow = 1;
        float ax = (box.x + box.width - (float)patchArrow - TEXT_PAD_X - 2.0f) * sc;
        float ay = (box.y + (float)patchArrow + TEXT_PAD_Y
                    + (float)((drawnRows > 0 ? drawnRows : 1) - 1) * LINE_HEIGHT
                    + 8.0f) * sc;
        Vector2 a = { ax, ay - 3.0f * sc };
        Vector2 b = { ax + 6.0f * sc, ay - 3.0f * sc };
        Vector2 c = { ax + 3.0f * sc, ay + 3.0f * sc };
        DrawTriangle(a, b, c, RAYWHITE);
    }

    drawChoices(box);
}