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
from collections import Counter


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


def build_palette(frames):
    """Pick up to 256 colours for all frames; return (palette, lookup).

    GIF allows 256 colours. A flat UI uses a few hundred, mostly the soft
    edges of text. Keep the 256 most common and map each of the rest to
    the nearest kept colour.
    """
    counts = Counter()
    for _, _, rgb in frames:
        counts.update(rgb[i:i + 3] for i in range(0, len(rgb), 3))
    palette = [colour for colour, _ in counts.most_common(256)]
    lookup = {colour: index for index, colour in enumerate(palette)}
    for colour in counts:
        if colour not in lookup:
            lookup[colour] = min(range(len(palette)), key=lambda i: sum(
                (a - b) ** 2 for a, b in zip(palette[i], colour)))
    return palette, lookup


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
    palette, lookup = build_palette(frames)
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
            indices = bytes(lookup[rgb[i:i + 3]] for i in range(0, len(rgb), 3))
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
    frames = [read_png(path) for path in sys.argv[3:]]
    write_gif(output, frames, seconds)
    print(f"wrote {output}: {len(frames)} frames")


if __name__ == "__main__":
    main()
