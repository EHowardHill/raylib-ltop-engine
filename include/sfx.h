#pragma once

#include "engine.h"

// Shared "sfx" action vocabulary (trigger vocabulary, used by dialogue
// lines and item use-actions): play a named audio file from
// assets/audio/, lazily loaded and cached. Games cue sounds by name in
// data; nothing preloads per-feature banks.
void playNamedSfx(const char* name);

// Free the cached sounds (shutdown).
void shutdownNamedSfx();
