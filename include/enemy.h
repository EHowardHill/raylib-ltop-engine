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

    Enemy();

    void spawn(const EnemyConfig& config, Texture2D sheet, int tileX, int tileY);
    void update(const Room& room, Vector2 heroCenter, Rng& rng, float dt);
    void draw() const;

    bool alive() const { return hp > 0; }
    bool isBoss() const { return cfg && cfg->boss; }
    int health() const { return hp; }
    int maxHealth() const { return cfg ? cfg->hp : 0; }
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

private:
    void setMode(Mode mode);
    void pickDirection(Rng& rng);
    bool slide(const Room& room, float dx, float dy);
    void faceVector(float dx, float dy);
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
    float knockX;
    float knockY;
    int lastSwing;
    bool shotPending_;
    float shotDirX;
    float shotDirY;
    float animTime;
};