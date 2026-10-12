#!/usr/bin/env python3
"""Generate include_auto/assets.h and src_auto/assets.cpp from the .json
files (and Tiled .tmx/.tsx data) in resources/, so the Raylib binary never
parses json at runtime. Also creates the dialogue box border bitmap if the
resources folder does not contain one yet."""

import json
import math
import os
import re
import struct
import sys
import zlib
import xml.etree.ElementTree as ET

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


def load_dialogues():
    dialogues = []
    directory = res("dialogue")
    if not os.path.isdir(directory):
        return dialogues
    for filename in sorted(os.listdir(directory)):
        if not filename.endswith(".json"):
            continue
        with open(os.path.join(directory, filename), "r", encoding="utf-8") as handle:
            data = json.load(handle)
        lines = []
        for entry in data.get("lines") or []:
            if isinstance(entry, dict):
                lines.append(str(entry.get("text", "")))
            else:
                lines.append(str(entry))
        dialogues.append({"name": filename, "lines": lines})
    return dialogues


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

        anims = {}
        for kind in ("stand", "move"):
            block = data.get(kind) or {}
            anims[kind] = []
            for direction in DIRECTIONS:
                values = block.get(direction) or []
                anims[kind].append([int(v) for v in values])

        characters.append({
            "name": name,
            "image": "assets/characters/%s.png" % name,
            "columns": columns,
            "frameCount": frame_count,
            "anims": anims,
        })
    return characters


def load_tileset():
    directory = res("tiles")
    if not os.path.isdir(directory):
        return None
    for filename in sorted(os.listdir(directory)):
        if not filename.endswith(".tsx"):
            continue
        root = ET.parse(os.path.join(directory, filename)).getroot()
        image = root.find("image")
        source = image.attrib.get("source", "") if image is not None else ""
        return {
            "image": "assets/tiles/" + source.replace("\\", "/"),
            "firstGid": int(root.attrib.get("firstgid", 1)),
            "columns": int(root.attrib.get("columns") or 0),
            "tileCount": int(root.attrib.get("tilecount") or 0),
            "tileWidth": int(root.attrib.get("tilewidth") or TILE),
            "tileHeight": int(root.attrib.get("tileheight") or TILE),
        }
    return None


def parse_tmx(path):
    root = ET.parse(path).getroot()
    map_width = int(root.attrib.get("width", 0))
    map_height = int(root.attrib.get("height", 0))

    first_gid = 1
    tileset = root.find("tileset")
    if tileset is not None:
        first_gid = int(tileset.get("firstgid", 1))

    layers = {}
    for layer in root.iter("layer"):
        name = layer.attrib.get("name", "")
        data = layer.find("data")
        if data is None or data.attrib.get("encoding") != "csv":
            warn("layer '%s' in %s is not csv-encoded, skipping" % (name, os.path.basename(path)))
            continue
        values = [int(v) for v in data.text.split(",") if v.strip()]
        if len(values) != map_width * map_height:
            warn("layer '%s' in %s has %d tiles, expected %d" % (name, os.path.basename(path), len(values), map_width * map_height))
            continue
        layers[name] = [(0 if v == 0 else max(0, v - first_gid + 1)) for v in values]

    return map_width, map_height, layers


def load_rooms(dialogue_index, character_index):
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

        tmx_path = res("rooms", stem + ".tmx")
        if not os.path.exists(tmx_path):
            warn("room '%s' has no matching .tmx, skipping" % stem)
            continue
        map_width, map_height, layers = parse_tmx(tmx_path)
        if "decor" not in layers or "colliders" not in layers:
            warn("room '%s' is missing a decor or colliders layer, skipping" % stem)
            continue

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
            "mapWidth": map_width,
            "mapHeight": map_height,
            "decor": layers["decor"],
            "colliders": layers["colliders"],
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
    return rooms


def emit_array(lines, ctype, name, values, per_line=16):
    lines.append("static const %s %s[%d] = {" % (ctype, name, len(values)))
    for i in range(0, len(values), per_line):
        chunk = values[i:i + per_line]
        lines.append("    " + ", ".join(str(v) for v in chunk) + ",")
    lines.append("};")
    lines.append("")


def emit_dialogues(c, dialogues):
    for index, dialogue in enumerate(dialogues):
        name = "DIALOGUE_%s_LINES" % sym(dialogue["name"])
        if dialogue["lines"]:
            lines = [cstr(line) for line in dialogue["lines"]]
            c.append("static const char* %s[] = {" % name)
            for line in lines:
                c.append("    %s," % line)
            c.append("};")
        else:
            c.append("static const char* %s[] = { 0 };" % name)
        c.append("")

    c.append("const Dialogue DIALOGUES[] = {")
    for dialogue in dialogues:
        c.append("    { %s, %d, DIALOGUE_%s_LINES },"
                 % (cstr(dialogue["name"]), len(dialogue["lines"]), sym(dialogue["name"])))
    c.append("};")
    c.append("const int DIALOGUE_COUNT = %d;" % len(dialogues))
    c.append("")


