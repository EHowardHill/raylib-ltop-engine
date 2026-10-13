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

    bool alive() const { return hp > 0; }
    bool isBoss() const { return cfg && cfg->boss; }
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

    // Ice rod: encase in ice for the given seconds (AI stops; a sword
    // strike or fire shatters it instantly).
    void freeze(float seconds);
    bool frozen() const { return mode == ENEMY_FROZEN; }

    // Sword resolution: applies one hit per swing (single-hit-per-swing via
    // swing ids), knockback along knockDir, hitstun; returns true if this
    // call is the one that killed it.
    bool tryHit(Rectangle attackBox, int swingId, Vector2 knockDir);

    // Event drains: the room systems read these once per frame.
    bool popShot(float& dirX, float& dirY);

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
    CustomBehaviorFn customFn;
};
