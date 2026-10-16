#!/usr/bin/env python3
"""Room compiler for ENOKI AND THE HOLLOW KING: authors every room as a
painted tile canvas (one char per 32px tile) plus placed entities, and
writes resources/rooms/*.json in the format the asset generator bakes.

Tile chars resolve through scripts/game_art.py's TILE_ID table (the same
shared legend that drew the tileset), never by number. Solid tiles also
emit a collider id; canopy/roof tiles emit into the upper decor layer.

Entities are placed as dicts ("enemy"/"npc"/"object"/"chest"/"pickup"),
never as grid chars, so they can't collide with tiles. The compiler
validates: entities never on solid tiles or pits, exit reciprocity,
matching door kinds on both edges, warp/stair/pit/mirror targets landing
on walkable tiles, and per-room pool budgets (16 enemies, 16 objects,
8 npcs, 32 pickups).
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from game_art import TILE_ID  # noqa: E402  (the shared tile legend)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(ROOT, "resources")
OUT = os.path.join(RES, "rooms")

# Tile chars -> tile names (game_art owns the id layout).
TILE_CHARS = {
    ".": "grass_a",   ",": "grass_b",  "`": "grass_c",  "f": "flowers",
    '"': "tuft",      "_": "path",     ":": "path_b",
    "=": "sand_a",    "~": "sand_b",
    "w": "deep_a",    "x": "shallow_a", "h": "heal_a",
    "I": "ice",       "O": "pit",       "*": "spikes",   "d": "soil",
    "J": "ledge_d",   "K": "ledge_u",   "L": "ledge_l",  "M": "ledge_r",
    ">": "curb_r",    "<": "curb_l",    "v": "curb_d",   "^": "curb_u",
    "C": "cliff",     "G": "cliff_face", "R": "boulder", "#": "bombrock",
    "T": "tree",      "n": "canopy",    "B": "hedge",    "F": "fence",
    "H": "wall_house", "[": "roof_a",  "]": "roof_b",
    "q": "d_floor",   "e": "d_floor_b", "'": "d_floor_c", "y": "d_rubble",
    "W": "d_wall",    "X": "d_wall2",   "P": "d_pillar",  "c": "d_crack",
    "o": "d_pit",     "s": "d_spikes",
    "m": "hollow_a",  ";": "hollow_b",
}
SOLID_TILES = {"cliff", "cliff_face", "boulder", "bombrock", "tree", "hedge",
               "fence", "wall_house", "d_wall", "d_wall2", "d_pillar",
               "d_crack"}
UPPER_TILES = {"canopy", "roof_a", "roof_b"}

WALK_BLOCKING = SOLID_TILES | {"deep_a", "d_pit", "pit"}  # for entity checks

GERUDO = "gerudo.xm"
LAMENT = "zel1laby.xm"
DANGER = "anak-nes6.xm"


# --------------------------------------------------------------------------
# Canvas: paint API. Every room builds one; the compiler bakes it out.

class G:
    def __init__(self, width, height, base="."):
        self.w = width
        self.h = height
        self.base = base
        self.rows = [[base] * width for _ in range(height)]
        self.entities = []          # [(x, y, spec)]

    def put(self, x, y, text):
        """A horizontal run of chars starting at (x, y)."""
        assert 0 <= y < self.h, "y out of range"
        assert 0 <= x < self.w, "x out of range"
        for i, ch in enumerate(text):
            assert x + i < self.w, "put runs off the right edge: %r" % text
            self.rows[y][x + i] = ch
        return self

    def rect(self, x0, y0, x1, y1, ch):
        """An inclusive filled rect."""
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.rows[y][x] = ch
        return self

    def border(self, ch):
        self.rect(0, 0, self.w - 1, 0, ch)
        self.rect(0, self.h - 1, self.w - 1, self.h - 1, ch)
        self.rect(0, 0, 0, self.h - 1, ch)
        self.rect(self.w - 1, 0, self.w - 1, self.h - 1, ch)
        return self

    def gate(self, edge, at0, at1, ch):
        """Punch a gap in the border: edge N/S -> x range, E/W -> y range."""
        if edge == "north":
            for x in range(at0, at1 + 1):
                self.rows[0][x] = ch
        elif edge == "south":
            for x in range(at0, at1 + 1):
                self.rows[self.h - 1][x] = ch
        elif edge == "west":
            for y in range(at0, at1 + 1):
                self.rows[y][0] = ch
        elif edge == "east":
            for y in range(at0, at1 + 1):
                self.rows[y][self.w - 1] = ch
        return self

    def tree(self, x, y):
        """A tree collider with its canopy one row up (walk-behind)."""
        self.put(x, y, "T")
        assert y - 1 >= 0
        self.put(x, y - 1, "n")
        return self

    def house(self, x0, y0, w_tiles, door=None):
        """A house: two roof rows (upper layer) above a wall row with an
        optional door char (usually a stair/warp entity marker)."""
        self.rect(x0, y0, x0 + w_tiles - 1, y0 + 1, "[")
        for x in range(x0, x0 + w_tiles, 2):
            self.put(x, y0 + 1, "]")
        wall = ["H"] * w_tiles
        if door is not None:
            wall[door] = "_"
        self.put(x0, y0 + 2, "".join(wall))
        return self

    def ent(self, x, y, spec):
        assert 0 <= x < self.w and 0 <= y < self.h, "entity out of bounds"
        self.entities.append((x, y, spec))
        return self

    # Convenience placers -----------------------------------------------
    def enemy(self, name, x, y):
        return self.ent(x, y, {"enemy": name})

    def chest(self, key, x, y):
        return self.ent(x, y, {"chest": key})

    def npc(self, name, dialogue, x, y, wander=False, path=None,
            iflag=None, unless=None):
        spec = {"npc": name, "dialogue": dialogue, "wander": wander}
        if path:
            spec["path"] = path
        if iflag:
            spec["if"] = iflag
        if unless:
            spec["unless"] = unless
        return self.ent(x, y, spec)

    def obj(self, kind, x, y, **extra):
        spec = {"object": kind}
        spec.update(extra)
        return self.ent(x, y, spec)

    def sign(self, dialogue, x, y):
        return self.ent(x, y, {"object": "sign", "dialogue": dialogue})

    def stair(self, room, tx, ty, x, y):
        return self.ent(x, y, {"object": "stair",
                               "warp": {"room": room, "tile": [tx, ty]}})

    def warp(self, room, tx, ty, x, y, kind="warp"):
        return self.ent(x, y, {"object": kind,
                               "warp": {"room": room, "tile": [tx, ty]}})

    def pickup(self, kind, x, y):
        return self.ent(x, y, {"pickup": kind})

    def spawn(self, x, y):
        return self.ent(x, y, {"spawn": True})

    def tile(self, x, y):
        return self.rows[y][x]


# --------------------------------------------------------------------------
# Chest payloads.

CHESTS = {
    # --- the critical path ---
    "sword":         {"kind": "normal", "grant": {"kind": "item", "item": "sword"}},
    "bombs_bag":     {"kind": "normal", "grant": {"kind": "item", "item": "bombs"}},
    "bombs_bundle":  {"kind": "normal", "grant": {"kind": "bombs", "amount": 10}},
    "magic_meter":   {"kind": "normal", "grant": {"kind": "magic", "amount": 8}},
    "lantern":       {"kind": "normal", "grant": {"kind": "item", "item": "lantern"}},
    "net":           {"kind": "normal", "grant": {"kind": "item", "item": "net"}},
    "shovel":        {"kind": "normal", "grant": {"kind": "item", "item": "shovel"}},
    "flippers":      {"kind": "normal", "grant": {"kind": "item", "item": "flippers"}},
    "small_key":     {"kind": "normal", "grant": {"kind": "key"}},
    "big_key":       {"kind": "normal", "grant": {"kind": "big_key"}},
    "bow":           {"kind": "locked", "grant": {"kind": "item", "item": "bow"}},
    "arrows":        {"kind": "normal", "grant": {"kind": "arrows", "amount": 10}},
    "compass":       {"kind": "normal", "grant": {"kind": "item", "item": "compass"}},
    "hammer":        {"kind": "normal", "grant": {"kind": "item", "item": "hammer"}},
    "hookshot":      {"kind": "normal", "grant": {"kind": "item", "item": "hookshot"}},
    "somaria":       {"kind": "normal", "grant": {"kind": "item", "item": "somaria"}},
    "powder":        {"kind": "normal", "grant": {"kind": "item", "item": "powder"}},
    "fire_rod":      {"kind": "normal", "grant": {"kind": "item", "item": "fire_rod"}},
    "flute":         {"kind": "normal", "grant": {"kind": "item", "item": "flute"}},
    "ice_rod":       {"kind": "normal", "grant": {"kind": "item", "item": "ice_rod"}},
    "silver_arrows": {"kind": "normal", "grant": {"kind": "flag", "flag": "silver_arrows"}},
    "magic_boomerang": {"kind": "normal", "grant": {"kind": "flag", "flag": "magic_boomerang"}},
    # --- bottles and hearts ---
    "bottle":        {"kind": "normal", "grant": {"kind": "item", "item": "bottle"}},
    "potion_green":  {"kind": "normal", "grant": {"kind": "potion_green"}},
    "potion_red":    {"kind": "normal", "grant": {"kind": "potion_red"}},
    "heart_vessel":  {"kind": "normal", "grant": {"kind": "heart_container"}},
    # --- the Hollow World ---
    "cape":          {"kind": "normal", "grant": {"kind": "item", "item": "cape"}},
    "gloves":        {"kind": "normal", "grant": {"kind": "item", "item": "gloves"}},
    "quake":         {"kind": "normal", "grant": {"kind": "item", "item": "quake"}},
    "bombos":        {"kind": "normal", "grant": {"kind": "item", "item": "bombos"}},
    "ether":         {"kind": "normal", "grant": {"kind": "item", "item": "ether"}},
    "byrna":         {"kind": "normal", "grant": {"kind": "item", "item": "byrna"}},
    # --- treasure ---
    "rupees_50":     {"kind": "normal", "grant": {"kind": "rupees", "amount": 50}},
    "rupees_20":     {"kind": "normal", "grant": {"kind": "rupees", "amount": 20}},
}


# --------------------------------------------------------------------------
# Rooms.

def village():
    g = G(26, 16, ".")
    g.border("F")
    g.gate("north", 12, 13, "_")           # farm gate north (push-open door)
    g.gate("south", 12, 13, "_")           # shore path south
    g.gate("east", 10, 10, "_")            # grove path east
    # paths crossing at the square
    g.put(12, 1, "__"); g.put(12, 2, "__"); g.put(12, 3, "__")
    g.rect(12, 4, 13, 12, "_")
    g.put(14, 10, "___________")          # east path (x14..24) to the gate
    g.put(12, 14, "__")
    # Elder Rowan's house (northwest) with a door warp inside
    g.house(3, 1, 5, door=2)
    g.warp("06_elder_house", 5, 6, 5, 3, kind="door_warp")  # the door
    g.tree(21, 13)
    # Mara's stall (market square north): posts and the merchant behind
    # them, clear of the north gate's path columns (x12-13)
    g.put(8, 2, "F.F")
    g.sign("sign_village.json", 15, 2)
    g.npc("mara", "mara.json", 9, 2)
    # Aaron's cottage (northeast, no door) and his garden (bushes, soil)
    g.house(18, 1, 5)
    g.tree(24, 3)
    g.rect(20, 4, 21, 5, "d")              # tilled soil
    g.obj("bush", 22, 4)
    g.obj("bush", 19, 4)
    g.chest("bottle", 22, 5)               # Aaron's garden gift
    # village green: pots, flowers, tufts, a wandering kid and Aaron
    g.obj("pot", 3, 4)
    g.obj("pot", 4, 4)
    g.obj("pot", 17, 4)
    g.obj("pot", 17, 5)
    g.put(2, 6, ",f`"); g.put(6, 6, "f")
    g.put(18, 7, "f"); g.put(2, 10, "`f,")
    g.put(6, 9, "f"); g.put(9, 12, ",f")
    g.put(15, 6, "f"); g.put(22, 14, "f")
    g.put(4, 12, "f`"); g.put(8, 14, "`f")
    g.npc("fen", "fen.json", 15, 12, wander=True)
    g.npc("aaron", "aaron.json", 9, 11, wander=True)
    g.sign("sign_controls.json", 11, 9)
    g.pickup("heart", 3, 13)
    g.pickup("rupee", 22, 12)
    g.tree(2, 13); g.tree(23, 9)
    g.spawn(12, 10)
    # defaults + edges: north gate is a push-open door; east is open
    return g, dict(
        default={"hero": "enoki", "spawn": [12, 10], "music": GERUDO,
                 "walk_sfx": "walk_grass.wav", "ambient": "rain.wav"},
        exits={"north": "02_meadow", "east": "03_grove",
               "south": "04_shore"},
        doors={"north": {"kind": "closed"}, "east": {"kind": "open"}},
        weather="rain",
        encounters={"instant": [
            {"location": [12, 1], "dialogue": "beat_village.json",
             "repeat": False},
        ]},
    )


def elder_house():
    g = G(13, 9, "q")
    g.border("W")
    # rug + pillar decor
    g.rect(2, 3, 5, 5, "e")
    g.put(10, 3, "P"); g.put(10, 5, "P")
    g.rect(5, 1, 7, 1, "'")
    g.put(2, 7, "yy"); g.put(10, 7, "yy")
    # torches by the door (they catch fire as the hero passes)
    g.obj("torch", 3, 2, link=0)
    g.obj("torch", 9, 2, link=0)
    g.npc("elder", "elder.json", 6, 3)
    g.chest("sword", 9, 4)
    g.obj("pot", 2, 4)
    g.obj("pot", 2, 5)
    g.warp("01_village", 5, 4, 6, 7, kind="door_warp")  # back out the door
    g.spawn(6, 6)
    return g, dict(
        default={"hero": "enoki", "spawn": [6, 6], "music": GERUDO,
                 "walk_sfx": "walk_grass.wav"},
        encounters={},
    )


def meadow():
    g = G(28, 18, ".")
    g.border("C")
    g.gate("north", 13, 14, "_")           # to the ridge
    g.gate("south", 13, 14, "_")           # to the village
    g.rect(13, 1, 14, 16, "_")             # the north-south path
    g.put(10, 8, "___")
    g.put(15, 8, "____")                   # paths off to the sides
    # grass variety + cuttable tufts (drop rolls live here)
    for y in range(1, 17):
        for x in range(1, 27):
            if (x * 7 + y * 11) % 23 == 0:
                g.put(x, y, '"')
            elif (x * 5 + y * 13) % 19 == 0:
                g.put(x, y, ",")
            elif (x * 3 + y * 17) % 29 == 0:
                g.put(x, y, "f")
    g.rect(13, 1, 14, 16, "_")             # re-cut the path over the grass
    g.put(10, 8, "___"); g.put(15, 8, "____")
    # bomb-rock alcove west of the path: a chest pocket behind a cracked wall
    g.rect(3, 6, 8, 10, "C")
    g.rect(4, 7, 7, 9, ".")
    g.put(4, 7, ","); g.put(7, 9, ",")
    g.put(8, 7, "#")                        # the cracked bombable wall
    g.put(8, 8, "#")
    g.chest("magic_meter", 4, 8)            # the magic meter
    g.chest("rupees_20", 5, 8)
    # heavy cave mouth in the northeast cliffs (gloves move the rock)
    g.rect(22, 1, 27, 3, "C")
    g.rect(23, 1, 26, 2, ".")
    g.put(24, 3, ".")
    g.obj("big_rock", 24, 3)
    g.warp("07_cave_byrna", 6, 6, 24, 1)
    # fairy spring pond (southeast): heal water behind bushes
    g.rect(20, 11, 22, 13, "x")
    g.put(20, 11, "xxx")
    g.rect(21, 12, 21, 12, "h")
    g.obj("bush", 19, 11)
    g.obj("bush", 23, 13)
    g.pickup("fairy", 22, 12)
    g.tree(2, 3); g.tree(2, 12); g.tree(18, 2)
    g.put(5, 12, "R"); g.put(9, 4, "R")
    g.obj("pot", 11, 5)
    g.obj("bush", 11, 10)
    g.enemy("soldier", 12, 11)
    g.enemy("soldier", 17, 12)
    g.enemy("octorok", 7, 13)
    g.enemy("octorok", 21, 6)
    g.chest("bombs_bag", 12, 15)
    g.chest("bombs_bundle", 14, 15)
    g.sign("sign_meadow.json", 16, 16)
    # Soft soil: buried treasure hides under it (the shovel turns it up)
    g.put(10, 10, "dd"); g.put(8, 14, "dd"); g.put(18, 14, "dd")
    g.sign("sign_soil.json", 11, 9)      # teaches the dig, by the soil
    g.buried = [("rupee5", 10, 10), ("heart", 8, 14),
                ("key", 18, 14)]
    g.spawn(13, 16)
    return g, dict(
        default={"hero": "enoki", "spawn": [13, 16], "music": GERUDO,
                 "walk_sfx": "walk_grass.wav", "ambient": "rain.wav"},
        exits={"south": "01_village", "north": "05_ridge"},
        doors={"south": {"kind": "closed"}},
        weather="rain",
        encounters={"instant": [
            {"location": [13, 16], "dialogue": "beat_meadow.json",
             "repeat": False},
        ]},
    )


def cave_byrna():
    g = G(13, 9, "q")
    g.border("W")
    g.dark = True
    g.rect(3, 2, 9, 6, "q")
    g.put(2, 3, "y"); g.put(2, 6, "yy")
    g.put(10, 2, "yy")
    g.obj("torch", 2, 2, link=0)
    g.obj("torch", 10, 7, link=0)
    g.chest("byrna", 9, 3)
    g.chest("rupees_50", 9, 5)
    g.obj("pot", 4, 3)
    g.enemy("octorok", 5, 5)
    g.warp("02_meadow", 24, 4, 6, 7)
    g.spawn(6, 6)
    return g, dict(
        default={"hero": "enoki", "spawn": [6, 6], "music": LAMENT,
                 "walk_sfx": "walk_grass.wav"},
        dark=True,
        encounters={},
    )


def grove():
    g = G(28, 18, ".")
    g.border("C")
    g.gate("west", 9, 10, "_")            # back to the village
    g.rect(1, 9, 9, 10, "_")               # path in
    g.put(9, 5, ":__:")                    # path stub east
    # tree rows with walk-behind canopies
    g.tree(2, 3); g.tree(6, 2); g.tree(10, 3)
    g.tree(14, 2); g.tree(18, 3); g.tree(22, 2)
    g.tree(2, 8); g.tree(5, 12); g.tree(9, 9)
    g.tree(13, 13); g.tree(17, 8); g.tree(21, 12)
    g.tree(25, 3); g.tree(24, 8); g.tree(3, 15)
    g.tree(20, 15); g.tree(26, 13)
    g.put(8, 6, "B"); g.put(12, 6, "B")    # hedges
    # pond southeast: deep water with a shallow rim, fairies flit above
    g.rect(20, 8, 24, 11, "w")
    g.rect(19, 9, 19, 10, "x")
    g.rect(25, 9, 25, 10, "x")
    g.pickup("fairy", 22, 9)
    g.pickup("heart", 16, 16)
    # the shrine circle (north): the sleeping keeper statue
    g.rect(12, 3, 14, 3, ":")
    g.obj("shrine", 13, 3, flag="keeper_awake",
          dialogue="keeper_wakes.json")   # the awakening plays its moment
    g.sign("sign_shrine.json", 12, 4)
    g.npc("keeper", "keeper.json", 10, 5, iflag="keeper_awake")
    # the legend signpost near the path (a storybook card)
    g.sign("sign_legend.json", 8, 11)
    # tools tucked around the grove
    g.chest("lantern", 5, 14)
    g.chest("net", 25, 15)
    g.enemy("bee", 10, 7)
    g.enemy("bee", 18, 10)
    g.enemy("octorok", 22, 6)
    g.obj("bush", 7, 13); g.obj("bush", 15, 9)
    g.obj("pot", 3, 11); g.obj("pot", 4, 11)
    g.put(5, 10, "dd"); g.put(16, 5, "dd")
    g.buried = [("rupee5", 5, 10), ("heart", 16, 5)]
    g.pickup("heart", 12, 12)
    g.pickup("rupee", 24, 2)
    g.spawn(2, 10)
    return g, dict(
        default={"hero": "enoki", "spawn": [2, 10], "music": LAMENT,
                 "walk_sfx": "walk_grass.wav", "ambient": "birds.wav"},
        exits={"west": "01_village"},
        doors={"west": {"kind": "open"}},
        weather="fog",
        mirror="08_hollow_grove",
        encounters={"instant": [
            {"location": [6, 10], "dialogue": "beat_grove.json", "repeat": False},
        ]},
    )


def hollow_grove():
    # The Hollow: the grove's shadow pair. The Keeper's glass steps here.
    g = G(28, 18, "m")
    g.border("C")
    # no edge exits: mirror-only access (declared on the grove side too)
    # sickly grass + hollow trees
    for y in range(1, 17):
        for x in range(1, 27):
            if (x * 7 + y * 11) % 19 == 0:
                g.put(x, y, ";")
    g.tree(2, 3); g.tree(6, 2); g.tree(10, 3)
    g.tree(14, 2); g.tree(18, 3); g.tree(22, 2)
    g.tree(2, 8); g.tree(12, 11); g.tree(13, 13)
    g.tree(17, 8); g.tree(21, 12); g.tree(25, 3)
    g.tree(24, 8); g.tree(3, 15); g.tree(20, 15)
    # the shrine circle's shadow: quiet stone
    g.rect(12, 3, 14, 3, ":")
    g.obj("rock", 13, 4)
    g.put(11, 2, "R"); g.put(15, 2, "R")
    # the hollow pond: cold, deep, no fairies
    g.rect(20, 8, 24, 11, "w")
    g.rect(19, 9, 19, 10, "x")
    g.rect(25, 9, 25, 10, "x")
    # the lost gear, guarded
    g.chest("cape", 5, 14)
    g.chest("gloves", 25, 15)
    g.chest("quake", 16, 16)
    g.chest("heart_vessel", 26, 2)
    g.enemy("hollow_soldier", 8, 10)
    g.enemy("hollow_soldier", 14, 9)
    g.enemy("hollow_soldier", 20, 5)
    g.enemy("soldier_blue", 10, 14)
    g.enemy("octorok", 22, 6)
    g.obj("pot", 4, 11)
    g.obj("bush", 7, 13)
    g.pickup("heart", 12, 12)
    g.spawn(13, 6)
    return g, dict(
        default={"hero": "enoki", "spawn": [13, 6], "music": LAMENT,
                 "walk_sfx": "walk_grass.wav"},
        weather="fog",
        tint=[208, 200, 238],
        mirror="03_grove",
        encounters={"instant": [
            # The Hollow arrival beat: the mirror can land the hero
            # anywhere the grove paired, so likely spots share the
            # dialogue and the flag gates it — the beat plays once,
            # wherever it's first met.
            {"location": [13, 6], "dialogue": "beat_hollow.json",
             "unless": "hollow_seen", "repeat": False},
            {"location": [12, 5], "dialogue": "beat_hollow.json",
             "unless": "hollow_seen", "repeat": False},
            {"location": [11, 10], "dialogue": "beat_hollow.json",
             "unless": "hollow_seen", "repeat": False},
        ]},
    )


def shore():
    g = G(26, 16, ".")
    g.border("C")
    g.gate("north", 12, 13, "_")           # village path
    g.rect(12, 0, 13, 8, "_")               # the path down to the sand
    g.rect(1, 7, 24, 8, "=")
    g.rect(1, 9, 24, 9, "~")
    # the sea (south): deep water
    g.rect(1, 10, 24, 14, "w")
    # the longshore current pushes east along the shallows
    g.rect(1, 9, 24, 9, "=")
    for x in range(1, 25):
        if x % 3:
            g.put(x, 9, ">")                # curb tiles: push +48 px/s east
    g.put(1, 9, ">"); g.put(24, 9, ">")
    # island A (west): the raft ride lands here - flippers chest
    g.rect(3, 11, 5, 12, "=")
    g.chest("flippers", 4, 11)
    # island B (far east): bombos medallion, reached swimming or by hook
    g.rect(21, 12, 23, 13, "=")
    g.chest("bombos", 22, 12)
    g.obj("hook_post", 20, 9)
    g.obj("hook_post", 8, 9)
    # the raft rail: platform carrying riders out to island A
    g.obj("platform", 2, 10, path=[[2, 10], [3, 10], [4, 10], [4, 11]])
    # boulders and grass tufts on the dunes
    g.put(6, 4, "R"); g.put(18, 5, "R"); g.put(12, 6, "R")
    g.put(3, 3, "`f"); g.put(20, 3, "f`"); g.put(8, 2, ",f`")
    g.put(15, 5, "f,"); g.put(22, 2, "`f")
    # zora lurk in the deep water; octoroks patrol the dunes
    g.enemy("zora", 9, 12)
    g.enemy("zora", 16, 13)
    g.enemy("octorok", 10, 7)
    g.enemy("octorok", 19, 7)
    g.enemy("octorok", 14, 3)
    g.sign("sign_shore.json", 15, 8)
    g.pickup("heart", 7, 6)
    g.pickup("rupee", 21, 6)
    g.put(5, 5, "dd"); g.put(17, 6, "dd"); g.put(9, 4, "dd")
    g.buried = [("rupee", 5, 5), ("rupee5", 17, 6), ("heart", 9, 4)]
    g.spawn(13, 6)
    return g, dict(
        default={"hero": "enoki", "spawn": [13, 6], "music": GERUDO,
                 "walk_sfx": "walk_grass.wav", "ambient": "birds.wav"},
        exits={"north": "01_village"},
        encounters={"instant": [
            {"location": [13, 6], "dialogue": "beat_shore.json", "repeat": False},
        ]},
    )


def ridge():
    g = G(26, 18, ".")
    g.border("C")
    g.gate("south", 12, 13, "_")           # meadow path
    g.rect(12, 1, 13, 17, "_")
    # the storm plateau: cliff rows, ledges, spike strips
    g.rect(1, 2, 9, 3, "C")                 # north cliff band
    g.rect(1, 2, 9, 3, "C")
    g.put(2, 4, "JJ")                       # ledges hopping down east
    g.put(4, 4, "K")
    g.put(6, 4, "JJ")
    g.rect(1, 8, 7, 8, "*")                 # spike strip (with gaps)
    g.put(3, 8, "."); g.put(6, 8, ".")
    g.rect(16, 9, 22, 9, "*")
    g.put(18, 9, ".")
    # the crypt stair (east plateau) + its warning sign
    g.rect(19, 1, 24, 4, "C")
    g.rect(20, 2, 23, 3, ".")
    g.put(21, 4, ".")                       # pocket mouth (south)
    g.stair("10_crypt_hall", 11, 13, 21, 2)
    g.sign("sign_ridge.json", 19, 5)
    # rubble + boulders
    g.put(3, 12, "R"); g.put(9, 6, "R"); g.put(17, 13, "R")
    g.put(22, 8, "R")
    g.put(2, 15, "`f"); g.put(7, 16, "f`"); g.put(16, 15, ",f")
    g.put(10, 4, "f,"); g.put(19, 6, "`f")
    g.tree(2, 14); g.tree(23, 13)
    g.obj("pot", 11, 7)
    g.obj("bush", 14, 4)
    # storm patrol
    g.enemy("octorok", 6, 5)
    g.enemy("octorok", 15, 6)
    g.enemy("soldier", 9, 12)
    g.enemy("soldier", 18, 11)
    g.enemy("soldier_blue", 21, 8)
    g.enemy("bee", 4, 10)
    # the shovel: buried treasure hills
    g.chest("shovel", 5, 14)
    g.chest("rupees_20", 19, 15)
    g.put(8, 10, "dd"); g.put(15, 12, "dd"); g.put(3, 16, "dd")
    g.buried = [("rupee5", 8, 10), ("heart", 15, 12), ("rupee", 3, 16)]
    g.pickup("heart", 21, 15)
    g.pickup("rupee", 13, 9)
    g.spawn(12, 16)
    return g, dict(
        default={"hero": "enoki", "spawn": [12, 16], "music": GERUDO,
                 "walk_sfx": "walk_grass.wav", "ambient": "rain.wav"},
        exits={"south": "02_meadow"},
        weather="storm",
        encounters={"instant": [
            {"location": [12, 16], "dialogue": "beat_ridge.json", "repeat": False},
        ]},
    )


# ---- The Sunken Crypt -----------------------------------------------------

def crypt_hall():
    g = G(22, 16, "q")
    g.border("W")
    g.dark = None
    # entry stair from the ridge (back door, southwest corner)
    g.stair("05_ridge", 21, 2, 2, 13)
    # the entry hall: pots (one hides a surprise), torches, a sign
    g.obj("torch", 2, 2, link=0)
    g.obj("torch", 19, 13, link=0)
    g.sign("sign_crypt.json", 10, 13)
    g.obj("pot", 4, 10)
    g.obj("pot", 5, 10)
    g.obj("pot", 4, 12, hides="octorok")
    g.obj("pot", 17, 12)
    g.put(3, 4, "'"); g.put(18, 4, "'")
    g.put(3, 6, "'"); g.put(18, 6, "'")
    # door gaps: west wing (locked), east wing (shutter), sanctum (big)
    g.gate("north", 10, 11, "_")
    g.gate("west", 7, 8, "_")
    g.gate("east", 7, 8, "_")
    # the down-stair to the Mire (northeast corner)
    g.rect(16, 1, 21, 4, "W")
    g.put(17, 2, "_"); g.put(18, 2, "_")
    g.put(17, 3, "_")
    g.put(17, 4, "_")                      # pocket mouth (south)
    g.stair("13_crypt_pit", 10, 12, 18, 2)
    # garrison
    g.enemy("soldier", 8, 10)
    g.enemy("soldier", 14, 8)
    g.enemy("soldier_blue", 10, 5)
    g.enemy("octorok", 15, 12)
    g.pickup("heart", 7, 13)
    g.pickup("heart", 15, 3)
    g.pickup("key", 16, 7)                 # the garrison's spare key
    g.spawn(11, 13)
    return g, dict(
        default={"hero": "enoki", "spawn": [11, 13], "music": LAMENT,
                 "walk_sfx": "walk_grass.wav", "danger_music": DANGER},
        dungeon=True,
        exits={"east": "11_crypt_east", "west": "12_crypt_west",
               "north": "17_crypt_sanctum"},
        doors={"east": {"kind": "shutter"}, "west": {"kind": "locked"},
               "north": {"kind": "big"}},
        encounters={"instant": [
            {"location": [11, 13], "dialogue": "beat_crypt.json", "repeat": False},
        ],
        "interactive": [
            # The big door tease: one shot, so an opened door never
            # re-rattles.
            {"location": [10, 2], "dialogue": "peek_big_door.json",
             "repeat": False},
        ]},
    )


def crypt_east():
    g = G(20, 14, "q")
    g.border("W")
    g.gate("west", 6, 7, "_")
    # torches
    g.obj("torch", 2, 2, link=0)
    g.obj("torch", 2, 11, link=0)
    # the crystal + its blue/red barrier pair (strike the crystal to swap):
    # the south vault (bow) stands behind BLUE barriers, open while the
    # crystal reads blue; the north vault (small key) behind RED ones
    g.put(9, 7, ":")
    g.obj("crystal", 9, 7)
    # red barriers (raised while the crystal reads blue) bar the north
    # vault: the small key
    g.put(7, 3, "="); g.put(8, 3, "="); g.put(9, 3, "=")
    g.obj("barrier", 7, 3, color="red")
    g.obj("barrier", 8, 3, color="red")
    g.obj("barrier", 9, 3, color="red")
    g.chest("small_key", 8, 2)
    # ...and one strike later, the south vault lowers: the bow
    g.put(7, 11, "="); g.put(8, 11, "="); g.put(9, 11, "=")
    g.obj("barrier", 7, 11, color="blue")
    g.obj("barrier", 8, 11, color="blue")
    g.obj("barrier", 9, 11, color="blue")
    g.chest("bow", 8, 12)
    g.chest("arrows", 10, 12)
    # the frost floor (northwest): ice and spikes guard the compass
    g.rect(2, 5, 6, 8, "I")
    g.put(3, 6, "*"); g.put(5, 7, "*")
    g.chest("compass", 2, 7)
    # the cracked vault wall (east): bombs reveal the ice rod
    g.rect(16, 4, 18, 8, "W")
    g.rect(17, 5, 18, 7, "q")
    g.put(16, 5, "c"); g.put(16, 6, "c")
    g.put(17, 5, "'")
    g.chest("ice_rod", 18, 6)
    g.sign("sign_east.json", 13, 4)
    g.enemy("octorok", 13, 9)
    g.enemy("soldier_blue", 12, 6)
    g.enemy("soldier", 15, 11)
    g.pickup("heart", 5, 11)
    g.put(12, 2, "'")
    g.spawn(6, 6)
    return g, dict(
        default={"hero": "enoki", "spawn": [6, 6], "music": LAMENT,
                 "walk_sfx": "walk_grass.wav"},
        dungeon=True,
        exits={"west": "10_crypt_hall"},
        doors={"west": {"kind": "shutter"}},
        encounters={},
    )


def crypt_west():
    g = G(20, 14, "q")
    g.border("W")
    g.gate("east", 6, 7, "_")              # back to the hall (locked door)
    g.gate("north", 9, 10, "_")            # the cracked door down to the Cistern
    g.dark = True
    # torches scattered (the lantern reveals)
    g.obj("torch", 2, 6, link=0)
    g.obj("torch", 17, 2, link=0)
    # the lever toll: one pull drops every linked barrier in group 1 -
    # the hammer's nook in the northwest corner and the cache southeast
    g.put(1, 3, "X"); g.put(2, 3, "X")     # seal the hammer nook's south mouth
    g.obj("barrier", 3, 1, link=1)
    g.obj("barrier", 3, 2, link=1)
    g.chest("hammer", 2, 2)                # the hammer nook
    g.put(17, 10, "X"); g.put(18, 10, "X")  # seal the cache nook's north mouth
    g.obj("barrier", 16, 11, link=1)
    g.obj("barrier", 16, 12, link=1)
    g.chest("rupees_50", 18, 12)            # the toll cache
    g.obj("lever", 5, 2, link=1)           # pull it: both nooks open
    # a second bottle (green potion) out in the open
    g.chest("potion_green", 10, 11)
    # the down stair to the barracks (southeast)
    g.put(17, 9, "'")
    g.stair("15_crypt_barracks", 3, 8, 17, 9)
    g.sign("sign_west.json", 14, 9)
    g.put(6, 9, "'"); g.put(10, 2, "'")
    g.enemy("soldier_blue", 11, 7)
    g.enemy("soldier", 7, 10)
    g.enemy("octorok", 3, 8)
    g.obj("pot", 8, 10)
    g.obj("pot", 8, 11)
    g.pickup("heart", 6, 11)
    g.pickup("magic", 11, 2)
    g.spawn(16, 6)
    return g, dict(
        default={"hero": "enoki", "spawn": [16, 6], "music": LAMENT,
                 "walk_sfx": "walk_grass.wav", "danger_music": DANGER},
        dark=True,
        dungeon=True,
        exits={"east": "10_crypt_hall", "north": "14_crypt_lower"},
        doors={"east": {"kind": "locked"}, "north": {"kind": "bombable"}},
        encounters={},
    )


def crypt_pit():
    g = G(20, 14, "q")
    g.border("W")
    # the Mire: a pit lake with an island chest hoard in the middle
    g.rect(2, 3, 17, 10, "o")
    # the bank around the pits
    g.rect(1, 1, 18, 2, "q")
    g.rect(1, 11, 18, 12, "q")
    g.rect(1, 3, 1, 10, "q")
    g.rect(18, 3, 18, 10, "q")
    # the stair home (northwest corner)
    g.stair("10_crypt_hall", 17, 4, 2, 1)
    # the island: two chests, reached by the raft (or the hookshot posts)
    g.rect(9, 5, 11, 8, "q")
    g.chest("big_key", 10, 6)
    g.chest("magic_boomerang", 10, 7)
    # the raft rail out to the island
    g.obj("platform", 5, 2, path=[[5, 2], [7, 3], [9, 4], [9, 6]])
    # spike teeth along the banks
    g.put(3, 11, "ss"); g.put(8, 1, "ss"); g.put(14, 11, "ss")
    g.put(15, 1, "s")
    g.sign("sign_pit.json", 16, 12)
    g.enemy("octorok", 13, 2)
    g.enemy("octorok", 4, 11)
    g.enemy("soldier_blue", 12, 11)
    g.pickup("heart", 3, 2)
    g.pickup("rupee", 16, 2)
    # pits drop into the Cistern below (pit redirect)
    g.spawn(2, 11)
    return g, dict(
        default={"hero": "enoki", "spawn": [2, 11], "music": LAMENT,
                 "walk_sfx": "walk_grass.wav"},
        dungeon=True,
        pit={"room": "14_crypt_lower", "tile": [10, 6]},
        encounters={},
    )


def crypt_lower():
    g = G(20, 12, "q")
    g.border("W")
    g.dark = True
    g.gate("south", 9, 10, "_")            # the cracked door up to the west wing
    g.obj("torch", 2, 2, link=0)
    g.obj("torch", 17, 2, link=0)
    g.obj("torch", 2, 9, link=0)
    g.obj("torch", 17, 9, link=0)
    # the stair back up to the hall
    g.put(16, 1, "'"); g.put(17, 1, "'")
    g.stair("10_crypt_hall", 11, 13, 16, 1)
    # the fall landing (from the Mire) - hookshot reward right there
    g.put(10, 6, ":")
    g.chest("hookshot", 10, 5)
    # somaria practice: the plain chest first, then the conjured-block weight
    g.chest("somaria", 4, 3)
    # the powder nook (southwest corner): conjured blocks sink the latch
    # switch outside it; one flip of the barriers opens the way
    g.put(1, 8, "X"); g.put(2, 8, "X")     # seal the nook's north mouth
    g.obj("barrier", 3, 9, link=3)
    g.obj("barrier", 3, 10, link=3)
    g.chest("powder", 2, 10)
    g.obj("switch", 5, 9, mode="latch", link=3)
    # buried soil patch (the shovel turns it up)
    g.rect(12, 8, 14, 8, "d")
    g.sign("sign_lower.json", 12, 6)
    g.enemy("soldier_blue", 12, 4)
    g.enemy("octorok", 6, 4)
    g.obj("pot", 3, 6)
    g.pickup("magic", 13, 8)
    g.pickup("heart", 17, 6)
    g.spawn(10, 6)
    return g, dict(
        default={"hero": "enoki", "spawn": [10, 6], "music": LAMENT,
                 "walk_sfx": "walk_grass.wav"},
        dark=True,
        dungeon=True,
        exits={"south": "12_crypt_west"},
        doors={"south": {"kind": "bombable"}},
        buried=[("rupee5", 12, 8), ("heart", 13, 8), ("rupee5", 14, 8)],
        encounters={},
    )


def crypt_barracks():
    g = G(16, 12, "q")
    g.border("W")
    # the stair home (northwest)
    g.put(2, 1, "'")
    g.stair("12_crypt_west", 16, 9, 2, 1)
    # the stair down to the worm's lair
    g.put(3, 1, "'")
    g.stair("16_crypt_lair", 2, 8, 3, 1)
    # the Captain's dais (east): the stone backdrop, chests before it
    g.rect(13, 4, 14, 8, "X")
    g.chest("fire_rod", 13, 3)
    g.chest("flute", 13, 9)
    # torches + pots + rubble
    g.obj("torch", 5, 2, link=0)
    g.obj("torch", 5, 9, link=0)
    g.obj("torch", 14, 2, link=0)
    g.obj("pot", 6, 3)
    g.obj("pot", 6, 4)
    g.obj("pot", 9, 8)
    g.obj("pot", 8, 9)
    g.put(3, 5, "y"); g.put(4, 7, "y"); g.put(10, 5, "y")
    # the Captain holds the room
    g.enemy("captain", 8, 5)
    g.enemy("soldier", 4, 8)
    g.enemy("soldier", 11, 9)
    g.spawn(2, 3)
    return g, dict(
        default={"hero": "enoki", "spawn": [2, 3], "music": LAMENT,
                 "walk_sfx": "walk_grass.wav", "danger_music": DANGER},
        dungeon=True,
        encounters={},
    )


def crypt_lair():
    g = G(18, 12, "q")
    g.border("W")
    # the stair home (southwest)
    g.put(2, 9, "'")
    g.stair("15_crypt_barracks", 2, 2, 2, 9)
    # the lair: an open pit ring the worm careens through; rubble
    g.put(6, 6, "y"); g.put(11, 4, "y"); g.put(12, 8, "y")
    g.put(5, 3, "y"); g.put(8, 9, "y")
    g.obj("torch", 2, 2, link=0)
    g.obj("torch", 15, 2, link=0)
    # corner chests (the worm guards them; the tail is the weak point)
    g.chest("ether", 15, 3)
    g.chest("heart_vessel", 15, 9)
    g.enemy("moldorm", 9, 6)
    g.pickup("heart", 8, 2)
    g.spawn(3, 9)
    return g, dict(
        default={"hero": "enoki", "spawn": [3, 9], "music": LAMENT,
                 "walk_sfx": "walk_grass.wav", "danger_music": DANGER},
        dungeon=True,
        encounters={},
    )


def crypt_sanctum():
    g = G(18, 14, "q")
    g.border("W")
    g.gate("south", 8, 9, "_")            # the big door (from the hall)
    g.gate("north", 8, 9, "_")            # the throne room, past the Knight
    # torch rows + statues flanking the approach
    g.obj("torch", 3, 4, link=0)
    g.obj("torch", 14, 4, link=0)
    g.obj("statue", 6, 5)
    g.obj("statue", 11, 5)
    g.put(7, 7, "'"); g.put(10, 7, "'")
    # spike teeth hemming the arena
    g.put(2, 8, "ss"); g.put(14, 8, "ss")
    # the Ash Knight holds the north
    g.enemy("ash_knight", 8, 6)
    g.enemy("soldier", 4, 10)
    g.enemy("soldier", 13, 10)
    g.obj("pot", 2, 12)
    g.obj("pot", 3, 12)
    g.obj("pot", 14, 12)
    g.pickup("heart", 5, 11)
    g.pickup("rupee", 12, 11)
    g.sign("sign_sanctum.json", 11, 12)
    g.spawn(8, 12)
    return g, dict(
        default={"hero": "enoki", "spawn": [8, 12], "music": LAMENT,
                 "walk_sfx": "walk_grass.wav", "danger_music": DANGER},
        dungeon=True,
        exits={"south": "10_crypt_hall", "north": "18_crypt_throne"},
        doors={"south": {"kind": "big"}},
        encounters={"instant": [
            {"location": [8, 12], "dialogue": "beat_sanctum.json",
             "repeat": False},
        ]},
    )


def crypt_throne():
    g = G(20, 14, "q")
    g.border("W")
    g.gate("south", 9, 10, "_")            # from the sanctum
    # the ante-chamber (south): silver arrows + a vessel before the dais
    g.obj("torch", 2, 11, link=0)
    g.obj("torch", 17, 11, link=0)
    g.chest("silver_arrows", 2, 10)
    g.chest("heart_vessel", 17, 10)
    g.sign("sign_throne.json", 5, 12)
    # the dais (north): pillars, a wet floor, and the throne itself
    g.rect(7, 1, 12, 5, "e")
    g.put(7, 2, "P"); g.put(12, 2, "P")
    g.obj("statue", 7, 6)
    g.obj("statue", 12, 6)
    g.put(9, 4, "'")
    # the Hollow King, on the throne
    g.enemy("hollow_king", 9, 3)
    # the ending trigger: step to the throne once he falls
    g.put(9, 5, ":")
    g.enemy("soldier", 4, 6)
    g.enemy("soldier", 15, 6)
    g.obj("pot", 3, 4)
    g.obj("pot", 16, 4)
    g.obj("pot", 3, 5)
    g.pickup("heart", 5, 9)
    g.pickup("magic", 14, 9)
    g.spawn(9, 12)
    return g, dict(
        default={"hero": "enoki", "spawn": [9, 12], "music": LAMENT,
                 "walk_sfx": "walk_grass.wav", "danger_music": DANGER},
        dungeon=True,
        exits={"south": "17_crypt_sanctum"},
        encounters={"instant": [
            {"location": [9, 12], "dialogue": "beat_throne.json",
             "repeat": False},
            {"location": [9, 5], "dialogue": "ending.json",
             "if": "boss_defeated_18_crypt_throne", "repeat": False},
        ]},
    )


BUILDERS = {
    "01_village": village,
    "02_meadow": meadow,
    "03_grove": grove,
    "04_shore": shore,
    "05_ridge": ridge,
    "06_elder_house": elder_house,
    "07_cave_byrna": cave_byrna,
    "08_hollow_grove": hollow_grove,
    "10_crypt_hall": crypt_hall,
    "11_crypt_east": crypt_east,
    "12_crypt_west": crypt_west,
    "13_crypt_pit": crypt_pit,
    "14_crypt_lower": crypt_lower,
    "15_crypt_barracks": crypt_barracks,
    "16_crypt_lair": crypt_lair,
    "17_crypt_sanctum": crypt_sanctum,
    "18_crypt_throne": crypt_throne,
}

ID_TO_TILE = {}
for _name, _id in TILE_ID.items():
    ID_TO_TILE[_id] = _name

OPPOSITE = {"north": "south", "south": "north", "east": "west",
            "west": "east"}


def compile_room(name, g, extra):
    """Canvas + spec extras -> the room json the generator parses."""
    width, height = g.w, g.h
    decor = []
    colliders = []
    upper = []
    for y in range(height):
        for x in range(width):
            ch = g.rows[y][x]
            tile_name = TILE_CHARS[ch]
            tile_id = TILE_ID[tile_name]
            decor.append(tile_id)
            colliders.append(tile_id if tile_name in SOLID_TILES else 0)
            upper.append(tile_id if tile_name in UPPER_TILES else 0)

    enemies, objects, npcs, pickups, spawn_tile = [], [], [], [], [0, 0]
    for (x, y, spec) in g.entities:
        tile_name = TILE_CHARS[g.rows[y][x]]
        on_water = tile_name in ("deep_a", "shallow_a", "heal_a")
        on_solid_or_pit = tile_name in SOLID_TILES or \
            tile_name in ("pit", "d_pit")
        # Rafts float on deep water and hook posts stand in the shallows;
        # everything else wants dry land.
        if "object" in spec and spec["object"] in ("platform", "hook_post"):
            assert not on_solid_or_pit, \
                "%s: entity (%d,%d) sits on %s" % (name, x, y, tile_name)
        elif spec.get("spawn") or "npc" in spec or "chest" in spec \
                or "object" in spec:
            assert not on_water and not on_solid_or_pit, \
                "%s: entity (%d,%d) sits on %s" % (name, x, y, tile_name)
        else:  # enemies + pickups may haunt the water (zora, fairies)
            assert not on_solid_or_pit, \
                "%s: entity (%d,%d) sits on %s" % (name, x, y, tile_name)
        if spec.get("spawn"):
            spawn_tile = [x, y]
        elif "enemy" in spec:
            enemies.append({"name": spec["enemy"], "location": [x, y]})
        elif "pickup" in spec:
            pickups.append({"kind": spec["pickup"], "location": [x, y]})
        elif "npc" in spec:
            entry = {"character": spec["npc"], "dialogue": spec["dialogue"],
                     "location": [x, y]}
            if spec.get("wander"):
                entry["wander"] = True
            if spec.get("path"):
                entry["path"] = spec["path"]
            if spec.get("if"):
                entry["if"] = spec["if"]
            if spec.get("unless"):
                entry["unless"] = spec["unless"]
            npcs.append(entry)
        elif "chest" in spec:
            payload = CHESTS[spec["chest"]]
            objects.append({"type": "chest", "location": [x, y],
                             "kind": payload["kind"],
                             "grant": payload["grant"]})
        else:  # plain object
            entry = {"type": spec["object"], "location": [x, y]}
            if spec.get("dialogue"):
                entry["dialogue"] = spec["dialogue"]
            if spec.get("mode"):
                entry["mode"] = spec["mode"]
            if spec.get("link") is not None:
                entry["link"] = spec["link"]
            if spec.get("color"):
                entry["color"] = spec["color"]
            if spec.get("hides"):
                entry["hides"] = spec["hides"]
            if spec.get("flag"):
                entry["flag"] = spec["flag"]
            if spec.get("path"):
                entry["path"] = spec["path"]
            if spec.get("warp"):
                entry["warp"] = {"room": spec["warp"]["room"],
                                 "tile": list(spec["warp"]["tile"])}
            objects.append(entry)

    # Budgets (fixed pools the engine bakes against).
    assert len(enemies) <= 16, "%s: %d enemies" % (name, len(enemies))
    assert len(objects) <= 16, "%s: %d objects" % (name, len(objects))
    assert len(npcs) <= 8, "%s: %d npcs" % (name, len(npcs))
    assert len(pickups) <= 32, "%s: %d pickups" % (name, len(pickups))

    room = {"width": width, "height": height}
    default = dict(extra["default"])
    default["spawn"] = spawn_tile
    room["default"] = default
    room["decor"] = decor
    room["colliders"] = colliders
    room["upper"] = upper
    if enemies:
        room["enemies"] = enemies
    if objects:
        room["objects"] = objects
    if npcs:
        room["npcs"] = npcs
    if pickups:
        room["pickups"] = pickups
    if getattr(g, "buried", None):
        room["buried"] = [{"kind": kind, "location": [x, y]}
                          for (kind, x, y) in g.buried]
    if extra.get("exits"):
        room["exits"] = extra["exits"]
    if extra.get("doors"):
        room["doors"] = extra["doors"]
    if extra.get("weather"):
        room["weather"] = extra["weather"]
    if extra.get("tint"):
        room["tint"] = extra["tint"]
    if extra.get("dark"):
        room["dark"] = True
    if extra.get("dungeon"):
        room["dungeon"] = True
    if extra.get("pit"):
        room["pit"] = extra["pit"]
    if extra.get("mirror"):
        room["mirror"] = extra["mirror"]
    if extra.get("encounters"):
        room["encounters"] = extra["encounters"]
    return room


ID_TO_TILE = {}


def walkable(room, x, y):
    """Out-of-map, solid tiles, and pits block arrivals."""
    if x < 0 or y < 0 or x >= room["width"] or y >= room["height"]:
        return False
    index = y * room["width"] + x
    if room["colliders"][index]:
        return False
    tile_name = ID_TO_TILE.get(room["decor"][index])
    if tile_name in ("pit", "d_pit", "deep_a"):
        return False
    return True


def main():
    os.makedirs(OUT, exist_ok=True)
    for old in sorted(os.listdir(OUT)):
        if old.endswith(".json"):
            os.remove(os.path.join(OUT, old))
            print("removed old room %s" % old)

    rooms = {}
    for name, builder in BUILDERS.items():
        g, extra = builder()
        rooms[name] = compile_room(name, g, extra)

    # Cross-room validation: exits reciprocate, doors match, warps land on
    # walkable tiles, mirror pairs agree, pit redirects land.
    problems = []
    for name, room in rooms.items():
        for edge, target in room.get("exits", {}).items():
            if target not in rooms:
                problems.append("%s: exit %s -> unknown room %s"
                                % (name, edge, target))
                continue
            back = OPPOSITE[edge]
            target_exits = rooms[target].get("exits", {})
            if target_exits.get(back) != name:
                problems.append("%s: exit %s->%s not reciprocal (%s)"
                                % (name, edge, target, target_exits.get(back)))
            door_here = (room.get("doors") or {}).get(edge)
            door_there = (rooms[target].get("doors") or {}).get(back)
            if door_here or door_there:
                if not door_here or not door_there or \
                        door_here["kind"] != door_there["kind"]:
                    problems.append("%s: door %s / %s->%s kinds disagree"
                                    % (name, edge, target, back))
        for entry in room.get("objects", []):
            warp = entry.get("warp")
            if not warp:
                continue
            target, (tx, ty) = warp["room"], warp["tile"]
            if target not in rooms:
                problems.append("%s: warp -> unknown room %s"
                                % (name, target))
                continue
            if not walkable(rooms[target], tx, ty):
                problems.append("%s: warp lands on blocked tile %s:(%d,%d)"
                                % (name, target, tx, ty))
        pit = room.get("pit")
        if pit:
            target, (tx, ty) = pit["room"], pit["tile"]
            if target not in rooms or not walkable(rooms[target], tx, ty):
                problems.append("%s: pit redirect lands badly" % name)
        mirror = room.get("mirror")
        if mirror:
            pair = rooms.get(mirror)
            if not pair or pair.get("mirror") != name:
                problems.append("%s: mirror pair %s disagrees"
                                % (name, mirror))
        for key in ("instant", "interactive"):
            for entry in (room.get("encounters") or {}).get(key) or []:
                ex, ey = entry["location"]
                if not walkable(room, ex, ey):
                    problems.append("%s: %s encounter on blocked tile (%d,%d)"
                                    % (name, key, ex, ey))

    if problems:
        for line in problems:
            print("PROBLEM: %s" % line)
        sys.exit(1)

    # Walkable-adjacency sanity: every warp target needs a reachable
    # neighbor? (skipped - the engine nudges to the nearest free tile).

    for name, room in rooms.items():
        path = os.path.join(OUT, name + ".json")
        with open(path, "w") as handle:
            handle.write(json.dumps(room))
            handle.write("\n")
        counts = (len(room.get("enemies", [])),
                  len(room.get("objects", [])),
                  len(room.get("npcs", [])),
                  len(room.get("pickups", [])))
        print("wrote %s (%dx%d tiles, enemies=%d objects=%d npcs=%d "
              "pickups=%d)"
              % (os.path.relpath(path, ROOT), room["width"], room["height"],
                 *counts))
    print("%d rooms" % len(rooms))


if __name__ == "__main__":
    main()