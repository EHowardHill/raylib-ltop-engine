#!/usr/bin/env python3
"""Game art for ENOKI AND THE HOLLOW KING.

Regenerates the visual identity of the resources/ game folder:
  - resources/tiles/texture.png  (the tileset, ids = layout order)
  - resources/tiles/tiles.json   (per-tile behavior metadata)
  - resources/ui/items.png       (one 16x16 icon per item, frame = item id)
  - character/enemy recolors     (palette work off the shipped sheets)
  - resources/scenes/*.png       (storybook cutscene cards)

The tile order below is the single source of truth for tile ids; the room
compiler (scripts/game_rooms.py) imports TILE_ID so rooms author tiles by
name and never by number. Existing shipped art is never overwritten — this
script writes the files it owns and skips the rest.
"""

import colorsys
import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(ROOT, "resources")
TILE = 32


# --------------------------------------------------------------------------
# PNG plumbing (stdlib only): RGBA truecolor out, any type in.

def write_png(path, width, height, rows):
    """rows: list of bytearrays, width*height*4 each (RGBA)."""
    raw = bytearray()
    for row in rows:
        raw.append(0)  # filter type 0
        raw += row
    def chunk(tag, data):
        out = struct.pack(">I", len(data)) + tag + data
        out += struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        return out
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    with open(path, "wb") as handle:
        handle.write(b"\x89PNG\r\n\x1a\n")
        handle.write(chunk(b"IHDR", ihdr))
        handle.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        handle.write(chunk(b"IEND", b""))


def read_png(path):
    """-> (width, height, ctype, palette, rows[RGBA bytearrays])."""
    with open(path, "rb") as handle:
        data = handle.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a png: %s" % path)
    width = height = ctype = 0
    palette = []
    trns = []
    idat = bytearray()
    pos = 8
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        tag = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        if tag == b"IHDR":
            width, height, _, ctype = struct.unpack(">IIBB", chunk[:10])
        elif tag == b"PLTE":
            palette = [tuple(chunk[i:i + 3]) for i in range(0, len(chunk), 3)]
        elif tag == b"tRNS":
            trns = list(chunk)
        elif tag == b"IDAT":
            idat += chunk
        pos += 12 + length
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    stride = width * channels
    raw = zlib.decompress(bytes(idat))
    rows = []
    prev = bytearray(stride)
    index = 0
    for y in range(height):
        filter_type = raw[index]
        index += 1
        line = bytearray(raw[index:index + stride])
        index += stride
        if filter_type == 1:
            for x in range(channels, stride):
                line[x] = (line[x] + line[x - channels]) & 255
        elif filter_type == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 255
        elif filter_type == 3:
            for x in range(stride):
                left = line[x - channels] if x >= channels else 0
                line[x] = (line[x] + ((left + prev[x]) >> 1)) & 255
        elif filter_type == 4:
            for x in range(stride):
                a = line[x - channels] if x >= channels else 0
                b = prev[x]
                c = prev[x - channels] if x >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                predict = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + predict) & 255
        rows.append(line)
        prev = line
    out = []
    alpha = list(trns) + [255] * (256 - len(trns))
    for line in rows:
        rgba = bytearray(width * 4)
        for x in range(width):
            i = x * channels
            o = x * 4
            if ctype == 3:
                idx = line[i]
                if idx < len(palette):
                    rgba[o:o + 3] = bytes(palette[idx])
                rgba[o + 3] = alpha[idx] if idx < len(alpha) else 255
            elif channels >= 3:
                rgba[o:o + 3] = bytes(line[i:i + 3])
                rgba[o + 3] = 255 if channels == 3 else line[i + 3]
            else:  # grayscale
                g = line[i]
                rgba[o:o + 3] = bytes((g, g, g))
                rgba[o + 3] = 255 if channels == 1 else line[i + 1]
        out.append(rgba)
    return width, height, ctype, palette, out


def load_rgba(path):
    return read_png(path)[4]


def hue_shift(rgb, degrees, sat_scale=1.0, lum_scale=1.0):
    """Shift a color's hue by degrees (wrapped), scaling saturation/lum."""
    r, g, b = rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0
    h, l, s = colorsys.rgb_to_hls(r, g, b)
    if s <= 0:
        return rgb
    h = (h + degrees / 360.0) % 1.0
    s = min(1.0, s * sat_scale)
    l = min(1.0, l * lum_scale)
    r, g, b = colorsys.hls_to_rgb(h, l, s)
    return (int(r * 255), int(g * 255), int(b * 255), rgb[3] if len(rgb) > 3 else 255)


def shift_image(path_out, path_in, degrees, sat_scale=1.0, lum_scale=1.0,
                 mask=None, mask_hue=None):
    """Recolor a shipped sheet by hue-shifting (optionally only colors whose
    hue is near mask_hue, so e.g. only the red coat turns)."""
    if os.path.exists(path_out):
        return False
    width, height, _, _, rows = read_png(path_in)
    out = []
    for line in rows:
        new = bytearray(line)
        for x in range(width):
            o = x * 4
            r, g, b, a = line[o], line[o + 1], line[o + 2], line[o + 3]
            if a < 8:
                continue
            if mask_hue is not None:
                h, l, s = colorsys.rgb_to_hls(r / 255.0, g / 255.0, b / 255.0)
                if abs(((h - mask_hue) % 1.0) * 360.0) > 40 and \
                   abs(((mask_hue - h) % 1.0) * 360.0) > 40:
                    continue
            if mask and not mask(r, g, b):
                continue
            shifted = hue_shift((r, g, b, a), degrees, sat_scale, lum_scale)
            new[o:o + 4] = bytes(shifted)
        out.append(new)
    write_png(path_out, width, height, out)
    return True


# --------------------------------------------------------------------------
# Deterministic speckle noise (stable art across runs).

class Rng:
    def __init__(self, seed):
        self.state = (seed & 0xFFFFFFFF) or 0x6D2B79F5

    def next(self):
        x = self.state
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        self.state = x
        return x

    def below(self, chance):  # True with probability chance
        return (self.next() & 0xFFFF) < int(chance * 65536)


def px(x, y, color):
    return (x, y, color)


# --------------------------------------------------------------------------
# The palette.

PAL = {
    "grass":      (96, 144, 64, 255),
    "grass2":     (86, 132, 58, 255),
    "grass3":     (76, 120, 52, 255),
    "grass_hi":   (124, 168, 84, 255),
    "grass_lo":   (60, 100, 44, 255),
    "flower_w":   (232, 236, 216, 255),
    "flower_y":   (216, 200, 92, 255),
    "flower_r":   (196, 108, 96, 255),
    "tuft":       (72, 116, 52, 255),
    "tuft_hi":    (110, 152, 74, 255),
    "path":       (196, 172, 120, 255),
    "path2":      (182, 156, 104, 255),
    "path_lo":    (152, 126, 84, 255),
    "path_hi":    (216, 192, 140, 255),
    "pebble":     (158, 138, 100, 255),
    "sand":       (222, 204, 152, 255),
    "sand2":      (206, 186, 136, 255),
    "sand_lo":    (176, 156, 112, 255),
    "water":      (36, 76, 140, 255),
    "water_hi":   (66, 110, 172, 255),
    "water2":     (44, 88, 152, 255),
    "water_lo":   (22, 52, 104, 255),
    "spark":      (148, 190, 228, 255),
    "shallow":    (88, 142, 186, 255),
    "shallow2":   (104, 158, 198, 255),
    "shallow_lo": (66, 116, 160, 255),
    "shallow_bed": (150, 176, 200, 255),
    "heal":       (118, 196, 208, 255),
    "heal_hi":    (156, 224, 230, 255),
    "heal_lo":    (84, 164, 184, 255),
    "ice":        (196, 222, 240, 255),
    "ice2":       (172, 204, 228, 255),
    "ice_lo":     (140, 176, 208, 255),
    "ice_dark":   (108, 148, 190, 255),
    "hole":       (24, 20, 24, 255),
    "hole2":     (44, 36, 40, 255),
    "spike":      (172, 176, 188, 255),
    "spike_hi":   (216, 220, 228, 255),
    "spike_lo":   (108, 112, 128, 255),
    "soil":       (150, 110, 70, 255),
    "soil_hi":    (172, 130, 86, 255),
    "soil_lo":    (120, 86, 56, 255),
    "soil_dark":  (96, 68, 44, 255),
    "rock":       (170, 142, 112, 255),
    "rock_hi":    (198, 170, 138, 255),
    "rock_lo":    (122, 98, 78, 255),
    "rock_dark":  (84, 68, 56, 255),
    "trunk":      (122, 88, 56, 255),
    "trunk_lo":   (88, 62, 40, 255),
    "leaf":       (64, 112, 52, 255),
    "leaf_hi":    (92, 140, 72, 255),
    "leaf_lo":    (42, 82, 40, 255),
    "hedge":      (56, 104, 48, 255),
    "hedge_hi":   (82, 132, 64, 255),
    "hedge_lo":   (36, 72, 36, 255),
    "wood":       (172, 128, 76, 255),
    "wood_hi":    (196, 152, 96, 255),
    "wood_lo":    (128, 88, 52, 255),
    "plaster":    (214, 198, 168, 255),
    "plaster2":   (198, 180, 148, 255),
    "plaster_lo": (164, 146, 120, 255),
    "beam":       (128, 96, 64, 255),
    "roof":       (178, 92, 72, 255),
    "roof2":      (156, 76, 62, 255),
    "roof_hi":    (204, 116, 88, 255),
    "roof_lo":    (118, 56, 48, 255),
    "door":       (96, 64, 40, 255),
    "hollow":     (104, 124, 96, 255),
    "hollow2":    (92, 110, 86, 255),
    "hollow_hi":  (128, 146, 116, 255),
    "hollow_lo":  (72, 88, 70, 255),
    "dfloor":     (104, 112, 132, 255),
    "dfloor2":    (92, 100, 120, 255),
    "dfloor_lo":  (74, 82, 100, 255),
    "dfloor_hi":  (126, 134, 154, 255),
    "dwall":      (84, 88, 108, 255),
    "dwall_hi":   (110, 114, 136, 255),
    "dwall_lo":   (58, 62, 80, 255),
    "dwall2":     (70, 74, 92, 255),
    "crack":      (44, 44, 54, 255),
    "crack_hi":   (30, 30, 38, 255),
}


def new_canvas():
    return [[(0, 0, 0, 0) for _ in range(TILE)] for _ in range(TILE)]


def fill(canvas, color):
    for y in range(TILE):
        for x in range(TILE):
            canvas[y][x] = color


def speckle(canvas, rng, color, chance=0.12, x0=0, y0=0, x1=TILE, y1=TILE):
    for y in range(y0, y1):
        for x in range(x0, x1):
            if rng.below(chance):
                canvas[y][x] = color


