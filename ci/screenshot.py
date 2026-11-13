#!/usr/bin/env python3
"""Take a screenshot of the running VM through QEMU's monitor socket.

Usage: screenshot.py MONITOR_SOCKET OUTPUT.png

QEMU's `screendump` command writes a PPM file (raw RGB with a short text
header). We convert it to PNG with only the standard library, so CI needs
no extra packages. Prints the image size and how many distinct colours it
has, which the desktop test uses to tell "something was drawn" from a
blank screen.
"""
import os
import socket
import struct
import sys
import time
import zlib


def screendump(monitor_path, ppm_path):
    """Ask QEMU to write the screen to ppm_path and wait for the file."""
    sock = socket.socket(socket.AF_UNIX)
    sock.connect(monitor_path)
    sock.sendall(f"screendump {ppm_path}\n".encode())
    deadline = time.time() + 10
    last_size = -1
    while time.time() < deadline:
        time.sleep(0.3)
        size = os.path.getsize(ppm_path) if os.path.exists(ppm_path) else 0
        if size > 0 and size == last_size:   # stopped growing: done
            break
        last_size = size
    sock.close()


def read_ppm(path):
    """Return (width, height, rgb_bytes) from a binary PPM (type P6)."""
    with open(path, "rb") as f:
        data = f.read()
    # Header is: "P6" width height maxval, separated by whitespace.
    fields = data.split(None, 4)
    assert fields[0] == b"P6", "not a binary PPM"
    width, height = int(fields[1]), int(fields[2])
    return width, height, fields[4][: width * height * 3]


def write_png(path, width, height, rgb):
    """Write 8-bit RGB pixels as a PNG file."""
    def chunk(kind, body):
        crc = zlib.crc32(kind + body)
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", crc)

    # Each row is prefixed with a filter byte; 0 means "no filter".
    stride = width * 3
    rows = b"".join(b"\x00" + rgb[y * stride:(y + 1) * stride] for y in range(height))
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)  # 8-bit RGB
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", header))
        f.write(chunk(b"IDAT", zlib.compress(rows, 6)))
        f.write(chunk(b"IEND", b""))


def main():
    monitor_path, png_path = sys.argv[1], sys.argv[2]
    ppm_path = png_path + ".ppm"
    screendump(monitor_path, ppm_path)
    width, height, rgb = read_ppm(ppm_path)
    write_png(png_path, width, height, rgb)
    os.remove(ppm_path)
    colours = len({rgb[i:i + 3] for i in range(0, len(rgb), 3)})
    print(f"size={width}x{height} colours={colours}")


if __name__ == "__main__":
    main()
