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
RES = os.path.join(ROOT, "resources")
ASSETS = os.path.join(ROOT, "assets")
INCLUDE_AUTO = os.path.join(ROOT, "include_auto")
SRC_AUTO = os.path.join(ROOT, "src_auto")

TILE = 32
DIRECTIONS = ["down", "right", "left", "up"]


def res(*parts):
    return os.path.join(RES, *parts)


def warn(message):
    print("warning: %s" % message)


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

    def index(self, name):
        name = str(name)
        if name in self.names:
            return self.names.index(name)
        self.names.append(name)
        return len(self.names) - 1


def load_dialogues(flags):
    """Dialogue .json: {"lines": [<line>, ...]} where each <line> is either a
    plain text string or an object:
      {"text": ..., "label": ..., "if": <flag>, "unless": <flag>,
       "set": [<flags>], "clear": [<flags>], "sfx": "<file>",
       "music": "<file>"|"", "goto": "<label>",
       "warp": {"room": "<room>", "tile": [x, y]},
       "choice": [{"text": ..., "set": [...], "clear": [...], "sfx": ...,
                   "goto": ...}, ...]}"""
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
            return flags.index(value)

        def flag_list(values, what):
            if values is None:
                return []
            if isinstance(values, (str, int, float)):
                values = [values]
            return [flags.index(v) for v in values]

        lines = []
        for entry in data.get("lines") or []:
            if not isinstance(entry, dict):
                entry = {"text": str(entry)}

            warp_room = None
            tile = None
            if isinstance(entry.get("warp"), dict):
                warp_room = str(entry["warp"].get("room") or "")
                tile = entry["warp"].get("tile") or [0, 0]

            choices = []
            for opt in entry.get("choice") or []:
                if not isinstance(opt, dict):
                    opt = {"text": str(opt)}
                choices.append({
                    "text": str(opt.get("text") or ""),
                    "set": flag_list(opt.get("set"), "set"),
                    "clear": flag_list(opt.get("clear"), "clear"),
                    "sfx": str(opt["sfx"]) if opt.get("sfx") else None,
                    "goto": opt.get("goto"),
                })

            lines.append({
                "text": str(entry.get("text") or ""),
                "label": entry.get("label"),
                "if": flag(entry.get("if"), "if"),
                "unless": flag(entry.get("unless"), "unless"),
                "set": flag_list(entry.get("set"), "set"),
                "clear": flag_list(entry.get("clear"), "clear"),
                "sfx": str(entry["sfx"]) if entry.get("sfx") else None,
                "music": str(entry["music"]) if entry.get("music") is not None else None,
                "goto": entry.get("goto"),
                "warpRoom": warp_room,
                "warpTile": tile,
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

        dialogues.append({"name": filename, "lines": lines})
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


BEHAVIORS = {"": 0, "soldier": 1, "octorok": 2, "boss": 3}

# Room object types: what "objects" entries in room json may reference. The
# index order is baked into the engine as OBJECT_* constants (engine.h).
# Per-type extras in room json: switches take "mode" ("hold"/"latch") and a
# "link" id; barriers take a "link" id; signs take a "dialogue" name.
OBJECT_TYPES = [
    {"name": "pot", "image": "pot.png"},
    {"name": "bush", "image": "bush.png"},
    {"name": "block", "image": "block.png"},
    {"name": "switch", "image": "switch.png"},
    {"name": "barrier", "image": "barrier.png"},
    {"name": "sign", "image": "sign.png"},
    {"name": "chest", "image": "chest.png"},
    {"name": "torch", "image": "torch.png"},
]
OBJECT_INDEX = {entry["name"]: i for i, entry in enumerate(OBJECT_TYPES)}

# Chest kinds and grant kinds; baked as CHEST_*/GRANT_* (engine.h).
CHEST_KINDS = {"normal": 0, "locked": 1, "big": 2}

# Door kinds on room edges; baked as DOOR_* (engine.h).
DOOR_KINDS = {"open": 0, "closed": 1, "locked": 2, "big": 3,
              "bombable": 4, "shutter": 5}
GRANT_KINDS = {"item": 0, "rupees": 1, "arrows": 2, "bombs": 3,
               "magic": 4, "heart_container": 5, "key": 6, "big_key": 7}

# Item button items. The index order is baked into the engine as ITEM_*
# constants (engine.h); keep both sides in sync when adding items.
ITEMS = [
    {"name": "boomerang", "label": "BOOMERANG"},
    {"name": "bow", "label": "BOW"},
    {"name": "bombs", "label": "BOMBS"},
    {"name": "fire_rod", "label": "FIRE ROD"},
    {"name": "ice_rod", "label": "ICE ROD"},
    {"name": "lantern", "label": "LANTERN"},
]
ITEM_INDEX = {entry["name"]: i for i, entry in enumerate(ITEMS)}


def load_enemies():
    """Enemy definitions live in resources/enemies/<name>.json next to their
    sprite sheet (resources/enemies/<name>.png, or an "image" override to
    reuse another sheet - variants). Stats: hp, speed (px/s), damage
    (half-hearts), behavior, sight (px), shoot_period (seconds)."""
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
        behavior = str(stats.get("behavior") or "")
        if behavior not in BEHAVIORS:
            warn("enemy '%s' has unknown behavior '%s'" % (name, behavior))

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
            "behavior": BEHAVIORS.get(behavior, 0),
            "boss": bool(stats.get("boss", False)) or behavior == "boss",
            "sight": max(0, int(stats.get("sight") or 150)),
            "shootPeriod": max(0, int(stats.get("shoot_period") or 0)),
            "color": [int(max(0, min(255, c))) for c in color],
        })
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
    "bombable": true, "bomb_result": <tile id>}}}
    Tile ids are 1-based frame indices, matching the ids used in room layers.
    "damage" is measured in half-hearts. "cut_result" is the tile id left
    behind when the tile is cut (0 = empty). "push" is a conveyor/current/wind
    drift in px/s applied to whoever stands on the tile. "ledge" marks a
    one-way vault-over ledge facing that direction. "bombable" marks cracked
    walls/floors; "bomb_result" (0 = empty, default) is what blasts leave.
    """
    count = tileset["tileCount"] if tileset else 0
    meta = [{"flags": 0, "damage": 0, "cutResult": 0, "bombResult": 255,
             "pushX": 0, "pushY": 0, "ledgeDir": 0}
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


def load_rooms(dialogue_index, character_index, enemy_index, tile_count=0):
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
                encounters.append({"x": int(location[0]), "y": int(location[1]),
                                   "dialogue": index, "repeat": repeat})
            return encounters

        enemies = []
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

        objects = []
        for entry in data.get("objects") or []:
            if not isinstance(entry, dict):
                continue
            type_name = entry.get("type") or ""
            index = OBJECT_INDEX.get(type_name, -1)
            if index < 0:
                warn("room '%s' references unknown object type '%s'" % (stem, type_name))
                continue
            location = entry.get("location") or [0, 0]
            aux1 = -1
            aux2 = 0
            aux3 = 0
            if type_name == "switch":
                mode = entry.get("mode") or "hold"
                if mode not in ("hold", "latch"):
                    warn("room '%s' switch has unknown mode '%s' (hold/latch); using hold"
                         % (stem, mode))
                    mode = "hold"
                aux1 = 1 if mode == "latch" else 0
                try:
                    aux2 = int(entry.get("link") or 0)
                except (TypeError, ValueError):
                    warn("room '%s' switch has a bad link id %r" % (stem, entry.get("link")))
            elif type_name == "barrier":
                try:
                    aux2 = int(entry.get("link") or 0)
                except (TypeError, ValueError):
                    warn("room '%s' barrier has a bad link id %r" % (stem, entry.get("link")))
            elif type_name == "sign":
                dialogue_name = entry.get("dialogue")
                if dialogue_name:
                    aux1 = dialogue_index.get(dialogue_name, -1)
                    if aux1 < 0:
                        warn("room '%s' sign references unknown dialogue '%s'"
                             % (stem, dialogue_name))
            elif type_name == "torch":
                try:
                    aux2 = int(entry.get("link") or 0)
                except (TypeError, ValueError):
                    warn("room '%s' torch has a bad link id %r" % (stem, entry.get("link")))
            elif type_name == "chest":
                kind = entry.get("kind") or "normal"
                if kind in CHEST_KINDS:
                    aux1 = CHEST_KINDS[kind]
                else:
                    warn("room '%s' chest has unknown kind '%s' (normal/locked/big)"
                         % (stem, kind))
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
            objects.append({"x": int(location[0]), "y": int(location[1]),
                            "type": index, "aux1": aux1, "aux2": aux2, "aux3": aux3})

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
            npc_spawns.append({
                "x": int(location[0]), "y": int(location[1]),
                "character": character, "dialogue": dialogue_idx,
                "wander": bool(entry.get("wander", False)),
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

        rooms.append({
            "name": filename,
            "hero": character_index.get(hero_name, -1),
            "hasSpawn": has_spawn,
            "spawnX": int(spawn[0]) if has_spawn else 0,
            "spawnY": int(spawn[1]) if has_spawn else 0,
            "music": default.get("music") or None,
            "walkSfx": default.get("walk_sfx") or None,
            "instant": encounter_list("instant"),
            "interactive": encounter_list("interactive"),
            "enemySpawns": enemies,
            "objectSpawns": objects,
            "npcSpawns": npc_spawns,
            "dark": bool(data.get("dark", False)),
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
                choice_refs.append("    { %s, %d, %s, %d, %s, %d, %s },"
                                   % (cstr(choice["text"]), choice["gotoLine"],
                                      (cbase + "_SET") if choice["set"] else "0",
                                      len(choice["set"]),
                                      (cbase + "_CLEAR") if choice["clear"] else "0",
                                      len(choice["clear"]),
                                      cstr(choice["sfx"]) if choice["sfx"] else "0"))

            if line["choices"]:
                c.append("static const DialogueChoice %s_CHOICES[] = {" % lbase)
                for ref in choice_refs:
                    c.append(ref)
                c.append("};")
                c.append("")

            line_refs.append(
                "    { %s, %d, %d, %d, %s, %d, %s, %d, %s, %s, %d, %d, %d, %d, %s },"
                % (cstr(line["text"]), line["gotoLine"], line["if"], line["unless"],
                   (lbase + "_SET") if line["set"] else "0", len(line["set"]),
                   (lbase + "_CLEAR") if line["clear"] else "0", len(line["clear"]),
                   cstr(line["sfx"]) if line["sfx"] else "0",
                   cstr(line["music"]) if line["music"] is not None else "0",
                   line.get("warpRoomIndex", -1),
                   line["warpTile"][0] if line["warpTile"] else 0,
                   line["warpTile"][1] if line["warpTile"] else 0,
                   len(line["choices"]),
                   (lbase + "_CHOICES") if line["choices"] else "0"))

        if line_refs:
            c.append("static const DialogueLine %s_LINES[] = {" % base)
            for ref in line_refs:
                c.append(ref)
            c.append("};")
        else:
            c.append("static const DialogueLine %s_LINES[] = { { 0, -1, -1, -1, 0, 0, 0, 0, 0, 0, -1, 0, 0, 0, 0 } };" % base)
        c.append("")

    c.append("const Dialogue DIALOGUES[] = {")
    for dialogue in dialogues:
        c.append("    { %s, %d, DIALOGUE_%s_LINES },"
                 % (cstr(dialogue["name"]), len(dialogue["lines"]), sym(dialogue["name"])))
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


def emit_enemies(c, enemies):
    for enemy in enemies:
        base = "ENEMY_%s" % sym(enemy["name"])
        emit_character_frame_arrays(c, base, enemy["anims"])

    c.append("const EnemyConfig ENEMIES[] = {")
    for enemy in enemies:
        base = "ENEMY_%s" % sym(enemy["name"])
        extra = "%d, %d, %d, %d, %d, %d, %s" % (enemy["hp"], enemy["speed"], enemy["damage"],
                                                enemy["behavior"], enemy["sight"],
                                                enemy["shootPeriod"],
                                                "true" if enemy["boss"] else "false")
        emit_character_config_entries(c, base, enemy, extra)
    c.append("};")
    c.append("const int ENEMY_COUNT = %d;" % len(enemies))
    c.append("")

    c.append("const ObjectConfig OBJECTS[] = {")
    for entry in OBJECT_TYPES:
        c.append("    { %s, %s }," % (cstr(entry["name"]),
                                      cstr("assets/objects/%s" % entry["image"])))
    c.append("};")
    c.append("const int OBJECT_COUNT = %d;" % len(OBJECT_TYPES))
    c.append("")

    c.append("const ItemConfig ITEMS[] = {")
    for entry in ITEMS:
        c.append("    { %s, %s }," % (cstr(entry["name"]), cstr(entry["label"])))
    c.append("};")
    c.append("const int ITEM_COUNT = %d;" % len(ITEMS))
    c.append("")


def emit_tile_meta(c, meta):
    if meta:
        c.append("const TileMeta TILE_META[] = {")
        per_line = 6
        for i in range(0, len(meta), per_line):
            chunk = meta[i:i + per_line]
            c.append("    " + " ".join(
                "{ %d, %d, %d, %d, %d, %d, %d }," % (m["flags"], m["damage"],
                                                     m["cutResult"], m["bombResult"],
                                                     m["pushX"], m["pushY"], m["ledgeDir"])
                for m in chunk))
        c.append("};")
    else:
        c.append("const TileMeta TILE_META[] = { { 0, 0, 0, 255, 0, 0, 0 } };")
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
                entries = ", ".join("{ %d, %d, %d, %d, %s }"
                                    % (e["x"], e["y"], e["dialogue"], e["id"],
                                       "true" if e["repeat"] else "false")
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
            name = "%s_OBJECTS" % base
            c.append("static const ObjectSpawn %s[%d] = {" % (name, len(room["objectSpawns"])))
            entries = ", ".join("{ %d, %d, %d, %d, %d, %d }"
                                % (o["x"], o["y"], o["type"], o["aux1"], o["aux2"],
                                   o.get("aux3", 0))
                                for o in room["objectSpawns"])
            c.append("    %s," % entries)
            c.append("};")
            c.append("")

        if room["npcSpawns"]:
            name = "%s_NPCS" % base
            c.append("static const NpcSpawn %s[%d] = {" % (name, len(room["npcSpawns"])))
            entries = ", ".join("{ %d, %d, %d, %d, %s, %d, %d }"
                                % (n["x"], n["y"], n["character"], n["dialogue"],
                                   "true" if n["wander"] else "false",
                                   n["ifFlag"], n["unlessFlag"])
                                for n in room["npcSpawns"])
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
            instant_ref, str(len(room["instant"])),
            interactive_ref, str(len(room["interactive"])),
            enemy_ref, str(len(room["enemySpawns"])),
            object_ref, str(len(room["objectSpawns"])),
            npc_ref, str(len(room["npcSpawns"])),
            "true" if room["dark"] else "false",
            str(room["doorN"]), str(room["doorS"]),
            str(room["doorE"]), str(room["doorW"]),
            str(room["mapWidth"]), str(room["mapHeight"]),
            decor_ref, colliders_ref, upper_ref,
            str(room["exitN"]), str(room["exitS"]),
            str(room["exitE"]), str(room["exitW"]),
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

    render = {"pot": pot, "bush": bush, "block": block,
              "switch": switch, "barrier": barrier, "sign": sign,
              "torch": torch}.get(kind)
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
    4 arrow, 5 bomb, 6 magic, 7 heart container."""
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

    frames = [heart_frame(), rupee1, rupee5, rock_frame(),
              arrow_frame(), bomb_frame(), magic_frame(), container_frame()]
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

    created = []
    for name, samples in (("sword.wav", synth_sword()),
                          ("hurt.wav", synth_hurt()),
                          ("fall.wav", synth_fall())):
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


