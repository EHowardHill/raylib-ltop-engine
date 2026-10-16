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
    : font(), fontOwned(false), border(), cursor(), letterSfx(), finishSfx(),
      errorSfx(),
      current(0), lineIndex(0), totalChars(0), reveal(0.0f), rowCount(0),
      rowOffset(0), choiceActive(false), choiceCursor(0),
      musicRequest(), musicPending(false),
      giveItem(-1), givePending(false),
      warpRoom(-1), warpTileX(0), warpTileY(0), warpPending(false),
      camPending(false), camAction(0), camTileX(0), camTileY(0),
      camSeconds(0.0f), letterboxPending(false), letterboxState(-1),
      movePending(false), moveActor(-1), moveTileX(0), moveTileY(0),
      moveSeconds(0.0f), facePending(false), faceActor(-1), faceDir(0),
      showPending(false), showActor(-1), hidePending(false), hideActor(-1),
      fadePending(false), fadeTo(0), fadeSeconds(0.0f),
      imagePending(false), imageScene(-1), sceneBackdrop(false),
      nextDialogue(-1), nextPending(false),
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
    // Menu cursor arrows (list + continue prompts) — sprites, like every
    // other piece of chrome.
    cursor = LoadTexture("assets/ui/cursor.png");
    SetTextureFilter(cursor, TEXTURE_FILTER_POINT);
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
    UnloadTexture(cursor);
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
    facePending = false;
    showPending = false;
    hidePending = false;
    fadePending = false;
    imagePending = false;
    nextPending = false;
    startLine();
}