def dots(canvas, rng, color, count, x0=0, y0=0, x1=TILE, y1=TILE):
    for _ in range(count):
        x = x0 + rng.next() % max(1, x1 - x0)
        y = y0 + rng.next() % max(1, y1 - y0)
        canvas[y][x] = color


def rect(canvas, color, x0, y0, x1, y1):
    for y in range(max(0, y0), min(TILE, y1)):
        for x in range(max(0, x0), min(TILE, x1)):
            canvas[y][x] = color


def hline(canvas, color, y, x0=0, x1=TILE):
    rect(canvas, color, x0, y, x1, y + 1)


def vline(canvas, color, x, y0=0, y1=TILE):
    rect(canvas, color, x, y0, x + 1, y1)


# --------------------------------------------------------------------------
# Tile painters. Each returns a 32x32 canvas. Order = tile id (1-based).

def grass(base, rng, extras=()):
    c = new_canvas()
    fill(c, base)
    speckle(c, rng, PAL["grass_lo"], 0.10)
    speckle(c, rng, PAL["grass_hi"], 0.08)
    for color, chance in extras:
        speckle(c, rng, color, chance)
    return c


def tile_grass_a(rng):
    return grass(PAL["grass"], rng, ((PAL["grass_hi"], 0.06),))


def tile_grass_b(rng):
    c = grass(PAL["grass"], rng)
    # little blade tufts
    for _ in range(3):
        bx = rng.next() % 24 + 4
        by = rng.next() % 22 + 6
        c[by][bx] = PAL["grass_lo"]
        c[by + 1][bx] = PAL["grass_hi"]
        c[by][bx + 1] = PAL["grass_hi"]
        c[by + 1][bx + 1] = PAL["grass2"]
    return c


def tile_grass_c(rng):
    c = grass(PAL["grass"], rng)
    for _ in range(14):
        x = rng.next() % 30 + 1
        y = rng.next() % 30 + 1
        c[y][x] = PAL["grass_lo"]
        c[y][x + 1] = PAL["grass_hi"]
    return c


def tile_flowers(rng):
    c = grass(PAL["grass"], rng)
    for color in (PAL["flower_w"], PAL["flower_y"], PAL["flower_r"]):
        for _ in range(3):
            x = rng.next() % 28 + 2
            y = rng.next() % 28 + 2
            c[y][x] = PAL["grass_lo"]
            c[y][x + 1] = color
            c[y + 1][x] = color
            c[y][x - 1] = color
            c[y - 1][x] = color
    return c


def tile_tuft(rng):
    """Cuttable tall grass (sword cuts it, revealing plain grass)."""
    c = grass(PAL["grass"], rng)
    for _ in range(7):
        bx = 2 + rng.next() % 26
        by = 3 + rng.next() % 24
        h = 3 + rng.next() % 3
        for i in range(h):
            y = by + i
            if 0 <= y < TILE:
                c[y][bx] = PAL["tuft"]
                if i == h - 1:
                    c[y][bx] = PAL["tuft_hi"]
                if rng.below(0.4) and 0 <= y < TILE and bx + 1 < TILE:
                    c[y][bx + 1] = PAL["tuft"]
    return c


def pathed(base, rng, pebbles=0.05):
    c = new_canvas()
    fill(c, base)
    speckle(c, rng, PAL["path_lo"], 0.08)
    speckle(c, rng, PAL["path_hi"], 0.06)
    if pebbles:
        dots(c, rng, PAL["pebble"], int(6 * pebbles * 40))
    return c


def tile_path(rng):
    return pathed(PAL["path"], rng)


def tile_path_b(rng):
    c = pathed(PAL["path"], rng, pebbles=0.10)
    for _ in range(4):
        x = rng.next() % 26 + 3
        y = rng.next() % 26 + 3
        c[y][x] = PAL["pebble"]
        c[y][x + 1] = PAL["path_lo"]
        c[y + 1][x] = PAL["path_lo"]
    return c


def sand(rng):
    c = new_canvas()
    fill(c, PAL["sand"])
    speckle(c, rng, PAL["sand_lo"], 0.10)
    speckle(c, rng, PAL["sand2"], 0.08)
    return c


def tile_sand_b(rng):
    c = sand(rng)
    # ripple lines
    for y in range(6, TILE, 8):
        for x in range(2, TILE - 2):
            if rng.below(0.7):
                c[y][x] = PAL["sand_lo"]
    return c


def water(base, rng, anim_phase=0):
    c = new_canvas()
    fill(c, base)
    speckle(c, rng, PAL["water_lo"], 0.06)
    # horizontal wave bands, staggered per phase
    for y in range(4 + anim_phase * 3, TILE, 8):
        for x in range(1, TILE - 1):
            if rng.below(0.55):
                c[y][x] = PAL["water_hi"]
    for _ in range(3):
        x = rng.next() % 24 + 4
        y = rng.next() % 26 + 3
        c[y][x] = PAL["spark"]
        if anim_phase % 2 == 0 and x + 2 < TILE:
            c[y][x + 2] = PAL["spark"]
    return c


def tile_deep_a(rng):
    return water(PAL["water"], rng, 0)


def tile_deep_b(rng):
    return water(PAL["water"], rng, 1)


def tile_deep_c(rng):
    return water(PAL["water"], rng, 2)


def shallow(base, rng, phase=0):
    c = new_canvas()
    fill(c, base)
    speckle(c, rng, PAL["shallow_lo"], 0.07)
    dots(c, rng, PAL["shallow_bed"], 6)
    for y in range(3 + phase * 3, TILE, 7):
        for x in range(0, TILE):
            if rng.below(0.4):
                c[y][x] = PAL["shallow2"]
    return c


def tile_shallow_a(rng):
    return shallow(PAL["shallow"], rng, 0)


def tile_shallow_b(rng):
    return shallow(PAL["shallow"], rng, 1)


def tile_shallow_c(rng):
    return shallow(PAL["shallow"], rng, 2)


def heal_water(rng, phase=0):
    c = shallow(PAL["heal"], rng, phase)
    speckle(c, rng, PAL["heal_hi"], 0.05)
    for _ in range(5):
        x = rng.next() % 28 + 2
        y = rng.next() % 28 + 2
        c[y][x] = (220, 250, 252, 255)
    return c


def tile_heal_a(rng):
    return heal_water(rng, 0)


def tile_heal_b(rng):
    return heal_water(rng, 1)


def tile_heal_c(rng):
    return heal_water(rng, 2)


def tile_ice(rng):
    c = new_canvas()
    fill(c, PAL["ice"])
    speckle(c, rng, PAL["ice_lo"], 0.08)
    speckle(c, rng, PAL["ice2"], 0.10)
    # sheen streaks
    for x in range(3, TILE, 9):
        for y in range(2, TILE - 2):
            if rng.below(0.8):
                c[y][x] = PAL["ice_lo"]
            if y % 7 == 0 and y < TILE - 1:
                c[y][x] = PAL["ice_dark"]
    return c


def tile_pit(rng):
    c = grass(PAL["grass"], rng)
    rect(c, PAL["grass_lo"], 4, 4, TILE - 4, TILE - 4)
    rect(c, PAL["rock_dark"], 6, 6, TILE - 6, TILE - 6)
    rect(c, PAL["hole"], 8, 8, TILE - 8, TILE - 8)
    # rim shading
    for i in range(6, TILE - 6):
        c[6][i] = PAL["hole2"]
        c[TILE - 7][i] = PAL["hole"]
        c[i][6] = PAL["hole2"]
        c[i][TILE - 7] = PAL["hole"]
    return c


