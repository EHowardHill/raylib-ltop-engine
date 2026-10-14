#pragma once

#include "engine.h"

// Shared "sfx" action vocabulary (trigger vocabulary, used by dialogue
// lines and item use-actions): play a named audio file from
// assets/audio/, lazily loaded and cached. Games cue sounds by name in
// data; nothing preloads per-feature banks.
void playNamedSfx(const char* name);

// Free the cached sounds (shutdown).
void shutdownNamedSfx();

// Lazy sfx wrapper: a named sound that decodes on first use through the
// shared lazy cache — the memory-conscious loading strategy (the web
// build only pays for the cues it actually hears, and nothing decodes at
// boot or on room switches). Implicitly converts wherever a Sound goes,
// so `static LazySound sndFoo = { "hurt.wav" };` drops straight in for
// an eager bank; names are files under assets/audio/.
struct LazySound
{
    const char* name;
    Sound& get() const;
    operator Sound&() const { return get(); }
};