def generate():
    flags = FlagRegistry()
    dialogues = load_dialogues(flags)
    dialogue_index = {d["name"]: i for i, d in enumerate(dialogues)}

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

    enemies = load_enemies()
    enemy_index = {e["name"]: i for i, e in enumerate(enemies)}

    tileset = load_tileset()
    if tileset is None:
        warn("no tileset found in resources/tiles")

    tile_meta = load_tile_meta(tileset)

    rooms = load_rooms(dialogue_index, character_index, enemy_index,
                       tileset["tileCount"] if tileset else 0)

    # NPC spawn conditions resolve against the same flag registry the
    # dialogues filled (story-state presence).
    for room in rooms:
        for npc in room["npcSpawns"]:
            npc["ifFlag"] = (flags.index(npc.pop("ifFlag"))
                             if npc["ifFlag"] else -1)
            npc["unlessFlag"] = (flags.index(npc.pop("unlessFlag"))
                                 if npc["unlessFlag"] else -1)

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
    for entry in OBJECT_TYPES:
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
    h.append("extern const int DIALOGUE_COUNT;")
    h.append("extern const Dialogue DIALOGUES[];")
    h.append("")
    h.append("extern const int CHARACTER_COUNT;")
    h.append("extern const CharacterConfig CHARACTERS[];")
    h.append("")
    h.append("extern const int ENEMY_COUNT;")
    h.append("extern const EnemyConfig ENEMIES[];")

    h.append("extern const int OBJECT_COUNT;")
    h.append("extern const ObjectConfig OBJECTS[];")
    h.append("")
    h.append("extern const int ITEM_COUNT;")
    h.append("extern const ItemConfig ITEMS[];")
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
    h.append("")

    c = []
    c.append("// Generated by scripts/generate_assets.py - do not edit by hand.")
    c.append('#include "assets.h"')
    c.append("")
    emit_dialogues(c, dialogues)
    emit_characters(c, characters)
    emit_enemies(c, enemies)
    emit_rooms(c, rooms)
    emit_tile_meta(c, tile_meta)

    c.append("const char* const WORLD_FLAG_NAMES[] = {")
    for name in flags.names:
        c.append("    %s," % cstr(name))
    c.append("};")
    c.append("const int WORLD_FLAG_COUNT = %d;" % len(flags.names))
    c.append("bool gWorldFlags[WORLD_FLAG_COUNT > 0 ? WORLD_FLAG_COUNT : 1] = {};")
    c.append("")
    if tileset:
        c.append("const TilesetConfig TILESET = { %s, %d, %d, %d, %d, %d };"
                 % (cstr(tileset["image"]), tileset["firstGid"], tileset["columns"],
                    tileset["tileCount"], tileset["tileWidth"], tileset["tileHeight"]))
    else:
        c.append("const TilesetConfig TILESET = { 0, 1, 1, 0, %d, %d };" % (TILE, TILE))
    c.append("")

    os.makedirs(INCLUDE_AUTO, exist_ok=True)
    os.makedirs(SRC_AUTO, exist_ok=True)
    with open(os.path.join(INCLUDE_AUTO, "assets.h"), "w") as handle:
        handle.write("\n".join(h))
    with open(os.path.join(SRC_AUTO, "assets.cpp"), "w") as handle:
        handle.write("\n".join(c))

    print("generated: include_auto/assets.h")
    print("generated: src_auto/assets.cpp")
    print("  %d dialogues, %d characters, %d enemies, %d rooms, %d tile meta entries"
          % (len(dialogues), len(characters), len(enemies), len(rooms),
             sum(1 for m in tile_meta if m["flags"] or m["damage"])))
    sync_runtime_assets()

    if len(rooms) == 0:
        warn("no rooms were generated - the game has nothing to load")
        return 1
    if len(characters) == 0:
        warn("no characters were generated")
    return 0


if __name__ == "__main__":
    sys.exit(generate())