def spike_row(c, y, x0, w, rng):
    """One spike: a silver wedge with a bright tip."""
    for row in range(8):
        span = max(1, w - row)
        for i in range(span):
            x = x0 + i + row // 2
            if 0 <= y + row < TILE and 0 <= x < TILE:
                c[y + row][x] = PAL["spike"]
        if 0 <= y + row < TILE and 0 <= x0 + row // 2 < TILE:
            c[y + row][x0 + row // 2] = PAL["spike_hi"]
    for i in range(w):
        x = x0 + i + 4
        if 0 <= y + 8 < TILE and 0 <= x < TILE:
            c[y + 8][x] = PAL["spike_lo"]


def spikes(base_floor):
    def paint(rng):
        c = base_floor(rng)
        for spike in range(4):
            x0 = 1 + spike * 8
            y0 = 7 + (spike % 2) * 2
            spike_row(c, y0, x0, 7, rng)
        return c
    return paint


def tile_spikes_grass(rng):
    return spikes(lambda r: grass(PAL["grass"], r))(rng)


def tile_soil(rng):
    c = new_canvas()
    fill(c, PAL["soil"])
    for y in range(2, TILE, 5):
        for x in range(1, TILE - 1):
            if rng.below(0.6):
                c[y][x] = PAL["soil_lo"]
    speckle(c, rng, PAL["soil_hi"], 0.08)
    return c


def tile_soil_dug(rng):
    c = tile_soil(rng)
    rect(c, PAL["soil_lo"], 6, 8, TILE - 6, TILE - 8)
    rect(c, PAL["soil_dark"], 7, 9, TILE - 7, TILE - 7)
    for _ in range(6):
        x = 8 + rng.next() % 16
        y = 10 + rng.next() % 12
        c[y][x] = PAL["soil_hi"]
    return c


def current(base, arrow_color, rng, dx, dy):
    """Flowing shallow water with chevrons showing the push direction."""
    c = shallow(base, rng)
    for k in range(4):
        cx = 16 + (k - 2) * 7
        cy = 16
        for i in (-3, 0, 3):
            x = cx + i * 0
            y = cy + i
            # chevron pointing along (dx, dy)
            for t in range(4):
                xx = cx + (t - 1)
                yy = y + (dy * (t - 2) if dy else 0)
                if 0 <= xx < TILE and 0 <= yy < TILE:
                    c[yy][xx] = arrow_color if t < 3 else base
    return c


def tile_curb_r(rng):
    c = current(PAL["shallow"], (216, 240, 250, 255), rng, 1, 0)
    for y in range(10, 22):
        for t in range(4):
            x = 14 + t
            c[y][x] = PAL["shallow_bed"]
    return c


def tile_curb_l(rng):
    return current(PAL["shallow"], (216, 240, 250, 255), rng, -1, 0)


def tile_curb_d(rng):
    return current(PAL["shallow"], (216, 240, 250, 255), rng, 0, 1)


def tile_curb_u(rng):
    return current(PAL["shallow"], (216, 240, 250, 255), rng, 0, -1)


def ledge(direction, floor_fn):
    """A grass lip with a dark face: vaulting hops across it along direction."""
    def paint(rng):
        c = floor_fn(rng)
        lip = PAL["grass_hi"]
        lo = PAL["grass_lo"]
        dark = PAL["rock_dark"]
        if direction == "down":  # face visible on the south side
            hline(c, lo, 0)
            rect(c, lip, 0, 1, TILE, 3)
            rect(c, PAL["rock"], 0, 4, TILE, 8)
            rect(c, PAL["rock_lo"], 0, 8, TILE, 10)
            rect(c, dark, 0, 10, TILE, 12)
            for y in range(12, TILE):
                hline(c, PAL["grass2"], y)
            rect(c, PAL["grass"], 0, 12, TILE, TILE)
            speckle(c, rng, PAL["grass_lo"], 0.08, 0, 12)
        elif direction == "up":
            rect(c, dark, 0, 0, TILE, 6)
            rect(c, PAL["rock_lo"], 0, 6, TILE, 9)
            rect(c, PAL["rock"], 0, 9, TILE, 11)
            rect(c, lip, 0, 11, TILE, 13)
            hline(c, lo, 13)
        elif direction == "left":
            rect(c, dark, 0, 0, 6, TILE)
            rect(c, PAL["rock_lo"], 6, 0, 9, TILE)
            rect(c, PAL["rock"], 9, 0, 11, TILE)
            vline(c, lip, 11)
            vline(c, lip, 12)
            vline(c, lo, 13)
        else:
            vline(c, lo, 0)
            vline(c, lip, 1)
            vline(c, lip, 2)
            vline(c, lo, 3)
            rect(c, PAL["rock"], 4, 0, 6, TILE)
            rect(c, PAL["rock_lo"], 6, 0, 9, TILE)
            rect(c, dark, 9, 0, 13, TILE)
        return c
    return paint


def tile_ledge_down(rng):
    return ledge("down", lambda r: grass(PAL["grass"], r))(rng)


def tile_ledge_up(rng):
    return ledge("up", lambda r: grass(PAL["grass2"], r))(rng)


def tile_ledge_left(rng):
    return ledge("left", lambda r: grass(PAL["grass"], r))(rng)


def tile_ledge_right(rng):
    return ledge("right", lambda r: grass(PAL["grass2"], r))(rng)


def cliffish(rng, cracked=False):
    c = new_canvas()
    # grass cap
    rect(c, PAL["grass"], 0, 0, TILE, 6)
    rect(c, PAL["grass_hi"], 0, 0, TILE, 2)
    hline(c, PAL["grass_lo"], 6)
    # rock face
    rect(c, PAL["rock"], 0, 7, TILE, TILE)
    rect(c, PAL["rock_hi"], 0, 7, TILE, 9)
    rect(c, PAL["rock_lo"], 0, 26, TILE, TILE)
    # strata + cracks
    for y in range(11, 26, 6):
        for x in range(0, TILE):
            c[y][x] = PAL["rock_lo"] if rng.below(0.75) else PAL["rock"]
    for x in range(4, TILE, 10):
        for y in range(9, 24):
            c[y][x] = PAL["rock_dark"] if rng.below(0.8) else PAL["rock_lo"]
    if cracked:
        # blast crack
        cx, cy = 16, 18
        for t in range(-9, 10):
            x = cx + t
            y = cy + (t * t) // 22 - 4
            if 7 <= y < TILE and 0 <= x < TILE:
                c[y][x] = PAL["crack"] if t % 3 else PAL["crack_hi"]
        for t in range(-5, 6):
            x = cx + t // 2
            y = cy + t
            if 7 <= y < TILE and 0 <= x < TILE:
                c[y][x] = PAL["crack"]
    return c


def tile_cliff(rng):
    return cliffish(rng)


def tile_cliff_face(rng):
    """Wall with no grass cap (interior faces / dungeon-adjacent)."""
    c = new_canvas()
    rect(c, PAL["rock"], 0, 0, TILE, TILE)
    rect(c, PAL["rock_hi"], 0, 0, TILE, 3)
    rect(c, PAL["rock_lo"], 0, 27, TILE, TILE)
    for y in range(6, 27, 7):
        for x in range(0, TILE):
            c[y][x] = PAL["rock_lo"] if rng.below(0.7) else PAL["rock"]
    for x in range(5, TILE, 11):
        for y in range(3, 28):
            if rng.below(0.7):
                c[y][x] = PAL["rock_dark"]
    return c


def tile_bombrock(rng):
    return cliffish(rng, cracked=True)


def tile_tree(rng):
    """A full tree drawn as a collider (canopy goes in the upper layer)."""
    c = grass(PAL["grass"], rng)
    # trunk
    rect(c, PAL["trunk"], 13, 20, 19, 31)
    rect(c, (86, 62, 40, 255), 13, 20, 15, 31)
    rect(c, PAL["trunk_lo"], 17, 20, 19, 31)
    # root flare
    rect(c, PAL["trunk"], 11, 29, 13, 31)
    rect(c, PAL["trunk"], 19, 29, 21, 31)
    # crown
    cx, cy = 16, 14
    for y in range(TILE):
        for x in range(TILE):
            dx, dy = x - cx, y - cy
            d = (dx * dx) / 130.0 + (dy * dy) / 150.0
            if d < 1.0:
                c[y][x] = PAL["leaf"]
                if rng.below(0.18):
                    c[y][x] = PAL["leaf_hi"]
                if rng.below(0.10):
                    c[y][x] = PAL["leaf_lo"]
            elif d < 1.18:
                c[y][x] = PAL["leaf_lo"]
    return c


def tile_canopy(rng):
    """Upper-layer canopy: dense foliage hiding the tile behind it."""
    c = new_canvas()
    fill(c, PAL["leaf"])
    speckle(c, rng, PAL["leaf_hi"], 0.16)
    speckle(c, rng, PAL["leaf_lo"], 0.16)
    speckle(c, rng, (36, 70, 34, 255), 0.06)
    return c


def tile_hedge(rng):
    c = grass(PAL["grass2"], rng)
    for y in range(3, 29):
        for x in range(1, 31):
            dx, dy = x - 16, y - 16
            d = (dx * dx) / 190.0 + (dy * dy) / 165.0
            if d < 1.0:
                c[y][x] = PAL["hedge"] if rng.below(0.8) else PAL["hedge_hi"]
            elif d < 1.15:
                c[y][x] = PAL["hedge_lo"]
    hline(c, PAL["hedge_lo"], 28)
    return c


def tile_fence(rng):
    c = grass(PAL["grass"], rng)
    hline(c, PAL["wood"], 13, 1, 31)
    hline(c, PAL["wood_hi"], 14, 1, 31)
    hline(c, PAL["wood_lo"], 15, 1, 31)
    for x in (3, 12, 21, 28):
        rect(c, PAL["wood"], x, 8, x + 3, 24)
        rect(c, PAL["wood_hi"], x, 8, x + 1, 24)
        rect(c, PAL["wood_lo"], x + 2, 8, x + 3, 24)
    return c


def tile_boulder(rng):
    c = grass(PAL["grass"], rng)
    cx, cy = 16, 17
    for y in range(4, 30):
        for x in range(2, 30):
            dx, dy = x - cx, y - cy
            d = (dx * dx) / 150.0 + (dy * dy) / 130.0
            if d < 1.0:
                c[y][x] = (140, 140, 148, 255) if rng.below(0.7) else (164, 164, 172, 255)
            elif d < 1.16:
                c[y][x] = (104, 104, 116, 255)
    rect(c, (88, 88, 100, 255), 10, 22, 18, 26)
    return c


def tile_house_wall(rng):
    c = new_canvas()
    rect(c, PAL["plaster"], 0, 0, TILE, 26)
    speckle(c, rng, PAL["plaster_lo"], 0.06)
    speckle(c, rng, PAL["plaster2"], 0.10)
    # timber frame
    rect(c, PAL["beam"], 0, 0, TILE, 3)
    rect(c, (96, 96, 96, 255), 0, 26, TILE, 31)
    rect(c, (70, 70, 78, 255), 0, 29, TILE, 31)
    rect(c, (128, 128, 136, 255), 0, 26, TILE, 27)
    vline(c, PAL["beam"], 4)
    vline(c, PAL["beam"], 27)
    return c


def roof(kind):
    def paint(rng):
        c = new_canvas()
        fill(c, PAL[kind])
        for y in range(0, TILE, 4):
            hline(c, PAL["roof_lo"], y)
            for x in range((y // 4 % 2) * 4, TILE, 8):
                vline(c, PAL["roof_hi"], x, y, min(TILE, y + 4))
        return c
    return paint


def tile_roof(rng):
    return roof("roof")(rng)


def tile_roof_b(rng):
    return roof("roof2")(rng)


def hollow_grass(base):
    def paint(rng):
        c = new_canvas()
        fill(c, base)
        speckle(c, rng, PAL["hollow_lo"], 0.12)
        speckle(c, rng, PAL["hollow_hi"], 0.05)
        return c
    return paint


def tile_hollow_a(rng):
    return hollow_grass(PAL["hollow"])(rng)


def tile_hollow_b(rng):
    c = hollow_grass(PAL["hollow"])(rng)
    for _ in range(3):
        x = rng.next() % 26 + 3
        y = rng.next() % 26 + 3
        rect(c, PAL["hollow_lo"], x, y, x + 3, y + 1)
        c[y][x] = PAL["hollow_hi"]
    return c


def dungeon_floor(kind):
    def paint(rng):
        c = new_canvas()
        fill(c, PAL["dfloor"])
        # grout grid
        for y in (0, 16):
            hline(c, PAL["dfloor_lo"], y)
        vline(c, PAL["dfloor_lo"], 0)
        vline(c, PAL["dfloor_lo"], 16)
        speckle(c, rng, PAL["dfloor_hi"] if kind else PAL["dfloor_lo"], 0.07)
        if kind == 2:
            # checker shading on one quadrant
            rect(c, PAL["dfloor2"], 17, 1, 31, 15)
            rect(c, PAL["dfloor2"], 1, 17, 15, 31)
        if kind == 3:
            # mossy/dark patches
            dots(c, rng, PAL["dfloor_lo"], 14)
        return c
    return paint


def tile_d_floor(rng):
    return dungeon_floor(0)(rng)


def tile_d_floor_b(rng):
    return dungeon_floor(2)(rng)


def tile_d_floor_c(rng):
    return dungeon_floor(3)(rng)


def tile_d_wall(rng):
    c = new_canvas()
    fill(c, PAL["dwall"])
    # brick courses
    for row in range(4):
        y0 = row * 8
        hline(c, PAL["dwall_lo"], y0)
        hline(c, PAL["dwall_hi"], y0 + 1)
        offset = (row % 2) * 8
        for x in range(offset + 8, TILE, 16):
            vline(c, PAL["dwall_lo"], x, y0 + 2, y0 + 8)
            vline(c, PAL["dwall_hi"], x + 1, y0 + 2, y0 + 8)
    hline(c, PAL["dwall_lo"], 31)
    return c


def tile_d_wall2(rng):
    c = tile_d_wall(rng)
    # variant: split courses + darker key line
    rect(c, PAL["dwall2"], 0, 12, TILE, 20)
    hline(c, PAL["dwall_hi"], 13)
    hline(c, PAL["dwall_lo"], 19)
    return c


def tile_d_pillar(rng):
    c = tile_d_wall(rng)
    rect(c, PAL["dwall_lo"], 6, 0, 8, TILE)
    rect(c, PAL["dwall_lo"], 24, 0, 26, TILE)
    rect(c, PAL["dwall_hi"], 8, 0, 24, TILE)
    rect(c, PAL["dwall"], 14, 0, 18, TILE)
    rect(c, (58, 62, 82, 255), 6, 28, 26, TILE)
    return c


def tile_d_crack(rng):
    c = tile_d_wall(rng)
    cx, cy = 16, 16
    for t in range(-10, 11):
        x = cx + t
        y = cy + t * t // 26
        if 0 <= y < TILE and 0 <= x < TILE:
            c[y][x] = PAL["crack"] if t % 3 else PAL["crack_hi"]
    for t in range(-7, 8):
        x = cx + t
        y = cy - t
        if 0 <= y < TILE and 0 <= x < TILE:
            c[y][x] = PAL["crack"]
    return c


def tile_d_pit(rng):
    c = dungeon_floor(0)(rng)
    rect(c, PAL["dfloor_lo"], 3, 3, TILE - 3, TILE - 3)
    rect(c, (36, 38, 50, 255), 5, 5, TILE - 5, TILE - 5)
    rect(c, PAL["hole2"], 7, 7, TILE - 7, TILE - 7)
    rect(c, PAL["hole"], 9, 9, TILE - 9, TILE - 9)
    return c


def tile_d_spikes(rng):
    return spikes(dungeon_floor(0))(rng)


def tile_d_rubble(rng):
    c = dungeon_floor(0)(rng)
    for _ in range(4):
        x = rng.next() % 24 + 3
        y = rng.next() % 24 + 3
        rect(c, PAL["dwall_hi"], x, y, x + 2, y + 2)
        c[y][x] = PAL["dwall_lo"]
    dots(c, rng, PAL["dwall_lo"], 8)
    return c


# --------------------------------------------------------------------------
# Tile table: (name, painter, meta) in id order. meta keys match
# resources/tiles/tiles.json exactly.

TILE_SPECS = [
    # 1-8 ground
    ("grass_a",     tile_grass_a,    {}),
    ("grass_b",     tile_grass_b,    {}),
    ("grass_c",     tile_grass_c,    {}),
    ("flowers",     tile_flowers,    {}),
    ("tuft",        tile_tuft,       {"cuttable": True, "cut_result": 1}),
    ("path",        tile_path,       {}),
    ("path_b",      tile_path_b,     {}),
    ("sand_a",      sand,            {}),
    # 9-16 water
    ("sand_b",      tile_sand_b,     {}),
    ("deep_a",      tile_deep_a,     {"water": "deep",    "anim": [10, 11, 12]}),
    ("deep_b",      tile_deep_b,     {}),
    ("deep_c",      tile_deep_c,     {}),
    ("shallow_a",   tile_shallow_a,  {"water": "shallow", "anim": [13, 14, 15]}),
    ("shallow_b",   tile_shallow_b,  {}),
    ("shallow_c",   tile_shallow_c,  {}),
    ("heal_a",      tile_heal_a,     {"water": "shallow", "heal": True, "anim": [16, 17, 18]}),
    # 17-24 springs, ice, hazards, soil, currents
    ("heal_b",      tile_heal_b,     {}),
    ("heal_c",      tile_heal_c,     {}),
    ("ice",         tile_ice,        {"ice": True}),
    ("pit",         tile_pit,        {"pit": True}),
    ("spikes",      tile_spikes_grass, {"damage": 1}),
    ("soil",        tile_soil,       {"dig": True}),
    ("soil_dug",    tile_soil_dug,   {}),
    ("curb_r",      tile_curb_r,     {"water": "shallow", "push": [48, 0]}),
    # 25-32 currents, ledges, bombable rock
    ("curb_l",      tile_curb_l,     {"water": "shallow", "push": [-48, 0]}),
    ("curb_d",      tile_curb_d,     {"water": "shallow", "push": [0, 48]}),
    ("curb_u",      tile_curb_u,     {"water": "shallow", "push": [0, -48]}),
    ("ledge_d",     tile_ledge_down,  {"ledge": "down"}),
    ("ledge_u",     tile_ledge_up,    {"ledge": "up"}),
    ("ledge_l",     tile_ledge_left,  {"ledge": "left"}),
    ("ledge_r",     tile_ledge_right, {"ledge": "right"}),
    ("bombrock",    tile_bombrock,    {"bombable": True, "bomb_result": 0}),
    # 33-40 overworld solids
    ("cliff",       tile_cliff,      {}),
    ("cliff_face",  tile_cliff_face, {}),
    ("tree",        tile_tree,       {}),
    ("canopy",      tile_canopy,     {}),
    ("hedge",       tile_hedge,      {}),
    ("fence",       tile_fence,      {}),
    ("boulder",     tile_boulder,    {}),
    ("wall_house",  tile_house_wall, {}),
    # 41-48 houses, hollow world
    ("roof_a",      tile_roof,       {}),
    ("roof_b",      tile_roof_b,     {}),
    ("hollow_a",    tile_hollow_a,   {}),
    ("hollow_b",    tile_hollow_b,   {}),
    ("d_floor",     tile_d_floor,    {}),
    ("d_floor_b",   tile_d_floor_b,  {}),
    ("d_wall",      tile_d_wall,     {}),
    ("d_wall2",     tile_d_wall2,    {}),
    # 49-56 dungeon
    ("d_pillar",    tile_d_pillar,   {}),
    ("d_crack",     tile_d_crack,    {"bombable": True, "bomb_result": 0}),
    ("d_pit",       tile_d_pit,      {"pit": True}),
    ("d_spikes",    tile_d_spikes,   {"damage": 1}),
    ("d_rubble",    tile_d_rubble,   {}),
    ("d_floor_c",   tile_d_floor_c,  {}),
    ("spare_b",     tile_d_rubble,  {}),
    ("spare_c",     tile_d_rubble,  {}),
]

TILE_ID = {}
for _i, (_name, _paint, _meta) in enumerate(TILE_SPECS):
    TILE_ID[_name] = _i + 1

TILESET_COLUMNS = 8


def build_tileset(path_png, path_json):
    rows_needed = (len(TILE_SPECS) + TILESET_COLUMNS - 1) // TILESET_COLUMNS
    height = rows_needed * TILE
    width = TILESET_COLUMNS * TILE
    canvas = [[(0, 0, 0, 0) for _ in range(width)] for _ in range(height)]
    meta_out = {}
    for index, (name, paint, meta) in enumerate(TILE_SPECS):
        rng = Rng(0xBEEF + index * 7919)
        tile_canvas = paint(rng)
        tx = (index % TILESET_COLUMNS) * TILE
        ty = (index // TILESET_COLUMNS) * TILE
        for y in range(TILE):
            for x in range(TILE):
                canvas[ty + y][tx + x] = tile_canvas[y][x]
        if meta:
            meta_out[str(index + 1)] = meta
    rows = []
    for y in range(height):
        row = bytearray()
        for x in range(width):
            row += bytes(canvas[y][x])
        rows.append(row)
    write_png(path_png, width, height, rows)
    with open(path_json, "w") as handle:
        handle.write('{\n    "tiles": {\n')
        entries = []
        for key in sorted(meta_out, key=int):
            entries.append("        %s: %s" % ('"%s"' % key, json_dumps(meta_out[key])))
        handle.write(",\n".join(entries))
        handle.write("\n    }\n}\n")
    return len(TILE_SPECS)


def json_dumps(value):
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, float)):
        return str(value)
    if isinstance(value, str):
        return '"%s"' % value
    if isinstance(value, (list, tuple)):
        return "[" + ", ".join(json_dumps(v) for v in value) + "]"
    if isinstance(value, dict):
        return "{" + ", ".join('%s: %s' % ('"%s"' % k, json_dumps(v))
                               for k, v in value.items()) + "}"
    raise ValueError(value)


# --------------------------------------------------------------------------
# Item icons: one 16x16 frame per item, id = sorted filename order. The
# icon painter table is keyed by item name (the json stem).

ICON_BASE = (32, 34, 52, 255)
ICON_HI = (216, 220, 228, 255)
ICON_MID = (128, 134, 156, 255)
GOLD = (216, 178, 74, 255)
STEEL = (186, 192, 206, 255)
STEEL_D = (110, 116, 134, 255)
WOODC = (150, 104, 56, 255)
RED = (196, 84, 72, 255)
BLUE = (88, 122, 196, 255)
GREEN = (92, 160, 88, 255)
PURPLE = (150, 108, 190, 255)
ORANGE = (224, 140, 72, 255)
WHITE = (244, 244, 244, 255)
CYAN = (128, 200, 224, 255)


def icon_canvas():
    c = [[(0, 0, 0, 0) for _ in range(16)] for _ in range(16)]
    # faint slot border
    for x in range(16):
        c[0][x] = (40, 44, 62, 255)
        c[15][x] = (40, 44, 62, 255)
    for y in range(16):
        c[y][0] = (40, 44, 62, 255)
        c[y][15] = (40, 44, 62, 255)
    return c


def ic_rect(c, color, x0, y0, x1, y1):
    for y in range(max(0, y0), min(16, y1)):
        for x in range(max(0, x0), min(16, x1)):
            c[y][x] = color


def icon_sword(c):
    # diagonal blade
    for i in range(9):
        c[3 + i][9 - i] = STEEL
        c[3 + i][10 - i] = STEEL_D
    ic_rect(c, GOLD, 10, 2, 12, 4)
    ic_rect(c, WOODC, 12, 3, 15, 6)
    ic_rect(c, GOLD, 12, 5, 15, 7)


def icon_bombos(c):
    # medallion: sun disc
    ic_rect(c, GOLD, 5, 5, 11, 11)
    ic_rect(c, (240, 208, 120, 255), 6, 6, 10, 7)
    for i in range(8):
        c[2 + i][2] = GOLD if i % 2 == 0 else (0, 0, 0, 0)
        c[2 + i][13] = GOLD if i % 2 == 0 else (0, 0, 0, 0)
        c[2][2 + i] = GOLD if i % 2 == 0 else (0, 0, 0, 0)
        c[13][2 + i] = GOLD if i % 2 == 0 else (0, 0, 0, 0)


def icon_bombs(c):
    ic_rect(c, (60, 60, 72, 255), 4, 7, 10, 13)
    ic_rect(c, (92, 92, 108, 255), 4, 7, 6, 13)
    ic_rect(c, WHITE, 6, 9, 7, 10)
    ic_rect(c, ORANGE, 8, 4, 9, 5)
    c[8][3] = ORANGE
    c[7][3] = (200, 160, 90, 255)


def icon_boomerang(c):
    for i in range(6):
        c[5 + i][4] = RED
        c[5 + i][5] = (150, 60, 50, 255)
        c[5][4 + i] = RED
        c[6][4 + i] = (150, 60, 50, 255)
        c[11 - i][11] = RED
    ic_rect(c, (150, 60, 50, 255), 6, 6, 8, 8)


def icon_bottle(c):
    ic_rect(c, CYAN, 6, 4, 7, 5)
    ic_rect(c, (200, 230, 240, 255), 6, 2, 7, 4)
    ic_rect(c, (140, 180, 200, 255), 4, 6, 9, 13)
    ic_rect(c, CYAN, 5, 9, 8, 13)
    ic_rect(c, WHITE, 5, 7, 6, 9)


def icon_bow(c):
    for i in range(9):
        t = i - 4
        x = 4 + (t * t) // 5
        if x < 16:
            c[2 + i][x] = WOODC
            if x + 1 < 16:
                c[2 + i][x + 1] = (110, 76, 40, 255)
    ic_rect(c, (60, 44, 28, 255), 4, 2, 5, 3)
    ic_rect(c, (60, 44, 28, 255), 4, 12, 5, 14)
    ic_rect(c, STEEL, 2, 7, 14, 8)


def icon_byrna(c):
    # spark ring
    ring = [(7, 2), (8, 2), (10, 3), (12, 5), (13, 7), (13, 8),
            (12, 10), (10, 12), (8, 13), (7, 13), (5, 12), (3, 10),
            (2, 8), (2, 7), (3, 5), (5, 3)]
    for x, y in ring:
        c[y][x] = WHITE
        c[y + 1][x + 1] = (150, 160, 200, 255)
    ic_rect(c, (150, 160, 200, 255), 7, 7, 9, 9)


def icon_cape(c):
    ic_rect(c, PURPLE, 5, 3, 11, 5)
    ic_rect(c, PURPLE, 4, 5, 12, 12)
    ic_rect(c, (110, 78, 148, 255), 4, 10, 12, 12)
    ic_rect(c, (110, 78, 148, 255), 5, 5, 6, 10)
    ic_rect(c, GOLD, 7, 3, 9, 5)


def icon_compass(c):
    ic_rect(c, GOLD, 4, 4, 12, 12)
    ic_rect(c, (150, 120, 50, 255), 4, 11, 12, 12)
    ic_rect(c, WHITE, 6, 6, 10, 10)
    ic_rect(c, RED, 7, 6, 9, 8)
    ic_rect(c, BLUE, 7, 8, 9, 10)


def icon_ether(c):
    # medallion: snowflake
    ic_rect(c, CYAN, 7, 2, 9, 14)
    ic_rect(c, CYAN, 2, 7, 14, 9)
    for i in range(5):
        c[3 + i][3 + i] = CYAN
        c[12 - i][12 - i] = CYAN
        c[12 - i][3 + i] = CYAN
        c[3 + i][12 - i] = CYAN
    ic_rect(c, WHITE, 7, 7, 9, 9)


def icon_quake(c):
    ic_rect(c, (120, 100, 88, 255), 5, 5, 11, 11)
    ic_rect(c, (150, 126, 108, 255), 5, 5, 11, 7)
    for i in range(6):
        c[5 + i][8] = (40, 34, 30, 255)
    c[8][7] = (40, 34, 30, 255)
    c[8][9] = (40, 34, 30, 255)


def icon_fire_rod(c):
    ic_rect(c, RED, 3, 11, 12, 13)
    ic_rect(c, ORANGE, 4, 12, 11, 13)
    ic_rect(c, (240, 200, 110, 255), 5, 3, 8, 6)
    ic_rect(c, RED, 5, 4, 8, 7)
    ic_rect(c, ORANGE, 6, 2, 8, 5)
    ic_rect(c, GOLD, 4, 8, 6, 11)


def icon_flippers(c):
    ic_rect(c, RED, 3, 6, 8, 9)
    ic_rect(c, RED, 9, 6, 14, 9)
    ic_rect(c, (150, 60, 50, 255), 3, 9, 8, 10)
    ic_rect(c, (150, 60, 50, 255), 9, 9, 14, 10)
    ic_rect(c, WHITE, 4, 6, 5, 8)
    ic_rect(c, WHITE, 10, 6, 11, 8)


def icon_flute(c):
    ic_rect(c, (200, 160, 100, 255), 2, 7, 14, 9)
    ic_rect(c, WOODC, 2, 7, 14, 8)
    ic_rect(c, (110, 82, 48, 255), 2, 8, 14, 9)
    ic_rect(c, (60, 44, 28, 255), 4, 7, 5, 9)
    ic_rect(c, (60, 44, 28, 255), 7, 7, 8, 9)
    ic_rect(c, (60, 44, 28, 255), 10, 7, 11, 9)


def icon_gloves(c):
    ic_rect(c, BLUE, 4, 4, 8, 10)
    ic_rect(c, BLUE, 9, 4, 13, 10)
    ic_rect(c, (62, 88, 148, 255), 4, 8, 8, 10)
    ic_rect(c, (62, 88, 148, 255), 9, 8, 13, 10)
    ic_rect(c, WHITE, 4, 4, 5, 6)
    ic_rect(c, WHITE, 9, 4, 10, 6)
    ic_rect(c, STEEL, 3, 11, 8, 12)
    ic_rect(c, STEEL, 9, 11, 14, 12)


def icon_hammer(c):
    ic_rect(c, STEEL, 3, 3, 11, 7)
    ic_rect(c, STEEL_D, 3, 3, 4, 7)
    ic_rect(c, STEEL_D, 10, 3, 11, 7)
    ic_rect(c, WHITE, 4, 3, 6, 4)
    ic_rect(c, WOODC, 6, 7, 8, 14)
    ic_rect(c, (110, 76, 40, 255), 7, 7, 9, 14)


def icon_hookshot(c):
    ic_rect(c, STEEL, 3, 8, 12, 10)
    ic_rect(c, STEEL_D, 3, 8, 4, 10)
    # hook claw
    ic_rect(c, STEEL, 11, 3, 14, 8)
    ic_rect(c, STEEL_D, 13, 3, 14, 8)
    ic_rect(c, (60, 60, 72, 255), 11, 7, 12, 10)
    ic_rect(c, GOLD, 2, 7, 4, 11)


def icon_ice_rod(c):
    ic_rect(c, BLUE, 3, 11, 12, 13)
    ic_rect(c, (62, 88, 148, 255), 3, 11, 5, 13)
    ic_rect(c, CYAN, 5, 2, 9, 7)
    ic_rect(c, WHITE, 6, 3, 8, 5)
    ic_rect(c, (90, 130, 200, 255), 5, 6, 9, 8)
    ic_rect(c, GOLD, 4, 8, 6, 11)


def icon_lantern(c):
    ic_rect(c, (90, 90, 100, 255), 7, 1, 9, 3)
    ic_rect(c, STEEL_D, 5, 3, 11, 5)
    ic_rect(c, ORANGE, 5, 6, 11, 11)
    ic_rect(c, (240, 200, 120, 255), 6, 7, 10, 10)
    ic_rect(c, WHITE, 7, 8, 9, 9)
    ic_rect(c, RED, 6, 5, 10, 6)


def icon_mirror(c):
    ic_rect(c, STEEL, 6, 2, 10, 3)
    ic_rect(c, CYAN, 4, 3, 12, 11)
    ic_rect(c, WHITE, 5, 4, 8, 7)
    ic_rect(c, (90, 130, 160, 255), 4, 9, 12, 11)
    ic_rect(c, GOLD, 3, 12, 13, 14)
    ic_rect(c, (150, 120, 50, 255), 3, 13, 13, 14)


def icon_net(c):
    # hoop + mesh
    for i in range(7):
        c[3 + i][10 - i] = WOODC
    ic_rect(c, WOODC, 6, 2, 9, 4)
    ic_rect(c, WOODC, 6, 2, 7, 8)
    ic_rect(c, WOODC, 6, 7, 10, 8)
    for x in range(7, 10):
        for y in range(4, 7):
            c[y][x] = (170, 160, 120, 255) if (x + y) % 2 else (120, 110, 84, 255)


def icon_powder(c):
    ic_rect(c, (150, 120, 60, 255), 5, 5, 11, 12)
    ic_rect(c, (120, 96, 48, 255), 5, 10, 11, 12)
    ic_rect(c, (180, 150, 90, 255), 6, 6, 8, 8)
    ic_rect(c, GOLD, 7, 2, 9, 5)
    c[4][12] = WHITE
    c[3][10] = WHITE
    c[4][13] = (200, 200, 210, 255)


def icon_shovel(c):
    ic_rect(c, WOODC, 7, 2, 9, 10)
    ic_rect(c, (110, 76, 40, 255), 8, 2, 9, 10)
    ic_rect(c, STEEL, 4, 10, 12, 14)
    ic_rect(c, STEEL_D, 4, 10, 6, 14)
    ic_rect(c, STEEL_D, 10, 10, 12, 14)
    ic_rect(c, WHITE, 5, 11, 7, 12)


def icon_somaria(c):
    ic_rect(c, RED, 4, 4, 12, 12)
    ic_rect(c, (240, 130, 110, 255), 4, 4, 12, 6)
    ic_rect(c, (150, 60, 50, 255), 4, 10, 12, 12)
    ic_rect(c, GOLD, 7, 7, 9, 9)
    c[2][7] = GOLD
    c[13][7] = GOLD
    c[7][2] = GOLD
    c[7][13] = GOLD


ITEM_ICONS = {
    "bombos": icon_bombos,
    "bombs": icon_bombs,
    "boomerang": icon_boomerang,
    "bottle": icon_bottle,
    "bow": icon_bow,
    "byrna": icon_byrna,
    "cape": icon_cape,
    "compass": icon_compass,
    "ether": icon_ether,
    "fire_rod": icon_fire_rod,
    "flippers": icon_flippers,
    "flute": icon_flute,
    "gloves": icon_gloves,
    "hammer": icon_hammer,
    "hookshot": icon_hookshot,
    "ice_rod": icon_ice_rod,
    "lantern": icon_lantern,
    "mirror": icon_mirror,
    "net": icon_net,
    "powder": icon_powder,
    "quake": icon_quake,
    "shovel": icon_shovel,
    "somaria": icon_somaria,
    "sword": icon_sword,
}


def build_items(path):
    items = sorted(name for name in ITEM_ICONS)
    rows = []
    for y in range(16):
        row = bytearray()
        for index, name in enumerate(items):
            canvas = icon_canvas()
            ITEM_ICONS[name](canvas)
            for x in range(16):
                row += bytes(canvas[y][x])
        rows.append(row)
    write_png(path, 16 * len(items), 16, rows)
    return len(items)


# --------------------------------------------------------------------------
# Scenes: 320x224 storybook cards.

SCENE_W, SCENE_H = 320, 224


def scene_canvas():
    return [[(0, 0, 0, 255) for _ in range(SCENE_W)] for _ in range(SCENE_H)]


def scene_sky(c, top, bottom):
    for y in range(SCENE_H):
        t = y / float(SCENE_H)
        color = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,)
        for x in range(SCENE_W):
            c[y][x] = color


def scene_band(c, color, y0, y1, jitter=0, seed=1):
    rng = Rng(seed)
    for y in range(y0, min(y1, SCENE_H)):
        for x in range(SCENE_W):
            if jitter and rng.below(jitter):
                continue
            c[y][x] = color


def scene_hill(c, color, y_top, seed, scale=0.08):
    rng = Rng(seed)
    height_map = [0.0] * SCENE_W
    value = 0.0
    for x in range(SCENE_W):
        value += (rng.next() % 1000) / 1000.0 - 0.5
        value *= 0.94
        height_map[x] = value
    lo = min(height_map)
    hi = max(height_map)
    for x in range(SCENE_W):
        h = (height_map[x] - lo) / (hi - lo + 1e-6)
        y0 = int(y_top - h * SCENE_H * scale)
        for y in range(max(0, y0), SCENE_H):
            c[y][x] = color


def scene_tree(c, cx, base_y, scale=1.0, color=None, hi=None):
    """A storybook tree: round canopy on a short trunk."""
    color = color or (34, 58, 30, 255)
    hi = hi or (52, 82, 44, 255)
    trunk = (52, 38, 28, 255)
    r = max(4, int(10 * scale))
    # canopy: squashed disc
    for y in range(-2 * r, 1):
        for x in range(-2 * r, 2 * r + 1):
            dx, dy = x / float(2 * r), (y / float(r) + 1.0)
            d = dx * dx + dy * dy
            if d < 1.0:
                px_color = color
                if (x * 7 + y * 5) % 11 < 2:
                    px_color = hi
                yy, xx = base_y + y - r, cx + x
                if 0 <= yy < SCENE_H and 0 <= xx < SCENE_W:
                    c[yy][xx] = px_color
    # trunk
    for y in range(0, r + 3):
        for x in range(-max(1, r // 5), max(2, r // 4)):
            yy, xx = base_y + y, cx + x
            if 0 <= yy < SCENE_H and 0 <= xx < SCENE_W:
                c[yy][xx] = trunk


def scene_rect(c, color, x0, y0, x1, y1):
    for y in range(max(0, y0), min(SCENE_H, y1)):
        for x in range(max(0, x0), min(SCENE_W, x1)):
            c[y][x] = color


def scene_dither(c, color, y0, y1, seed, chance=0.35):
    rng = Rng(seed)
    for y in range(y0, y1):
        for x in range(SCENE_W):
            if rng.below(chance):
                c[y][x] = color


def scene_intro(rng):
    """The storm night card: dark clouds, rain, lightning, tiny village."""
    c = scene_canvas()
    scene_sky(c, (18, 16, 34), (52, 48, 84))
    # roiling cloud bands
    for i, y in enumerate(range(20, 110, 22)):
        shade = (34 + i * 3, 32 + i * 3, 58 + i * 4, 255)
        scene_band(c, shade, y, y + 14, jitter=0.25, seed=100 + i)
    # lightning bolt
    bolt = [(180, 176, 140, 255), (240, 240, 220, 255)]
    points = [(230, 30)]
    x, y = 230, 30
    for i in range(9):
        x += 9 if i % 2 else -  (rng.next() % 12)
        y += 12
        points.append((x, y))
    for i, (px_x, px_y) in enumerate(points):
        scene_rect(c, bolt[i % 2], px_x - 2, px_y - 2, px_x + 3, px_y + 4)
    # far ridge + near hill
    scene_hill(c, (26, 30, 40, 255), 150, 7, 0.10)
    scene_hill(c, (16, 20, 28, 255), 185, 11, 0.14)
    # village: tiny lit windows at the foot
    scene_rect(c, (30, 26, 24, 255), 60, 178, 240, 200)
    for hx, hy in ((80, 182), (120, 184), (170, 180), (200, 186)):
        scene_rect(c, (30, 26, 24, 255), hx - 12, hy - 10, hx + 12, hy)
        scene_rect(c, (30, 26, 24, 255), hx - 2, hy, hx + 2, hy + 4)
        scene_rect(c, (214, 178, 92, 255), hx - 6, hy - 6, hx - 2, hy - 2)
    # rain streaks
    for _ in range(160):
        x = rng.next() % SCENE_W
        y = rng.next() % (SCENE_H - 20)
        scene_rect(c, (110, 116, 148, 160), x, y, x + 1, y + 5)
    return c


def scene_legend(rng):
    """The legend card: the valley whole, shrine at the heart."""
    c = scene_canvas()
    scene_sky(c, (94, 130, 176), (176, 196, 210))
    # sun
    for y in range(40):
        for x in range(40):
            dx, dy = x - 20, y - 20
            if dx * dx + dy * dy < 300:
                c[24 + y][250 + x] = (248, 232, 168, 255)
    scene_hill(c, (96, 124, 92, 255), 140, 21, 0.10)
    # shrine at the heart
    scene_rect(c, (122, 96, 70, 255), 140, 130, 184, 172)
    scene_rect(c, (94, 72, 54, 255), 140, 130, 184, 136)
    scene_rect(c, (150, 122, 88, 255), 150, 140, 174, 148)
    scene_rect(c, (94, 72, 54, 255), 148, 160, 176, 172)
    # stone ring around it
    for sx in (128, 136, 144, 152, 160, 168, 176, 184, 192):
        scene_rect(c, (110, 104, 96, 255), sx, 176, sx + 4, 182)
    # grove trees around the shrine
    for tx, ty in ((96, 120), (112, 128), (204, 118), (222, 132), (88, 150)):
        scene_tree(c, tx, ty + 8, 1.2)
    # near meadow
    scene_hill(c, (76, 118, 60, 255), 190, 33, 0.06)
    scene_rect(c, (58, 96, 48, 255), 0, 196, SCENE_W, SCENE_H)
    scene_dither(c, (88, 132, 68, 255), 196, SCENE_H, 9, 0.4)
    return c


def scene_ending(rng):
    """The storm breaks: clouds parting, light on the hills, rainbow arc."""
    import math
    c = scene_canvas()
    scene_sky(c, (120, 140, 168), (208, 214, 208))
    # parting clouds
    for i, y in enumerate(range(16, 104, 26)):
        shade = (198 + i * 8, 202 + i * 8, 210 + i * 6, 255)
        scene_band(c, shade, y + i * 4, y + 14 + i * 4, jitter=0.3, seed=200 + i)
    # rainbow arc over the hills
    hues = ((196, 108, 96), (216, 176, 92), (110, 176, 96), (96, 150, 196),
            (120, 100, 190), (160, 96, 150))
    for band, color in enumerate(hues):
        radius = 86 + band * 5
        for step in range(140):
            t = step / 139.0
            angle = math.pi * (0.08 + 0.84 * t)
            x = int(168 + radius * math.cos(angle))
            y = int(198 - radius * math.sin(angle) * 0.62)
            if 0 <= x < SCENE_W and 0 <= y < SCENE_H:
                c[y][x] = color
                if x + 1 < SCENE_W:
                    c[y][x + 1] = color
    scene_hill(c, (92, 122, 88, 255), 168, 41, 0.09)
    scene_hill(c, (66, 106, 56, 255), 196, 43, 0.05)
    # village lights below, warm now
    for hx, hy in ((100, 196), (150, 200), (210, 194)):
        scene_rect(c, (60, 52, 44, 255), hx - 12, hy - 10, hx + 12, hy)
        scene_rect(c, (250, 226, 140, 255), hx - 5, hy - 6, hx - 1, hy - 2)
    scene_dither(c, (82, 128, 62, 255), 200, SCENE_H, 55, 0.4)
    return c


def scene_logo(rng):
    """The company card: a quiet emblem on deep navy (the studio line is
    engine-drawn text at the bottom - GAME_LOGO_TEXT)."""
    import math
    c = scene_canvas()
    scene_sky(c, (16, 14, 26), (30, 26, 44))
    cx, cy = 160, 104
    # diamond ring
    for t in range(360):
        a = math.radians(t)
        x = int(cx + 66 * math.cos(a))
        y = int(cy - 52 * math.sin(a))
        if 0 <= x < SCENE_W and 0 <= y < SCENE_H:
            c[y][x] = (92, 88, 132, 255)
            if t % 5 == 0 and 0 <= y - 1:
                c[y - 1][x] = (58, 54, 88, 255)
    # the enoki: a cluster of tall stems under a small cap
    for stem in range(-5, 6):
        sx = cx + stem * 5
        for y in range(cy - 10, cy + 34):
            if 0 <= sx < SCENE_W and 0 <= y < SCENE_H:
                c[y][sx] = (222, 214, 196, 255)
    for y in range(cy - 22, cy - 8):
        for x in range(cx - 34, cx + 35):
            dx = (x - cx) / 34.0
            dy = (y - (cy - 8)) / 14.0
            if dx * dx + dy * dy < 1.0:
                c[y][x] = (236, 230, 214, 255)
                if rng.below(0.2):
                    c[y][x] = (206, 198, 180, 255)
    for y in range(cy - 26, cy - 20):
        for x in range(cx - 28, cx + 29):
            dx = (x - cx) / 28.0
            dy = (y - (cy - 20)) / 6.0
            if dx * dx + dy * dy < 1.0:
                c[y][x] = (250, 246, 232, 255)
    # a soft glow behind the emblem
    for y in range(cy - 60, cy + 60):
        for x in range(cx - 90, cx + 90):
            d = ((x - cx) / 90.0) ** 2 + ((y - cy) / 60.0) ** 2
            if d < 1.0:
                lift = int(18 * (1.0 - d))
                base = c[y][x]
                c[y][x] = (min(255, base[0] + lift), min(255, base[1] + lift // 2),
                           min(255, base[2] + lift), base[3])
    scene_dither(c, (12, 10, 20, 255), 180, SCENE_H, 77, 0.12)
    return c


def scene_title_card(rng):
    """The title backdrop: the storm over Willowdown. The engine draws
    GAME_TITLE near the top and the save slots over the middle, so the
    art keeps its contrast low there and saves its detail for the sky
    and the valley floor."""
    c = scene_canvas()
    scene_sky(c, (14, 16, 30), (66, 74, 96))
    # heavy cloud banks
    for i, y in enumerate(range(14, 92, 20)):
        shade = (26 + i * 4, 30 + i * 4, 48 + i * 6, 255)
        scene_band(c, shade, y, y + 13, jitter=0.3, seed=300 + i)
    # the heartstone's glow on the horizon: the one warm light
    for y in range(96, 128):
        for x in range(120, 200):
            d = ((x - 160) / 40.0) ** 2 + ((y - 112) / 18.0) ** 2
            if d < 1.0:
                warm = int(46 * (1.0 - d))
                c[y][x] = (36 + warm, 34 + warm * 2 // 3, 44 + warm // 2, 255)
    c[112][160] = (232, 216, 180, 255)
    c[111][159] = (200, 184, 150, 255)
    c[111][161] = (200, 184, 150, 255)
    c[113][160] = (200, 184, 150, 255)
    # the valley silhouette
    scene_hill(c, (30, 36, 40, 255), 148, 61, 0.08)
    scene_hill(c, (20, 24, 26, 255), 176, 63, 0.05)
    # village lights at the foot of the hills
    for hx, hy in ((100, 186), (148, 190), (206, 184)):
        scene_rect(c, (26, 22, 24, 255), hx - 10, hy - 8, hx + 10, hy)
        scene_rect(c, (216, 186, 110, 255), hx - 4, hy - 5, hx - 1, hy - 2)
    scene_dither(c, (16, 20, 20, 255), 196, SCENE_H, 99, 0.35)
    # rain over everything
    for _ in range(220):
        x = rng.next() % SCENE_W
        y = rng.next() % (SCENE_H - 16)
        scene_rect(c, (110, 120, 150, 90), x, y, x + 1, y + 4)
    return c


# Scenes are built in main() (deterministic, always rewritten).


# --------------------------------------------------------------------------
# Character + enemy recolors (sheets shipped with the game stay as-is).

def build_copies():
    """The hero keeps her sprite (link.png) and the villager keeps his
    (villager.png): copy them under the game's character names."""
    made = []
    for source, target in (("link", "enoki"), ("villager", "aaron")):
        src = os.path.join(RES, "characters", source + ".png")
        out = os.path.join(RES, "characters", target + ".png")
        if os.path.exists(out) or not os.path.exists(src):
            continue
        width, height, _, _, rows = read_png(src)
        write_png(out, width, height, rows)
        made.append(out)
    return made


CHARACTER_RECOLORS = {
    # name -> (hue degrees) — recolored off the shipped villager sheet
    # (only saturated robe/hair colors shift; skin + outline stay).
    "elder": 160,    # Rowan: red robe -> indigo robe
    "mara": 130,     # the merchant: -> teal robe
    "fen": 55,       # the kid: -> ochre robe
    "keeper": 115,   # the grove keeper: -> deep green robe
}

ENEMY_RECOLORS = {
    "hollow_soldier": ("soldier", 80),
}


def build_recolors():
    made = []
    aaron = os.path.join(RES, "characters", "aaron.png")
    for name, degrees in CHARACTER_RECOLORS.items():
        out = os.path.join(RES, "characters", name + ".png")
        if os.path.exists(out) or not os.path.exists(aaron):
            continue
        def mask(r, g, b):
            _, l, s = colorsys.rgb_to_hls(r / 255.0, g / 255.0, b / 255.0)
            return s > 0.25 and l > 0.12
        if shift_image(out, aaron, degrees, sat_scale=0.9, lum_scale=0.92, mask=mask):
            made.append(out)
    for name, (source, degrees) in ENEMY_RECOLORS.items():
        out = os.path.join(RES, "enemies", name + ".png")
        src = os.path.join(RES, "enemies", source + ".png")
        if os.path.exists(out) or not os.path.exists(src):
            continue
        def emask(r, g, b):
            _, l, s = colorsys.rgb_to_hls(r / 255.0, g / 255.0, b / 255.0)
            return s > 0.2
        if shift_image(out, src, degrees, sat_scale=1.15, lum_scale=0.75, mask=emask):
            made.append(out)
    return made


# --------------------------------------------------------------------------
# Doorway sprites (resources/ui/doors.png): the edge-door chrome. Two
# rows (horizontal doors for N/S walls, vertical for E/W) x six columns
# matching the generator's door-kind order (open, closed, locked, big,
# bombable, shutter); the open archway serves any kind that stands open.
# Cells are 40x48: horizontal doors occupy the middle band, vertical
# doors the middle columns, so both center on the wall midpoint.

DOOR_WOOD = (150, 104, 52, 255)
DOOR_WOOD_HI = (188, 138, 84, 255)
DOOR_WOOD_LO = (100, 66, 30, 255)
DOOR_TRIM = (60, 40, 22, 255)
DOOR_STONE = (122, 116, 108, 255)
DOOR_STONE_HI = (156, 150, 140, 255)
DOOR_STONE_LO = (88, 84, 78, 255)
DOOR_DARK = (12, 10, 20, 255)
DOOR_STEEL = (120, 124, 138, 255)
DOOR_STEEL_HI = (168, 172, 186, 255)
DOOR_STEEL_LO = (70, 72, 84, 255)
DOOR_GOLD = (216, 178, 74, 255)
DOOR_CRACK = (30, 18, 10, 255)


def _door_cell():
    return [[(0, 0, 0, 0) for _ in range(40)] for _ in range(48)]


def _door_slab(c, vertical, keyhole=None, cracks=False, slats=False):
    """A wooden slab filling the doorway; keyhole "small"/"big", crack
    lines, or steel shutter slats on top."""
    if vertical:
        for y in range(48):
            for x in range(8, 32):
                c[y][x] = DOOR_WOOD
            c[y][8] = DOOR_WOOD_LO
            c[y][9] = DOOR_WOOD_HI
            c[y][31] = DOOR_WOOD_LO
            c[y][30] = DOOR_WOOD_HI
        for x in range(8, 32):
            c[0][x] = DOOR_TRIM
            c[47][x] = DOOR_TRIM
        # plank seams
        for y in range(6, 48, 10):
            for x in range(10, 30):
                c[y][x] = DOOR_WOOD_LO
    else:
        for y in range(12, 36):
            for x in range(40):
                c[y][x] = DOOR_WOOD
            c[y][12] = DOOR_WOOD_LO
            c[y][13] = DOOR_WOOD_HI
            c[y][35] = DOOR_WOOD_LO
            c[y][34] = DOOR_WOOD_HI
        for x in range(40):
            c[12][x] = DOOR_TRIM
            c[35][x] = DOOR_TRIM
        for y in range(14, 35, 8):
            for x in range(2, 38):
                c[y][x] = DOOR_WOOD_LO
    # handle
    hx = 26 if vertical else 30
    hy = 24 if vertical else 26
    for y in range(hy - 2, hy + 2):
        for x in range(hx - 2, hx + 2):
            if 0 <= y < 48 and 0 <= x < 40:
                c[y][x] = DOOR_GOLD

    if keyhole:
        kx = 20 if vertical else 20
        ky = 22 if vertical else 24
        r = 3 if keyhole == "small" else 5
        for y in range(ky - r, ky + r + 1):
            for x in range(kx - r, kx + r + 1):
                dx, dy = x - kx, y - ky
                if dx * dx + dy * dy <= r * r and 0 <= y < 48 and 0 <= x < 40:
                    c[y][x] = DOOR_DARK if keyhole == "small" else DOOR_TRIM
        slit = 5 if keyhole == "small" else 8
        for i in range(slit):
            yy, xx = (ky + r + i, kx) if vertical else (ky + r + i, kx)
            if yy < 48 and xx < 40:
                c[yy][xx] = DOOR_DARK if keyhole == "small" else DOOR_TRIM
    if cracks:
        # jagged blast cracks across the slab
        cx, cy = (20, 24) if vertical else (20, 24)
        for t in range(-12, 13):
            x = cx + t
            y = cy + (t * t) // 14
            if 0 <= y < 48 and 0 <= x < 40 and c[y][x] != (0, 0, 0, 0):
                c[y][x] = DOOR_CRACK if t % 3 else (8, 5, 12, 255)
        for t in range(-8, 9):
            x = cx + t
            y = cy - t
            if 0 <= y < 48 and 0 <= x < 40 and c[y][x] != (0, 0, 0, 0):
                c[y][x] = DOOR_CRACK
    if slats:
        if vertical:
            for y0 in range(8, 48, 6):
                for y in range(y0, min(y0 + 4, 48)):
                    for x in range(8, 32):
                        c[y][x] = DOOR_STEEL
                    c[y][8] = DOOR_STEEL_HI
                    c[y][31] = DOOR_STEEL_LO
                for x in range(8, 32, 6):
                    if y0 + 1 < 48:
                        c[y0 + 1][x] = DOOR_STEEL_HI
        else:
            for y0 in range(12, 36, 6):
                for y in range(y0, min(y0 + 4, 36)):
                    for x in range(40):
                        c[y][x] = DOOR_STEEL
                    c[y][12] = DOOR_STEEL_HI
                    c[y][35] = DOOR_STEEL_LO
                for x in range(2, 38, 6):
                    c[y0 + 1][x] = DOOR_STEEL_HI


def _door_arch(c, vertical):
    """An open doorway: dark passage with a stone frame."""
    if vertical:
        for y in range(48):
            for x in range(5, 9):
                c[y][x] = DOOR_STONE
            for x in range(9, 31):
                c[y][x] = DOOR_DARK
            for x in range(31, 35):
                c[y][x] = DOOR_STONE
            c[y][5] = DOOR_STONE_HI
            c[y][34] = DOOR_STONE_LO if 34 < 40 else c[y][34]
        for x in range(5, 35):
            c[0][x] = DOOR_STONE_HI
            c[47][x] = DOOR_STONE_LO
    else:
        for y in range(13, 36):
            for x in range(40):
                c[y][x] = DOOR_DARK
        for x in range(40):
            c[10][x] = DOOR_STONE_HI
            c[11][x] = DOOR_STONE
            c[12][x] = DOOR_STONE_LO
            c[36][x] = DOOR_STONE
            c[37][x] = DOOR_STONE_LO
        for y in range(10, 38):
            c[y][0] = DOOR_STONE_HI
            c[y][1] = DOOR_STONE
            c[y][39] = DOOR_STONE


def build_doors(path):
    """Six door kinds (generator order: open, closed, locked, big,
    bombable, shutter) x two orientations, 40x48 cells -> 240x96."""
    cells = []
    for vertical in (False, True):
        # open
        c = _door_cell()
        _door_arch(c, vertical)
        cells.append(c)
        # closed
        c = _door_cell()
        _door_slab(c, vertical)
        cells.append(c)
        # locked
        c = _door_cell()
        _door_slab(c, vertical, keyhole="small")
        cells.append(c)
        # big
        c = _door_cell()
        _door_slab(c, vertical, keyhole="big")
        cells.append(c)
        # bombable
        c = _door_cell()
        _door_slab(c, vertical, cracks=True)
        cells.append(c)
        # shutter
        c = _door_cell()
        _door_slab(c, vertical, slats=True)
        cells.append(c)
    rows = []
    for band in range(2):
        for y in range(48):
            row = bytearray()
            for index in range(band * 6, band * 6 + 6):
                for x in range(40):
                    row += bytes(cells[index][y][x])
            rows.append(row)
    write_png(path, 40 * 6, 96, rows)
    return len(cells)


# --------------------------------------------------------------------------
# Hero tool sprites (resources/effects/tools.png): the hammer, shovel,
# and bug net the hero swings (drawn by rotation around the handle base,
# so one frame per tool serves every facing), plus the carried shield.
# Frames are 32x32, pointing up, pivot at the handle base (16, 28); the
# shield is centered in its cell.

TOOL_WOOD = (172, 128, 76, 255)
TOOL_WOOD_D = (128, 88, 52, 255)
TOOL_STEEL = (186, 192, 206, 255)
TOOL_STEEL_D = (110, 116, 134, 255)
TOOL_STEEL_HI = (222, 226, 234, 255)
TOOL_MESH = (206, 192, 150, 255)


def _tool_cell():
    return [[(0, 0, 0, 0) for _ in range(32)] for _ in range(32)]


def _tool_handle(canvas, y0, y1):
    for y in range(y0, y1):
        for x in range(14, 18):
            canvas[y][x] = TOOL_WOOD
        canvas[y][17] = TOOL_WOOD_D
    # grip wrap
    for y in range(y1 - 4, y1):
        canvas[y][13] = TOOL_WOOD_D
        canvas[y][18] = TOOL_WOOD_D


def tool_hammer():
    c = _tool_cell()
    _tool_handle(c, 12, 29)
    # steel head across the top
    for y in range(4, 12):
        for x in range(6, 26):
            c[y][x] = TOOL_STEEL
        c[y][6] = TOOL_STEEL_D
        c[y][25] = TOOL_STEEL_D
    for x in range(6, 26):
        c[4][x] = TOOL_STEEL_HI
    for y in range(5, 11):
        c[y][9] = TOOL_STEEL_HI
    for x in range(6, 26):
        c[11][x] = TOOL_STEEL_D
    return c


def tool_shovel():
    c = _tool_cell()
    _tool_handle(c, 14, 29)
    # blade: a rounded wedge pointing up
    for y in range(3, 14):
        half = 8 if y < 5 else 6
        for x in range(16 - half, 16 + half):
            c[y][x] = TOOL_STEEL
        c[y][16 - half] = TOOL_STEEL_D
        c[y][16 + half - 1] = TOOL_STEEL_D
    for x in range(12, 20):
        c[3][x] = TOOL_STEEL_HI
        c[4][x] = TOOL_STEEL_HI
    for y in range(3, 14):
        c[y][13] = TOOL_STEEL_HI if y < 8 else c[y][13]
    for x in range(10, 22):
        c[13][x] = TOOL_STEEL_D
    return c


def tool_net():
    c = _tool_cell()
    _tool_handle(c, 20, 29)
    # hoop ring
    cx, cy, r = 16, 11, 8
    for y in range(32):
        for x in range(32):
            dx, dy = x - cx, y - cy
            d = dx * dx + dy * dy
            if r * r - 14 <= d <= r * r + 6:
                c[y][x] = TOOL_WOOD
            if r * r + 6 < d <= r * r + 20:
                c[y][x] = TOOL_WOOD_D
    # mesh crosshatch inside the hoop
    for i in range(-4, 5):
        if 0 <= cy + i < 32:
            for x in range(cx - 5, cx + 5):
                if (x + i) % 3 == 0:
                    c[cy + i][x] = TOOL_MESH if 0 <= x < 32 else c[cy + i][x]
        if 0 <= cx + i < 32:
            for y in range(cy - 5, cy + 5):
                if (y + i) % 3 == 0 and 0 <= y < 32:
                    c[y][cx + i] = TOOL_MESH
    return c


def tool_shield():
    c = _tool_cell()
    cx, cy = 16, 16
    for y in range(32):
        for x in range(32):
            dx, dy = (x - cx) / 10.0, (y - cy) / 12.0
            d = dx * dx + dy * dy
            if d < 1.0:
                c[y][x] = TOOL_WOOD
            elif d < 1.25:
                c[y][x] = TOOL_STEEL
            elif d < 1.45:
                c[y][x] = TOOL_STEEL_D
    # steel boss + a highlight crescent
    for y in range(32):
        for x in range(32):
            dx, dy = x - cx, y - cy + 3
            if dx * dx + dy * dy <= 9:
                c[y][x] = TOOL_STEEL
    for x in range(11, 16):
        for y in range(7, 18):
            dx, dy = (x - cx) / 9.0, (y - cy) / 11.0
            if 0.55 < dx * dx + dy * dy < 0.95:
                c[y][x] = TOOL_STEEL_HI
    return c


def build_tools(path):
    frames = [tool_hammer(), tool_shovel(), tool_net(), tool_shield()]
    rows = []
    for y in range(32):
        row = bytearray()
        for frame in frames:
            for x in range(32):
                row += bytes(frame[y][x])
        rows.append(row)
    write_png(path, 32 * len(frames), 32, rows)
    return len(frames)


# --------------------------------------------------------------------------
# House door (resources/objects/door_warp.png): a front-door sprite for
# the "door_warp" object type — a step-through like the warp pad, but it
# reads as a door in a house wall instead of a swirl.

def build_house_door(path):
    c = [[(0, 0, 0, 0) for _ in range(32)] for _ in range(32)]
    # stone frame around the tile
    frame = (122, 116, 108, 255)
    frame_hi = (156, 150, 140, 255)
    frame_lo = (88, 84, 78, 255)
    for y in range(32):
        for x in range(32):
            edge = x < 3 or y < 3 or x > 28 or y > 28
            if edge:
                c[y][x] = frame
    for x in range(32):
        c[0][x] = frame_hi
        c[3][x] = frame_lo
    for y in range(32):
        c[y][0] = frame_hi
        c[y][3] = frame_lo
    # the door itself
    for y in range(6, 29):
        for x in range(6, 26):
            c[y][x] = (150, 104, 52, 255)
        c[y][6] = (188, 138, 84, 255)
        c[y][25] = (100, 66, 30, 255)
    for y in range(9, 29, 7):
        for x in range(7, 25):
            c[y][x] = (128, 86, 42, 255)
    # lintel + handle
    for x in range(5, 27):
        c[5][x] = (60, 40, 22, 255)
    for y in range(16, 20):
        for x in range(21, 25):
            c[y][x] = (216, 178, 74, 255)
    rows = []
    for y in range(32):
        row = bytearray()
        for x in range(32):
            row += bytes(c[y][x])
        rows.append(row)
    write_png(path, 32, 32, rows)


# --------------------------------------------------------------------------
# Zora ripple frames: the sheet's stand row (the AI's submerged lurk)
# becomes water rings; the move row (the surfaced body) keeps the
# shipped sprite. The behavior's channel design — idle = ripple,
# surfaced = body — finally has art that reads underwater.

def build_zora(path):
    width, height, _, _, rows = read_png(path)
    ripple_rows = []
    cy = 19
    for y in range(32):
        row = bytearray()
        for x in range(width):
            color = (0, 0, 0, 0)
            for frame in range(4):
                cx = frame * 32 + 16
                dx = (x - cx) / 11.0
                dy = (y - cy) / 5.0
                d = dx * dx + dy * dy
                # concentric wake rings, out of phase per frame
                for ring, phase in ((1.0, 0.0), (0.62, 0.33), (0.3, 0.66)):
                    offset = ((frame * 2 + int(phase * 3)) % 3)
                    if abs(d - ring) < 0.07 and (x + y + frame) % 3 != offset:
                        color = (150, 196, 226, 255) if ring < 0.5 else (96, 150, 196, 255)
                if d < 0.08:
                    color = (176, 214, 238, 255)
            row += bytes(color)
        ripple_rows.append(row)
    # stand row = ripples, move row = the shipped surfaced body
    rows = ripple_rows + rows[32:]
    write_png(path, width, height, rows)


def main():
    tileset = os.path.join(RES, "tiles", "texture.png")
    tiles_json = os.path.join(RES, "tiles", "tiles.json")
    count = build_tileset(tileset, tiles_json)
    print("tileset: %d tiles -> %s" % (count, tileset))
    items_png = os.path.join(RES, "ui", "items.png")
    icons = build_items(items_png)
    print("items: %d icons -> %s" % (icons, items_png))
    for path in build_copies():
        print("copy: %s" % path)
    for path in build_recolors():
        print("recolor: %s" % path)
    tools_png = os.path.join(RES, "effects", "tools.png")
    tool_count = build_tools(tools_png)
    print("hero tools: %d frames -> %s" % (tool_count, tools_png))
    doors_png = os.path.join(RES, "ui", "doors.png")
    door_count = build_doors(doors_png)
    print("doorways: %d frames -> %s" % (door_count, doors_png))
    house_door_png = os.path.join(RES, "objects", "door_warp.png")
    build_house_door(house_door_png)
    print("house door: %s" % house_door_png)
    zora_png = os.path.join(RES, "enemies", "zora.png")
    build_zora(zora_png)
    print("zora ripples: %s" % zora_png)
    for name, paint in (("intro_card", scene_intro),
                        ("legend", scene_legend),
                        ("ending", scene_ending),
                        ("company_logo", scene_logo),
                        ("title_card", scene_title_card)):
        path = os.path.join(RES, "scenes", name + ".png")
        rng = Rng(0x5EED)
        canvas = paint(rng)
        rows = []
        for y in range(SCENE_H):
            row = bytearray()
            for x in range(SCENE_W):
                row += bytes(canvas[y][x])
            rows.append(row)
        write_png(path, SCENE_W, SCENE_H, rows)
        print("scene: %s" % path)


if __name__ == "__main__":
    main()