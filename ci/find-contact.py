#!/usr/bin/env python3
"""Find a radar contact on the wallpaper, for the desktop test to click.

Usage: find-contact.py SHOT1.png SHOT2.png   -> prints "X Y"

The two screenshots are taken a moment apart. A contact is a bright green
dot that is in the same place in both; the sweep line and its afterglow
have moved in between, so they are ignored. Looks inside the radar scope
only (the left screen of the cockpit panel on a 1024x768 display; the
numbers below match layout_for() in shell/wallpaper/scene.c), away from
its cross lines.
"""
import sys

sys.path.insert(0, sys.path[0])
from gif import read_png

RADAR_X, RADAR_Y, RADIUS = 136, 612, 78


def main():
    w, h, first = read_png(sys.argv[1])
    _, _, second = read_png(sys.argv[2])

    def green(rgb, x, y):
        return rgb[(y * w + x) * 3 + 1]

    best = None
    for y in range(RADAR_Y - RADIUS, RADAR_Y + RADIUS):
        for x in range(RADAR_X - RADIUS, RADAR_X + RADIUS):
            distance2 = (x - RADAR_X) ** 2 + (y - RADAR_Y) ** 2
            if not 20 ** 2 < distance2 < (RADIUS - 10) ** 2:
                continue
            if abs(x - RADAR_X) < 5 or abs(y - RADAR_Y) < 5:
                continue                      # the cross lines
            # Dimmest of the two shots: a moving thing is dark in one.
            level = min(green(first, x, y), green(second, x, y))
            if level > 70 and (best is None or level > best[0]):
                best = (level, x, y)
    if best is None:
        sys.exit("no radar contact found")
    print(best[1], best[2])


if __name__ == "__main__":
    main()
