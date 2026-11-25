#!/usr/bin/env python3
"""Draw the boot splash's pictures as PNGs.

Usage: make-images.py OUTPUT_DIR

Plymouth shows the splash before the disk, fonts and graphics libraries are
ready, so it can only use ready-made pictures. Only the standard library.
"""
import math
import os
import struct
import sys
import zlib

GREEN = (0x33, 0xFF, 0x66)
RADAR = 300                     # pixels across

# The letters of PERKINSOS, 5 pixels wide and 7 tall.
LETTERS = {
    "P": "####. #...# #...# ####. #.... #.... #....",
    "E": "##### #.... #.... ####. #.... #.... #####",
    "R": "####. #...# #...# ####. #.#.. #..#. #...#",
    "K": "#...# #..#. #.#.. ##... #.#.. #..#. #...#",
    "I": "##### ..#.. ..#.. ..#.. ..#.. ..#.. #####",
    "N": "#...# ##..# ##..# #.#.# #..## #..## #...#",
    "S": ".#### #.... #.... .###. ....# ....# ####.",
    "O": ".###. #...# #...# #...# #...# #...# .###.",
}


def write_png(path, width, height, alpha):
    """Green everywhere; alpha(x, y), 0..1, says how strongly."""
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            a = max(0.0, min(1.0, alpha(x, y)))
            rows += bytes(GREEN) + bytes([round(255 * a)])

    def chunk(kind, body):
        crc = zlib.crc32(kind + body)
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", crc)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(rows), 9)))
        f.write(chunk(b"IEND", b""))


def radar(x, y):
    dx, dy = x + 0.5 - RADAR / 2, y + 0.5 - RADAR / 2
    r = math.hypot(dx, dy)
    outer = RADAR / 2 - 3
    strength = max(0.0, 1.6 - abs(r - outer))
    for ring in (outer / 3, outer * 2 / 3):
        strength = max(strength, 0.3 * max(0.0, 1.5 - abs(r - ring)))
    if r < outer and min(abs(dx), abs(dy)) < 1:
        strength = max(strength, 0.25)
    return strength


def sweep(x, y):
    """The beam, pointing up, with its glow fading out behind it."""
    dx, dy = x + 0.5 - RADAR / 2, y + 0.5 - RADAR / 2
    r = math.hypot(dx, dy)
    if r > RADAR / 2 - 5:
        return 0
    behind = math.degrees(math.atan2(-dx, -dy)) % 360
    strength = 0.35 * (1 - behind / 60) if behind < 60 else 0
    if dy < 0 and abs(dx) < 1.2:
        strength = 1
    return strength


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    write_png(os.path.join(out, "radar.png"), RADAR, RADAR, radar)
    write_png(os.path.join(out, "sweep.png"), RADAR, RADAR, sweep)
    write_png(os.path.join(out, "dot.png"), 12, 12, lambda x, y: 1)

    word, size = "PERKINSOS", 6
    rows = {c: LETTERS[c].split() for c in LETTERS}

    def title(x, y):
        column, row = x // size, y // size
        letter, part = divmod(column, 6)
        return 1 if part < 5 and rows[word[letter]][row][part] == "#" else 0
    write_png(os.path.join(out, "title.png"), (6 * len(word) - 1) * size, 7 * size, title)


if __name__ == "__main__":
    main()