def emit_characters(c, characters):
    for character in characters:
        base = "CHAR_%s" % sym(character["name"])
        for kind in ("stand", "move"):
            for index, direction in enumerate(DIRECTIONS):
                values = character["anims"][kind][index]
                if values:
                    emit_array(c, "int", "%s_%s_%s" % (base, kind.upper(), direction.upper()), values, per_line=24)

    c.append("const CharacterConfig CHARACTERS[] = {")
    for character in characters:
        base = "CHAR_%s" % sym(character["name"])
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
        c.append("    },")
    c.append("};")
    c.append("const int CHARACTER_COUNT = %d;" % len(characters))
    c.append("")


def emit_rooms(c, rooms):
    for room in rooms:
        base = "ROOM_%s" % sym(room["name"])
        if room["decor"]:
            emit_array(c, "unsigned short", base + "_DECOR", room["decor"], per_line=12)
        if room["colliders"]:
            emit_array(c, "unsigned short", base + "_COLLIDERS", room["colliders"], per_line=12)

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

    c.append("const RoomConfig ROOMS[] = {")
    for room in rooms:
        base = "ROOM_%s" % sym(room["name"])

        decor_ref = (base + "_DECOR") if room["decor"] else "0"
        colliders_ref = (base + "_COLLIDERS") if room["colliders"] else "0"

        instant_ref = "0"
        if room["instant"]:
            instant_ref = "%s_INSTANT" % base
        interactive_ref = "0"
        if room["interactive"]:
            interactive_ref = "%s_INTERACTIVE" % base

        c.append("    { %s, %d, %s, %d, %d, %s, %s, %s, %d, %s, %d, %d, %d, %s, %s },"
                 % (cstr(room["name"]), room["hero"],
                    "true" if room["hasSpawn"] else "false",
                    room["spawnX"], room["spawnY"],
                    cstr(room["music"]) if room["music"] else "0",
                    cstr(room["walkSfx"]) if room["walkSfx"] else "0",
                    instant_ref, len(room["instant"]),
                    interactive_ref, len(room["interactive"]),
                    room["mapWidth"], room["mapHeight"],
                    decor_ref, colliders_ref))
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


def sync_runtime_assets():
    """Mirror resources/ into assets/ (what ships with the binary).

    Data files (.json/.tmx/.tsx) are excluded: they are baked into the
    generated C++ at build time, so the binary never reads them.
    """
    import shutil
    if os.path.isdir(ASSETS):
        shutil.rmtree(ASSETS)
    shutil.copytree(RES, ASSETS,
                    ignore=shutil.ignore_patterns("*.json", "*.tmx", "*.tsx", ".DS_Store"))
    count = sum(len(files) for _, _, files in os.walk(ASSETS))
    print("synced %d runtime files to assets/" % count)


def generate():
    dialogues = load_dialogues()
    dialogue_index = {d["name"]: i for i, d in enumerate(dialogues)}

    characters = load_characters()
    character_index = {c["name"]: i for i, c in enumerate(characters)}

    tileset = load_tileset()
    if tileset is None:
        warn("no tileset found in resources/tiles")

    rooms = load_rooms(dialogue_index, character_index)

    os.makedirs(res("ui"), exist_ok=True)
    border_path = res("ui", "border.png")
    if create_border_bitmap(border_path):
        print("created dialogue border bitmap: %s" % os.path.relpath(border_path, ROOT))

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
    h.append("extern const int ROOM_COUNT;")
    h.append("extern const RoomConfig ROOMS[];")
    h.append("")
    h.append("extern const int ENCOUNTER_TOTAL;")
    h.append("")
    h.append("extern const TilesetConfig TILESET;")
    h.append("")

    c = []
    c.append("// Generated by scripts/generate_assets.py - do not edit by hand.")
    c.append('#include "assets.h"')
    c.append("")
    emit_dialogues(c, dialogues)
    emit_characters(c, characters)
    emit_rooms(c, rooms)
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
    print("  %d dialogues, %d characters, %d rooms" % (len(dialogues), len(characters), len(rooms)))
    sync_runtime_assets()

    if len(rooms) == 0:
        warn("no rooms were generated - the game has nothing to load")
        return 1
    if len(characters) == 0:
        warn("no characters were generated")
    return 0


if __name__ == "__main__":
    sys.exit(generate())
