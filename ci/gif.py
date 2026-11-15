#!/usr/bin/env python3
"""Turn a series of PNG screenshots into an animated GIF.

Usage: gif.py OUTPUT.gif SECONDS_PER_FRAME FRAME1.png FRAME2.png ...

Standard library only, like screenshot.py, so no image tools need to be
installed. It reads the PNGs that screenshot.py writes (8-bit RGB, no row
filtering), not PNGs in general.
"""
import struct
import sys
import zlib


def read_png(path):
    """Return (width, height, rgb_bytes) for a PNG written by screenshot.py."""
    with open(path, "rb") as f:
        data = f.read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", f"{path}: not a PNG"
    pos, compressed, width, height = 8, b"", 0, 0
    while pos < len(data):
        # Each chunk: 4-byte length, 4-byte type, body, 4-byte checksum.
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, colour_type = struct.unpack(">IIBB", body[:10])
            assert (depth, colour_type) == (8, 2), f"{path}: not 8-bit RGB"
        elif kind == b"IDAT":
            compressed += body
        pos += 12 + length
    rows = zlib.decompress(compressed)
    stride = width * 3
    rgb = bytearray()
    for y in range(height):
        start = y * (stride + 1)
        assert rows[start] == 0, f"{path}: filtered rows are not supported"
        rgb += rows[start + 1:start + 1 + stride]
    return width, height, bytes(rgb)


def halve(width, height, rgb):
    """Return the image at half size: each pixel is the average of a 2x2
    block. Keeps the GIF small enough for a README."""
    w, h = width // 2, height // 2
    out = bytearray(w * h * 3)
    for y in range(h):
        top = y * 2 * width * 3
        bottom = top + width * 3
        for x in range(w):
            a = top + x * 6
            b = bottom + x * 6
            o = (y * w + x) * 3
            out[o] = (rgb[a] + rgb[a + 3] + rgb[b] + rgb[b + 3]) >> 2
            out[o + 1] = (rgb[a + 1] + rgb[a + 4] + rgb[b + 1] + rgb[b + 4]) >> 2
            out[o + 2] = (rgb[a + 2] + rgb[a + 5] + rgb[b + 2] + rgb[b + 5]) >> 2
    return w, h, bytes(out)


# GIF allows 256 colours. The desktop has a photographic sky with tens of
# thousands, so use a fixed, evenly spaced set: 6 levels of red, 7 of green
# (the eye is most sensitive to green) and 6 of blue = 252 colours.
LEVELS = (6, 7, 6)
PALETTE = [bytes((r * 255 // 5, g * 255 // 6, b * 255 // 5))
           for r in range(6) for g in range(7) for b in range(6)]

# Ordered dithering: before rounding each pixel to the nearest level, nudge
# it by an amount that depends on its position in a repeating 4x4 pattern.
# Neighbouring pixels then round in different directions, and a smooth
# gradient becomes a fine mix of two colours instead of visible bands.
BAYER = (0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5)


def to_indices(width, height, rgb):
    """Map every pixel to an index into PALETTE, with dithering."""
    out = bytearray(width * height)
    # For each channel, a table from (value + nudge) to its level.
    tables = []
    for levels in LEVELS:
        step = 255 / (levels - 1)
        tables.append([min(levels - 1, max(0, int((v - 32) / step + 0.5)))
                       for v in range(256 + 64)])
    red, green, blue = tables
    spread = 255 / 6 / 16          # one level's worth, over the 16 patterns
    for y in range(height):
        row = y * width
        pattern = BAYER[(y & 3) * 4:(y & 3) * 4 + 4]
        for x in range(width):
            i = (row + x) * 3
            nudge = int((pattern[x & 3] - 7.5) * spread) + 32
            out[row + x] = (red[rgb[i] + nudge] * 7 +
                            green[rgb[i + 1] + nudge]) * 6 + blue[rgb[i + 2] + nudge]
    return bytes(out)


def lzw_encode(indices):
    """Compress palette indexes with GIF's variant of LZW.

    LZW replaces sequences it has seen before with a single code. The table
    of sequences is built identically by encoder and decoder as the data
    goes by, so it never has to be stored. Codes start 9 bits wide and grow
    to 12; then the table is reset.
    """
    clear, end = 256, 257
    out = bytearray()
    acc = bits = 0          # bit accumulator: codes are packed low bit first
    table = {}
    width, next_code = 9, end + 1

    def emit(code):
        nonlocal acc, bits
        acc |= code << bits
        bits += width
        while bits >= 8:
            out.append(acc & 0xFF)
            acc >>= 8
            bits -= 8

    emit(clear)
    prefix = indices[0]
    for index in indices[1:]:
        key = (prefix << 8) | index     # "sequence `prefix` followed by index"
        if key in table:
            prefix = table[key]         # seen before: keep extending
            continue
        emit(prefix)                    # longest known sequence ends here
        if next_code < 4096:
            table[key] = next_code      # remember the new, longer sequence
            next_code += 1
            if next_code > (1 << width) and width < 12:
                width += 1
        else:
            emit(clear)                 # table full: start again
            table = {}
            width, next_code = 9, end + 1
        prefix = index
    emit(prefix)
    emit(end)
    if bits:
        out.append(acc & 0xFF)
    return bytes(out)


def write_gif(path, frames, seconds_per_frame):
    width, height, _ = frames[0]
    palette = PALETTE
    delay = int(seconds_per_frame * 100)          # GIF counts in 1/100 s

    with open(path, "wb") as f:
        f.write(b"GIF89a")
        # Logical screen: size, "global 256-colour palette follows", bg, aspect.
        f.write(struct.pack("<HHBBB", width, height, 0xF7, 0, 0))
        for colour in palette:
            f.write(colour)
        f.write(b"\x00\x00\x00" * (256 - len(palette)))
        # Loop forever (the NETSCAPE2.0 application extension).
        f.write(b"\x21\xFF\x0BNETSCAPE2.0\x03\x01\x00\x00\x00")

        for frame_width, frame_height, rgb in frames:
            assert (frame_width, frame_height) == (width, height)
            indices = to_indices(width, height, rgb)
            # Graphic control extension: how long to show this frame.
            f.write(b"\x21\xF9\x04\x00" + struct.pack("<H", delay) + b"\x00\x00")
            # Image descriptor: position, size, no local palette.
            f.write(b"\x2C" + struct.pack("<HHHHB", 0, 0, width, height, 0))
            f.write(b"\x08")                       # LZW minimum code size
            compressed = lzw_encode(indices)
            for i in range(0, len(compressed), 255):   # data goes in sub-blocks
                block = compressed[i:i + 255]
                f.write(bytes([len(block)]) + block)
            f.write(b"\x00")                       # end of image data
        f.write(b"\x3B")                           # end of file


def main():
    output, seconds = sys.argv[1], float(sys.argv[2])
    frames = [halve(*read_png(path)) for path in sys.argv[3:]]
    write_gif(output, frames, seconds)
    print(f"wrote {output}: {len(frames)} frames")


if __name__ == "__main__":
    main()
