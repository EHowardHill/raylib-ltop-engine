#pragma once

#include "engine.h"
#include "sfx.h"

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
    HERO_SWIM,     // deep water with flippers: stroke-glide swimming
    HERO_HOP,      // vaulted over a ledge tile: arc + landing dust
    HERO_LIFT,     // hoisting a liftable object overhead (resolves to carrying)
    HERO_GRAB,     // holding a grabbable object (statue/block): pull to drag
    HERO_HAMMER,   // hammer pound: windup, quake in the facing, recover
    HERO_DIG,      // shovel: windup, turn over the tile ahead, recover
    HERO_HOOKSHOT, // firing the chained hook: locked while it flies, yanked on latch
    HERO_SPRINKLE, // magic powder: flick into the facing (main resolves the puff)
    HERO_NET,      // bug net: swipe in the facing (main catches fairies/bees, agitates)
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
    // Shallow-water reflection: a flipped, translucent copy of the sprite
    // mirrored below the feet (drawn by the y-sort pass before the hero).
    void drawReflection() const;

    void tileUnder(int& tileX, int& tileY) const;
    Vector2 center() const;

    // Pit-landing: arm the room+tile a fall-through warps to (room < 0 =
    // the usual safe-tile respawn); main arms it at room load from the
    // RoomConfig and drains the pending warp right after the fall.
    void setFallRedirect(int room, int tileX, int tileY);
    bool takeFallRedirect(int& room, int& tileX, int& tileY);
    // Direction the hero is currently pressing (cardinal-dominant), zero
    // while busy or idle-with-no-input. Pushable blocks read this.
    Vector2 inputDir() const { return Vector2{ inputX, inputY }; }
    // Escape hatch for overlap resolution (settle-shoves): raw position
    // move ignoring collision.
    void nudge(float dx, float dy) { x += dx; y += dy; }
    // Face a vector (cardinal-dominant): the cutscene blocking glide
    // steers the hero's sprite while it walks.
    void faceVector(float dx, float dy);

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
    // Active shield (held button): raised while free to move, slowing the
    // walk and blocking a wider arc of projectiles.
    bool guarding() const { return guarding_; }
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
    // Grabbing a pullable object: the action button grabs it while the
    // hero faces it (main pairs the grabbed object index); pressing away
    // drags it one tile (main checks both paths before starting the
    // pull). Releasing the action button lets go.
    bool grabbing() const { return state_ == HERO_GRAB; }
    bool pulling() const { return state_ == HERO_GRAB && pullActive; }
    void startGrab();
    void startPull(float dx, float dy);
    // Hammer (use-action item): windup, then the pound (main lands the
    // quake effects at HAMMER_WINDUP), then recover. attackBox() is live
    // for the whole swing; hits dedupe by swing id like sword swings.
    static const float HAMMER_WINDUP;
    static const float HAMMER_DURATION;
    void startHammer();
    bool hammering() const { return state_ == HERO_HAMMER; }

    // Shovel (use-action item): windup, then the dig (main turns over the
    // tile ahead at DIG_WINDUP — drops + buried treasure), then recover.
    static const float DIG_WINDUP;
    static const float DIG_DURATION;
    void startDig();
    bool digging() const { return state_ == HERO_DIG; }

    // Hookshot (use-action item): the hero stands locked while the chain
    // flies (main owns the head); latching onto a hook target yanks the
    // hero across — a collision-free glide over pits and water.
    void startHookshot();
    // Magic powder (use-action item): windup, then the puff in the facing
    // (main resolves stuns/torches/shrines at SPRINKLE_WINDUP).
    static const float SPRINKLE_WINDUP;
    static const float SPRINKLE_DURATION;
    void startSprinkle();
    bool sprinkling() const { return state_ == HERO_SPRINKLE; }
    // Bug net (use-action item): windup, then the swipe in the facing
    // (main resolves catches/agitation at NET_WINDUP).
    static const float NET_WINDUP;
    static const float NET_DURATION;
    void startNet();
    bool netting() const { return state_ == HERO_NET; }
    void startHookPull(float targetX, float targetY);
    void endHookshot();  // the chain resolved (main calls it)
    bool hookshotting() const { return state_ == HERO_HOOKSHOT; }
    bool hookPulling() const { return state_ == HERO_HOOKSHOT && hookPullActive; }
    // Riding a moving platform (main computes the overlap each frame):
    // tile hazards are suppressed — the platform is the ground underfoot.
    void setRiding(bool riding) { riding_ = riding; }
    bool riding() const { return riding_; }

    // Protection (Byrna aura, magic cape): incoming damage is ignored
    // while set — main recomputes it every frame from the item states.
    void setProtected(bool isProtected) { protected_ = isProtected; }

    // Cloak (magic cape): the hero blinks ghostly while set (main recomputes).
    void setCloaked(bool isCloaked) { cloaked_ = isCloaked; }
    bool cloaked() const { return cloaked_; }

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

    // Swimming (deep water with flippers, as a passive item grants).
    bool swimming() const { return state_ == HERO_SWIM; }
    void setCanSwim(bool canSwim) { canSwim_ = canSwim; }
    bool canSwim() const { return canSwim_; }

private:
    void setState(HeroState next);
    void startFall();
    void startSplash();
    void startSwim();
    void startHop(float dx, float dy, const Room& room);
    void startDeath();
    void startSpin();
    void startDash(float dx, float dy);
    bool slideWithCollision(const Room& room, float dx, float dy);
    void updateFootsteps(const Room& room, float movedDist, const char* overrideSfx = 0);
    void drawFrame(int frame, float scaleX, float scaleY, float yOff = 0.0f) const;

    const CharacterConfig* cfg;
    Texture2D sheet;
    Texture2D swordSheet;
    // Sfx decode lazily through the shared cache (memory-conscious: no
    // per-room re-decode, nothing decodes until first heard).
    LazySound swordSfx;
    LazySound hurtSfx;
    LazySound fallSfx;
    LazySound spinChargeSfx;
    LazySound spinSfx;
    LazySound dashSfx;
    LazySound crashSfx;
    LazySound dieSfx;
    LazySound splashSfx;
    LazySound wadeSfx;
    LazySound swimSfx;
    LazySound landSfx;
    LazySound liftSfx;
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
    // Pit-landing redirect (see setFallRedirect / takeFallRedirect).
    int fallRedirectRoom;
    int fallRedirectX;
    int fallRedirectY;
    bool fallRedirectPending;
    // Deep-water swimming (granted by a passive item): stroke impulses
    // glide against water drag; ripples mark each stroke.
    bool canSwim_;
    float strokeTime;
    // Riding a moving platform (see setRiding).
    bool riding_;
    // Protection (Byrna aura / magic cape): incoming damage ignored.
    bool protected_;
    // Cloak (magic cape): ghostly blink while set.
    bool cloaked_;
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
    // Raised shield (held button, idle/walk only): wider block arc + slow.
    bool guarding_;
    // Grab-and-pull (see grabbing()): one-tile backward glide while the
    // grabbed object slides along in step (main drives the object).
    bool pullActive;
    float pullT;
    float pullFromX;
    float pullFromY;
    float pullToX;
    float pullToY;
    // Hookshot yank (see startHookPull()): collision-free glide to the
    // latched target; hazards are skipped until it lands.
    bool hookPullActive;
    float hookPullT;
    float hookPullDuration;
    float hookFromX;
    float hookFromY;
    float hookToX;
    float hookToY;
};