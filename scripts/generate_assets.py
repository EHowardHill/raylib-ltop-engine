#!/usr/bin/env python3
"""Generate include_auto/assets.h and src_auto/assets.cpp from the .json
files in resources/, so the Raylib binary never parses json at runtime.
Rooms store their tile layers directly in json (edited with the web editor);
the tileset is derived from the tile png's dimensions. Also creates the
dialogue box border bitmap if the resources folder does not contain one yet."""

import json
import math
import os
import re
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# The game folder: the repo's own resources/ by default, or any other
# game folder via LTOP_GAME (the editor sets it when the open project is
# a new game folder rather than the engine repo — a new resources folder
# is a new game).
RES = os.environ.get("LTOP_GAME") or os.path.join(ROOT, "resources")
ASSETS = os.path.join(ROOT, "assets")
INCLUDE_AUTO = os.path.join(ROOT, "include_auto")
SRC_AUTO = os.path.join(ROOT, "src_auto")

TILE = 32
DIRECTIONS = ["down", "right", "left", "up"]


def res(*parts):
    return os.path.join(RES, *parts)


def warn(message):
    print("warning: %s" % message, file=sys.stderr)


def cstr(text):
    out = ['"']
    for b in text.encode("utf-8"):
        if b == 0x22:
            out.append('\\"')
        elif b == 0x5C:
            out.append("\\\\")
        elif b == 0x0A:
            out.append("\\n")
        elif 0x20 <= b <= 0x7E:
            out.append(chr(b))
        else:
            out.append("\\x%02x" % b)
            out.append('""')
    out.append('"')
    return "".join(out)


def sym(name):
    cleaned = re.sub(r"[^A-Za-z0-9]", "_", name).upper()
    if cleaned and cleaned[0].isdigit():
        cleaned = "_" + cleaned
    return cleaned


