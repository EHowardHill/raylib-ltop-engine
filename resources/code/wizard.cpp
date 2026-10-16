#include "gamecode.h"
#include "enemy.h"
#include "room.h"

#include <math.h>

// Wizard: the reflectable-magic archetype (Agahnim-style, layer 4 — the
// choreography is game code; the engine owns hit resolution). It hovers
// at anchor points around the arena, fires volleys of three shots at the
// hero, and teleports when the volley lands. Its config composes
// "strike_immune": direct hero strikes bounce off, so the fight is won
// by batting the volleys back — the engine deflects projectiles caught
// by a swing, and deflected projectiles strike enemies.
//
// Game-code rules apply: the engine Rng (passed in), fixed timing, no
// per-frame rolls inside conditions, no file I/O.

// Cycle shape: an aim beat, three shots an interval apart, then a hop.
// The pace hurries below half health (two-phase pacing).
static const int WIZARD_VOLLEY = 3;
static const float WIZARD_AIM = 0.45f;
static const float WIZARD_SHOT_INTERVAL = 0.35f;
static const float WIZARD_HURRIED_AIM = 0.22f;
static const float WIZARD_HURRIED_INTERVAL = 0.22f;

static void wizardBehavior(Enemy& self, const Room& room, Vector2 heroCenter,
                          Rng& rng, float dt)
{
    const RoomConfig& cfg = room.config();
    float t = self.modeElapsed();

    // Phase pacing: below half health the wizard hurries.
    bool hurried = (self.maxHealth() > 0
                    && self.health() * 2 <= self.maxHealth());
    float aim = hurried ? WIZARD_HURRIED_AIM : WIZARD_AIM;
    float interval = hurried ? WIZARD_HURRIED_INTERVAL : WIZARD_SHOT_INTERVAL;

    // Anchors: quarter points of the arena (tiles), clamped on the map —
    // small arenas still fight (firing needs no anchors; hopping just
    // clumps up). One picked per hop with the engine Rng (deterministic).
    int w = cfg.mapWidth;
    int h = cfg.mapHeight;
    if (w < 1 || h < 1) return;  // degenerate room data only
    const int anchors[5][2] = {
        { w / 4, h / 4 },
        { w - 1 - w / 4, h / 4 },
        { w / 2, h / 2 },
        { w / 4, h - 1 - h / 4 },
        { w - 1 - w / 4, h - 1 - h / 4 },
    };

    // Hover beat: keep facing the hero between shots.
    Vector2 c = self.center();
    self.faceVector(heroCenter.x - c.x, heroCenter.y - c.y);

    switch (self.modeState())
    {
    case Enemy::ENEMY_IDLE:  // hovering, aiming the volley
        if (t > aim)
        {
            self.setAiTimer(0.0f);  // volley shot count
            self.setMode(Enemy::ENEMY_WALK);
        }
        break;

    case Enemy::ENEMY_WALK:  // firing: one shot per interval, then hop
        if (t >= interval)
        {
            float dx = heroCenter.x - c.x;
            float dy = heroCenter.y - c.y;
            float len = sqrtf(dx * dx + dy * dy);
            if (len > 0.0001f) self.queueShot(dx / len, dy / len);

            int fired = (int)self.aiTimer() + 1;
            if (fired >= WIZARD_VOLLEY)
            {
                self.setMode(Enemy::ENEMY_PAUSE);
            }
            else
            {
                self.setAiTimer((float)fired);
                self.setMode(Enemy::ENEMY_WALK);  // restart the interval
            }
        }
        break;

    case Enemy::ENEMY_PAUSE:  // volley spent (or a hit's stun): teleport
    {                          // once the hold elapses — never mid-flinch
        // The engine routes hit recovery through PAUSE with a 0.4s
        // timer; honoring it here is what keeps the stun visible.
        if (t < 0.4f) break;
        // Hop to the first free anchor from a random start (checked
        // against the room's solids — a buried boss box would wall the
        // fight in); boxed in on every anchor, hold position instead.
        Rectangle b = self.box();
        int start = rng.range(5);
        for (int attempt = 0; attempt < 5; attempt++)
        {
            int next = (start + attempt) % 5;
            float px = (float)(anchors[next][0] * TILE_SIZE);
            float py = (float)(anchors[next][1] * TILE_SIZE);
            Rectangle cand = { px, py, b.width, b.height };
            if (room.solidBox(cand)) continue;
            self.setPosition(px, py);
            break;
        }
        self.setMode(Enemy::ENEMY_IDLE);
        break;
    }

    default:
        self.setMode(Enemy::ENEMY_IDLE);
        break;
    }
}

// Bake-time-linked registration (the gamecode seam).
static const bool wizardRegistered =
    (gamecodeRegisterEnemyBehavior("wizard", wizardBehavior), true);
