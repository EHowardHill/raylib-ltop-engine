#pragma once

#include "engine.h"

class Enemy;
class Room;

// Layer-4 seam: game-local C++ (resources/code/*.cpp, compiled into the
// binary by the build scripts) registers named behaviors at static-init
// time; game JSON references them by "custom:<name>" and the generator
// validates the names against resources/code/manifest.json at bake.
// Registration is bake-time linked (no runtime parsing, no filesystem),
// and game code follows the engine rules: frameDt(), the engine Rng,
// fixed arrays, no exceptions/RTTI (Dreamcast toolchain).

// Enemy custom behavior: owns the whole AI while the enemy is in an AI
// state (idle/walk/pause/charge). The engine keeps handling
// hurt/stun/frozen recovery and hit resolution around the behavior.
typedef void (*EnemyBehaviorFn)(Enemy& self, const Room& room,
                                Vector2 heroCenter, Rng& rng, float dt);

// Register a behavior by name (idempotent on re-registration; warns and
// ignores on duplicate names or a full registry).
void gamecodeRegisterEnemyBehavior(const char* name, EnemyBehaviorFn fn);

// Look up a registered behavior (0 = not found).
EnemyBehaviorFn gamecodeFindEnemyBehavior(const char* name);
