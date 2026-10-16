#pragma once

#include "engine.h"

class Room;

// Runtime enemy instance. Config + sheet + sfx are baked/generated; the
// instance owns lifecycle state only. Rooms spawn their sets on entry
// (defeated enemies stay dead until the room reloads).
class Enemy
{
public:
    enum Mode
    {
        ENEMY_IDLE,
        ENEMY_WALK,
        ENEMY_PAUSE,
        ENEMY_CHARGE,
        ENEMY_STUN,
        ENEMY_FROZEN,  // ice rod: solid ice block until it thaws
        ENEMY_HURT,
        ENEMY_MODE_COUNT
    };

    // Game-code custom behaviors (resources/code/) implement the whole AI
    // for enemies whose config references "custom:<name>"; the engine
    // still owns hurt/stun/frozen recovery around them.
    typedef void (*CustomBehaviorFn)(Enemy& self, const Room& room,
                                     Vector2 heroCenter, Rng& rng, float dt);

    Enemy();

    void spawn(const EnemyConfig& config, Texture2D sheet, int tileX, int tileY);
    void update(const Room& room, Vector2 heroCenter, Rng& rng, float dt);
    void draw() const;
    // Shallow-water reflection: flipped, translucent copy below the feet.
    void drawReflection() const;

    bool alive() const { return hp > 0; }
    bool isBoss() const { return cfg && cfg->boss; }
    // Mid-fight: seals the room + health bar while alive; room-local death.
    bool isMiniBoss() const { return cfg && cfg->miniBoss; }
    bool isArenaLock() const { return isBoss() || isMiniBoss(); }
    int health() const { return hp; }
    int maxHealth() const { return cfg ? cfg->hp : 0; }
    const EnemyConfig& config() const { return *cfg; }
    // Registered hits so far (lethal or not); lets the caller tell "no
    // connect" apart from a non-lethal strike (hit-pause feedback).
    int hitsLanded() const { return hits; }
    Rectangle box() const;
    Vector2 center() const;
    int contactDamage() const { return cfg ? cfg->contactDamage : 0; }

    // Stun (boomerang, wall crashes): freeze AI for the given seconds.
    void stun(float seconds);

    // Contact-damage recoil: the attacker bounces off the hero too
    // (LttP's mutual shove) and rides its HURT->PAUSE recovery. Without
    // it, a same-speed pursuer just strides back in during the hero's
    // i-frames and the hero dies pinned. Bosses keep their choreography
    // and frozen enemies stay frozen (they can't chase anyway).
    void bounce(Vector2 dir);

    // Bug net agitation: the swished enemy dashes about, faster and
    // erratic, for the given seconds (no damage — it's a net).
    void agitate(float seconds);
    bool agitated() const { return agitateTimer > 0.0f; }
    // Net-catchable critter (bees): the bug net bottles it.
    bool catchable() const { return cfg && cfg->catchable; }
    // Caught (or otherwise lifted out of the room) without dying: no
    // poof, no drop — the slot simply frees up for the room-clear check.
    void despawn() { if (hp > 0) hp = 0; }

    // Ice rod: encase in ice for the given seconds (AI stops; a sword
    // strike or fire shatters it instantly).
    void freeze(float seconds);
    bool frozen() const { return mode == ENEMY_FROZEN; }

    // Harmless (behavior-set): the enemy carries no contact damage
    // while set — a submerged zora is part of the scenery until it
    // surfaces. Projectiles and sword hits still land like normal.
    void setHarmless(bool isHarmless) { harmless = isHarmless; }
    bool isHarmless() const { return harmless; }

    // Sword resolution: applies `damage` hits per swing (single hit per
    // swing id), knockback along knockDir, hitstun; returns true if this
    // call is the one that killed it.
    bool tryHit(Rectangle attackBox, int swingId, Vector2 knockDir,
                int damage = 1);

    // Event drains: the room systems read these once per frame.
    bool popShot(float& dirX, float& dirY);
    // Queue a projectile along a direction (drained by the room's
    // projectile spawner). Custom behaviors shoot with this.
    void queueShot(float dirX, float dirY)
    {
        shotDirX = dirX;
        shotDirY = dirY;
        shotPending_ = true;
    }

    // --- Custom behavior API (gamecode layer 4) ---
    // Movement/mode primitives custom behaviors drive the enemy with. The
    // engine handles ENEMY_HURT/STUN/FROZEN recovery on its own; a custom
    // behavior runs in the AI states (idle/walk/pause/charge).
    void setMode(Mode next);
    Mode modeState() const { return mode; }
    bool slide(const Room& room, float dx, float dy);
    void faceVector(float dx, float dy);
    void pickDirection(Rng& rng);
    void setPosition(float px, float py) { x = px; y = py; }
    float modeElapsed() const { return modeTime; }
    float aiTimer() const { return timer; }

    // --- Segmented-body vocabulary (Moldorm-style worms, Lanmolas) ---
    // A distance-sampled position trail the engine records and draws as
    // shrinking segments behind the sprite; the oldest sample is the
    // "tail" (count <= 12, spacing in px between samples). Recorded
    // automatically once enabled; cleared at spawn.
    void enableTrail(int count, float spacing);
    bool tailPosition(float& x, float& y) const;

    // Weak point (the tail): when armed (w > 0), only strikes landing
    // inside the sub-box at the given world position connect; everything
    // else bounces off harmlessly (one bounce per swing id). Behaviors
    // re-arm it every frame; cleared at spawn.
    void setWeakPointAt(float worldX, float worldY, float w, float h);

    // Layer-4 scratch: a few private floats per instance for game-code
    // behaviors to keep arbitrary state in (headings, phases, sentinels).
    // Zeroed at spawn; the engine never reads them.
    float& scratch(int i) { return scratch_[i & 3]; }
    void setAiTimer(float seconds) { timer = seconds; }
    Direction facingDirection() const { return facing; }
    void setFacing(Direction dir) { facing = dir; }

private:
    void drawFrame(int frame) const;

    const EnemyConfig* cfg;
    Texture2D sheet;
    float x;
    float y;
    Direction facing;
    Mode mode;
    float modeTime;
    float timer;
    int hp;
    int hits;
    float knockX;
    float knockY;
    int lastSwing;
    bool shotPending_;
    float shotDirX;
    float shotDirY;
    float animTime;
    float agitateTimer;
    // Segmented-body trail: distance-sampled positions, oldest first.
    static const int TRAIL_MAX = 12;
    float trailX[TRAIL_MAX];
    float trailY[TRAIL_MAX];
    int trailCount;
    float trailSpacing;
    float trailDist;       // px accumulated since the last sample
    float lastSampleX;
    float lastSampleY;
    // Weak point (armed by behaviors; see setWeakPointAt).
    float weakX;
    float weakY;
    float weakW;
    float weakH;
    // Contact-damage off switch (see setHarmless).
    bool harmless;
    // Layer-4 scratch (see scratch()).
    float scratch_[4];
    CustomBehaviorFn customFn;
};
