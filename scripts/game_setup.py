#!/usr/bin/env python3
"""Game setup for ENOKI AND THE HOLLOW KING: game identity, characters,
enemy roster, the sword item, and the old demo content cleanup.

Run after scripts/game_art.py (the character/enemy sheets it recolors and
copies must exist before their old names go away).
"""

import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(ROOT, "resources")


def write_json(path, data):
    with open(path, "w") as handle:
        handle.write(json.dumps(data, indent=4))
        handle.write("\n")
    print("wrote %s" % os.path.relpath(path, ROOT))


def delete(path):
    if os.path.exists(path):
        os.remove(path)
        print("removed %s" % os.path.relpath(path, ROOT))


# --------------------------------------------------------------------------
# Game identity: title, save slug, font, boot intro, and the boot cards -
# the company logo and the title card that holds the save menu (all
# editor-configurable through resources/game.json).

write_json(os.path.join(RES, "game.json"), {
    "title": "ENOKI AND THE HOLLOW KING",
    "slug": "enoki",
    "font": "fonts/dialogue.otf",
    "intro": "intro",
    "logo": {
        "image": "company_logo.png",
        "text": "WILLOWDOWN GAMES",
        "sfx": "sword shine 2.wav",
        "seconds": 2.4,
    },
    "title_card": {
        "image": "title_card.png",
        "music": "zel1laby.xm",
        "seconds": 2.4,
    },
})

# --------------------------------------------------------------------------
# Characters. Enoki keeps the hero sprite (and its swing-arc sheet); Aaron
# keeps the villager sprite. The swim channel now stays inside the sheet
# (the old demo referenced frames 21..47 on a 16-frame sheet, which the
# engine clamps to the last frame — a blank swimming hero).

VILLAGER_FRAMES = {
    "stand": {
        "down": [0], "right": [3], "left": [6], "up": [9],
    },
    "move": {
        "down": [0, 1, 0, 2],
        "right": [3, 4, 3, 5],
        "left": [6, 7, 6, 8],
        "up": [9, 10, 9, 11],
    },
}

CHARACTERS = {
    "enoki": {
        "width": 1,
        "stand": {"down": [0], "right": [3], "left": [6], "up": [9]},
        "move": {
            "down": [0, 1, 0, 2],
            "right": [3, 4, 3, 5],
            "left": [6, 7, 6, 8],
            "up": [9, 10, 9, 11],
        },
        "attack": {
            "right": [5, 4], "down": [1, 0], "up": [12, 13], "left": [9, 8],
        },
        "lift": {
            "right": [4, 5], "down": [12, 13], "left": [8, 9], "up": [12, 13],
        },
        # The stroke-glide animation, treading-water bob style: the swim
        # state animates these alternations while the engine bobs the hero.
        "swim": {
            "down": [0, 1], "right": [3, 4], "left": [6, 7], "up": [9, 10],
        },
    },
    "aaron": dict(VILLAGER_FRAMES),
    "elder": dict(VILLAGER_FRAMES),
    "mara": dict(VILLAGER_FRAMES),
    "fen": dict(VILLAGER_FRAMES),
    "keeper": dict(VILLAGER_FRAMES),
}

for name, frames in CHARACTERS.items():
    write_json(os.path.join(RES, "characters", name + ".json"), frames)

# --------------------------------------------------------------------------
# Enemies: the roster renames to the story's cast (the engine names bosses
# in the arena letterbox from these), plus the Hollow soldier recolor.

