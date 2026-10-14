#include "gamecode.h"
#include "enemy.h"
#include "room.h"

#include <math.h>

// Moldorm: the tail-weak-point archetype (layer 4 — the choreography is
// game code; the engine owns hit resolution through the armed weak
// point). A segmented worm careens around its arena in long arcs, its
// body bobbing behind it as a distance-sampled trail; only the tail
// takes damage — body strikes bounce off. It hurries as its health
// drops and bursts briefly off wall bounces.
//
// Scratch slots: [0] heading (radians), [1] wall-burst timer,
// [2] sway phase, [3] armed sentinel (0 = first run).

static const float MOLDORM_BASE_SPEED = 72.0f;
static const float MOLDORM_HURT_BONUS = 90.0f;   // extra px/s per health lost
static const float MOLDORM_BURST_SPEED = 60.0f;  // wall bounce kick, px/s
static const float MOLDORM_BURST_TIME = 0.5f;    // seconds
static const float MOLDORM_TRAIL_SEGMENTS = 8.0f;
static const float MOLDORM_TRAIL_SPACING = 10.0f;
static const float MOLDORM_WEAK_SIZE = 22.0f;     // tail hit box

static void moldormBehavior(Enemy& self, const Room& room, Vector2 heroCenter,
                            Rng& rng, float dt)
{
    float& heading = self.scratch(0);
    float& burst = self.scratch(1);
    float& sway = self.scratch(2);
    float& armed = self.scratch(3);

    if (armed == 0.0f)
    {
        armed = 1.0f;
        self.enableTrail((int)MOLDORM_TRAIL_SEGMENTS, MOLDORM_TRAIL_SPACING);
    }

    sway += dt * 2.6f;
    if (burst > 0.0f) burst -= dt;

    // Phase pacing: faster as the health drops, plus the wall-burst kick.
    float hpRatio = (self.maxHealth() > 0)
                        ? (float)self.health() / (float)self.maxHealth()
                        : 1.0f;
    float speed = MOLDORM_BASE_SPEED + (1.0f - hpRatio) * MOLDORM_HURT_BONUS
                  + (burst > 0.0f ? MOLDORM_BURST_SPEED : 0.0f);

    // Long arcs: a slow sinusoidal sway bends the heading rather than a
    // jitter, so the worm carves across the arena.
    heading += sinf(sway) * 1.35f * dt;

    float dx = cosf(heading);
    float dy = sinf(heading);
    self.faceVector(dx, dy);

    if (!self.slide(room, dx * speed * dt, dy * speed * dt))
    {
        // Wall: reverse along the blocked path with a jittered angle and
        // a short burst, so the bounce never stalls in a corner.
        heading = atan2f(-dy, -dx) + (rng.nextFloat() - 0.5f) * 0.9f;
        burst = MOLDORM_BURST_TIME;
    }

    // The tail is the weak point; body hits bounce (engine-side, one
    // bounce per swing id).
    float tailX = 0.0f;
    float tailY = 0.0f;
    if (self.tailPosition(tailX, tailY))
        self.setWeakPointAt(tailX, tailY, MOLDORM_WEAK_SIZE, MOLDORM_WEAK_SIZE);
}

// Bake-time-linked registration (the gamecode seam).
static const bool moldormRegistered =
    (gamecodeRegisterEnemyBehavior("moldorm", moldormBehavior), true);