def png_size(path):
    with open(path, "rb") as handle:
        head = handle.read(24)
    if head[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s is not a PNG file" % path)
    width, height = struct.unpack(">II", head[16:24])
    return width, height


class FlagRegistry:
    """World flag names -> global indices, assigned in first-use order across
    every dialogue (and later: rooms, objects). Baked as WORLD_FLAG_NAMES /
    WORLD_FLAG_COUNT; runtime state lives in gWorldFlags."""

    def __init__(self):
        self.names = []
        self.sources = {}  # name -> first file that introduced it (tooling)

    def index(self, name, source=""):
        name = str(name)
        if name in self.names:
            return self.names.index(name)
        self.names.append(name)
        self.sources[name] = str(source)
        return len(self.names) - 1


def load_dialogues(flags):
    """Dialogue .json: {"lines": [<line>, ...]} where each <line> is either a
    plain text string or an object:
      {"text": ..., "label": ..., "if": <flag>, "unless": <flag>,
       "if_item": <item>, "unless_item": <item>, "give": <item>,
       "set": [<flags>], "clear": [<flags>], "sfx": "<file>",
       "music": "<file>"|"", "goto": "<label>",
       "warp": {"room": "<room>", "tile": [x, y]},
       "wait": <seconds>                       # cutscene beat: auto-advance
       "camera": {"pan": [tileX, tileY], "time": <s>} | "hero"
       "letterbox": true|false                  # cinematic bars
       "move": {"actor": "hero"|<npc index>, "tile": [x, y], "time": <s>}
       "choice": [{"text": ..., "set": [...], "clear": [...], "sfx": ...,
                   "give": <item>, "cost": <rupees>, "goto": ...}, ...]}
    Dialogue-level keys: "credits": true (endgame roll: centered lines over
    black, auto-advancing), "next": "<dialogue>" (chain when it runs out)."""
    dialogues = []
    directory = res("dialogue")
    if not os.path.isdir(directory):
        return dialogues
    for filename in sorted(os.listdir(directory)):
        if not filename.endswith(".json"):
            continue
        with open(os.path.join(directory, filename), "r", encoding="utf-8") as handle:
            data = json.load(handle)

        def flag(value, what):
            if value is None or value == "":
                return -1
            return flags.index(value, "dialogue/%s" % filename)

        def item(value, what):
            if value is None or value == "":
                return -1
            index = ITEM_INDEX.get(str(value), -1)
            if index < 0:
                warn("dialogue '%s' %s references unknown item '%s'"
                     % (filename, what, value))
            return index

        def flag_list(values, what):
            if values is None:
                return []
            if isinstance(values, (str, int, float)):
                values = [values]
            return [flags.index(v, "dialogue/%s" % filename) for v in values]

        lines = []
        for entry in data.get("lines") or []:
            if not isinstance(entry, dict):
                entry = {"text": str(entry)}

            warp_room = None
            tile = None
            if isinstance(entry.get("warp"), dict):
                warp_room = str(entry["warp"].get("room") or "")
                tile = entry["warp"].get("tile") or [0, 0]

            # Cutscene vocabulary: beat hold, camera pan/return, bars,
            # actor blocking (hero or a room npc index).
            try:
                wait_seconds = max(0.0, float(entry.get("wait") or 0))
            except (TypeError, ValueError):
                warn("dialogue '%s' has a bad wait %r" % (filename, entry.get("wait")))
                wait_seconds = 0.0
            cam_action = 0
            cam_tile = [0, 0]
            cam_seconds = 0.0
            camera = entry.get("camera")
            if camera == "hero" or camera == "follow":
                cam_action = 2
            elif isinstance(camera, dict):
                cam_action = 1
                cam_tile = camera.get("pan") or [0, 0]
                try:
                    cam_seconds = max(0.0, float(camera.get("time") or 1.0))
                except (TypeError, ValueError):
                    cam_seconds = 1.0
            letterbox = -1
            if entry.get("letterbox") is not None:
                letterbox = 1 if entry.get("letterbox") else 0
            move_actor = -1
            move_tile = [0, 0]
            move_seconds = 0.0
            move = entry.get("move")
            if isinstance(move, dict):
                actor = move.get("actor")
                if actor == "hero":
                    move_actor = -2
                else:
                    try:
                        move_actor = int(actor)
                    except (TypeError, ValueError):
                        warn("dialogue '%s' move has a bad actor %r"
                             % (filename, actor))
                        move_actor = -1
                move_tile = move.get("tile") or [0, 0]
                try:
                    move_seconds = max(0.0, float(move.get("time") or 1.0))
                except (TypeError, ValueError):
                    move_seconds = 1.0

            choices = []
            for opt in entry.get("choice") or []:
                if not isinstance(opt, dict):
                    opt = {"text": str(opt)}
                cost = opt.get("cost") or 0
                try:
                    cost = max(0, int(cost))
                except (TypeError, ValueError):
                    warn("dialogue '%s' choice has a bad cost %r" % (filename, opt.get("cost")))
                    cost = 0
                choices.append({
                    "text": str(opt.get("text") or ""),
                    "set": flag_list(opt.get("set"), "set"),
                    "clear": flag_list(opt.get("clear"), "clear"),
                    "sfx": str(opt["sfx"]) if opt.get("sfx") else None,
                    "give": item(opt.get("give"), "give"),
                    "cost": cost,
                    "goto": opt.get("goto"),
                })

            lines.append({
                "text": str(entry.get("text") or ""),
                "label": entry.get("label"),
                "if": flag(entry.get("if"), "if"),
                "unless": flag(entry.get("unless"), "unless"),
                "ifItem": item(entry.get("if_item"), "if_item"),
                "unlessItem": item(entry.get("unless_item"), "unless_item"),
                "give": item(entry.get("give"), "give"),
                "set": flag_list(entry.get("set"), "set"),
                "clear": flag_list(entry.get("clear"), "clear"),
                "sfx": str(entry["sfx"]) if entry.get("sfx") else None,
                "music": str(entry["music"]) if entry.get("music") is not None else None,
                "goto": entry.get("goto"),
                "warpRoom": warp_room,
                "warpTile": tile,
                "wait": wait_seconds,
                "camAction": cam_action,
                "camTileX": int(cam_tile[0]),
                "camTileY": int(cam_tile[1]),
                "camSeconds": cam_seconds,
                "letterbox": letterbox,
                "moveActor": move_actor,
                "moveTileX": int(move_tile[0]),
                "moveTileY": int(move_tile[1]),
                "moveSeconds": move_seconds,
                "choices": choices,
            })

        # Resolve label references (goto) within the dialogue.
        labels = {i: line["label"] for i, line in enumerate(lines) if line["label"]}
        for line in lines:
            goto = line.pop("goto")
            if goto is None:
                line["gotoLine"] = -1
            else:
                hits = [i for i, label in labels.items() if label == goto]
                if not hits:
                    warn("dialogue '%s' goto '%s' has no matching label" % (filename, goto))
                    line["gotoLine"] = -1
                else:
                    line["gotoLine"] = hits[0]
            for choice in line["choices"]:
                goto = choice.pop("goto")
                if goto is None:
                    choice["gotoLine"] = -1
                elif goto in labels.values():
                    choice["gotoLine"] = [i for i, label in labels.items() if label == goto][0]
                else:
                    warn("dialogue '%s' choice goto '%s' has no matching label"
                         % (filename, goto))
                    choice["gotoLine"] = -1
            line.pop("label")

        dialogues.append({"name": filename, "lines": lines,
                          "credits": bool(data.get("credits", False)),
                          # Resolved to an index once every dialogue loads.
                          "next": str(data.get("next") or "")})
    return dialogues


def validate_anims(owner, anims, frame_count):
    """Warn when animation frames fall outside the sprite sheet."""
    if frame_count <= 0:
        return
    for kind in ("stand", "move"):
        for index, direction in enumerate(DIRECTIONS):
            for frame in anims[kind][index]:
                if frame < 0 or frame >= frame_count:
                    warn("%s animation frame %d (%s %s) is outside the sheet (0..%d)"
                         % (owner, frame, kind, direction, frame_count - 1))


def load_directional_anims(data):
    """Parse the stand/move per-direction frame lists shared by characters and
    enemies: {"stand": {"down": [0, 1], ...}, "move": {...}}."""
    anims = {}
    for kind in ("stand", "move"):
        block = data.get(kind) or {}
        anims[kind] = []
        for direction in DIRECTIONS:
            values = block.get(direction) or []
            anims[kind].append([int(v) for v in values])
    return anims


def load_characters():
    characters = []
    directory = res("characters")
    if not os.path.isdir(directory):
        return characters
    for filename in sorted(os.listdir(directory)):
        if not filename.endswith(".json"):
            continue
        name = filename[:-5]
        with open(os.path.join(directory, filename), "r", encoding="utf-8") as handle:
            data = json.load(handle)

        image_path = res("characters", name + ".png")
        image_width, image_height = (0, 0)
        if os.path.exists(image_path):
            image_width, image_height = png_size(image_path)

        columns = int(data.get("width") or 0)
        if columns <= 0 and image_width:
            columns = image_width // TILE
        frame_count = (image_width // TILE) * (image_height // TILE)

        anims = load_directional_anims(data)
        validate_anims("character '%s'" % name, anims, frame_count)
        characters.append({
            "name": name,
            "image": "assets/characters/%s.png" % name,
            "columns": columns,
            "frameCount": frame_count,
            "anims": anims,
        })
    return characters


# --- Game identity ----------------------------------------------------------

def load_game():
    """resources/game.json (optional): {"title": ..., "slug": ...}.
    The title is engine chrome (title screen subtitle); the slug identifies
    the game in save files so a save from one game is never loaded by
    another (filesystem-safe, <= 15 chars)."""
    title = "UNTITLED GAME"
    slug = "game"
    path = res("game.json")
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as handle:
            data = json.load(handle)
        title = str(data.get("title") or title)
        slug = str(data.get("slug") or slug)
    clean = re.sub(r"[^a-z0-9-]", "", str(slug).lower()) or "game"
    if clean != str(slug).lower():
        warn("game slug '%s' is not filesystem-safe; using '%s'" % (slug, clean))
    if len(clean) > 15:
        warn("game slug '%s' is longer than 15 chars; truncating" % clean)
        clean = clean[:15]
    return {"title": title, "slug": clean}


# --- Items (resources/items/<name>.json) -------------------------------------

# Item use-action vocabulary, interpreted by the engine (ITEM_USE_*).
ITEM_USE_KINDS = {"none": 0, "boomerang": 1, "arrow": 2, "bomb": 3, "bolt": 4,
                  "hammer": 5, "shovel": 6, "hookshot": 7, "medallion": 8,
                  "byrna": 9, "cape": 10, "somaria": 11, "powder": 12,
                  "flute": 13, "net": 14, "bottle": 15, "mirror": 16}
ELEMENTS = {"none": 0, "fire": 1, "ice": 2}

# Room weather vocabulary (engine WEATHER_* constants).
WEATHER_KINDS = {"": 0, "rain": 1, "storm": 2, "fog": 3}


def load_items():
    """Item definitions live in resources/items/<name>.json:
    {"label": <subscreen label>,
     "use": {"kind": <use-action>, "element": "fire"|"ice" (bolts),
             "cost": {"magic": N} or {"ammo": "arrow"|"bomb", "amount": N,
                                       "rupee_fallback": bool},
             "sfx": <audio file>},
     "passive": {"light": <radius px>, "swim": <bool flippers>,
                "lift": <bool gloves>, "compass": <bool dungeon compass>},
     "upgrade": {"flag": <world flag>, "speed": N, "damage": N},
     "spawn": <object type name (somaria-style block use-actions)>}
    Ids are assigned in sorted-filename order and emitted as ITEM_<NAME>
    constants. All behavior is composed from the engine's use-action
    vocabulary; the useEquippedItem switch is gone."""
    items = []
    directory = res("items")
    if not os.path.isdir(directory):
        warn("no items found in resources/items")
        return items
    for filename in sorted(os.listdir(directory)):
        if not filename.endswith(".json"):
            continue
        name = filename[:-5]
        with open(os.path.join(directory, filename), "r", encoding="utf-8") as handle:
            data = json.load(handle)

        label = str(data.get("label") or name.upper())
        use = data.get("use") or {}
        if not isinstance(use, dict):
            warn("item '%s' has a bad 'use' %r (expected an object)" % (name, use))
            use = {}

        kind_name = str(use.get("kind") or "none")
        if kind_name not in ITEM_USE_KINDS:
            warn("item '%s' has unknown use kind '%s' %s"
                 % (name, kind_name, sorted(ITEM_USE_KINDS)))
            kind_name = "none"
        element_name = str(use.get("element") or "none")
        if element_name not in ELEMENTS:
            warn("item '%s' has unknown element '%s' %s"
                 % (name, element_name, sorted(ELEMENTS)))
            element_name = "none"

        cost = use.get("cost") or {}
        if not isinstance(cost, dict):
            warn("item '%s' has a bad 'cost' %r (expected an object)" % (name, cost))
            cost = {}
        magic_cost = max(0, int(cost.get("magic") or 0))
        ammo = str(cost.get("ammo") or "")
        bomb_cost = 0
        arrow_cost = 0
        arrow_fallback = False
        if ammo == "bomb":
            bomb_cost = max(0, int(cost.get("amount") or 1))
        elif ammo == "arrow":
            arrow_cost = max(0, int(cost.get("amount") or 1))
            arrow_fallback = bool(cost.get("rupee_fallback", False))
        elif ammo:
            warn("item '%s' has unknown ammo kind '%s' (arrow/bomb)" % (name, ammo))

        sfx = str(use["sfx"]) if use.get("sfx") else None

        passive = data.get("passive") or {}
        if not isinstance(passive, dict):
            warn("item '%s' has a bad 'passive' %r (expected an object)"
                 % (name, passive))
            passive = {}
        light = max(0, int(passive.get("light") or 0))
        swim = bool(passive.get("swim", False))
        lift = bool(passive.get("lift", False))
        compass = bool(passive.get("compass", False))

        # World-flag upgrade (silver arrows, magical boomerang): while the
        # flag is set, the use-action runs with the upgraded stats. The
        # flag name resolves to an index once the flag registry is built.
        upgrade = data.get("upgrade") or {}
        if not isinstance(upgrade, dict):
            warn("item '%s' has a bad 'upgrade' %r (expected an object)"
                 % (name, upgrade))
            upgrade = {}
        upgrade_flag = str(upgrade.get("flag") or "")
        upgrade_speed = max(0, int(upgrade.get("speed") or 0))
        upgrade_damage = max(0, int(upgrade.get("damage") or 0))
        if upgrade_flag and not (upgrade_speed or upgrade_damage):
            warn("item '%s' upgrade '%s' sets no speed/damage"
                 % (name, upgrade_flag))

        # Spawned object (Somaria-style use-actions): the object type the
        # use conjures/removes; resolves once object types are loaded.
        spawn_object_name = str(data.get("spawn") or "")

        items.append({
            "name": name,
            "label": label,
            "useKind": ITEM_USE_KINDS[kind_name],
            "element": ELEMENTS[element_name],
            "magicCost": magic_cost,
            "bombCost": bomb_cost,
            "arrowCost": arrow_cost,
            "arrowRupeeFallback": arrow_fallback,
            "sfx": sfx,
            "light": light,
            "swim": swim,
            "lift": lift,
            "compass": compass,
            "upgradeFlagName": upgrade_flag,
            "upgradeSpeed": upgrade_speed,
            "upgradeDamage": upgrade_damage,
            "spawnObjectName": spawn_object_name,
        })
    if len(items) > 24:
        warn("%d items exceed the inventory slots (24); the extras are"
             " unreachable" % len(items))
    return items


ITEM_INDEX = {}  # name -> id, filled by generate() after load_items()


# --- Object types (resources/objects/types.json) -----------------------------

# The engine's interaction vocabulary (OBJINT_* in engine.h): what a type
# composed of, behaviorally. The engine interprets interactions; a game
# defines its own types by composing them.
OBJINT_NAMES = {
    "solid": 0x1,          # blocks movement while on the ground
    "floor": 0x2,          # walkable plate, drawn under entities
    "liftable": 0x4,       # action button hoists it overhead
    "cut": 0x8,            # sword swings smash it (grass-style)
    "dash_smash": 0x10,    # pegasus dash shatters it
    "pushable": 0x20,      # sustained push slides it one tile
    "sinks": 0x40,         # push-block sinks onto latch switches
    "weight": 0x80,        # counts as weight on floor switches
    "switch": 0x100,       # floor plate (aux1 hold/latch, aux2 link)
    "barrier": 0x200,      # linked gate (aux2 link, aux3 color)
    "readable": 0x400,     # sign (aux1 dialogue)
    "chest": 0x800,        # container (aux1 kind, aux2/aux3 grant)
    "torch": 0x1000,       # lightable post (aux2 link)
    "warp": 0x2000,        # warp pad (aux1 room, aux2/aux3 tile)
    "crystal": 0x4000,     # sword strikes toggle the crystal state
    "leaves": 0x8000,      # smashes into leaves instead of shards
    "two_frame": 0x10000,  # two-frame sheet (frame 2 once opened)
    "hides": 0x20000,      # hides an enemy (aux1) until lifted
    "pullable": 0x40000,   # grabbable + pullable (grab via action, pull away)
    "hammer_smash": 0x80000,  # hammer pounds shatter it
    "heavy": 0x100000,     # liftable only with a lift passive item (gloves)
    "lever": 0x200000,     # pull-handle (aux2 = barrier link group it toggles)
    "platform": 0x400000,  # moving platform ("path" = tile waypoints, looped)
    "hookable": 0x800000,  # hookshot latch: the chain grabs it and reels the hero in
    "powder": 0x1000000,   # magic powder awakens it (aux2 = story flag to set)
    "stair": 0x2000000,    # staircase ("warp" aux payloads; step-on layer warp)
}

# Baseline type set, used when the game ships no types.json (a fresh game
# folder is playable from day one).
DEFAULT_OBJECT_TYPES = [
    {"name": "pot", "interactions": ["solid", "liftable", "dash_smash", "hammer_smash", "hides"]},
    {"name": "bush", "interactions": ["solid", "liftable", "dash_smash", "cut", "leaves"]},
    {"name": "block", "interactions": ["solid", "pushable", "pullable", "sinks", "weight"]},
    {"name": "switch", "interactions": ["floor", "switch"]},
    {"name": "barrier", "interactions": ["solid", "barrier"]},
    {"name": "sign", "interactions": ["solid", "readable"]},
    {"name": "chest", "interactions": ["solid", "chest", "two_frame"]},
    {"name": "torch", "interactions": ["solid", "torch"]},
    {"name": "warp", "interactions": ["floor", "warp"]},
    {"name": "stair", "interactions": ["floor", "stair"]},
    {"name": "crystal", "interactions": ["solid", "crystal"]},
    {"name": "rock", "interactions": ["solid", "hammer_smash"]},
    {"name": "big_rock", "interactions": ["solid", "liftable", "heavy"]},
    {"name": "statue", "interactions": ["solid", "pushable", "pullable", "sinks", "weight"]},
    {"name": "lever", "interactions": ["solid", "pullable", "lever"]},
    {"name": "platform", "interactions": ["floor", "platform"]},
    {"name": "hook_post", "interactions": ["solid", "hookable"]},
    {"name": "somaria_block", "interactions": ["solid", "pushable", "sinks",
                                               "weight"]},
    {"name": "shrine", "interactions": ["solid", "powder"]},
]


def load_object_types():
    """Object type definitions live in resources/objects/types.json:
    {"types": [{"name": ..., "image": <file>, "interactions": [...]}]}.
    Types compose the engine's interaction vocabulary; room objects
    reference types by name. Missing file = the baseline set above."""
    global OBJECT_TYPES
    path = res("objects", "types.json")
    if not os.path.exists(path):
        types = []
        for t in DEFAULT_OBJECT_TYPES:
            mask = 0
            for interaction in t["interactions"]:
                mask |= OBJINT_NAMES.get(interaction, 0)
            types.append({
                "name": t["name"],
                "image": t["name"] + ".png",
                "interactions": list(t["interactions"]),
                "interactionsMask": mask,
            })
        OBJECT_TYPES = types
        return types
    with open(path, "r", encoding="utf-8") as handle:
        data = json.load(handle)
    types = []
    for entry in data.get("types") or []:
        if not isinstance(entry, dict) or not entry.get("name"):
            warn("object type entry %r has no name" % entry)
            continue
        mask = 0
        for interaction in entry.get("interactions") or []:
            if interaction in OBJINT_NAMES:
                mask |= OBJINT_NAMES[interaction]
            else:
                warn("object type '%s' has unknown interaction '%s' %s"
                     % (entry["name"], interaction, sorted(OBJINT_NAMES)))
        types.append({
            "name": str(entry["name"]),
            "image": str(entry.get("image") or (str(entry["name"]) + ".png")),
            "interactionsMask": mask,
        })
    if len(types) > 32:
        warn("%d object types exceed the engine pool (32)" % len(types))
    OBJECT_TYPES = types
    return types


OBJECT_INDEX = {}  # name -> id, filled by generate() after load_object_types()
OBJECT_TYPES = []  # the loaded type list (load_rooms reads the masks)


# --- Doors, chests, grants: rule tables ---------------------------------------

# Door kinds on room edges. The kind's RULE is data (interpreted by the
# engine): no per-kind switch branches exist in the engine.
#   starts_open:   always passable (cosmetic when open)
#   push_open:     a push from either side opens it, stays open
#   key:           "small"/"big" key consumed on first pass
#   bombable:      a blast at the wall midpoint blows it open
#   live:          "enemies_dead" = open only while the room is cleared
DOOR_KIND_DEFS = [
    {"name": "open", "rule": {"starts_open": True}},
    {"name": "closed", "rule": {"push_open": True}},
    {"name": "locked", "rule": {"key": "small"}},
    {"name": "big", "rule": {"key": "big"}},
    {"name": "bombable", "rule": {"bombable": True}},
    {"name": "shutter", "rule": {"live": "enemies_dead"}},
]
DOOR_KINDS = {entry["name"]: i for i, entry in enumerate(DOOR_KIND_DEFS)}

# Chest kinds: what the aux1 kind costs to open.
CHEST_KIND_DEFS = [
    {"name": "normal", "key_cost": 0},
    {"name": "locked", "key_cost": 1},
    {"name": "big", "key_cost": 2},
]
CHEST_KINDS = {entry["name"]: i for i, entry in enumerate(CHEST_KIND_DEFS)}

# Grant kinds (chest payloads; the trigger-vocabulary "grant" action).
# effect = engine grant-effect vocabulary, pickup = pickups-sheet frame for
# the rise visual (-1 = medallion), text = item-get description template
# (%s = item label, %d = amount).
GRANT_KIND_DEFS = [
    {"name": "item", "effect": 0, "pickup": -1,
     "text": "You got the %s!"},
    {"name": "rupees", "effect": 1, "pickup": 1,
     "text": "You found %d rupees!"},
    {"name": "arrows", "effect": 2, "pickup": 4,
     "text": "You got %d arrows!"},
    {"name": "bombs", "effect": 3, "pickup": 5,
     "text": "You got %d bombs!"},
    {"name": "magic", "effect": 4, "pickup": 6,
     "text": "Your magic power was restored!"},
    {"name": "heart_container", "effect": 5, "pickup": 7,
     "text": "You got a heart container! Your life force grew!"},
    {"name": "flag", "effect": 8, "pickup": -1,
     "text": "A legend unfolds... Something has changed!"},
    {"name": "key", "effect": 6, "pickup": -1,
     "text": "You got a small key!"},
    {"name": "big_key", "effect": 7, "pickup": -1,
     "text": "You got the big key! It opens the biggest chest."},
    # Bottles: a slot each (aux3 = starting contents, engine vocabulary).
    # The decanter frame doubles as the bottle rise visual.
    {"name": "bottle", "effect": 9, "pickup": 6,
     "text": "You got a bottle! Fill it with water or catch something!"},
    {"name": "potion_red", "effect": 9, "pickup": 6,
     "text": "You got a bottle of red potion! It refills your hearts!"},
    {"name": "potion_green", "effect": 9, "pickup": 6,
     "text": "You got a bottle of green potion! It refills your magic!"},
    {"name": "potion_blue", "effect": 9, "pickup": 6,
     "text": "You got a bottle of blue potion! Hearts and magic, both restored!"},
]
GRANT_KINDS = {entry["name"]: i for i, entry in enumerate(GRANT_KIND_DEFS)}

# Crystal barrier colors (barrier "color" extra, baked to aux3): linked
# barriers follow their switch groups; colored ones follow the crystal
# state (a saved world flag) instead.
BARRIER_COLORS = {"linked": 0, "blue": 1, "red": 2}

# Placed pickups (room "pickups" list) and per-enemy drop tables: kinds map
# to the engine's pickups-sheet frames (PICKUP_*).
PICKUP_KINDS = {"heart": 0, "rupee": 1, "rupee5": 2, "arrow": 4,
                "bomb": 5, "magic": 6, "fairy": 8, "key": 9}


# --- Game code manifest (resources/code/) ------------------------------------

def load_code_manifest():
    """resources/code/manifest.json lists the behavior names the game's
    C++ (resources/code/*.cpp) registers; JSON references them as
    "custom:<name>" and the generator validates against this list."""
    path = res("code", "manifest.json")
    if not os.path.exists(path):
        return []
    with open(path, "r", encoding="utf-8") as handle:
        data = json.load(handle)
    behaviors = data.get("behaviors") or []
    if not isinstance(behaviors, list):
        warn("code manifest 'behaviors' is not a list")
        return []
    return [str(name) for name in behaviors]


# Enemy behavior presets: named compositions of the engine's component
# vocabulary. A preset fills component params from the enemy's stats;
# explicit "components" in stats override/extend the preset.
ENEMY_PRESETS = ["", "soldier", "octorok", "boss"]


def enemy_components(name, stats, manifest):
    """Compose the baked component parameters for one enemy.

    Presets (the "behavior" stat):
      ""        plain wanderer
      soldier   patrols, charges when the hero is within "sight" px
      octorok   walks, pauses, spits rocks roughly every "shoot_period"
      boss      arena boss: phased charges (faster as health drops)
      custom:X  a registered game-code behavior (resources/code/)

    Explicit "components" (optional, overrides the preset):
      {"charge": {"sight": px, "speed_scale": F, "stun_on_wall": bool},
       "shoot": {"period": N}, "mover": {"walk_time": s, "turn_chance": bool},
       "phases": [{"hp_below": F, "speed_scale": F}, ...],
       "boss": bool, "custom": "<name>"}
    """
    comps = {
        "charge_sight": 0.0, "charge_scale": 0.0, "charge_stun": False,
        "shoot_period": 0, "walk_segment": 0.0, "turn_in_place": False,
        "phases": [], "boss": False, "mini_boss": False,
        "strike_immune": False, "catchable": False, "custom": None,
    }
    behavior = str(stats.get("behavior") or "")
    sight = int(stats.get("sight") or 0)
    period = int(stats.get("shoot_period") or 0)

    if behavior == "soldier":
        comps["charge_sight"] = float(sight or 150)
        comps["charge_scale"] = 2.2
        comps["charge_stun"] = True
    elif behavior == "octorok":
        comps["shoot_period"] = period or 2
        comps["walk_segment"] = float(period or 2)
        comps["turn_in_place"] = True
    elif behavior == "boss":
        comps["boss"] = True
        comps["charge_sight"] = float(sight or 150)
        comps["charge_scale"] = 2.2
        comps["charge_stun"] = True
        # Phases: charge speed scales up as health drops (ascending
        # hp_below; the engine takes the first threshold matched).
        comps["phases"] = [(0.34, 3.4), (0.67, 2.6)]
    elif behavior.startswith("custom:"):
        comps["custom"] = behavior[len("custom:"):]
    elif behavior != "":
        warn("enemy '%s' has unknown behavior '%s' (presets: %s, or custom:<name>)"
             % (name, behavior, ENEMY_PRESETS[1:]))

    explicit = stats.get("components") or {}
    if not isinstance(explicit, dict):
        warn("enemy '%s' has a bad 'components' %r (expected an object)"
             % (name, explicit))
        explicit = {}
    charge = explicit.get("charge")
    if isinstance(charge, dict):
        if "sight" in charge: comps["charge_sight"] = max(0.0, float(charge["sight"]))
        if "speed_scale" in charge: comps["charge_scale"] = max(0.0, float(charge["speed_scale"]))
        if "stun_on_wall" in charge: comps["charge_stun"] = bool(charge["stun_on_wall"])
    shoot = explicit.get("shoot")
    if isinstance(shoot, dict) and "period" in shoot:
        comps["shoot_period"] = max(0, int(shoot["period"]))
        if "walk_time" not in (explicit.get("mover") or {}):
            comps["walk_segment"] = float(comps["shoot_period"])
    mover = explicit.get("mover")
    if isinstance(mover, dict):
        if "walk_time" in mover: comps["walk_segment"] = max(0.0, float(mover["walk_time"]))
        if "turn_chance" in mover: comps["turn_in_place"] = bool(mover["turn_chance"])
    if "boss" in explicit:
        comps["boss"] = bool(explicit["boss"])
    if "mini_boss" in explicit:
        # Mid-fight: seals the room + health bar while alive, room-local death.
        comps["mini_boss"] = bool(explicit["mini_boss"])
    if "strike_immune" in explicit:
        # Reflectable-magic bosses: direct hero strikes bounce off;
        # only projectile hits (deflected volleys) land.
        comps["strike_immune"] = bool(explicit["strike_immune"])
    if "catchable" in explicit:
        # Net-catchable critter (bees): the bug net bottles it.
        comps["catchable"] = bool(explicit["catchable"])
    phases = explicit.get("phases")
    if isinstance(phases, list) and phases:
        table = []
        for phase in phases:
            if not isinstance(phase, dict):
                warn("enemy '%s' has a bad phase %r" % (name, phase))
                continue
            table.append((max(0.0, min(1.0, float(phase.get("hp_below") or 0.0))),
                          max(0.0, float(phase.get("speed_scale") or 1.0))))
        if table:
            table.sort(key=lambda p: p[0])  # ascending hp_below
            comps["phases"] = table
    if explicit.get("custom"):
        comps["custom"] = str(explicit["custom"])

    # "boss": true stat shortcut (kept for compatibility).
    if stats.get("boss"):
        comps["boss"] = True
    # "mini_boss": true stat shortcut (mid-fight: seals + health bar, but
    # its defeat stays room/session-local instead of banking).
    if stats.get("mini_boss"):
        comps["mini_boss"] = True
    # "catchable": true stat shortcut (same deal).
    if stats.get("catchable"):
        comps["catchable"] = True

    if comps["custom"] and comps["custom"] not in manifest:
        warn("enemy '%s' references custom behavior '%s' that is not in"
             " resources/code/manifest.json" % (name, comps["custom"]))
    return comps


def parse_enemy_drops(name, stats):
    """Per-enemy drop table: {"drops": {"heart": 20, "rupee": 11, ...}}
    maps pickup kinds to weights. No table = the engine default roll."""
    drops = stats.get("drops")
    if drops is None:
        return []
    if not isinstance(drops, dict):
        warn("enemy '%s' has a bad 'drops' %r (expected kind -> weight)"
             % (name, drops))
        return []
    table = []
    for kind_name, weight in drops.items():
        if kind_name not in PICKUP_KINDS:
            warn("enemy '%s' drop kind '%s' is unknown %s"
                 % (name, kind_name, sorted(PICKUP_KINDS)))
            continue
        try:
            weight_value = max(0, int(weight))
        except (TypeError, ValueError):
            warn("enemy '%s' drop '%s' has a bad weight %r" % (name, kind_name, weight))
            continue
        if weight_value <= 0:
            continue
        table.append((PICKUP_KINDS[kind_name], weight_value))
    return table


def load_enemies(manifest):
    """Enemy definitions live in resources/enemies/<name>.json next to their
    sprite sheet (resources/enemies/<name>.png, or an "image" override to
    reuse another sheet - variants). Stats compose engine components via a
    behavior preset or explicit "components"; "drops" is an optional
    per-enemy drop table."""
    enemies = []
    directory = res("enemies")
    if not os.path.isdir(directory):
        return enemies
    for filename in sorted(os.listdir(directory)):
        if not filename.endswith(".json"):
            continue
        name = filename[:-5]
        with open(os.path.join(directory, filename), "r", encoding="utf-8") as handle:
            data = json.load(handle)

        image_name = data.get("image") or (name + ".png")
        image_path = res("enemies", image_name)
        image_width, image_height = (0, 0)
        if os.path.exists(image_path):
            image_width, image_height = png_size(image_path)

        columns = int(data.get("width") or 0)
        if columns <= 0 and image_width:
            columns = image_width // TILE
        frame_count = (image_width // TILE) * (image_height // TILE) if image_width else 0

        stats = data.get("stats") or {}
        comps = enemy_components(name, stats, manifest)
        drops = parse_enemy_drops(name, stats)
        color = data.get("color") or [200, 60, 60]

        enemies.append({
            "name": name,
            "image": "assets/enemies/%s" % image_name,
            "imageName": image_name,
            "columns": columns,
            "frameCount": frame_count,
            "anims": (lambda a: (validate_anims("enemy '%s'" % name, a, frame_count), a)[1])(
                load_directional_anims(data)),
            "hp": max(0, int(stats.get("hp") or 2)),
            "speed": max(0, int(stats.get("speed") or 40)),
            "damage": max(0, int(stats.get("damage") or 1)),
            "chargeSight": comps["charge_sight"],
            "chargeScale": comps["charge_scale"],
            "chargeStun": comps["charge_stun"],
            "shootPeriod": comps["shoot_period"],
            "walkSegment": comps["walk_segment"],
            "turnInPlace": comps["turn_in_place"],
            "phases": comps["phases"],
            "boss": comps["boss"],
            "miniBoss": comps["mini_boss"],
            "strikeImmune": comps["strike_immune"],
            "catchable": comps["catchable"],
            "custom": comps["custom"],
            "drops": drops,
            "color": [int(max(0, min(255, c))) for c in color],
        })
    if len(enemies) > 16:
        warn("%d enemies exceed the engine pool (16)" % len(enemies))
    return enemies


def load_tileset():
    """The tileset is a single png in resources/tiles: a 32x32 tile grid.
    All metadata (columns, tile count) is derived from its dimensions, so no
    Tiled .tsx file is needed."""
    directory = res("tiles")
    if not os.path.isdir(directory):
        return None
    for filename in sorted(os.listdir(directory)):
        if not filename.endswith(".png"):
            continue
        image_path = os.path.join(directory, filename)
        image_width, image_height = png_size(image_path)
        columns = image_width // TILE
        return {
            "image": "assets/tiles/" + filename,
            "firstGid": 1,
            "columns": columns,
            "tileCount": columns * (image_height // TILE),
            "tileWidth": TILE,
            "tileHeight": TILE,
        }
    return None


def load_tile_meta(tileset):
    """Tile property metadata lives in resources/tiles/tiles.json (optional).

    Format: {"tiles": {"<id>": {"pit": true, "damage": 1, "cuttable": true,
    "cut_result": <tile id>, "water": "deep"|"shallow", "ice": true,
    "push": [dx, dy], "ledge": "down"|"right"|"left"|"up",
    "bombable": true, "bomb_result": <tile id>, "dig": true, "heal": true}}}
    Tile ids are 1-based frame indices, matching the ids used in room layers.
    "damage" is measured in half-hearts. "cut_result" is the tile id left
    behind when the tile is cut (0 = empty). "push" is a conveyor/current/wind
    drift in px/s applied to whoever stands on the tile. "ledge" marks a
    one-way vault-over ledge facing that direction.     "bombable" marks cracked
    walls/floors; "bomb_result" (0 = empty, default) is what blasts leave.
    "dig" marks soft soil the shovel can turn over (drops + buried items).
    "heal" marks fairy-fountain water: standing on it slowly refills hearts
    and magic.
    """
    count = tileset["tileCount"] if tileset else 0
    meta = [{"flags": 0, "damage": 0, "cutResult": 0, "bombResult": 255,
             "pushX": 0, "pushY": 0, "ledgeDir": 0, "heal": 0}
            for _ in range(max(count, 0))]

    path = res("tiles", "tiles.json")
    if not os.path.exists(path):
        return meta

    with open(path, "r", encoding="utf-8") as handle:
        data = json.load(handle)

    def resolve_tile(value, what):
        try:
            tile_id = int(value)
        except (TypeError, ValueError):
            warn("tiles.json %s '%s' is not a tile id" % (what, value))
            return 0
        if tile_id < 0 or tile_id > count:
            warn("tiles.json %s %d is outside the tileset (0..%d)" % (what, tile_id, count))
            return 0
        return tile_id

    for key, props in (data.get("tiles") or {}).items():
        try:
            tile_id = int(key)
        except ValueError:
            warn("tile id '%s' in tiles.json is not a number" % key)
            continue
        if not isinstance(props, dict):
            warn("tile %d in tiles.json has no property object" % tile_id)
            continue
        if tile_id < 1 or tile_id > count:
            warn("tiles.json references tile %d, outside the tileset (1..%d)"
                 % (tile_id, count))
            continue
        entry = meta[tile_id - 1]
        if props.get("pit"):
            entry["flags"] |= 1
        if props.get("damage"):
            entry["damage"] = max(0, min(255, int(props["damage"])))
            entry["flags"] |= 2
        if props.get("cuttable"):
            entry["flags"] |= 4
            if props.get("cut_result") is not None:
                entry["cutResult"] = resolve_tile(props.get("cut_result"), "cut_result")
        water = props.get("water")
        if water is not None:
            if water == "deep":
                entry["flags"] |= 8
            elif water == "shallow":
                entry["flags"] |= 16
            else:
                warn("tiles.json tile %d has unknown water depth '%s' "
                     "(expected 'deep' or 'shallow')" % (tile_id, water))
        if props.get("ice"):
            entry["flags"] |= 32
        ledge = props.get("ledge")
        if ledge is not None:
            ledge_dirs = {"down": 0, "right": 1, "left": 2, "up": 3}
            if ledge in ledge_dirs:
                entry["ledgeDir"] = ledge_dirs[ledge]
                entry["flags"] |= 64
            else:
                warn("tiles.json tile %d has unknown ledge direction '%s' "
                     "(expected down/right/left/up)" % (tile_id, ledge))
        push = props.get("push")
        if push is not None:
            if (isinstance(push, (list, tuple)) and len(push) == 2
                    and all(isinstance(v, (int, float)) for v in push)):
                def clamp_push(value):
                    return max(-127, min(127, int(value)))
                entry["pushX"] = clamp_push(push[0])
                entry["pushY"] = clamp_push(push[1])
            else:
                warn("tiles.json tile %d has a bad 'push' %r "
                     "(expected [dx, dy] in px/s)" % (tile_id, push))
        if props.get("bombable"):
            entry["flags"] |= 128
            if props.get("bomb_result") is not None:
                entry["bombResult"] = min(255, max(0, int(props["bomb_result"])))
        if props.get("dig"):
            # Soft soil: the shovel turns it over (drops + buried items).
            entry["flags"] |= 256
        if props.get("heal"):
            entry["heal"] = 1
    return meta


def load_room_layers(stem, filename, data, tile_count=0):
    """Read the tile layers stored in the room json. Tile ids are 0-based
    frame indices + 1 (0 = empty); out-of-range ids are clamped to empty
    with a warning. Warns about legacy .tmx leftovers."""
    if "width" not in data or "height" not in data:
        warn("room '%s' has no width/height (legacy format?), skipping" % stem)
        return None
    map_width = int(data["width"])
    map_height = int(data["height"])
    expected = map_width * map_height

    layers = {}
    for name in ("decor", "colliders", "upper"):
        raw = data.get(name)
        values = [int(v) for v in (raw or [])]
        if len(values) != expected:
            if name == "upper" and raw is None:
                # Optional layer: simply missing -> all zeros, no warning.
                values = [0] * expected
            elif name == "upper":
                # Present but malformed: pad/truncate to size.
                warn("room '%s' layer 'upper' has %d tiles, expected %d (padding)"
                     % (stem, len(values), expected))
                if len(values) > expected:
                    values = values[:expected]
                else:
                    values = values + [0] * (expected - len(values))
            else:
                warn("room '%s' layer '%s' has %d tiles, expected %d"
                     % (stem, name, len(values), expected))
                return None
        for i, v in enumerate(values):
            if v < 0 or (tile_count > 0 and v > tile_count):
                warn("room '%s' layer '%s' tile id %d out of range (0..%d); cleared"
                     % (stem, name, v, tile_count))
                values[i] = 0
        layers[name] = values

    tmx_path = res("rooms", stem + ".tmx")
    if os.path.exists(tmx_path):
        warn("room '%s' has a legacy .tmx file that is now ignored (delete it)" % stem)

    return map_width, map_height, layers


def load_rooms(dialogue_index, character_index, enemy_index, enemy_defs=None,
               flags=None, tile_count=0):
    rooms = []
    directory = res("rooms")
    if not os.path.isdir(directory):
        return rooms
    for filename in sorted(os.listdir(directory)):
        if not filename.endswith(".json"):
            continue
        stem = filename[:-5]
        with open(os.path.join(directory, filename), "r", encoding="utf-8") as handle:
            data = json.load(handle)

        parsed = load_room_layers(stem, filename, data, tile_count)
        if parsed is None:
            continue
        map_width, map_height, layers = parsed

        default = data.get("default") or {}
        hero_name = default.get("hero") or ""

        spawn = default.get("spawn") or [0, 0]
        has_spawn = isinstance(spawn, (list, tuple)) and len(spawn) >= 2

        def encounter_list(key):
            encounters = []
            for entry in (data.get("encounters") or {}).get(key) or []:
                if not isinstance(entry, dict):
                    continue
                location = entry.get("location") or [0, 0]
                dialogue_name = entry.get("dialogue")
                index = -1
                if dialogue_name:
                    index = dialogue_index.get(dialogue_name, -1)
                    if index < 0:
                        warn("room '%s' references unknown dialogue '%s'" % (stem, dialogue_name))
                repeat = bool(entry.get("repeat", False))
                # Story-flag conditions (e.g. a post-boss endgame trigger),
                # stored raw and resolved with the flag registry below.
                encounters.append({"x": int(location[0]), "y": int(location[1]),
                                   "dialogue": index, "repeat": repeat,
                                   "ifFlag": entry.get("if"),
                                   "unlessFlag": entry.get("unless")})
            return encounters

        enemies = []
        room_has_boss = False
        for entry in data.get("enemies") or []:
            if not isinstance(entry, dict):
                continue
            enemy_name = entry.get("name") or ""
            index = enemy_index.get(enemy_name, -1)
            if index < 0:
                warn("room '%s' references unknown enemy '%s'" % (stem, enemy_name))
                continue
            location = entry.get("location") or [0, 0]
            enemies.append({"x": int(location[0]), "y": int(location[1]),
                            "enemy": index})
            if enemy_defs is not None and index < len(enemy_defs) \
                    and enemy_defs[index].get("boss"):
                room_has_boss = True

        objects = []
        for entry in data.get("objects") or []:
            if not isinstance(entry, dict):
                continue
            type_name = entry.get("type") or ""
            index = OBJECT_INDEX.get(type_name, -1)
            if index < 0:
                warn("room '%s' references unknown object type '%s'" % (stem, type_name))
                continue
            mask = OBJECT_TYPES[index]["interactionsMask"]
            location = entry.get("location") or [0, 0]
            aux1 = -1
            aux2 = 0
            aux3 = 0
            obj_path = []
            warp_room_raw = None
            # Aux packing is driven by the type's interactions (engine
            # vocabulary), never by type name: a new type that composes the
            # same interactions consumes the same extras.
            if mask & OBJINT_NAMES["switch"]:
                mode = entry.get("mode") or "hold"
                if mode not in ("hold", "latch", "timed"):
                    warn("room '%s' switch has unknown mode '%s' "
                         "(hold/latch/timed); using hold" % (stem, mode))
                    mode = "hold"
                aux1 = {"hold": 0, "latch": 1, "timed": 2}[mode]
                try:
                    aux2 = int(entry.get("link") or 0)
                except (TypeError, ValueError):
                    warn("room '%s' switch has a bad link id %r" % (stem, entry.get("link")))
            if mask & OBJINT_NAMES["barrier"]:
                try:
                    aux2 = int(entry.get("link") or 0)
                except (TypeError, ValueError):
                    warn("room '%s' barrier has a bad link id %r" % (stem, entry.get("link")))
                color = entry.get("color") or "linked"
                if color in BARRIER_COLORS:
                    aux3 = BARRIER_COLORS[color]
                else:
                    warn("room '%s' barrier has unknown color '%s' %s"
                         % (stem, color, sorted(BARRIER_COLORS)))
                    aux3 = 0
            if mask & OBJINT_NAMES["readable"]:
                dialogue_name = entry.get("dialogue")
                if dialogue_name:
                    aux1 = dialogue_index.get(dialogue_name, -1)
                    if aux1 < 0:
                        warn("room '%s' sign references unknown dialogue '%s'"
                             % (stem, dialogue_name))
            if mask & OBJINT_NAMES["torch"]:
                try:
                    aux2 = int(entry.get("link") or 0)
                except (TypeError, ValueError):
                    warn("room '%s' torch has a bad link id %r" % (stem, entry.get("link")))
            if mask & OBJINT_NAMES["powder"]:
                # Shrine-style: the powder sprinkle sets a story flag
                # ("flag": <world flag name>, through the shared registry).
                flag_name = entry.get("flag") or ""
                if flags is not None and flag_name:
                    aux2 = flags.index(flag_name)
                else:
                    aux2 = -1
            if mask & OBJINT_NAMES["lever"]:
                try:
                    aux2 = int(entry.get("link") or 0)
                except (TypeError, ValueError):
                    warn("room '%s' lever has a bad link id %r" % (stem, entry.get("link")))
            if mask & OBJINT_NAMES["platform"]:
                # "path": tile waypoints glided in a loop ([x, y], ...).
                for point in entry.get("path") or []:
                    if (isinstance(point, (list, tuple)) and len(point) >= 2
                            and all(isinstance(v, (int, float)) for v in point[:2])):
                        obj_path.extend([int(point[0]), int(point[1])])
                    else:
                        warn("room '%s' platform path point %r is not [x, y]"
                             % (stem, point))
            if mask & OBJINT_NAMES["hides"]:
                # "hides": enemy name — lifting the object reveals an ambush.
                hides_name = entry.get("hides")
                if hides_name:
                    aux1 = enemy_index.get(str(hides_name), -1)
                    if aux1 < 0:
                        warn("room '%s' object hides unknown enemy '%s'"
                             % (stem, hides_name))
            if (mask & OBJINT_NAMES["warp"]) or (mask & OBJINT_NAMES["stair"]):
                # "warp": {"room": ..., "tile": [x, y]} — the room resolves
                # to a ROOMS[] index once every room is loaded (aux1).
                # Stair objects carry the same payload shape.
                warp_spec = entry.get("warp") or {}
                warp_room_raw = str(warp_spec.get("room") or "")
                tile = warp_spec.get("tile") or [0, 0]
                aux2 = int(tile[0])
                aux3 = int(tile[1])
            if mask & OBJINT_NAMES["chest"]:
                kind = entry.get("kind") or "normal"
                if kind in CHEST_KINDS:
                    aux1 = CHEST_KINDS[kind]
                else:
                    warn("room '%s' chest has unknown kind '%s' %s"
                         % (stem, kind, sorted(CHEST_KINDS)))
                    aux1 = 0
                grant = entry.get("grant") or {}
                grant_kind = grant.get("kind") or "rupees"
                if grant_kind not in GRANT_KINDS:
                    warn("room '%s' chest has unknown grant kind '%s' %s"
                         % (stem, grant_kind, sorted(GRANT_KINDS)))
                    aux2, aux3 = 1, 1
                else:
                    aux2 = GRANT_KINDS[grant_kind]
                    aux3 = int(grant.get("amount") or 1)
                    if grant_kind == "item":
                        item_name = grant.get("item") or ""
                        aux3 = ITEM_INDEX.get(item_name, -1)
                        if aux3 < 0:
                            warn("room '%s' chest grants unknown item '%s'"
                                 % (stem, item_name))
                    if grant_kind == "flag":
                        # Story-flag grant (e.g. unlocking item upgrades):
                        # param = flag index from the shared registry.
                        flag_name = grant.get("flag") or ""
                        if flags is not None and flag_name:
                            aux3 = flags.index(flag_name)
                        else:
                            warn("room '%s' chest grants a flag without a"
                                 " registry" % stem)
                            aux3 = -1
                    if grant_kind == "bottle":
                        aux3 = 0  # empty
                    if grant_kind == "potion_red":
                        aux3 = 2
                    if grant_kind == "potion_green":
                        aux3 = 3
                    if grant_kind == "potion_blue":
                        aux3 = 4
            objects.append({"x": int(location[0]), "y": int(location[1]),
                            "type": index, "aux1": aux1, "aux2": aux2, "aux3": aux3,
                            "path": obj_path,
                            "warpRoomRaw": warp_room_raw})

        # Placed pickups ("hearts in grass", buried rupees): spawned fresh
        # on every room entry, like LttP field pickups.
        pickup_spawns = []
        for entry in data.get("pickups") or []:
            if not isinstance(entry, dict):
                continue
            kind_name = entry.get("kind") or ""
            if kind_name not in PICKUP_KINDS:
                warn("room '%s' pickup has unknown kind '%s' %s"
                     % (stem, kind_name, sorted(PICKUP_KINDS)))
                continue
            location = entry.get("location") or [0, 0]
            pickup_spawns.append({"x": int(location[0]), "y": int(location[1]),
                                 "kind": PICKUP_KINDS[kind_name]})

        # Buried treasure ("buried"): invisible until the shovel turns the
        # tile over; spawned once per visit (dug spots stay empty).
        buried_spawns = []
        for entry in data.get("buried") or []:
            if not isinstance(entry, dict):
                continue
            kind_name = entry.get("kind") or ""
            if kind_name not in PICKUP_KINDS:
                warn("room '%s' buried item has unknown kind '%s' %s"
                     % (stem, kind_name, sorted(PICKUP_KINDS)))
                continue
            location = entry.get("location") or [0, 0]
            buried_spawns.append({"x": int(location[0]), "y": int(location[1]),
                                 "kind": PICKUP_KINDS[kind_name]})
        if len(buried_spawns) > 16:
            warn("room '%s' buries %d items; the dig registry holds 16"
                 % (stem, len(buried_spawns)))

        npc_spawns = []
        for entry in data.get("npcs") or []:
            if not isinstance(entry, dict):
                continue
            character_name = entry.get("character") or ""
            character = character_index.get(character_name, -1)
            if character < 0:
                warn("room '%s' npc references unknown character '%s'" % (stem, character_name))
                continue
            dialogue_name = entry.get("dialogue")
            dialogue_idx = -1
            if dialogue_name:
                dialogue_idx = dialogue_index.get(dialogue_name, -1)
                if dialogue_idx < 0:
                    warn("room '%s' npc references unknown dialogue '%s'" % (stem, dialogue_name))
            location = entry.get("location") or [0, 0]
            path = []
            for point in entry.get("path") or []:
                if not isinstance(point, (list, tuple)) or len(point) < 2:
                    warn("room '%s' npc path point %r is not [x, y]" % (stem, point))
                    continue
                path.extend([int(point[0]), int(point[1])])
            npc_spawns.append({
                "x": int(location[0]), "y": int(location[1]),
                "character": character, "dialogue": dialogue_idx,
                "wander": bool(entry.get("wander", False)),
                "path": path,
                # Raw flag names; resolved to indices once every dialogue
                # has registered its flags.
                "ifFlag": entry.get("if"),
                "unlessFlag": entry.get("unless"),
            })

        doors = {}
        for edge, spec in (data.get("doors") or {}).items():
            if edge not in ("north", "south", "east", "west"):
                warn("room '%s' doors has unknown edge '%s'" % (stem, edge))
                continue
            kind = spec.get("kind") if isinstance(spec, dict) else spec
            if kind not in DOOR_KINDS:
                warn("room '%s' door '%s' has unknown kind %r %s"
                     % (stem, edge, kind, sorted(DOOR_KINDS)))
                continue
            doors[edge] = DOOR_KINDS[kind]

        # Weather (engine WEATHER_* vocabulary) + ambient tint:
        # "weather": "rain"|"storm"|"fog"; "tint": [r, g, b] multiply
        # (255 each = neutral); "night": true is a blue-dim preset.
        weather_name = str(data.get("weather") or "").lower()
        if weather_name not in WEATHER_KINDS:
            warn("room '%s' has unknown weather %r %s"
                 % (stem, weather_name, sorted(WEATHER_KINDS)))
            weather_kind = 0
        else:
            weather_kind = WEATHER_KINDS[weather_name]
        tint_r, tint_g, tint_b = 255, 255, 255
        tint = data.get("tint")
        if tint is not None:
            try:
                tint_r = int(max(0, min(255, tint[0])))
                tint_g = int(max(0, min(255, tint[1])))
                tint_b = int(max(0, min(255, tint[2])))
            except (TypeError, ValueError, IndexError):
                warn("room '%s' has a bad tint %r (expected [r, g, b])"
                     % (stem, tint))
        if data.get("night"):
            tint_r = min(tint_r, 118)
            tint_g = min(tint_g, 126)
            tint_b = min(tint_b, 185)

        rooms.append({
            "name": filename,
            "hero": character_index.get(hero_name, -1),
            "hasSpawn": has_spawn,
            "spawnX": int(spawn[0]) if has_spawn else 0,
            "spawnY": int(spawn[1]) if has_spawn else 0,
            "music": default.get("music") or None,
            "walkSfx": default.get("walk_sfx") or None,
            # Danger layer: crossfaded in while an arena fight lives.
            "dangerMusic": default.get("danger_music") or None,
            "ambient": default.get("ambient") or None,
            "instant": encounter_list("instant"),
            "interactive": encounter_list("interactive"),
            "enemySpawns": enemies,
            "objectSpawns": objects,
            "pickupSpawns": pickup_spawns,
            "buriedSpawns": buried_spawns,
            "hasBoss": room_has_boss,
            "hasChest": any(OBJECT_TYPES[o["type"]]["interactionsMask"]
                            & OBJINT_NAMES["chest"]
                            for o in objects) if objects else False,
            # Boss defeat banks a story flag ("boss_defeated_<stem>") so
            # dialogue can branch on the victory — same flag registry.
            "bossFlag": (flags.index("boss_defeated_%s" % stem,
                                      "rooms/%s.json (boss)" % stem)
                         if (room_has_boss and flags is not None) else -1),
            "npcSpawns": npc_spawns,
            "dark": bool(data.get("dark", False)),
            "weather": weather_kind,
            "tintR": tint_r,
            "tintG": tint_g,
            "tintB": tint_b,
            # Dungeon treatment: defeated spawns stay dead across
            # re-entries (session-lifetime); overworld rooms respawn.
            "dungeon": bool(data.get("dungeon", False)),
            # Pit-landing: falling into a pit tile warps here instead of
            # the safe-tile respawn. Raw room name until rooms resolve.
            "pitRoomRaw": (data.get("pit") or {}).get("room"),
            "pitTileX": int(((data.get("pit") or {}).get("tile") or [0, 0])[0]),
            "pitTileY": int(((data.get("pit") or {}).get("tile") or [0, 0])[1]),
            # Parallel-world pair (the mirror): declared on either side
            # ("mirror": "07.json"); the reciprocal bakes automatically.
            "mirrorRoomRaw": data.get("mirror"),
            "doorN": doors.get("north", -1),
            "doorS": doors.get("south", -1),
            "doorE": doors.get("east", -1),
            "doorW": doors.get("west", -1),
            "mapWidth": map_width,
            "mapHeight": map_height,
            "decor": layers["decor"],
            "colliders": layers["colliders"],
            "upper": layers["upper"],
            # Raw until every room is loaded; resolved to indices below.
            "exitsRaw": data.get("exits") or {},
        })

    # Assign a global id to every encounter so one-shot state can live in
    # engine-global memory that persists across room loads.
    next_id = 0
    for room in rooms:
        for key in ("instant", "interactive"):
            for encounter in room[key]:
                encounter["id"] = next_id
                next_id += 1
    for room in rooms:
        room["encounterTotal"] = next_id

    # Resolve map-graph edges ("exits": {"north": "<room>", ...}) to
    # ROOMS[] indices. Values may be the file name ("02.json") or the stem
    # ("02"); every direction key must be north/south/east/west.
    room_index = {}
    for i, room in enumerate(rooms):
        room_index[room["name"]] = i
        if room["name"].endswith(".json"):
            room_index[room["name"][:-5]] = i

    def resolve_exit(room_name, raw):
        if raw is None or raw == "":
            return -1
        name = str(raw)
        if name in room_index:
            return room_index[name]
        if (name + ".json") in room_index:
            return room_index[name + ".json"]
        warn("room '%s' exit references unknown room '%s'" % (room_name, raw))
        return -1

    for room in rooms:
        raw = room.pop("exitsRaw")
        for key in raw:
            if key not in ("north", "south", "east", "west"):
                warn("room '%s' exits has unknown direction '%s' "
                     "(north/south/east/west)" % (room["name"], key))
        room["exitN"] = resolve_exit(room["name"], raw.get("north"))
        room["exitS"] = resolve_exit(room["name"], raw.get("south"))
        room["exitE"] = resolve_exit(room["name"], raw.get("east"))
        room["exitW"] = resolve_exit(room["name"], raw.get("west"))

    # Pit-landing targets resolve like exits (raw name -> ROOMS[] index).
    for room in rooms:
        room["pitRoom"] = resolve_exit(room["name"], room.pop("pitRoomRaw"))

    # Mirror pairs (the parallel world): each side lands on its partner.
    # A declaration on one side bakes the reciprocal on the other; two
    # rooms declaring different partners for the same room warn (the
    # first pairing wins).
    mirror_targets = {}
    for room in rooms:
        raw = room.pop("mirrorRoomRaw")
        room["mirrorRoom"] = resolve_exit(room["name"], raw) if raw else -1
        if room["mirrorRoom"] >= 0:
            mirror_targets.setdefault(room["mirrorRoom"], []).append(room["name"])
    for i, room in enumerate(rooms):
        if room["mirrorRoom"] >= 0:
            continue
        decls = mirror_targets.get(i)
        if not decls:
            continue
        if len(decls) > 1:
            warn("rooms %s all mirror into '%s' — the first pairing wins"
                 % (", ".join(decls), room["name"]))
        room["mirrorRoom"] = room_index[decls[0]]

    # Symmetry check: a declared pair whose target points elsewhere is a
    # data smell (the mirror would not round-trip).
    for i, room in enumerate(rooms):
        target = room["mirrorRoom"]
        if target < 0:
            continue
        if rooms[target]["mirrorRoom"] != i:
            warn("room '%s' mirrors into '%s', which pairs elsewhere — "
                 "mirror travel will not round-trip"
                 % (room["name"], rooms[target]["name"]))

    # Resolve warp-pad targets ("warp": {"room": ...}) to ROOMS[] indices.
    for room in rooms:
        for obj in room["objectSpawns"]:
            raw = obj.pop("warpRoomRaw")
            if raw is None:
                continue
            obj["aux1"] = resolve_exit(room["name"], raw)
    return rooms


def emit_array(lines, ctype, name, values, per_line=16):
    lines.append("static const %s %s[%d] = {" % (ctype, name, len(values)))
    for i in range(0, len(values), per_line):
        chunk = values[i:i + per_line]
        lines.append("    " + ", ".join(str(v) for v in chunk) + ",")
    lines.append("};")
    lines.append("")


def emit_dialogues(c, dialogues):
    for dialogue in dialogues:
        base = "DIALOGUE_%s" % sym(dialogue["name"])
        line_refs = []

        for li, line in enumerate(dialogue["lines"]):
            lbase = "%s_L%d" % (base, li)

            if line["set"]:
                emit_array(c, "int", lbase + "_SET", line["set"], per_line=12)
            if line["clear"]:
                emit_array(c, "int", lbase + "_CLEAR", line["clear"], per_line=12)

            choice_refs = []
            for ci, choice in enumerate(line["choices"]):
                cbase = "%s_C%d" % (lbase, ci)
                if choice["set"]:
                    emit_array(c, "int", cbase + "_SET", choice["set"], per_line=12)
                if choice["clear"]:
                    emit_array(c, "int", cbase + "_CLEAR", choice["clear"], per_line=12)
                choice_refs.append("    { %s, %d, %s, %d, %s, %d, %s, %d, %d },"
                                   % (cstr(choice["text"]), choice["gotoLine"],
                                      (cbase + "_SET") if choice["set"] else "0",
                                      len(choice["set"]),
                                      (cbase + "_CLEAR") if choice["clear"] else "0",
                                      len(choice["clear"]),
                                      cstr(choice["sfx"]) if choice["sfx"] else "0",
                                      choice.get("give", -1),
                                      choice.get("cost", 0)))

            if line["choices"]:
                c.append("static const DialogueChoice %s_CHOICES[] = {" % lbase)
                for ref in choice_refs:
                    c.append(ref)
                c.append("};")
                c.append("")

            line_refs.append(
                "    { %s, %d, %d, %d, %s, %d, %s, %d, %s, %s, %d, %d, %d, %d, %d, %d, %d, %s, "
                "%s, %d, %d, %d, %s, %d, %d, %d, %d, %s },"
                % (cstr(line["text"]), line["gotoLine"], line["if"], line["unless"],
                   (lbase + "_SET") if line["set"] else "0", len(line["set"]),
                   (lbase + "_CLEAR") if line["clear"] else "0", len(line["clear"]),
                   cstr(line["sfx"]) if line["sfx"] else "0",
                   cstr(line["music"]) if line["music"] is not None else "0",
                   line.get("warpRoomIndex", -1),
                   line["warpTile"][0] if line["warpTile"] else 0,
                   line["warpTile"][1] if line["warpTile"] else 0,
                   line["ifItem"], line["unlessItem"], line["give"],
                   len(line["choices"]),
                   (lbase + "_CHOICES") if line["choices"] else "0",
                   cfloat(line["wait"]),
                   line["camAction"], line["camTileX"], line["camTileY"],
                   cfloat(line["camSeconds"]),
                   line["letterbox"], line["moveActor"],
                   line["moveTileX"], line["moveTileY"],
                   cfloat(line["moveSeconds"])))

        if line_refs:
            c.append("static const DialogueLine %s_LINES[] = {" % base)
            for ref in line_refs:
                c.append(ref)
            c.append("};")
        else:
            c.append("static const DialogueLine %s_LINES[] = { { 0, -1, -1, -1, 0, 0, 0, 0, 0, 0, -1, 0, 0, -1, -1, -1, 0, 0, "
                     "0.0f, 0, 0, 0, 0.0f, -1, -1, 0, 0, 0.0f } };" % base)
        c.append("")

    c.append("const Dialogue DIALOGUES[] = {")
    for dialogue in dialogues:
        c.append("    { %s, %d, DIALOGUE_%s_LINES, %s, %d },"
                 % (cstr(dialogue["name"]), len(dialogue["lines"]),
                    sym(dialogue["name"]),
                    "true" if dialogue["credits"] else "false",
                    dialogue["nextIndex"]))
    c.append("};")
    c.append("const int DIALOGUE_COUNT = %d;" % len(dialogues))
    c.append("")


def emit_character_frame_arrays(c, base, anims):
    for kind in ("stand", "move"):
        for index, direction in enumerate(DIRECTIONS):
            values = anims[kind][index]
            if values:
                emit_array(c, "int", "%s_%s_%s" % (base, kind.upper(), direction.upper()), values, per_line=24)


def emit_character_config_entries(c, base, character, extra):
    c.append("    { %s, %s, %d, %d," % (cstr(character["name"]), cstr(character["image"]),
                                       character["columns"], character["frameCount"]))
    for kind in ("stand", "move"):
        frames = []
        for index, direction in enumerate(DIRECTIONS):
            values = character["anims"][kind][index]
            reference = "0"
            if values:
                reference = "%s_%s_%s" % (base, kind.upper(), direction.upper())
            frames.append("{ %d, %s }" % (len(values), reference))
        c.append("        { %s }," % ", ".join(frames))
    if extra:
        c.append("      " + extra)
    c.append("    },")


def emit_characters(c, characters):
    for character in characters:
        base = "CHAR_%s" % sym(character["name"])
        emit_character_frame_arrays(c, base, character["anims"])

    c.append("const CharacterConfig CHARACTERS[] = {")
    for character in characters:
        base = "CHAR_%s" % sym(character["name"])
        emit_character_config_entries(c, base, character, None)
    c.append("};")
    c.append("const int CHARACTER_COUNT = %d;" % len(characters))
    c.append("")


def cfloat(value):
    """Emit a C float literal ("2.2f", "0.0f")."""
    text = "%.6g" % float(value)
    if "." not in text and "e" not in text and "inf" not in text and "nan" not in text:
        text += ".0"
    return text + "f"


def emit_enemies(c, enemies):
    for enemy in enemies:
        base = "ENEMY_%s" % sym(enemy["name"])
        emit_character_frame_arrays(c, base, enemy["anims"])

        # Boss phase tables (ascending hp_below; first threshold matched wins).
        if enemy["phases"]:
            c.append("static const EnemyPhase %s_PHASES[] = {" % base)
            entries = ", ".join("{ %s, %s }" % (cfloat(hp), cfloat(scale))
                                for hp, scale in enemy["phases"])
            c.append("    %s," % entries)
            c.append("};")
            c.append("")
        # Per-enemy drop tables (kind -> weight); 0 = the engine default roll.
        if enemy["drops"]:
            c.append("static const DropEntry %s_DROPS[] = {" % base)
            entries = ", ".join("{ %d, %d }" % (kind, weight)
                                for kind, weight in enemy["drops"])
            c.append("    %s," % entries)
            c.append("};")
            c.append("")

    c.append("const EnemyConfig ENEMIES[] = {")
    for enemy in enemies:
        base = "ENEMY_%s" % sym(enemy["name"])
        extra = (
            "%d, %d, %d, "
            "%s, %s, %s, "
            "%d, %s, %s, "
            "%s, %d, "
            "%s, %s, "
            "%s, %s, %s, %s, %d"
            % (enemy["hp"], enemy["speed"], enemy["damage"],
               cfloat(enemy["chargeSight"]), cfloat(enemy["chargeScale"]),
               "true" if enemy["chargeStun"] else "false",
               enemy["shootPeriod"], cfloat(enemy["walkSegment"]),
               "true" if enemy["turnInPlace"] else "false",
               (base + "_PHASES") if enemy["phases"] else "0",
               len(enemy["phases"]),
               "true" if enemy["boss"] else "false",
               "true" if enemy["miniBoss"] else "false",
               "true" if enemy["strikeImmune"] else "false",
               "true" if enemy["catchable"] else "false",
               cstr(enemy["custom"]) if enemy["custom"] else "0",
               (base + "_DROPS") if enemy["drops"] else "0",
               len(enemy["drops"]))
        )
        emit_character_config_entries(c, base, enemy, extra)
    c.append("};")
    c.append("const int ENEMY_COUNT = %d;" % len(enemies))
    c.append("")


def emit_objects(c, object_types):
    """Object types: name, sprite sheet, and the baked interaction mask
    (engine vocabulary). Room objects reference types by index; the engine
    interprets the interactions, never the type identity."""
    c.append("const ObjectConfig OBJECTS[] = {")
    for entry in object_types:
        c.append("    { %s, %s, 0x%X },"
                 % (cstr(entry["name"]),
                    cstr("assets/objects/%s" % entry["image"]),
                    entry["interactionsMask"]))
    c.append("};")
    c.append("const int OBJECT_COUNT = %d;" % len(object_types))
    c.append("")


def emit_items(c, items):
    """Item configs: subscreen label, use-action (engine vocabulary +
    costs + sfx name), passive flags. The engine interprets these; the
    per-item switch is gone."""
    c.append("const ItemConfig ITEMS[] = {")
    for item in items:
        c.append("    { %s, %s, %d, %d, %d, %d, %d, %s, %s, %s, %d, %s, %s, %s, %d, %d, %d, %d },"
                 % (cstr(item["name"]), cstr(item["label"]),
                    item["useKind"], item["element"],
                    item["magicCost"], item["bombCost"], item["arrowCost"],
                    "true" if item["arrowRupeeFallback"] else "false",
                    cstr(item["sfx"]) if item["sfx"] else "0",
                    "true" if item["light"] > 0 else "false",
                    item["light"],
                    "true" if item["swim"] else "false",
                    "true" if item["lift"] else "false",
                    "true" if item["compass"] else "false",
                    item["upgradeFlagIdx"],
                    item["upgradeSpeed"], item["upgradeDamage"],
                    item["spawnObjectIdx"]))
    c.append("};")
    c.append("const int ITEM_COUNT = %d;" % len(items))
    c.append("")


def emit_rules(c):
    """Rule tables for door kinds, chest kinds, and grant kinds. The engine
    consults these instead of switching on kind constants: kinds are data."""
    c.append("const DoorRule DOOR_RULES[] = {")
    for entry in DOOR_KIND_DEFS:
        rule = entry["rule"]
        c.append("    { %s, %s, %d, %s, %s },  // %s"
                 % ("true" if rule.get("starts_open") else "false",
                    "true" if rule.get("push_open") else "false",
                    1 if rule.get("key") == "small" else (2 if rule.get("key") == "big" else 0),
                    "true" if rule.get("bombable") else "false",
                    "true" if rule.get("live") == "enemies_dead" else "false",
                    entry["name"]))
    c.append("};")
    c.append("const int DOOR_KIND_COUNT = %d;" % len(DOOR_KIND_DEFS))
    c.append("")
    c.append("const ChestRule CHEST_RULES[] = {")
    for entry in CHEST_KIND_DEFS:
        c.append("    { %d },  // %s" % (entry["key_cost"], entry["name"]))
    c.append("};")
    c.append("const int CHEST_KIND_COUNT = %d;" % len(CHEST_KIND_DEFS))
    c.append("")
    c.append("const GrantRule GRANT_RULES[] = {")
    for entry in GRANT_KIND_DEFS:
        c.append("    { %d, %d, %s },  // %s"
                 % (entry["effect"], entry["pickup"], cstr(entry["text"]),
                    entry["name"]))
    c.append("};")
    c.append("const int GRANT_KIND_COUNT = %d;" % len(GRANT_KIND_DEFS))
    c.append("")


def emit_game(c, game, manifest):
    """Game identity + the game-code behavior manifest (resources/code/):
    the names game code registers; JSON references them as "custom:<name>"."""
    c.append("const char* const GAME_TITLE = %s;" % cstr(game["title"]))
    c.append("const char* const GAME_SLUG = %s;" % cstr(game["slug"]))
    c.append("")
    c.append("const char* const CUSTOM_BEHAVIOR_NAMES[] = {")
    for name in manifest:
        c.append("    %s," % cstr(name))
    c.append("};")
    c.append("const int CUSTOM_BEHAVIOR_COUNT = %d;" % len(manifest))
    c.append("")


def emit_tile_meta(c, meta):
    if meta:
        c.append("const TileMeta TILE_META[] = {")
        per_line = 6
        for i in range(0, len(meta), per_line):
            chunk = meta[i:i + per_line]
            c.append("    " + " ".join(
                "{ %d, %d, %d, %d, %d, %d, %d, %d }," % (m["flags"], m["damage"],
                                                        m["cutResult"], m["bombResult"],
                                                        m["pushX"], m["pushY"], m["ledgeDir"],
                                                        m["heal"])
                for m in chunk))
        c.append("};")
    else:
        c.append("const TileMeta TILE_META[] = { { 0, 0, 0, 255, 0, 0, 0, 0 } };")
    c.append("const int TILE_META_COUNT = %d;" % len(meta))
    c.append("")


def emit_rooms(c, rooms):
    for room in rooms:
        base = "ROOM_%s" % sym(room["name"])
        if room["decor"]:
            emit_array(c, "unsigned short", base + "_DECOR", room["decor"], per_line=12)
        if room["colliders"]:
            emit_array(c, "unsigned short", base + "_COLLIDERS", room["colliders"], per_line=12)
        if any(room["upper"]):
            emit_array(c, "unsigned short", base + "_UPPER", room["upper"], per_line=12)

        for key in ("instant", "interactive"):
            if room[key]:
                name = "%s_%s" % (base, key.upper())
                c.append("static const Encounter %s[%d] = {" % (name, len(room[key])))
                entries = ", ".join("{ %d, %d, %d, %d, %s, %d, %d }"
                                    % (e["x"], e["y"], e["dialogue"], e["id"],
                                       "true" if e["repeat"] else "false",
                                       e["ifFlagIdx"], e["unlessFlagIdx"])
                                    for e in room[key])
                c.append("    %s," % entries)
                c.append("};")
                c.append("")

        if room["enemySpawns"]:
            name = "%s_ENEMIES" % base
            c.append("static const EnemySpawn %s[%d] = {" % (name, len(room["enemySpawns"])))
            entries = ", ".join("{ %d, %d, %d }" % (e["x"], e["y"], e["enemy"])
                                for e in room["enemySpawns"])
            c.append("    %s," % entries)
            c.append("};")
            c.append("")

        if room["objectSpawns"]:
            # Platform rails bake as per-object short arrays (x,y pairs),
            # like NPC paths.
            for oi, o in enumerate(room["objectSpawns"]):
                if o.get("path"):
                    emit_array(c, "short", "%s_OBJ%d_PATH" % (base, oi), o["path"],
                               per_line=12)
            name = "%s_OBJECTS" % base
            c.append("static const ObjectSpawn %s[%d] = {" % (name, len(room["objectSpawns"])))
            entries = ", ".join(
                "{ %d, %d, %d, %d, %d, %d, %s, %d }"
                % (o["x"], o["y"], o["type"], o["aux1"], o["aux2"],
                   o.get("aux3", 0),
                   ("%s_OBJ%d_PATH" % (base, oi)) if o.get("path") else "0",
                   len(o.get("path") or []) // 2)
                for oi, o in enumerate(room["objectSpawns"]))
            c.append("    %s," % entries)
            c.append("};")
            c.append("")

        if room["pickupSpawns"]:
            name = "%s_PICKUPS" % base
            c.append("static const PickupSpawn %s[%d] = {" % (name, len(room["pickupSpawns"])))
            entries = ", ".join("{ %d, %d, %d }" % (p["x"], p["y"], p["kind"])
                                for p in room["pickupSpawns"])
            c.append("    %s," % entries)
            c.append("};")
            c.append("")

        if room["buriedSpawns"]:
            name = "%s_BURIED" % base
            c.append("static const PickupSpawn %s[%d] = {" % (name, len(room["buriedSpawns"])))
            entries = ", ".join("{ %d, %d, %d }" % (p["x"], p["y"], p["kind"])
                                for p in room["buriedSpawns"])
            c.append("    %s," % entries)
            c.append("};")
            c.append("")

        if room["npcSpawns"]:
            # Path waypoints bake as per-NPC short arrays (x,y pairs).
            for ni, n in enumerate(room["npcSpawns"]):
                if n["path"]:
                    emit_array(c, "short", "%s_NPC%d_PATH" % (base, ni), n["path"],
                               per_line=12)
            name = "%s_NPCS" % base
            c.append("static const NpcSpawn %s[%d] = {" % (name, len(room["npcSpawns"])))
            entries = ", ".join("{ %d, %d, %d, %d, %s, %s, %d, %d, %d }"
                                % (n["x"], n["y"], n["character"], n["dialogue"],
                                   "true" if n["wander"] else "false",
                                   ("%s_NPC%d_PATH" % (base, ni)) if n["path"] else "0",
                                   len(n["path"]) // 2,
                                   n["ifFlag"], n["unlessFlag"])
                                for ni, n in enumerate(room["npcSpawns"]))
            c.append("    %s," % entries)
            c.append("};")
            c.append("")

    c.append("const RoomConfig ROOMS[] = {")
    for room in rooms:
        base = "ROOM_%s" % sym(room["name"])

        decor_ref = (base + "_DECOR") if room["decor"] else "0"
        colliders_ref = (base + "_COLLIDERS") if room["colliders"] else "0"
        upper_ref = (base + "_UPPER") if any(room["upper"]) else "0"

        instant_ref = "0"
        if room["instant"]:
            instant_ref = "%s_INSTANT" % base
        interactive_ref = "0"
        if room["interactive"]:
            interactive_ref = "%s_INTERACTIVE" % base
        enemy_ref = "0"
        if room["enemySpawns"]:
            enemy_ref = "%s_ENEMIES" % base
        object_ref = "0"
        if room["objectSpawns"]:
            object_ref = "%s_OBJECTS" % base
        npc_ref = "0"
        if room["npcSpawns"]:
            npc_ref = "%s_NPCS" % base
        pickup_ref = "0"
        if room["pickupSpawns"]:
            pickup_ref = "%s_PICKUPS" % base
        buried_ref = "0"
        if room["buriedSpawns"]:
            buried_ref = "%s_BURIED" % base

        # Field list built programmatically: no positional format string to
        # keep in sync with the RoomConfig member order.
        fields = [
            cstr(room["name"]),
            str(room["hero"]),
            "true" if room["hasSpawn"] else "false",
            str(room["spawnX"]),
            str(room["spawnY"]),
            cstr(room["music"]) if room["music"] else "0",
            cstr(room["walkSfx"]) if room["walkSfx"] else "0",
            cstr(room["ambient"]) if room["ambient"] else "0",
            cstr(room["dangerMusic"]) if room["dangerMusic"] else "0",
            instant_ref, str(len(room["instant"])),
            interactive_ref, str(len(room["interactive"])),
            enemy_ref, str(len(room["enemySpawns"])),
            object_ref, str(len(room["objectSpawns"])),
            npc_ref, str(len(room["npcSpawns"])),
            pickup_ref, str(len(room["pickupSpawns"])),
            buried_ref, str(len(room["buriedSpawns"])),
            "true" if room["hasBoss"] else "false",
            "true" if room["hasChest"] else "false",
            str(room["bossFlag"]),
            "true" if room["dark"] else "false",
            str(room["weather"]),
            str(room["tintR"]), str(room["tintG"]), str(room["tintB"]),
            str(room["doorN"]), str(room["doorS"]),
            str(room["doorE"]), str(room["doorW"]),
            str(room["mapWidth"]), str(room["mapHeight"]),
            decor_ref, colliders_ref, upper_ref,
            str(room["exitN"]), str(room["exitS"]),
            str(room["exitE"]), str(room["exitW"]),
            "true" if room["dungeon"] else "false",
            str(room["pitRoom"]), str(room["pitTileX"]), str(room["pitTileY"]),
            str(room["mirrorRoom"]),
        ]
        c.append("    { " + ", ".join(fields) + " },")
    c.append("};")
    c.append("const int ROOM_COUNT = %d;" % len(rooms))
    c.append("")
    total_encounters = sum(len(r["instant"]) + len(r["interactive"]) for r in rooms)
    c.append("const int ENCOUNTER_TOTAL = %d;" % total_encounters)
    c.append("")


def write_png(path, width, height, rows):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + bytes(row) for row in rows)
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    with open(path, "wb") as handle:
        handle.write(b"\x89PNG\r\n\x1a\n")
        handle.write(chunk(b"IHDR", header))
        handle.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        handle.write(chunk(b"IEND", b""))


def create_border_bitmap(path):
    if os.path.exists(path):
        return False

    size = 24
    outline = (8, 8, 24, 255)
    light = (248, 248, 216, 255)
    fill = (16, 24, 112, 255)
    clear = (0, 0, 0, 0)
    corner = 7.0

    rows = []
    for y in range(size):
        row = bytearray()
        for x in range(size):
            dx = float(min(x, size - 1 - x))
            dy = float(min(y, size - 1 - y))
            if dx <= corner and dy <= corner:
                distance = corner - math.hypot(dx - corner, dy - corner)
            else:
                distance = min(dx, dy)
            if distance < 0.5:
                pixel = outline
            elif distance < 2.5:
                pixel = light
            elif distance < 3.5:
                pixel = outline
            else:
                pixel = fill
            row.extend(pixel)
        rows.append(row)

    write_png(path, size, size, rows)
    return True


def create_sword_bitmap(path):
    """Placeholder slash-arc sheet: 4 columns (down, right, left, up) x 3 rows
    (sweep frames), 64x64 per frame so the arc reaches past the 32x32 hero."""
    if os.path.exists(path):
        return False

    frame = 64
    columns = ["down", "right", "left", "up"]
    base_angle = {"down": 90.0, "right": 0.0, "left": 180.0, "up": 270.0}
    sweep_sign = {"down": 1.0, "right": 1.0, "left": -1.0, "up": -1.0}

    steel = (188, 196, 210, 255)
    bright = (240, 244, 252, 255)
    clear = (0, 0, 0, 0)

    # Build all 12 frames (column = direction, row = frame index).
    frames = []
    for f in range(3):
        t = (f + 0.5) / 3.0
        for col, direction in enumerate(columns):
            center_angle = base_angle[direction] + sweep_sign[direction] * (t - 0.5) * 2.0 * 55.0
            image = []
            for y in range(frame):
                row = bytearray()
                for x in range(frame):
                    dx = x - 31.5
                    dy = y - 31.5
                    radius = math.hypot(dx, dy)
                    pixel = clear
                    if radius < 34.0:
                        angle = math.degrees(math.atan2(dy, dx))
                        delta = (angle - center_angle + 180.0) % 360.0 - 180.0
                        if abs(delta) < 26.0 and 22.0 < radius < 30.0:
                            pixel = steel
                            if abs(delta) < 14.0 and 24.0 < radius < 28.0:
                                pixel = bright
                    row.extend(pixel)
                image.append(row)
            frames.append((col, f, image))

    # Compose into a 4-column sheet.
    width = frame * 4
    sheet = [bytearray(width * 4) for _ in range(frame * 3)]
    for col, f, image in frames:
        for y in range(frame):
            row = sheet[f * frame + y]
            base = col * frame * 4
            row[base:base + frame * 4] = image[y]

    write_png(path, width, frame * 3, [bytes(r) for r in sheet])
    return True


def create_hearts_bitmap(path):
    """Placeholder heart gauge: 3 frames (full, half, empty), 16x16 each."""
    if os.path.exists(path):
        return False

    def heart_field(x, y):
        # Classic implicit heart, y up, centered at (8, 8.5).
        u = (x - 8.0) / 5.2
        v = -(y - 8.5) / 5.2
        f = (u * u + v * v - 1.0) ** 3 - u * u * v * v * v
        return f

    full_red = (216, 40, 48, 255)
    glint = (255, 168, 168, 255)
    dark = (88, 16, 24, 255)
    empty_fill = (58, 12, 16, 255)
    outline = (24, 8, 12, 255)
    clear = (0, 0, 0, 0)

    def render(filled_side):
        image = []
        for y in range(16):
            row = bytearray()
            for x in range(16):
                f = heart_field(x + 0.5, y + 0.5)
                pixel = clear
                filled = (filled_side == "both"
                          or (filled_side == "left" and x < 8)
                          or (filled_side == "right" and x >= 8))
                if f < -0.25:
                    if filled:
                        pixel = full_red
                        if 3 <= x <= 5 and 4 <= y <= 6:
                            pixel = glint
                    else:
                        pixel = empty_fill
                elif f < 0.45:
                    pixel = outline
                row.extend(pixel)
            image.append(row)
        return image

    frames = [render("both"), render("left"), render("none")]

    width = 16 * 3
    sheet = []
    for y in range(16):
        row = bytearray()
        for frame_image in frames:
            row.extend(frame_image[y])
        sheet.append(bytes(row))

    write_png(path, width, 16, sheet)
    return True


def create_enemy_bitmap(path, color):
    """Placeholder enemy sheet: 4 columns (down, right, left, up) x 2 rows
    (stand, move), 32x32 frames. Stand frames are the sheet's base frame for
    each direction; move frames repeat with a squash. A generic mob blob with
    eyes that face each direction."""
    if os.path.exists(path):
        return False

    frame = 32
    r, g, b = color
    dark = (int(r * 0.45), int(g * 0.45), int(b * 0.45), 255)
    body = (r, g, b, 255)
    light = (min(255, int(r * 1.3)), min(255, int(g * 1.3)), min(255, int(b * 1.3)), 255)
    white = (240, 240, 240, 255)
    pupil = (20, 20, 30, 255)
    clear = (0, 0, 0, 0)

    def render(column, move):
        image = []
        # Pupil offset per facing direction (down/right/left/up).
        pupil_offsets = [(0, 1), (1, 0), (-1, 0), (0, -1)]
        pox, poy = pupil_offsets[column]
        squash = 1.6 if move else 0.0
        for y in range(frame):
            row = bytearray()
            for x in range(frame):
                dx = x - 15.5
                dy = y - (18.0 + squash)
                rx = 10.5
                ry = 9.5 - squash * 0.4
                pixel = clear
                e = (dx * dx) / (rx * rx) + (dy * dy) / (ry * ry)
                if e < 0.82:
                    pixel = light if e < 0.4 else body
                    # eyes (skip pupils when facing up: back of the head)
                    ex = 0
                    if abs(x - 11) <= 1: ex = -1
                    elif abs(x - 20) <= 1: ex = 1
                    if ex != 0 and 12 <= y <= 14:
                        pixel = white
                        if abs(y - 13) <= 1 and abs((x + pox) - (15.5 + ex * 4.5)) <= 1 and column != 3:
                            if abs(y - 13 + poy) <= 1:
                                pixel = pupil
                elif e < 1.05:
                    pixel = dark
                row.extend(pixel)
            image.append(row)
        return image

    frames = []
    for row in (0, 1):
        for column in range(4):
            frames.append((column, row, render(column, row == 1)))

    width = frame * 4
    sheet = [bytearray(width * 4) for _ in range(frame * 2)]
    for column, row_index, image in frames:
        base = column * frame * 4
        for y in range(frame):
            sheet[row_index * frame + y][base:base + frame * 4] = image[y]

    write_png(path, width, frame * 2, [bytes(r_) for r_ in sheet])
    return True


def create_character_bitmap(path, seed_name):
    """Placeholder character sheet for NPCs: 4 columns (down, right, left,
    up) x 2 rows (stand, move), 32x32 frames — a cloaked townsfolk blob.
    Row 1 repeats with a slight lean so walk cycles alternate."""
    if os.path.exists(path):
        return False

    # Stable palette from the name so different NPCs read differently.
    hash_value = zlib.crc32(seed_name.encode("utf-8"))
    hue = (hash_value % 360) / 360.0
    import colorsys
    r, g, b = colorsys.hsv_to_rgb(hue, 0.45, 0.72)
    shade = colorsys.hsv_to_rgb(hue, 0.55, 0.40)
    body = (int(r * 255), int(g * 255), int(b * 255), 255)
    dark = (int(shade[0] * 255), int(shade[1] * 255), int(shade[2] * 255), 255)
    skin = (232, 184, 144, 255)
    clear = (0, 0, 0, 0)

    def render(column, move):
        image = []
        lean = 1.2 if move else 0.0
        for y in range(32):
            row = bytearray()
            for x in range(32):
                dx = x - 15.5 + lean
                dy = y - 19.0
                # robe
                robe = (dx * dx) / (150.0) + (dy * dy) / (190.0)
                # head
                hx = x - 15.5
                hy = y - 11.0
                head = (hx * hx) / (36.0) + (hy * hy) / (30.0)
                pixel = clear
                if head < 1.0:
                    pixel = skin
                    # simple eyes on the down/right/left frames
                    if column != 3 and 14 <= y <= 15:
                        if abs(hx - 3.0) <= 1.2 or abs(hx + 3.0) <= 1.2:
                            pixel = (30, 24, 40, 255)
                elif robe < 1.0:
                    pixel = body if robe > 0.4 else (255, 255, 255, 0)
                    if robe < 0.4:
                        pixel = dark
                elif robe < 1.15:
                    pixel = dark
                row.extend(pixel if pixel[3] else clear)
            image.append(row)
        return image

    frames = [render(column, row == 1) for row in (0, 1) for column in range(4)]
    width = 32 * 4
    sheet = [bytearray(width * 4) for _ in range(32 * 2)]
    for index, image in enumerate(frames):
        column = index % 4
        row_index = index // 4
        base = column * 32 * 4
        for y in range(32):
            sheet[row_index * 32 + y][base:base + 32 * 4] = image[y]

    write_png(path, width, 32 * 2, [bytes(r_) for r_ in sheet])
    return True


def create_object_bitmap(path, kind):
    """Placeholder object sprites, one 32x32 frame each: a clay pot, a
    leafy bush, a push block, a floor switch, a barrier, and a sign."""
    if os.path.exists(path):
        return False

    frame = 32
    clear = (0, 0, 0, 0)

    def pot(x, y):
        # body
        dx = x - 15.5
        dy = y - 19.0
        body = (dx * dx) / (9.0 * 9.0) + (dy * dy) / (10.5 * 10.5)
        rim = (dx * dx) / (7.5 * 7.5) + (dy * dy) / (3.0 * 3.0)
        base = (188, 108, 60, 255)
        dark = (122, 66, 34, 255)
        light = (228, 152, 96, 255)
        if body < 0.92:
            pixel = base if body > 0.35 else light
            if 6 <= y <= 9 and x >= 9 and x <= 22:
                pixel = dark if rim >= 0.5 else (150, 84, 44, 255)  # rim ring
            return pixel
        if body < 1.08:
            return dark
        return clear

    def bush(x, y):
        dx = x - 15.5
        dy = y - 18.5
        blob = (dx * dx) / (12.5 * 12.5) + (dy * dy) / (10.5 * 10.5)
        base = (58, 118, 52, 255)
        dark = (32, 70, 34, 255)
        light = (88, 154, 74, 255)
        if blob < 0.9:
            pixel = base
            if blob < 0.45:
                pixel = light
            # leafy speckles
            if (x * 7 + y * 5) % 23 < 2:
                pixel = dark
            return pixel
        if blob < 1.1:
            return dark
        return clear

    def block(x, y):
        # beveled stone cube filling the tile
        base = (126, 130, 146, 255)
        light = (164, 168, 186, 255)
        dark = (82, 86, 102, 255)
        edge = (52, 56, 70, 255)
        if x < 2 or y < 2 or x > 29 or y > 29:
            return clear
        if x == 2 or y == 2:
            return light
        if x == 29 or y == 29:
            return edge
        if x in (3, 4) and y in range(3, 29):
            return light
        if y in (27, 28) or x in (27, 28):
            return dark
        # chisel marks
        if (x % 8 in (0,) and y % 8 not in (0, 1)):
            return dark
        return base

    def switch(x, y):
        # floor plate with a round button
        plate = (150, 142, 130, 255)
        rim = (104, 98, 90, 255)
        button = (196, 190, 178, 255)
        button_rim = (120, 114, 108, 255)
        dx = x - 15.5
        dy = y - 15.5
        if x < 4 or y < 4 or x > 27 or y > 27:
            return clear
        if x < 6 or y < 6 or x > 25 or y > 25:
            return rim
        d = (dx * dx) / (81.0) + (dy * dy) / (81.0)
        if d < 0.62:
            return button
        if d < 0.85:
            return button_rim
        return plate

    def barrier(x, y):
        # raised spiky gate: pillar body + jagged crest (teeth every 8px)
        base = (96, 88, 132, 255)
        light = (140, 130, 180, 255)
        dark = (58, 52, 88, 255)
        if y < 8:
            tooth = x % 8
            height = 6 - abs(tooth - 3)  # 2..6 px tall
            if y >= 8 - height:
                return light if tooth < 2 else dark
            return clear
        if x < 6 or x > 25:
            return clear
        if x < 8:
            return light
        if x > 23:
            return dark
        if (y % 8) == 0:
            return dark
        return base

    def torch(x, y):
        # iron post with a wide fire bowl on top
        iron = (110, 108, 120, 255)
        iron_dark = (70, 68, 80, 255)
        iron_light = (150, 148, 162, 255)
        bowl = (88, 74, 60, 255)
        if 14 <= x <= 17 and 12 <= y <= 27:
            if y in (12, 27):
                return iron_dark
            return iron if (x + y * 3) % 11 else iron_light
        if 9 <= y <= 13 and 8 <= x <= 23:
            if y in (9, 13) or x in (8, 23):
                return iron_dark
            if y == 10:
                return iron_light
            return bowl
        return clear

    def sign(x, y):
        # wooden sign: post + board with grain
        wood = (168, 120, 72, 255)
        wood_dark = (116, 78, 44, 255)
        wood_light = (198, 152, 100, 255)
        if 14 <= x <= 17 and 16 <= y <= 29:
            return wood_dark if y % 6 == 0 else wood
        if 4 <= y <= 15 and 4 <= x <= 27:
            if y in (4, 5, 14, 15) or x in (4, 5, 26, 27):
                return wood_dark
            if (x + y * 3) % 17 < 2:
                return wood_dark  # grain lines
            if y in (8, 11) and 8 <= x <= 23 and (x % 4) < 2:
                return (60, 44, 28, 255)  # scratched text
            return wood if (x * 5 + y * 3) % 13 else wood_light
        return clear

    def warp(x, y):
        # blue rune pad: square plate with a glowing triple spiral
        plate = (52, 66, 110, 255)
        plate_light = (78, 96, 152, 255)
        plate_dark = (32, 42, 78, 255)
        rune = (130, 205, 255, 255)
        if x < 3 or y < 3 or x > 28 or y > 28:
            return clear
        if x in (3, 28) or y in (3, 28):
            return plate_dark
        if x < 6 or y < 6 or x > 25 or y > 25:
            return plate_light if (x + y) % 7 else plate
        dx = x - 15.5
        dy = y - 15.5
        d = math.sqrt(dx * dx + dy * dy)
        if d < 11.5:
            angle = math.atan2(dy, dx)
            phase = (angle * 3.0 - d * 0.85) % (2.0 * math.pi)
            if min(phase, 2.0 * math.pi - phase) < 0.55:
                return rune
        return plate if (x * 3 + y * 7) % 11 else plate_light

    if kind == "chest":
        # Two frames: 0 closed, 1 open (lid up + dark interior).
        def chest(opened):
            def render(x, y):
                wood = (150, 104, 52, 255)
                wood_dark = (100, 66, 30, 255)
                gold = (216, 168, 60, 255)
                band = (70, 46, 22, 255)
                interior = (34, 22, 12, 255)
                lid_low = 12 if opened else 20
                lid_high = 6 if opened else 10
                if y < 4 or y > 29 or x < 4 or x > 27:
                    return (0, 0, 0, 0)
                if x in (4, 27) or y == 29:
                    return band
                if lid_high <= y <= lid_low:
                    # lid slab (raised on the open frame)
                    if (x + y) % 11 < 2:
                        return wood_dark
                    return wood
                if opened and lid_high + 2 < y < lid_low:
                    return interior
                if y == lid_low + 1:
                    return wood_dark
                # body with gold bands
                if 12 <= x <= 19 and y == 24:
                    return gold  # lock plate
                if x in (11, 20) or y in (26, 27):
                    return wood_dark
                if (x * 3 + y * 5) % 19 < 2:
                    return wood_dark
                return wood
            return render

        rows = []
        for y in range(frame):
            row = bytearray()
            for x in range(frame * 2):
                render = chest(x >= frame)
                row.extend(render(x % frame, y))
            rows.append(bytes(row))
        write_png(path, frame * 2, frame, [bytes(r) for r in rows])
        return True

    def crystal(x, y):
        # stone pedestal with a faceted gem floating above it
        stone = (120, 118, 132, 255)
        stone_dark = (78, 76, 92, 255)
        stone_light = (158, 156, 172, 255)
        gem = (150, 170, 230, 255)
        if 10 <= y <= 28 and 11 <= x <= 20:
            if y == 28 or x in (11, 20) or y == 10:
                return stone_dark
            if y in (11, 12) or x in (12, 13):
                return stone_light
            if (x + y) % 9 < 2:
                return stone_dark
            return stone
        # floating gem (diamond shape) with facets
        dx = x - 15.5
        dy = y - 8.0
        if abs(dx) <= 6 - abs(dy) * 0.5 and abs(dy) <= 5:
            if dx < 0 and dy < 1.0:
                return (198, 214, 250, 255)
            if dx > 0 and dy > 1.0:
                return (108, 130, 198, 255)
            return gem
        return clear

    def rock(x, y):
        # rounded boulder: gray stone with a highlight facet
        dx = x - 15.5
        dy = y - 19.0
        blob = (dx * dx) / (11.0 * 11.0) + (dy * dy) / (9.0 * 9.0)
        base = (134, 128, 118, 255)
        dark = (84, 80, 74, 255)
        light = (176, 172, 162, 255)
        if blob < 0.85:
            pixel = base
            if dx + dy < -6.0:
                pixel = light
            if (x + y * 2) % 17 < 2:
                pixel = dark
            return pixel
        if blob < 1.05:
            return dark
        return clear

    def big_rock(x, y):
        # a chunk of mountain: same boulder, bigger and darker
        dx = x - 15.5
        dy = y - 17.5
        blob = (dx * dx) / (14.0 * 14.0) + (dy * dy) / (12.0 * 12.0)
        base = (110, 104, 96, 255)
        dark = (64, 60, 56, 255)
        light = (150, 146, 138, 255)
        if blob < 0.9:
            pixel = base
            if dx + dy < -8.0:
                pixel = light
            if (x * 2 + y * 3) % 19 < 2:
                pixel = dark
            return pixel
        if blob < 1.08:
            return dark
        return clear

    def platform(x, y):
        # wooden raft: a planked deck with darker seams and worn edges
        wood = (158, 116, 66, 255)
        wood_dark = (104, 74, 40, 255)
        wood_light = (196, 152, 96, 255)
        if x < 3 or y < 9 or x > 28 or y > 29:
            return clear
        if y in (9, 28) or x in (3, 28):
            return wood_dark
        if x in (7, 8) or x in (15, 16) or x in (23, 24):
            return wood_dark  # plank seams
        if y in (13, 14) or y in (19, 20) or y in (24, 25):
            return wood_light  # worn highlights
        if (x + y * 7) % 29 == 0:
            return wood_dark
        return wood

    def hook_post(x, y):
        # a stone pillar with a steel catch-ring the hookshot latches
        stone = (142, 140, 152, 255)
        stone_dark = (92, 90, 104, 255)
        stone_light = (182, 180, 192, 255)
        steel = (176, 182, 196, 255)
        steel_dark = (110, 116, 132, 255)
        # pillar
        if 10 <= y <= 29 and 12 <= x <= 19:
            if y == 29 or x in (12, 19):
                return stone_dark
            if x in (13, 14):
                return stone_light
            if (x * 3 + y * 5) % 17 < 2:
                return stone_dark
            return stone
        # catch-ring above the pillar
        dx = x - 15.5
        dy = y - 7.5
        rr = dx * dx + dy * dy
        if 16 <= rr <= 26:
            return steel
        if 20 <= rr <= 24 and (x + y) % 3 == 0:
            return steel_dark  # ring shadow
        return clear

    def somaria_block(x, y):
        # a rune-crested block: the Somaria conjures it on clear ground
        base = (170, 92, 96, 255)
        dark = (112, 52, 62, 255)
        light = (222, 140, 140, 255)
        rune = (255, 230, 190, 255)
        if x < 3 or y < 4 or x > 28 or y > 28:
            return clear
        if x in (3, 4) and y in range(4, 28):
            return light
        if y in (4, 5) or x in (27, 28) or y in (27, 28) or x == 3 or y == 4:
            return dark
        # the Somaria cross, glowing on the face
        cx = x - 15.5
        cy = y - 15.5
        if (abs(cx) <= 1 and abs(cy) <= 7) or (abs(cy) <= 1 and abs(cx) <= 7):
            if abs(cx) > 5 or abs(cy) > 5:
                return dark
            return rune
        if (x + y * 5) % 17 < 2:
            return dark
        return base

    def shrine(x, y):
        # a stone altar with an offering bowl — magic powder wakes it
        stone = (150, 148, 158, 255)
        stone_dark = (98, 96, 108, 255)
        stone_light = (188, 186, 196, 255)
        bowl = (196, 178, 130, 255)
        # stepped altar base
        if 24 <= y <= 28 and 9 <= x <= 22:
            if y in (24, 25) and 11 <= x <= 20:
                return stone_light
            if y == 28 or x in (9, 22):
                return stone_dark
            return stone
        # altar column
        if 14 <= y <= 23 and 12 <= x <= 19:
            if x in (12, 13):
                return stone_light
            if x in (18, 19) or y == 23:
                return stone_dark
            if (x * 3 + y * 5) % 19 < 2:
                return stone_dark
            return stone
        # offering bowl
        dx = x - 15.5
        dy = y - 11.0
        if dx * dx / (6.5 * 6.5) + dy * dy / (3.0 * 3.0) < 1.0:
            return stone_dark
        if dx * dx / (4.5 * 4.5) + dy * dy / (1.8 * 1.8) < 1.0:
            return bowl
        return clear

    def lever(x, y):
        # wall-mounted pull handle: a plate with a lever leaning left
        # (the engine mirrors the frame when the lever is pulled)
        plate = (126, 118, 134, 255)
        plate_dark = (84, 78, 92, 255)
        plate_light = (166, 158, 174, 255)
        handle = (196, 150, 62, 255)
        handle_dark = (128, 92, 30, 255)
        # plate
        if 12 <= x <= 19 and 10 <= y <= 23:
            if x in (12, 13):
                return plate_light
            if x in (18, 19) or y in (22, 23):
                return plate_dark
            if (x + y * 3) % 13 == 0:
                return plate_dark
            return plate
        # handle pivots from the plate center, leaning left-up
        dx = x - 16.0
        dy = y - 16.5
        along = dx * 0.94 + dy * 0.34  # along the leaning handle axis
        across = -dx * 0.34 + dy * 0.94
        if along < -12.5 or along > -1.0:
            return clear
        if abs(across) > 1.8:
            return clear
        if along > -11.0 and abs(across) < 1.2:
            return handle  # grip
        return handle_dark

    def statue(x, y):        # stone guardian: pedestal, torso column, hooded head
        stone = (150, 148, 158, 255)
        stone_dark = (98, 96, 108, 255)
        stone_light = (188, 186, 196, 255)
        # pedestal
        if 26 <= y <= 28 and 8 <= x <= 23:
            return stone_dark if y == 28 or x in (8, 23) else stone
        # torso column
        if 12 <= y <= 25 and 11 <= x <= 20:
            if x in (11, 12) and y < 24:
                return stone_light
            if x in (19, 20) or y == 25:
                return stone_dark
            if (x * 3 + y * 5) % 21 < 2:
                return stone_dark
            return stone
        # hooded head
        dx = x - 15.5
        dy = y - 8.0
        if dx * dx / (5.5 * 5.5) + dy * dy / (6.0 * 6.0) < 1.0:
            if dx + dy > 3.5:
                return stone_dark
            return stone
        # eye slits
        if 6 <= y <= 7 and 13 <= x <= 18 and (x in (13, 14) or x in (17, 18)):
            return stone_dark
        return clear

    render = {"pot": pot, "bush": bush, "block": block,
              "switch": switch, "barrier": barrier, "sign": sign,
              "torch": torch, "warp": warp, "crystal": crystal,
              "rock": rock, "big_rock": big_rock, "statue": statue,
              "lever": lever, "platform": platform,
              "hook_post": hook_post, "somaria_block": somaria_block,
              "shrine": shrine}.get(kind)
    if render is None:
        return False
    rows = []
    for y in range(frame):
        row = bytearray()
        for x in range(frame):
            row.extend(render(x, y))
        rows.append(bytes(row))

    write_png(path, frame, frame, rows)
    return True


def create_pickups_bitmap(path):
    """Placeholder pickup sheet, 16x16 frames in a row:
    0 heart, 1 rupee (1), 2 rupee (5), 3 rock (enemy projectile),
    4 arrow, 5 bomb, 6 magic, 7 heart container, 8 fairy, 9 small key."""
    if os.path.exists(path):
        return False

    def heart_frame():
        image = []
        for y in range(16):
            row = bytearray()
            for x in range(16):
                u = (x + 0.5 - 8.0) / 5.2
                v = -((y + 0.5) - 8.5) / 5.2
                f = (u * u + v * v - 1.0) ** 3 - u * u * v * v * v
                pixel = (0, 0, 0, 0)
                if f < -0.25:
                    pixel = (216, 40, 48, 255)
                    if 3 <= x <= 5 and 4 <= y <= 6:
                        pixel = (255, 168, 168, 255)
                elif f < 0.45:
                    pixel = (24, 8, 12, 255)
                row.extend(pixel)
            image.append(row)
        return image

    def container_frame():
        # Big heart in a golden setting.
        image = heart_frame()
        for y in range(16):
            for x in range(16):
                dx = x + 0.5 - 8.0
                dy = y + 0.5 - 8.0
                d = math.hypot(dx, dy)
                if 7.2 < d < 8.6:
                    image[y][x * 4:x * 4 + 4] = bytes((216, 168, 60, 255))
        return image

    def rupee_frame(base, shade, outline):
        image = []
        for y in range(16):
            row = bytearray()
            for x in range(16):
                dx = abs(x + 0.5 - 8.0)
                if y < 3 or y > 12:
                    pixel = (0, 0, 0, 0)
                elif y < 5 or y > 10:
                    pixel = outline
                elif dx < 3.0:
                    pixel = outline
                elif dx < 4.2:
                    pixel = base if y < 8 else shade
                else:
                    pixel = (0, 0, 0, 0)
                row.extend(pixel)
            image.append(row)
        return image

    rupee1 = rupee_frame((64, 200, 96, 255), (40, 150, 72, 255), (16, 60, 32, 255))
    rupee5 = rupee_frame((96, 160, 240, 255), (64, 120, 210, 255), (24, 40, 90, 255))

    def rock_frame():
        image = []
        for y in range(16):
            row = bytearray()
            for x in range(16):
                dx = x + 0.5 - 8.0
                dy = y + 0.5 - 8.0
                d = math.hypot(dx, dy)
                pixel = (0, 0, 0, 0)
                if d < 5.8:
                    pixel = (90, 90, 100, 255)
                    if dx < -0.5 and dy < -0.5:
                        pixel = (130, 130, 140, 255)
                    if dx > 1.5 or dy > 1.5:
                        pixel = (60, 60, 68, 255)
                elif d < 6.6:
                    pixel = (40, 40, 46, 255)
                row.extend(pixel)
            image.append(row)
        return image

    def arrow_frame():
        # Horizontal arrow pointing right: shaft + head + fletch.
        image = []
        for y in range(16):
            row = bytearray()
            for x in range(16):
                pixel = (0, 0, 0, 0)
                if y in (7, 8) and 4 <= x <= 11:
                    pixel = (196, 150, 90, 255)  # wooden shaft
                if x in (12, 13) and 6 <= y <= 9:
                    pixel = (220, 220, 228, 255)  # steel head
                if 2 <= x <= 3 and 5 <= y <= 10:
                    pixel = (200, 60, 60, 255)  # fletching
                row.extend(pixel)
            image.append(row)
        return image

    def bomb_frame():
        # Black round bomb with a lit fuse.
        image = []
        for y in range(16):
            row = bytearray()
            for x in range(16):
                dx = x + 0.5 - 8.0
                dy = y + 0.5 - 9.5
                d = math.hypot(dx, dy)
                pixel = (0, 0, 0, 0)
                if d < 6.0:
                    pixel = (40, 40, 48, 255)
                    if dx < -1.0 and dy < -0.5:
                        pixel = (110, 110, 120, 255)
                    if dx > 1.5 and dy > 1.0:
                        pixel = (20, 20, 26, 255)
                elif d < 6.8:
                    pixel = (16, 16, 20, 255)
                if y == 4 and 11 <= x <= 12:
                    pixel = (240, 200, 80, 255)  # spark
                row.extend(pixel)
            image.append(row)
        return image

    def magic_frame():
        # Glowing blue magic decanter.
        image = []
        for y in range(16):
            row = bytearray()
            for x in range(16):
                dx = abs(x + 0.5 - 8.0)
                pixel = (0, 0, 0, 0)
                if 4 <= y <= 5 and dx < 2.0:
                    pixel = (220, 230, 250, 255)  # neck
                elif 6 <= y <= 13 and dx < 5.0 - (y - 10) * 0.8:
                    if y >= 7:
                        pixel = (70, 140, 240, 255)  # glowing potion
                    else:
                        pixel = (40, 70, 140, 255)  # empty glass
                    if dx > 3.4:
                        pixel = (200, 220, 255, 255)
                row.extend(pixel)
            image.append(row)
        return image

    def fairy_frame():
        # Hovering fairy: spread wings, glowing pink body, trailing sparkles.
        image = []
        for y in range(16):
            row = bytearray()
            for x in range(16):
                dx = x + 0.5 - 8.0
                dy = y + 0.5 - 8.0
                pixel = (0, 0, 0, 0)
                # Wings: swept shapes up and out from the body, tapering.
                if -3.0 <= dy <= 0.5 and 1.0 <= abs(dx) <= 5.5:
                    span = (5.5 - abs(dx)) * 0.9
                    if span > 0.0 and abs(dy + 1.25) < span:
                        pixel = (235, 245, 255, 255)
                # Body: a bright orb with a white-hot core.
                d = math.hypot(dx, dy - 1.0)
                if d < 2.8:
                    pixel = (255, 130, 190, 255)
                    if d < 1.5:
                        pixel = (255, 235, 245, 255)
                # Trailing sparkle dots.
                if (x == 2 and y == 11) or (x == 13 and y == 4):
                    pixel = (255, 200, 230, 255)
                row.extend(pixel)
            image.append(row)
        return image

    def key_frame():
        # Gold small key: ringed bow, horizontal shaft, one tooth.
        gold = (216, 168, 60, 255)
        shade = (140, 100, 30, 255)
        image = []
        for y in range(16):
            row = bytearray()
            for x in range(16):
                dx = x + 0.5 - 5.5
                dy = y + 0.5 - 7.0
                pixel = (0, 0, 0, 0)
                # Bow: a gold ring around a shadowed hole.
                d = math.hypot(dx, dy)
                if d < 3.4:
                    pixel = gold
                    if d < 1.8:
                        pixel = (26, 18, 10, 255)
                # Shaft out to the right.
                if 7 <= x <= 13 and 6 <= y <= 7:
                    pixel = gold
                    if x == 13:
                        pixel = shade
                # Tooth hanging at the shaft's end.
                if 11 <= x <= 12 and 8 <= y <= 10:
                    pixel = gold
                    if y == 10:
                        pixel = shade
                row.extend(pixel)
            image.append(row)
        return image

    frames = [heart_frame(), rupee1, rupee5, rock_frame(),
              arrow_frame(), bomb_frame(), magic_frame(), container_frame(),
              fairy_frame(), key_frame()]
    sheet = []
    for y in range(16):
        row = bytearray()
        for frame_image in frames:
            row.extend(frame_image[y])
        sheet.append(bytes(row))

    write_png(path, 16 * len(frames), 16, sheet)
    return True


def create_light_bitmap(path):
    """Lightmap gradient: 128x128 opaque, white core fading to black at the
    rim (color carries the falloff; alpha stays opaque)."""
    if os.path.exists(path):
        return False

    size = 128
    rows = []
    for y in range(size):
        row = bytearray()
        for x in range(size):
            dx = (x + 0.5 - size * 0.5)
            dy = (y + 0.5 - size * 0.5)
            d = math.sqrt(dx * dx + dy * dy) / (size * 0.5)
            if d > 1.0:
                d = 1.0
            v = (1.0 - d) ** 1.6
            c = int(v * 255)
            row.extend((c, c, c, 255))
        rows.append(bytes(row))

    write_png(path, size, size, rows)
    return True


def create_beam_bitmap(path):
    """Placeholder sword-beam sheet: 4 columns (down, right, left, up) of
    16x16 frames - a glowing bolt per direction."""
    if os.path.exists(path):
        return False

    core = (250, 252, 255, 255)
    glow = (170, 220, 255, 255)
    halo = (110, 160, 255, 160)
    fade = (70, 110, 220, 90)
    clear = (0, 0, 0, 0)

    def render(direction):
        image = []
        for y in range(16):
            row = bytearray()
            for x in range(16):
                # Distance along the axis (d) and across (a), pointing forward.
                if direction == "down":
                    along, across = y - 4, x - 7.5
                elif direction == "right":
                    along, across = x - 4, y - 7.5
                elif direction == "left":
                    along, across = 11 - x, y - 7.5
                else:
                    along, across = 11 - y, x - 7.5
                pixel = clear
                if along < -2:
                    pixel = clear
                elif along < 0:
                    if abs(across) < 1.2:
                        pixel = fade
                    elif abs(across) < 2.4:
                        pixel = fade
                elif along < 8:
                    width = 1.6 - along * 0.12
                    if abs(across) < width:
                        pixel = core if along < 5 else glow
                    elif abs(across) < width + 1.6:
                        pixel = glow if along < 5 else halo
                    elif abs(across) < width + 3.0 and along < 5:
                        pixel = halo
                row.extend(pixel)
            image.append(row)
        return image

    frames = [render(d) for d in ("down", "right", "left", "up")]
    width = 16 * 4
    sheet = []
    for y in range(16):
        row = bytearray()
        for frame_image in frames:
            row.extend(frame_image[y])
        sheet.append(bytes(row))

    write_png(path, width, 16, sheet)
    return True


def create_boomerang_bitmap(path):
    """Placeholder boomerang sheet: 4 rotation frames, 16x16."""
    if os.path.exists(path):
        return False

    red = (216, 48, 40, 255)
    gold = (232, 190, 60, 255)
    edge = (120, 24, 24, 255)
    clear = (0, 0, 0, 0)

    def render(angle_offset):
        import math as m
        image = []
        for y in range(16):
            row = bytearray()
            for x in range(16):
                dx = x - 7.5
                dy = y - 7.5
                r, a = m.hypot(dx, dy), m.atan2(dy, dx)
                pixel = clear
                if r < 6.4:
                    for i in range(3):
                        arm = angle_offset + i * (2 * m.pi / 3.0)
                        aa = (a - arm + m.pi) % (2 * m.pi) - m.pi
                        # Three-armed spinner: arm along radius, thick near rim.
                        if abs(aa) < 0.55 and r > 2.0:
                            pixel = red if r > 4.0 else gold
                            if abs(aa) > 0.4:
                                pixel = edge
                row.extend(pixel)
            image.append(row)
        return image

    frames = [render(i * (3.14159 / 2.0)) for i in range(4)]
    width = 16 * 4
    sheet = []
    for y in range(16):
        row = bytearray()
        for frame_image in frames:
            row.extend(frame_image[y])
        sheet.append(bytes(row))

    write_png(path, width, 16, sheet)
    return True


def write_wav(path, samples, volume=0.5):
    import struct as st
    import wave

    rate = 22050
    with wave.open(path, "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(rate)
        frames = bytearray()
        for sample in samples:
            value = int(max(-1.0, min(1.0, sample)) * volume * 32767.0)
            frames.extend(st.pack("<h", value))
        handle.writeframes(bytes(frames))


def create_default_sfx(directory):
    """Small synthesized placeholder cues: sword swish, hurt, fall."""
    import math
    import random

    rate = 22050

    def synth_sword():
        rng = random.Random(1234)
        total = int(rate * 0.13)
        out = []
        last = 0.0
        for i in range(total):
            t = i / total
            noise = (rng.random() * 2.0 - 1.0)
            last = 0.55 * last + 0.45 * noise
            out.append(last * (1.0 - t) ** 2 * (0.7 + 0.3 * math.sin(2 * math.pi * 80.0 * i / rate)))
        return out

    def synth_hurt():
        total = int(rate * 0.18)
        out = []
        phase = 0.0
        for i in range(total):
            t = i / total
            freq = 190.0 - 70.0 * t
            phase += 2.0 * math.pi * freq / rate
            duty = 0.5 - 0.2 * t
            out.append((1.0 if math.sin(phase) > duty else -1.0) * (1.0 - t) * 0.8)
        return out

    def synth_fall():
        total = int(rate * 0.30)
        out = []
        phase = 0.0
        for i in range(total):
            t = i / total
            freq = 420.0 * (0.35 ** t)
            phase += 2.0 * math.pi * freq / rate
            out.append(math.sin(phase) * (1.0 - t * 0.7))
        return out

    def synth_birds():
        # ~6s of chirps over near-silence: an ambient loop placeholder.
        rng = random.Random(27)
        total = int(rate * 6.0)
        out = [0.0] * total
        for _ in range(14):
            start = rng.randrange(0, total - int(rate * 0.3))
            base = 1800.0 + rng.random() * 1400.0
            length = int(rate * (0.06 + rng.random() * 0.12))
            phase = 0.0
            for i in range(length):
                t = i / length
                freq = base * (1.0 + 0.35 * math.sin(2 * math.pi * 5.0 * t))
                phase += 2.0 * math.pi * freq / rate
                out[start + i] += (math.sin(phase) * (1.0 - t)
                                   * (0.25 + 0.2 * rng.random()))
        return out

    def synth_rain():
        # 6s of lowpassed noise: a rain loop placeholder.
        rng = random.Random(51)
        total = int(rate * 6.0)
        out = []
        last = 0.0
        for _ in range(total):
            last = 0.92 * last + 0.08 * (rng.random() * 2.0 - 1.0)
            out.append(last * 0.7)
        return out

    created = []
    for name, samples in (("sword.wav", synth_sword()),
                          ("hurt.wav", synth_hurt()),
                          ("fall.wav", synth_fall()),
                          ("birds.wav", synth_birds()),
                          ("rain.wav", synth_rain())):
        path = os.path.join(directory, name)
        if os.path.exists(path):
            continue
        write_wav(path, samples)
        created.append(name)
    return created


def sync_runtime_assets():
    """Mirror resources/ into assets/ (what ships with the binary).

    Data files (.json) are excluded: they are baked into the generated C++
    at build time, so the binary never reads them.
    """
    import shutil
    if os.path.isdir(ASSETS):
        shutil.rmtree(ASSETS)
    shutil.copytree(RES, ASSETS,
                    ignore=shutil.ignore_patterns("*.json", ".DS_Store"))
    count = sum(len(files) for _, _, files in os.walk(ASSETS))
    print("synced %d runtime files to assets/" % count)


def generate(dump_flags=False):
    game = load_game()
    manifest = load_code_manifest()

    # Game-defined content loads first: everything cross-references the
    # per-game ids (items, object types) these loaders assign.
    items = load_items()
    ITEM_INDEX.update({entry["name"]: i for i, entry in enumerate(items)})
    object_types = load_object_types()
    OBJECT_INDEX.update({entry["name"]: i for i, entry in enumerate(object_types)})

    # Somaria-style spawn references resolve to object type indices.
    for item in items:
        name = item.pop("spawnObjectName")
        if not name:
            item["spawnObjectIdx"] = -1
            continue
        item["spawnObjectIdx"] = OBJECT_INDEX.get(name, -1)
        if item["spawnObjectIdx"] < 0:
            warn("item '%s' spawns unknown object type '%s'"
                 % (item["name"], name))

    flags = FlagRegistry()
    dialogues = load_dialogues(flags)
    dialogue_index = {d["name"]: i for i, d in enumerate(dialogues)}

    # Chain targets ("next") resolve once every dialogue is loaded; the
    # endgame script chains into its credits roll this way.
    for dialogue in dialogues:
        name = dialogue.pop("next")
        if not name:
            dialogue["nextIndex"] = -1
        elif name in dialogue_index:
            dialogue["nextIndex"] = dialogue_index[name]
        else:
            warn("dialogue '%s' chains into unknown dialogue '%s'"
                 % (dialogue["name"], name))
            dialogue["nextIndex"] = -1

    # Item upgrade flags (silver arrows, magical boomerang) register into
    # the same world-flag registry the dialogues fill.
    for item in items:
        item["upgradeFlagIdx"] = (flags.index(item.pop("upgradeFlagName"),
                                               "items/%s.json" % item["name"])
                                  if item["upgradeFlagName"] else -1)

    # Placeholder sheets for characters that don't ship art (NPCs), before
    # the configs load (frame counts derive from the image).
    characters_dir = res("characters")
    if os.path.isdir(characters_dir):
        for filename in sorted(os.listdir(characters_dir)):
            if not filename.endswith(".json"):
                continue
            if filename == "link.json":
                continue  # the hero ships with art
            png = os.path.join(characters_dir, filename[:-5] + ".png")
            if create_character_bitmap(png, filename[:-5]):
                print("created placeholder character sheet: %s" % os.path.relpath(png, ROOT))

    characters = load_characters()
    character_index = {c["name"]: i for i, c in enumerate(characters)}

    enemies = load_enemies(manifest)
    enemy_index = {e["name"]: i for i, e in enumerate(enemies)}

    tileset = load_tileset()
    if tileset is None:
        warn("no tileset found in resources/tiles")

    tile_meta = load_tile_meta(tileset)

    rooms = load_rooms(dialogue_index, character_index, enemy_index,
                       enemy_defs=enemies, flags=flags,
                       tile_count=tileset["tileCount"] if tileset else 0)

    # Fixed-pool budget validation (the engine uses fixed-size arrays so
    # the console builds stay predictable; warn at bake instead of letting
    # the engine silently overflow).
    for room in rooms:
        if len(room["enemySpawns"]) > 16:
            warn("room '%s' spawns %d enemies; the pool holds 16"
                 % (room["name"], len(room["enemySpawns"])))
        if len(room["objectSpawns"]) > 16:
            warn("room '%s' spawns %d objects; the pool holds 16"
                 % (room["name"], len(room["objectSpawns"])))
        if len(room["npcSpawns"]) > 8:
            warn("room '%s' spawns %d npcs; the pool holds 8"
                 % (room["name"], len(room["npcSpawns"])))
        if len(room["pickupSpawns"]) > 32:
            warn("room '%s' spawns %d pickups; the pool holds 32"
                 % (room["name"], len(room["pickupSpawns"])))
    if len(rooms) > 32:
        warn("%d rooms exceed the boss-defeat registry (32); far-room boss"
             " defeats will not be banked" % len(rooms))
    if len(rooms) > 128:
        warn("%d rooms exceed the visited-room marks (128)" % len(rooms))
    if len(flags.names) > 256:
        warn("%d world flags exceed the save cap (256)" % len(flags.names))
    total_encounters = sum(len(r["instant"]) + len(r["interactive"]) for r in rooms)
    if total_encounters > 512:
        warn("%d encounters exceed the save cap (512)" % total_encounters)

    # Audio budget (web memory: everything ships in the package, and
    # decoded PCM lives in RAM once loaded). Long loops dominate — warn
    # so content authors keep them short.
    for subdir, cap_mb in (("audio", 0.5), ("music", 2.0)):
        directory = res(subdir)
        if not os.path.isdir(directory):
            continue
        for filename in sorted(os.listdir(directory)):
            path = os.path.join(directory, filename)
            if not os.path.isfile(path):
                continue
            size = os.path.getsize(path)
            if size > cap_mb * 1024 * 1024:
                warn("audio/%s is %.1f MB (> %.1f MB budget) — trim the "
                     "loop for the web build" % (filename, size / (1024.0 * 1024.0), cap_mb))

    # NPC spawn conditions resolve against the same flag registry the
    # dialogues filled (story-state presence).
    for room in rooms:
        for npc in room["npcSpawns"]:
            npc["ifFlag"] = (flags.index(npc.pop("ifFlag"))
                             if npc["ifFlag"] else -1)
            npc["unlessFlag"] = (flags.index(npc.pop("unlessFlag"))
                                 if npc["unlessFlag"] else -1)

        # Encounter conditions ("if"/"unless" story flags) resolve the same
        # way; a condition that never holds leaves the one-shot intact.
        for key in ("instant", "interactive"):
            for encounter in room[key]:
                encounter["ifFlagIdx"] = (flags.index(encounter.pop("ifFlag"))
                                           if encounter["ifFlag"] else -1)
                encounter["unlessFlagIdx"] = (flags.index(encounter.pop("unlessFlag"))
                                               if encounter["unlessFlag"] else -1)

    # The crystal-switch state (blue/red barriers) is a saved world flag so
    # it persists like story state; bake its index as a named constant.
    crystal_flag_index = flags.index("crystal_red")

    # Resolve dialogue warp room references (needs every room loaded).
    room_index = {}
    for i, room in enumerate(rooms):
        room_index[room["name"]] = i
        if room["name"].endswith(".json"):
            room_index[room["name"][:-5]] = i
    for dialogue in dialogues:
        for line in dialogue["lines"]:
            name = line.pop("warpRoom")
            if not name:
                line["warpRoomIndex"] = -1
                continue
            if name in room_index:
                line["warpRoomIndex"] = room_index[name]
            elif (name + ".json") in room_index:
                line["warpRoomIndex"] = room_index[name + ".json"]
            else:
                warn("dialogue '%s' warp references unknown room '%s'"
                     % (dialogue["name"], name))
                line["warpRoomIndex"] = -1

    os.makedirs(res("ui"), exist_ok=True)
    border_path = res("ui", "border.png")
    if create_border_bitmap(border_path):
        print("created dialogue border bitmap: %s" % os.path.relpath(border_path, ROOT))

    sword_path = res("characters", "link_sword.png")
    if create_sword_bitmap(sword_path):
        print("created placeholder sword sheet: %s" % os.path.relpath(sword_path, ROOT))

    hearts_path = res("ui", "hearts.png")
    if create_hearts_bitmap(hearts_path):
        print("created placeholder hearts sheet: %s" % os.path.relpath(hearts_path, ROOT))

    os.makedirs(res("enemies"), exist_ok=True)
    for enemy in enemies:
        image_path = res("enemies", enemy["imageName"])
        if create_enemy_bitmap(image_path, enemy["color"]):
            print("created placeholder enemy sheet: %s" % os.path.relpath(image_path, ROOT))

    os.makedirs(res("objects"), exist_ok=True)
    for entry in object_types:
        image_path = res("objects", entry["image"])
        if create_object_bitmap(image_path, entry["name"]):
            print("created placeholder object sheet: %s" % os.path.relpath(image_path, ROOT))

    os.makedirs(res("pickups"), exist_ok=True)
    pickups_path = res("pickups", "pickups.png")
    if create_pickups_bitmap(pickups_path):
        print("created placeholder pickups sheet: %s" % os.path.relpath(pickups_path, ROOT))

    os.makedirs(res("effects"), exist_ok=True)
    beam_path = res("effects", "beam.png")
    if create_beam_bitmap(beam_path):
        print("created placeholder beam sheet: %s" % os.path.relpath(beam_path, ROOT))
    boomerang_path = res("effects", "boomerang.png")
    if create_boomerang_bitmap(boomerang_path):
        print("created placeholder boomerang sheet: %s" % os.path.relpath(boomerang_path, ROOT))
    light_path = res("effects", "light.png")
    if create_light_bitmap(light_path):
        print("created placeholder light gradient: %s" % os.path.relpath(light_path, ROOT))

    os.makedirs(res("audio"), exist_ok=True)
    for name in create_default_sfx(res("audio")):
        print("created placeholder sfx: %s" % os.path.relpath(os.path.join(res("audio"), name), ROOT))

    h = []
    h.append("// Generated by scripts/generate_assets.py - do not edit by hand.")
    h.append("#pragma once")
    h.append("")
    h.append('#include "engine.h"')
    h.append("")
    h.append("// The baked index helpers below use strcmp/snprintf.")
    h.append("#include <stdio.h>")
    h.append("#include <string.h>")
    h.append("")
    h.append("// Game identity (resources/game.json). The slug is baked into save")
    h.append("// files so a save from one game is never loaded by another.")
    h.append("extern const char* const GAME_TITLE;")
    h.append("extern const char* const GAME_SLUG;")
    h.append("")
    h.append("// Game-defined id constants (stable, assigned by the generator from")
    h.append("// the game's own data; engine.h owns none of these).")
    for label, names, prefix in (("items", items, "ITEM"),
                                 ("object types", object_types, "OBJECT"),
                                 ("door kinds", DOOR_KIND_DEFS, "DOOR"),
                                 ("chest kinds", CHEST_KIND_DEFS, "CHEST"),
                                 ("grant kinds", GRANT_KIND_DEFS, "GRANT")):
        h.append("// %s" % label)
        for i, entry in enumerate(names):
            h.append("constexpr int %s_%s = %d;" % (prefix, sym(entry["name"]), i))
        h.append("")
    h.append("// Crystal barrier colors (barrier aux3).")
    for name, value in sorted(BARRIER_COLORS.items(), key=lambda kv: kv[1]):
        h.append("constexpr int BARRIER_%s = %d;" % (sym(name), value))
    h.append("")
    h.append("extern const int DIALOGUE_COUNT;")
    h.append("extern const Dialogue DIALOGUES[];")
    h.append("")
    h.append("extern const int CHARACTER_COUNT;")
    h.append("extern const CharacterConfig CHARACTERS[];")
    h.append("")
    h.append("extern const int ENEMY_COUNT;")
    h.append("extern const EnemyConfig ENEMIES[];")
    h.append("")
    h.append("extern const int DOOR_KIND_COUNT;")
    h.append("extern const DoorRule DOOR_RULES[];")
    h.append("extern const int CHEST_KIND_COUNT;")
    h.append("extern const ChestRule CHEST_RULES[];")
    h.append("extern const int GRANT_KIND_COUNT;")
    h.append("extern const GrantRule GRANT_RULES[];")
    h.append("")
    h.append("extern const int OBJECT_COUNT;")
    h.append("extern const ObjectConfig OBJECTS[];")
    h.append("")
    h.append("extern const int ITEM_COUNT;")
    h.append("extern const ItemConfig ITEMS[];")
    h.append("")
    h.append("// Game-code behaviors (resources/code/manifest.json; JSON references")
    h.append("// them as \"custom:<name>\").")
    h.append("extern const char* const CUSTOM_BEHAVIOR_NAMES[];")
    h.append("extern const int CUSTOM_BEHAVIOR_COUNT;")
    h.append("")
    h.append("extern const int ROOM_COUNT;")
    h.append("extern const RoomConfig ROOMS[];")
    h.append("")
    h.append("extern const int ENCOUNTER_TOTAL;")
    h.append("")
    h.append("extern const TilesetConfig TILESET;")
    h.append("")
    h.append("extern const TileMeta TILE_META[];")
    h.append("extern const int TILE_META_COUNT;")
    h.append("")
    h.append("// World flag store (story state; indexed by baked flag ids).")
    h.append("extern bool gWorldFlags[];")
    h.append("")
    h.append("extern const char* const WORLD_FLAG_NAMES[];")
    h.append("extern const int WORLD_FLAG_COUNT;")
    h.append("// Crystal-switch state (blue = unset / red = set); saved like story state.")
    h.append("extern const int WORLD_FLAG_CRYSTAL_RED;")
    h.append("")

    # Baked asset indexing: name -> index lookups over every data family,
    # emitted as inline helpers (bake, don't parse — the tables are the
    # baked arrays themselves, so lookups need no runtime string building).
    # Names match the baked file names (rooms: "01.json"; flags: flag
    # names), so callers resolve data references by name without
    # re-walking or guessing conventions.
    h.append("// Baked asset indexing: name -> index lookups (dialogues/rooms/")
    h.append("// enemies/objects/items by name, world flags by flag name).")
    h.append("// Returns -1 when the name isn't in the game's data.")
    h.append("inline int dialogueIndex(const char* name)")
    h.append("{")
    h.append("    for (int i = 0; i < DIALOGUE_COUNT; i++)")
    h.append("        if (strcmp(DIALOGUES[i].name, name) == 0) return i;")
    h.append("    return -1;")
    h.append("}")
    h.append("inline int roomIndex(const char* name)")
    h.append("{")
    h.append("    // Rooms accept both the file name (\"01.json\") and the stem (\"01\").")
    h.append("    size_t len = name ? strlen(name) : 0;")
    h.append("    bool stem = len >= 5 && strcmp(name + len - 5, \".json\") == 0;")
    h.append("    for (int i = 0; i < ROOM_COUNT; i++)")
    h.append("        if (strcmp(ROOMS[i].name, name) == 0) return i;")
    h.append("    if (stem)")
    h.append("    {")
    h.append("        char withJson[128];")
    h.append("        snprintf(withJson, sizeof(withJson), \"%s.json\", name);")
    h.append("        for (int i = 0; i < ROOM_COUNT; i++)")
    h.append("            if (strcmp(ROOMS[i].name, withJson) == 0) return i;")
    h.append("    }")
    h.append("    else")
    h.append("    {")
    h.append("        for (int i = 0; i < ROOM_COUNT; i++)")
    h.append("        {")
    h.append("            const char* rn = ROOMS[i].name;")
    h.append("            size_t rl = strlen(rn);")
    h.append("            if (rl >= 5 && strcmp(rn + rl - 5, \".json\") == 0")
    h.append("                && strncmp(rn, name, rl - 5) == 0 && len == rl - 5)")
    h.append("                return i;")
    h.append("        }")
    h.append("    }")
    h.append("    return -1;")
    h.append("}")
    h.append("inline int enemyIndex(const char* name)")
    h.append("{")
    h.append("    for (int i = 0; i < ENEMY_COUNT; i++)")
    h.append("        if (strcmp(ENEMIES[i].name, name) == 0) return i;")
    h.append("    return -1;")
    h.append("}")
    h.append("inline int objectIndex(const char* name)")
    h.append("{")
    h.append("    for (int i = 0; i < OBJECT_COUNT; i++)")
    h.append("        if (strcmp(OBJECTS[i].name, name) == 0) return i;")
    h.append("    return -1;")
    h.append("}")
    h.append("inline int itemIndex(const char* name)")
    h.append("{")
    h.append("    for (int i = 0; i < ITEM_COUNT; i++)")
    h.append("        if (strcmp(ITEMS[i].name, name) == 0) return i;")
    h.append("    return -1;")
    h.append("}")
    h.append("inline int worldFlagIndex(const char* name)")
    h.append("{")
    h.append("    for (int i = 0; i < WORLD_FLAG_COUNT; i++)")
    h.append("        if (strcmp(WORLD_FLAG_NAMES[i], name) == 0) return i;")
    h.append("    return -1;")
    h.append("}")

    c = []
    c.append("// Generated by scripts/generate_assets.py - do not edit by hand.")
    c.append('#include "assets.h"')
    c.append("")
    emit_game(c, game, manifest)
    emit_rules(c)
    emit_dialogues(c, dialogues)
    emit_characters(c, characters)
    emit_enemies(c, enemies)
    emit_objects(c, object_types)
    emit_items(c, items)
    emit_rooms(c, rooms)
    emit_tile_meta(c, tile_meta)

    c.append("const char* const WORLD_FLAG_NAMES[] = {")
    for name in flags.names:
        c.append("    %s," % cstr(name))
    c.append("};")
    c.append("const int WORLD_FLAG_COUNT = %d;" % len(flags.names))
    c.append("bool gWorldFlags[WORLD_FLAG_COUNT > 0 ? WORLD_FLAG_COUNT : 1] = {};")
    c.append("const int WORLD_FLAG_CRYSTAL_RED = %d;" % crystal_flag_index)
    c.append("")
    if tileset:
        c.append("const TilesetConfig TILESET = { %s, %d, %d, %d, %d, %d };"
                  % (cstr(tileset["image"]), tileset["firstGid"], tileset["columns"],
                     tileset["tileCount"], tileset["tileWidth"], tileset["tileHeight"]))
    else:
        c.append("const TilesetConfig TILESET = { 0, 1, 1, 0, %d, %d };" % (TILE, TILE))
    c.append("")

    # Tooling mode (--flags): print the world-flag registry as JSON and
    # stop — no generated files, no asset sync. The editor's flags tab
    # reads this (the bake is the single authority on flag ids).
    if dump_flags:
        import json as _json
        print(_json.dumps([
            {"name": name, "source": flags.sources.get(name, "")}
            for name in flags.names
        ], indent=4))
        return 0

    os.makedirs(INCLUDE_AUTO, exist_ok=True)
    os.makedirs(SRC_AUTO, exist_ok=True)
    header_text = "\n".join(h)
    source_text = "\n".join(c)
    with open(os.path.join(INCLUDE_AUTO, "assets.h"), "w") as handle:
        handle.write(header_text)
    with open(os.path.join(SRC_AUTO, "assets.cpp"), "w") as handle:
        handle.write(source_text)

    # Generated-code size management (web memory budget): the baked tables
    # all become .data in the binary — the web build pays for every byte.
    # Report each bake, and warn as it scales so content growth is a
    # visible cost, not a silent creep. Source-size warnings lag the
    # table-size ones (text compresses; the tables are what ship).
    header_kb = len(header_text.encode()) / 1024.0
    source_kb = len(source_text.encode()) / 1024.0
    print("generated: include_auto/assets.h")
    print("generated: src_auto/assets.cpp")
    print("  baked source size: %.1f KB header + %.1f KB source" % (header_kb, source_kb))
    if source_kb > 512.0:
        warn("generated source is %.0f KB (> 512 KB) — the web build compiles all of it; "
             "consider trimming content or splitting rooms" % source_kb)
    for room in rooms:
        tiles = room["mapWidth"] * room["mapHeight"]
        room_kb = (tiles * 2 * 2) / 1024.0    # decor + collider ushorts
        if room_kb > 128.0:
            warn("room '%s' layers bake ~%.0f KB each (> 128 KB) — "
                 "large rooms multiply the web budget" % (room["name"], room_kb))
    print("  %d dialogues, %d characters, %d enemies, %d items, %d object types, %d rooms, %d tile meta entries"
          % (len(dialogues), len(characters), len(enemies), len(items), len(object_types), len(rooms),
             sum(1 for m in tile_meta if m["flags"] or m["damage"])))
    sync_runtime_assets()

    if len(rooms) == 0:
        warn("no rooms were generated - the game has nothing to load")
        return 1
    if len(characters) == 0:
        warn("no characters were generated")
    return 0


if __name__ == "__main__":
    # Tooling mode: dump the world-flag registry (names + sources) as
    # JSON for the editor's flags tab — no bake, no writes.
    if len(sys.argv) > 1 and sys.argv[1] == "--flags":
        sys.exit(generate(dump_flags=True))
    sys.exit(generate())
