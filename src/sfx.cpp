#include "sfx.h"

#include <stdio.h>
#include <string.h>

// Lazy sfx cache for data cues and LazySound wrappers (name -> loaded
// Sound). One shared cache: footsteps, engine feedback banks, hero cues,
// and dialogue letters all decode here on first use and stay for the
// session. The cap covers a game's whole working set (the baked content
// alone cites ~73 distinct names); overflow reuses slot 0 with a logged
// warning (playing a wrong cue beats crashing, but it should be heard
// about too).
static const int MAX_NAMED_SFX = 128;
struct NamedSfx
{
    char name[96];
    Sound sound;
    bool used;
};
static NamedSfx namedSfx[MAX_NAMED_SFX] = {};

static Sound& loadNamedSfx(const char* name)
{
    for (int i = 0; i < MAX_NAMED_SFX; i++)
    {
        if (namedSfx[i].used && strcmp(namedSfx[i].name, name) == 0)
            return namedSfx[i].sound;
    }
    for (int i = 0; i < MAX_NAMED_SFX; i++)
    {
        if (namedSfx[i].used) continue;
        char path[160];
        snprintf(path, sizeof(path), "assets/audio/%s", name);
        namedSfx[i].sound = LoadSound(path);
        namedSfx[i].used = true;
        snprintf(namedSfx[i].name, sizeof(namedSfx[i].name), "%s", name);
        return namedSfx[i].sound;
    }
    // Cache full: reuse slot 0 (playing a wrong cue beats crashing).
    TraceLog(LOG_WARNING, "SFX cache full (%d): '%s' plays slot 0's cue",
             MAX_NAMED_SFX, name);
    return namedSfx[0].sound;
}

void playNamedSfx(const char* name)
{
    if (!name || !name[0]) return;
    PlaySound(loadNamedSfx(name));
}

Sound& LazySound::get() const
{
    static Sound emptySound = {};
    if (!name || !name[0]) return emptySound;
    return loadNamedSfx(name);
}

void shutdownNamedSfx()
{
    for (int i = 0; i < MAX_NAMED_SFX; i++)
    {
        if (namedSfx[i].used)
        {
            UnloadSound(namedSfx[i].sound);
            namedSfx[i].used = false;
            namedSfx[i].name[0] = '\0';
        }
    }
}
