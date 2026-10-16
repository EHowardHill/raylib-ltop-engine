#include "gamecode.h"
#include "enemy.h"

#include <math.h>

// Zora: an aquatic ambusher (LttP). It lurks submerged on its water tile,
// surfaces to spit at the hero, holds a beat, then dives — vulnerable
// while surfaced. The engine runs hurt/stun/hit resolution like any enemy;
// this behavior only owns the AI states (idle = under, walk = surfacing,
// pause = post-spit hold). Stand frames read as the submerged ripple,
// move frames as the surfaced body, so real art can play the dive cycle
// with no engine changes.
//
// Game-code rules apply: engine Rng only (passed in), fixed timing (no
// per-frame rng rolls in a condition), no file I/O.

static void zoraBehavior(Enemy& self, const Room& room, Vector2 heroCenter,
                         Rng& rng, float dt)
{
    Vector2 c = self.center();
    float t = self.modeElapsed();

    // Submerged (the lurk) carries no contact damage: the ripple is
    // scenery until the zora surfaces to spit. Hits still land.
    self.setHarmless(self.modeState() == Enemy::ENEMY_IDLE);

    switch (self.modeState())
    {
    case Enemy::ENEMY_IDLE:  // submerged: ripple, waiting to surface
        if (t > 3.0f)
        {
            self.setMode(Enemy::ENEMY_WALK);
            self.faceVector(heroCenter.x - c.x, heroCenter.y - c.y);
        }
        break;

    case Enemy::ENEMY_WALK:  // surfaced: track the hero, then spit
        self.faceVector(heroCenter.x - c.x, heroCenter.y - c.y);
        if (t > 0.45f)
        {
            float dx = heroCenter.x - c.x;
            float dy = heroCenter.y - c.y;
            float len = sqrtf(dx * dx + dy * dy);
            if (len > 0.0001f) self.queueShot(dx / len, dy / len);
            self.setMode(Enemy::ENEMY_PAUSE);
        }
        break;

    case Enemy::ENEMY_PAUSE:  // surfaced hold, then dive
        if (t > 0.9f) self.setMode(Enemy::ENEMY_IDLE);
        break;

    default:
        self.setMode(Enemy::ENEMY_IDLE);
        break;
    }
}

// Bake-time-linked registration (the gamecode seam): the manifest lists
// "zora" and enemy jsons reference "custom:zora".
static const bool zoraRegistered =
    (gamecodeRegisterEnemyBehavior("zora", zoraBehavior), true);