ENEMIES = {
    # Ash Knight: the crypt's phase-charging gatekeeper (was boss_knight)
    "ash_knight": {
        "width": 4,
        "color": [180, 50, 50],
        "stand": {"down": [0], "right": [1], "left": [2], "up": [3]},
        "move": {"down": [0, 4], "right": [1, 5], "left": [2, 6], "up": [3, 7]},
        "stats": {
            "hp": 12, "speed": 55, "damage": 3,
            "behavior": "boss", "sight": 260,
        },
    },
    # The Hollow King: strike-immune reflect fight (was boss_wizard)
    "hollow_king": {
        "width": 4,
        "color": [150, 90, 200],
        "stand": {"down": [0], "right": [1], "left": [2], "up": [3]},
        "move": {"down": [0, 4], "right": [1, 5], "left": [2, 6], "up": [3, 7]},
        "stats": {
            "hp": 8, "speed": 0, "damage": 2,
            "behavior": "custom:wizard",
            "components": {"boss": True, "strike_immune": True},
        },
    },
    # Moldorm: the tail-weak-point worm (was boss_moldorm)
    "moldorm": {
        "width": 4,
        "color": [168, 132, 96],
        "stand": {"down": [0], "right": [1], "left": [2], "up": [3]},
        "move": {"down": [0, 4], "right": [1, 5], "left": [2, 6], "up": [3, 7]},
        "stats": {
            "hp": 6, "speed": 80, "damage": 2,
            "behavior": "custom:moldorm",
            "components": {"boss": True},
            "drops": {},
        },
    },
    # The Crypt Captain: mini-boss guarding the barracks (unchanged)
    "captain": {
        "width": 4,
        "color": [148, 60, 180],
        "stand": {"down": [0], "right": [1], "left": [2], "up": [3]},
        "move": {"down": [0, 4], "right": [1, 5], "left": [2, 6], "up": [3, 7]},
        "stats": {
            "hp": 8, "speed": 46, "damage": 2,
            "behavior": "soldier", "sight": 220,
            "mini_boss": True,
            "drops": {"heart": 40, "rupee5": 20, "magic": 20},
        },
    },
    "soldier": {
        "width": 4,
        "color": [212, 64, 64],
        "stand": {"down": [0], "right": [1], "left": [2], "up": [3]},
        "move": {"down": [0, 4], "right": [1, 5], "left": [2, 6], "up": [3, 7]},
        "stats": {"hp": 2, "speed": 40, "damage": 1,
                  "behavior": "soldier", "sight": 150},
    },
    "soldier_blue": {
        "width": 4,
        "color": [64, 96, 224],
        "stand": {"down": [0], "right": [1], "left": [2], "up": [3]},
        "move": {"down": [0, 4], "right": [1, 5], "left": [2, 6], "up": [3, 7]},
        "stats": {"hp": 3, "speed": 60, "damage": 2,
                  "behavior": "soldier", "sight": 200},
    },
    # Hollow soldiers: the dark-world patrol (game_art recolors the sheet)
    "hollow_soldier": {
        "width": 4,
        "color": [110, 88, 140],
        "stand": {"down": [0], "right": [1], "left": [2], "up": [3]},
        "move": {"down": [0, 4], "right": [1, 5], "left": [2, 6], "up": [3, 7]},
        "stats": {"hp": 3, "speed": 52, "damage": 2,
                  "behavior": "soldier", "sight": 180},
    },
    "octorok": {
        "width": 4,
        "color": [224, 140, 48],
        "stand": {"down": [0], "right": [1], "left": [2], "up": [3]},
        "move": {"down": [0, 4], "right": [1, 5], "left": [2, 6], "up": [3, 7]},
        "stats": {"hp": 1, "speed": 30, "damage": 1,
                  "behavior": "octorok", "sight": 0, "shoot_period": 2},
    },
    "bee": {
        "width": 4,
        "color": [232, 200, 64],
        "stand": {"down": [0], "right": [1], "left": [2], "up": [3]},
        "move": {"down": [0, 4], "right": [1, 5], "left": [2, 6], "up": [3, 7]},
        "stats": {
            "hp": 1, "speed": 46, "damage": 0, "behavior": "",
            "components": {"mover": {"walk_time": 0.8, "turn_chance": True},
                           "catchable": True},
            "catchable": True,
            "drops": {},
        },
    },
    "zora": {
        "width": 4,
        "color": [64, 148, 180],
        "stand": {"down": [0], "right": [1], "left": [2], "up": [3]},
        # The move row only: surfacing shows the body, never the ripple.
        "move": {"down": [4], "right": [5], "left": [6], "up": [7]},
        "stats": {"hp": 2, "speed": 0, "damage": 1,
                  "behavior": "custom:zora"},
    },
}

for name, spec in ENEMIES.items():
    write_json(os.path.join(RES, "enemies", name + ".json"), spec)

# The old demo names (sheets move with their jsons).
for old in ("boss_knight", "boss_wizard", "boss_moldorm"):
    delete(os.path.join(RES, "enemies", old + ".json"))
    delete(os.path.join(RES, "enemies", old + ".png"))

# --------------------------------------------------------------------------
# Items: the sword joins the roster as the sword-button unlock (a game
# that ships a sword item gates swings behind owning one; the engine's
# swordUnlocked keeps the native blade for games without).

write_json(os.path.join(RES, "items", "sword.json"), {
    "label": "RUSTY SWORD",
    "passive": {"sword": True},
})

# --------------------------------------------------------------------------
# Old demo content: rooms and dialogues are rewritten by the compiler and
# dialogue scripts; the stale save files belong to the old slug.

for old_room in ("01", "02", "03", "04", "05", "06", "07", "gauntlet"):
    delete(os.path.join(RES, "rooms", old_room + ".json"))
for old_dialogue in range(1, 12):
    delete(os.path.join(RES, "dialogue", "%02d.json" % old_dialogue))
delete(os.path.join(RES, "scenes", "keeper.png"))

for old_save in ("save0.dat", "save1.dat"):
    delete(os.path.join(ROOT, old_save))

# The old hero/villager names move into the new character sheets.
delete(os.path.join(RES, "characters", "link.json"))
delete(os.path.join(RES, "characters", "villager.json"))
delete(os.path.join(RES, "characters", "link.png"))
delete(os.path.join(RES, "characters", "villager.png"))

print("game setup complete")