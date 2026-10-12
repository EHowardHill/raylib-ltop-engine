#pragma once

#include "engine.h"

class Room;

// Hero action states. Actions override/cancel movement cleanly: while a
// state other than idle/walk is active, input is ignored until it resolves.
enum HeroState
{
    HERO_IDLE,
    HERO_WALK,
    HERO_ATTACK,   // sword swing (tap)
    HERO_SPIN,      // spin attack (charged release)
    HERO_DASH,      // pegasus boots charge
    HERO_CRASH,     // dashed into a wall: stunned
    HERO_HURT,
    HERO_FALL,
    HERO_SPLASH,   // walked into deep water: flinch, splash, bounce back
    HERO_HOP,      // vaulted over a ledge tile: arc + landing dust
    HERO_LIFT,     // hoisting a liftable object overhead (resolves to carrying)
    HERO_DEAD,
    HERO_STATE_COUNT
};

class Hero
{
public:
    Hero();

    void load(const CharacterConfig& config);
    void unload();
    void place(int tileX, int tileY);
    void update(const Room& room, float dt);
    void draw() const;

    void tileUnder(int& tileX, int& tileY) const;
    Vector2 center() const;
    // Direction the hero is currently pressing (cardinal-dominant), zero
    // while busy or idle-with-no-input. Pushable blocks read this.
    Vector2 inputDir() const { return Vector2{ inputX, inputY }; }
    // Escape hatch for overlap resolution (settle-shoves): raw position
    // move ignoring collision.
    void nudge(float dx, float dy) { x += dx; y += dy; }

    HeroState state() const { return state_; }
    float stateElapsed() const { return stateTime; }
    bool attacking() const { return state_ == HERO_ATTACK; }
    bool spinning() const { return state_ == HERO_SPIN; }
    bool dashing() const { return state_ == HERO_DASH; }
    bool dead() const { return state_ == HERO_DEAD; }
    bool busy() const { return state_ != HERO_IDLE && state_ != HERO_WALK; }
    // Shield passively blocks projectiles arriving from the faced direction
    // while the hero is free to move (walk/idle).
    bool shielding() const { return state_ == HERO_IDLE || state_ == HERO_WALK; }
    // Sword reach hitbox for the current swing frame (or the 360-degree spin
    // radius during a spin), or a zero rect otherwise. Enemies check this
    // while the swing is live.
    Rectangle attackBox() const;
    // Collision box in world pixels (matches the movement box; doubles as the
    // dash contact hitbox).
    Rectangle box() const;
    // Increments every time an offensive action (swing, spin, dash) starts;
    // enemies compare it against the action they last took a hit from so one
    // action can only hit a given enemy once.
    int swingId() const { return swing_; }
    Direction facingDirection() const { return facing; }
    int healthHalves() const { return health_; }
    int maxHealthHalves() const { return maxHealth_; }
    // Carrying a liftable object (pot, bush): the object rides overhead,
    // sword/dash are locked out, and the action button throws it.
    bool carrying() const { return carrying_; }
    void setCarrying(bool carrying) { carrying_ = carrying; }
    // Hoist pause when picking a liftable object up off the ground.
    static const float LIFT_DURATION;
    void startLift();
    // Restore health (half-hearts), clamped at max.
    void heal(int halfHearts);
    // Heart container pickup: raises the max (half-hearts) and fills it.
    void addMaxHearts(int halfHearts);
    // Save restore: set current + max health directly (clamped sane).
    void setHealth(int halfHearts, int maxHalfHearts);
    // Full reset after death (health refilled); placement is loadRoom's job.
    void revive();
    // Take damage from an external source (enemies, projectiles); knockback
    // pushes along knockDir (expected normalized, (0,0) means straight up).
    void hurt(int halfHearts, Vector2 knockDir);

    // True while the hero is wading in shallow water (footstep sfx swap).
    bool wading() const { return wading_; }

private:
    void setState(HeroState next);
    void faceVector(float dx, float dy);
    void startFall();
    void startSplash();
    void startHop(float dx, float dy, const Room& room);
    void startDeath();
    void startSpin();
    void startDash(float dx, float dy);
    bool slideWithCollision(const Room& room, float dx, float dy);
    void updateFootsteps(const Room& room, float movedDist, const Sound* overrideSfx = 0);
    void drawFrame(int frame, float scaleX, float scaleY) const;

    const CharacterConfig* cfg;
    Texture2D sheet;
    Texture2D swordSheet;
    Sound swordSfx;
    Sound hurtSfx;
    Sound fallSfx;
    Sound spinChargeSfx;
    Sound spinSfx;
    Sound dashSfx;
    Sound crashSfx;
    Sound dieSfx;
    Sound splashSfx;
    Sound wadeSfx;
    Sound landSfx;
    Sound liftSfx;
    bool sfxLoaded;
    float x;
    float y;
    Direction facing;
    bool moving;
    float animTime;
    float stepDist;
    HeroState state_;
    float stateTime;
    float invulnTime;
    int health_;
    int maxHealth_;
    float knockX;
    float knockY;
    float swordHeldTime;
    bool chargeCuePlayed;
    float dashX;
    float dashY;
    int swing_;
    int safeTileX;
    int safeTileY;
    // Ice momentum: kept while sliding on ice, zeroed elsewhere.
    float velX;
    float velY;
    // Deep-water splash: where the hero stepped in (it hops back to the
    // safe tile from there).
    float splashX;
    float splashY;
    bool wading_;
    // Current input direction (see inputDir()).
    float inputX;
    float inputY;
    // Ledge vault: takeoff and landing positions plus the landing beat.
    float hopStartX;
    float hopStartY;
    float hopTargetX;
    float hopTargetY;
    bool hopLanded;
    // Carrying mode (see carrying()).
    bool carrying_;
};