void DialogueBox::startLine()
{
    reveal = 0.0f;
    rowCount = 0;
    rowOffset = 0;
    totalChars = 0;
    waitTimer = 0.0f;
    if (!current) return;

    // Skip lines whose conditions fail (skipped lines execute nothing).
    while (lineIndex >= 0 && lineIndex < current->lineCount)
    {
        const DialogueLine& line = current->lines[lineIndex];
        // Bounds-checked like every other flag access; an out-of-range
        // index reads adjacent memory otherwise (it fails open — the
        // line shows).
        if (line.ifFlag >= 0 && line.ifFlag < WORLD_FLAG_COUNT
            && !gWorldFlags[line.ifFlag]) { lineIndex++; continue; }
        if (line.unlessFlag >= 0 && line.unlessFlag < WORLD_FLAG_COUNT
            && gWorldFlags[line.unlessFlag]) { lineIndex++; continue; }
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
        if (line.faceActor != -1)
        {
            faceActor = line.faceActor;
            faceDir = line.faceDir;
            facePending = true;
        }
        if (line.showActor != -1)
        {
            showActor = line.showActor;
            showPending = true;
        }
        if (line.hideActor != -1)
        {
            hideActor = line.hideActor;
            hidePending = true;
        }
        if (line.fadeTo != 0)
        {
            fadeTo = line.fadeTo;
            fadeSeconds = line.fadeSeconds;
            fadePending = true;
        }
        if (line.imageScene != -1)
        {
            imageScene = line.imageScene;
            imagePending = true;
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
            if (candLen + wordLen >= 255)
            {
                if (len > 0) break;  // wrap; the word retries on a fresh row
                // A single word wider than the whole row buffer: hard-
                // break it where the buffer ends, or the row rebuilds
                // empty forever and the line stalls.
                int take = 254;
                memcpy(row.text, text + wordStart, (size_t)take);
                len = take;
                row.text[len] = '\0';
                i = wordStart + take;
                continue;
            }
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

    // Rows capped at MAX_ROWS: whatever past the last row is never drawn,
    // so the typewriter must not count it (invisible characters typed
    // out as dead air before the line can advance).
    if (text[i] && rowCount >= MAX_ROWS) totalChars = i;
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

    // Bounds-checked: an off-by-one goto falls through as a normal
    // advance instead of silently dropping the rest of the script.
    if (useGoto && line.gotoLine >= 0 && line.gotoLine < current->lineCount)
        lineIndex = line.gotoLine;
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

    // The visible page: BOX_ROWS of the wrapped rows at a time. Long
    // lines paginate instead of clipping — the typewriter works within
    // the page, and a full page turns (action, or the beat on its own
    // so unattended cutscenes never stall mid-sentence). Lines with
    // options keep the whole prompt: the option list owns the space.
    bool choiceLine = line.choiceCount > 0 && line.choices;
    int pageEnd = totalChars;
    if (!choiceLine && rowOffset + BOX_ROWS < rowCount)
        pageEnd = rows[rowOffset + BOX_ROWS].offset;
    // A choice prompt only ever shows two rows (the options own the rest
    // of the box), so the reveal/pick gates on exactly what's visible —
    // never on invisible rows the player can't read.
    else if (choiceLine && rowCount > 2) pageEnd = rows[2].offset;

    if (reveal < (float)pageEnd)
    {
        int previous = (int)reveal;
        reveal += CHARS_PER_SECOND * frameDt();
        if (reveal > (float)pageEnd) reveal = (float)pageEnd;

        int now = (int)reveal;
        if (now > previous && text[now - 1] != ' ') PlaySound(letterSfx);
        if (reveal >= (float)pageEnd) PlaySound(finishSfx);
    }

    if (!choiceLine && rowOffset + BOX_ROWS < rowCount)
    {
        // More rows wait off-page: turn before the line can advance. A
        // press hurries the page (finish its reveal first, then turn);
        // unattended beats turn on their own.
        if (actionPressed())
        {
            if (reveal < (float)pageEnd)
            {
                reveal = (float)pageEnd;
                PlaySound(finishSfx);
            }
            else
            {
                rowOffset += BOX_ROWS;
                waitTimer = 0.0f;
                PlaySound(finishSfx);
            }
        }
        else if (line.waitSeconds > 0.0f || current->credits)
        {
            if (reveal >= (float)pageEnd) waitTimer += frameDt();
            float hold = line.waitSeconds > 0.0f ? line.waitSeconds : CREDITS_BEAT;
            if (waitTimer >= hold)
            {
                rowOffset += BOX_ROWS;
                waitTimer = 0.0f;
                PlaySound(finishSfx);
            }
        }
        return;
    }

    // The last page: cutscene beats and credits lines advance on their
    // own — and the button can always hurry a beat along (a press first
    // finishes the reveal, then skips the hold). A choice line is always
    // interactive: its wait is ignored, or the early return below would
    // strand the cursor and the pick could never land.
    if ((line.waitSeconds > 0.0f || current->credits) && line.choiceCount <= 0)
    {
        if (reveal >= (float)pageEnd) waitTimer += frameDt();
        if (actionPressed())
        {
            if (reveal < (float)pageEnd)
            {
                reveal = (float)pageEnd;
                PlaySound(finishSfx);
            }
            else if (!choiceActive)
            {
                advanceLine(true);
            }
            return;
        }
        float hold = line.waitSeconds > 0.0f ? line.waitSeconds : CREDITS_BEAT;
        if (waitTimer >= hold && !choiceActive) advanceLine(true);
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
            if (reveal < (float)pageEnd)
            {
                reveal = (float)pageEnd;
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
            // own goto is skipped while branching). Bounds-checked like
            // the line goto: an off-by-one falls through as a normal
            // advance.
            if (choice.gotoLine >= 0 && choice.gotoLine < current->lineCount)
                lineIndex = choice.gotoLine;
            else lineIndex++;
            startLine();
        }
        return;
    }

    if (actionPressed())
    {
        if (reveal < (float)pageEnd)
        {
            reveal = (float)pageEnd;
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

bool DialogueBox::takeFaceRequest(int& actor, int& dir)
{
    if (!facePending) return false;
    facePending = false;
    actor = faceActor;
    dir = faceDir;
    return true;
}

bool DialogueBox::takeShowRequest(int& actor)
{
    if (!showPending) return false;
    showPending = false;
    actor = showActor;
    return true;
}

bool DialogueBox::takeHideRequest(int& actor)
{
    if (!hidePending) return false;
    hidePending = false;
    actor = hideActor;
    return true;
}

bool DialogueBox::takeFadeRequest(int& to, float& seconds)
{
    if (!fadePending) return false;
    fadePending = false;
    to = fadeTo;
    seconds = fadeSeconds;
    return true;
}

bool DialogueBox::takeImageRequest(int& scene)
{
    if (!imagePending) return false;
    imagePending = false;
    scene = imageScene;
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
    // right-aligned — starting below the prompt's drawn rows so they
    // never cover the text. The cursor points at the selected option
    // from the left, never over the text.
    int promptRows = rowCount < 2 ? rowCount : 2;
    float top = (box.y + (float)patch + TEXT_PAD_Y
                 + (float)promptRows * LINE_HEIGHT + 2.0f) * sc;
    float right = (box.x + box.width - (float)patch - TEXT_PAD_X) * sc;

    for (int i = 0; i < line.choiceCount; i++)
    {
        const char* option = line.choices[i].text ? line.choices[i].text : "";
        float width = MeasureTextEx(font, option, textSize, spacing).x;
        Vector2 pos = { right - width, top + (float)(i * LINE_HEIGHT) * sc };
        DrawTextEx(font, option, pos, textSize, spacing, RAYWHITE);

        if (i == choiceCursor)
        {
            // Cursor sprite pointing at the option from the left.
            Rectangle src = { 0.0f, 0.0f, 8.0f, 8.0f };
            Rectangle dest = { pos.x - 12.0f * sc, pos.y + textSize * 0.5f - 4.0f * sc,
                               8.0f * sc, 8.0f * sc };
            Vector2 origin = { 0.0f, 0.0f };
            DrawTexturePro(cursor, src, dest, origin, 0.0f, RAYWHITE);
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
        // lines center themselves, one beat at a time — unless a scene
        // picture is already the backdrop (the "image" cue composes into
        // a storybook roll).
        if (!sceneBackdrop)
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

    // Rows beyond the page stay buffered, only the visible ones draw
    // (long lines paginate: the box shows three rows, then turns).
    int rowsLeft = rowCount - rowOffset;
    int drawnRows = rowsLeft < BOX_ROWS ? rowsLeft : BOX_ROWS;
    // A choice list caps the prompt to two rows: the options own the
    // rest of the box and never overlap the text.
    if (choiceActive && drawnRows > 2) drawnRows = 2;
    int revealed = (int)reveal;
    for (int j = 0; j < drawnRows; j++)
    {
        const Row& row = rows[rowOffset + j];
        int rowLen = (int)strlen(row.text);
        int count = revealed - row.offset;
        if (count <= 0) break;
        if (count > rowLen) count = rowLen;

        char tmp[256];
        memcpy(tmp, row.text, (size_t)count);
        tmp[count] = '\0';

        Vector2 pos = { left, top + (float)(j * LINE_HEIGHT) * sc };
        DrawTextEx(font, tmp, pos, textSize, spacing, RAYWHITE);
    }

    // Waiting-state prompt: a blinking arrow once the page is fully typed
    // (skipped when the choice list takes over the box). It sits on its
    // own row below the last text line, right-aligned — never on top of
    // the text itself — whether the wait is for a page turn or the next
    // line.
    int pageEnd = totalChars;
    if (rowOffset + BOX_ROWS < rowCount) pageEnd = rows[rowOffset + BOX_ROWS].offset;
    if (reveal >= (float)pageEnd && !choiceActive
        && lineIndex >= 0 && lineIndex < current->lineCount
        && current->lines[lineIndex].choiceCount <= 0
        && ((int)(GetTime() * 3.0f) % 2 == 0))
    {
        int patchArrow = border.width / 3;
        if (patchArrow < 1) patchArrow = 1;
        float ax = (box.x + box.width - (float)patchArrow - TEXT_PAD_X - 4.0f) * sc;
        float ay = (box.y + (float)patchArrow + TEXT_PAD_Y
                    + (float)((drawnRows > 0 ? drawnRows : 1) - 1) * LINE_HEIGHT
                    + 8.0f) * sc;
        // Continue prompt: the down-arrow cursor sprite.
        Rectangle src = { 8.0f, 0.0f, 8.0f, 8.0f };
        Rectangle dest = { ax, ay - 4.0f * sc, 8.0f * sc, 8.0f * sc };
        Vector2 origin = { 0.0f, 0.0f };
        DrawTexturePro(cursor, src, dest, origin, 0.0f, RAYWHITE);
    }

    drawChoices(box);
}