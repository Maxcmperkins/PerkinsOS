#!/usr/bin/env python3
"""Draw the PerkinsOS mouse cursors, in the Xcursor format.

Usage: make-cursors.py OUTPUT_DIR

Each cursor is described below as outline shapes on a 24x24 grid and drawn
at several sizes, so there is no binary artwork in the repo and the colours
can be changed here. Only Python's standard library is used.

An Xcursor file is a header, a table of contents, then one picture per
size; `man 3 Xcursor` describes it.
"""
import os
import struct
import sys

GREEN = (0x33, 0xFF, 0x66)      # HUD_GREEN in shell/common/theme.h
DARK = (0x01, 0x0A, 0x04)       # the edge round it, for light backgrounds
SIZES = (24, 32, 48)
SAMPLES = 4                     # per pixel, each way: smooth edges

ARROW = [(3, 2), (3, 19), (7.5, 15), (10.5, 21.5), (13.2, 20.2), (10.3, 14),
         (16, 14)]
HAND = [(8, 21), (4, 14), (5.5, 12.5), (8, 14.5), (8, 4), (9.2, 3), (10.5, 4),
        (10.5, 10), (17, 11.5), (18.5, 13), (17.5, 21)]


def box(x1, y1, x2, y2):
    return [(x1, y1), (x2, y1), (x2, y2), (x1, y2)]


def flipped(shapes):
    """The same shapes with x and y swapped: across becomes down."""
    return [[(y, x) for x, y in s] for s in shapes]


def tilted(shapes, way):
    """The same shapes turned 45 degrees about the middle."""
    k = 0.5 ** 0.5
    return [[(12 + k * ((x - 12) - way * (y - 12)),
              12 + k * (way * (x - 12) + (y - 12))) for x, y in s]
            for s in shapes]


ACROSS = [box(6, 11, 18, 13), [(2, 12), (7, 8), (7, 16)],
          [(22, 12), (17, 8), (17, 16)]]
DOWN = flipped(ACROSS)
CROSS = [box(11, 2, 13, 9), box(11, 15, 13, 22), box(2, 11, 9, 13),
         box(15, 11, 22, 13), box(11, 11, 13, 13)]
TEXT = [box(11, 4, 13, 20), box(8, 3, 16, 5), box(8, 19, 16, 21)]
# Four corner brackets, like the box the radar puts round a locked target.
WAIT = [box(4, 4, 10, 6), box(4, 4, 6, 10), box(14, 4, 20, 6),
        box(18, 4, 20, 10), box(4, 18, 10, 20), box(4, 14, 6, 20),
        box(14, 18, 20, 20), box(18, 14, 20, 20), box(10.5, 10.5, 13.5, 13.5)]
NO = tilted([box(3, 11, 21, 13), box(11, 3, 13, 21)], 1)

# name: (shapes, hotspot x, hotspot y, other names for the same cursor).
# Apps ask for cursors by CSS name ("pointer") or older X11 name ("hand2").
CURSORS = {
    "default": ([ARROW], 3, 2, ["left_ptr", "arrow", "top_left_arrow",
                                "context-menu", "help", "alias", "copy"]),
    "pointer": ([HAND], 9, 3, ["hand1", "hand2", "pointing_hand"]),
    "text": (TEXT, 12, 12, ["xterm", "ibeam", "vertical-text"]),
    "crosshair": (CROSS, 12, 12, ["cross", "tcross", "cell"]),
    "wait": (WAIT, 12, 12, ["watch", "progress", "left_ptr_watch"]),
    "not-allowed": (NO, 12, 12, ["crossed_circle", "no-drop", "forbidden"]),
    "move": (ACROSS + DOWN, 12, 12, ["fleur", "all-scroll", "grab",
                                     "grabbing", "openhand", "closedhand"]),
    "ew-resize": (ACROSS, 12, 12, [
        "sb_h_double_arrow", "col-resize", "e-resize", "w-resize",
        "left_side", "right_side", "h_double_arrow"]),
    "ns-resize": (DOWN, 12, 12, [
        "sb_v_double_arrow", "row-resize", "n-resize", "s-resize",
        "top_side", "bottom_side", "v_double_arrow"]),
    "nwse-resize": (tilted(ACROSS, 1), 12, 12, [
        "nw-resize", "se-resize", "top_left_corner", "bottom_right_corner"]),
    "nesw-resize": (tilted(ACROSS, -1), 12, 12, [
        "ne-resize", "sw-resize", "top_right_corner", "bottom_left_corner"]),
}


def inside(shape, x, y):
    """Is the point in the shape? Count edges crossed going right: odd is in."""
    result = False
    for (x1, y1), (x2, y2) in zip(shape, shape[1:] + shape[:1]):
        if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
            result = not result
    return result


def picture(shapes, size):
    """The pixels, each packed as Xcursor wants: ARGB, with the colour
    already multiplied by how opaque it is."""
    n = size * SAMPLES
    scale = 24 / n
    solid = [[any(inside(s, (x + 0.5) * scale, (y + 0.5) * scale)
                  for s in shapes) for x in range(n)] for y in range(n)]
    # The dark edge: anywhere within `edge` samples of the shape.
    edge = max(1, round(SAMPLES * size / 24 * 1.1))
    reach = [(dx, dy) for dx in range(-edge, edge + 1)
             for dy in range(-edge, edge + 1) if dx * dx + dy * dy <= edge * edge]
    near = [[False] * n for _ in range(n)]
    for y in range(n):
        for x in range(n):
            if solid[y][x]:
                for dx, dy in reach:
                    if 0 <= x + dx < n and 0 <= y + dy < n:
                        near[y + dy][x + dx] = True
    out = bytearray()
    total = SAMPLES * SAMPLES
    for py in range(size):
        for px in range(size):
            fill = dark = 0
            for y in range(py * SAMPLES, (py + 1) * SAMPLES):
                for x in range(px * SAMPLES, (px + 1) * SAMPLES):
                    if solid[y][x]:
                        fill += 1
                    elif near[y][x]:
                        dark += 1
            alpha = 255 * (fill + dark) // total
            r, g, b = ((GREEN[i] * fill + DARK[i] * dark) // total
                       for i in range(3))
            out += struct.pack("<I", alpha << 24 | r << 16 | g << 8 | b)
    return bytes(out)


def xcursor(shapes, hot_x, hot_y):
    image_type = 0xFFFD0002
    first = 16 + 12 * len(SIZES)
    contents, images = b"", b""
    for size in SIZES:
        contents += struct.pack("<III", image_type, size, first + len(images))
        images += struct.pack(
            "<IIIIIIIII", 36, image_type, size, 1, size, size,
            round(hot_x * size / 24), round(hot_y * size / 24), 0)
        images += picture(shapes, size)
    return b"Xcur" + struct.pack("<III", 16, 0x10000, len(SIZES)) + contents + images


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for name, (shapes, hot_x, hot_y, others) in CURSORS.items():
        with open(os.path.join(out, name), "wb") as f:
            f.write(xcursor(shapes, hot_x, hot_y))
        for other in others:
            link = os.path.join(out, other)
            if os.path.lexists(link):
                os.remove(link)
            os.symlink(name, link)


if __name__ == "__main__":
    main()
