#!/usr/bin/env python3
"""Tests for the Python tools that need no VM. Run: python3 ci/test_tools.py"""
import importlib.machinery
import importlib.util
import os
import struct
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
TOP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def load(path):
    """Import a script by its path, whatever its file name."""
    name = os.path.basename(path).replace("-", "_").replace(".py", "")
    loader = importlib.machinery.SourceFileLoader(name, os.path.join(TOP, path))
    module = importlib.util.module_from_spec(
        importlib.util.spec_from_loader(name, loader))
    loader.exec_module(module)
    return module


screenshot = load("ci/screenshot.py")
weather = load("distro/perkinsos-weather")
cursors = load("distro/theme/make-cursors.py")


class Screenshot(unittest.TestCase):
    def test_rows_with_and_without_padding(self):
        for width in (1024, 1821, 1822, 1823):
            for padding in (0, -(width * 3) % 4):
                rows = [os.urandom(width * 3) for _ in range(4)]
                with tempfile.NamedTemporaryFile() as f:
                    f.write(b"P6\n%d 4\n255\n" % width)
                    f.write(b"".join(r + b"\0" * padding for r in rows))
                    f.flush()
                    self.assertEqual(screenshot.read_ppm(f.name),
                                     (width, 4, b"".join(rows)))

    def test_first_pixels_that_look_like_spaces(self):
        with tempfile.NamedTemporaryFile() as f:
            f.write(b"P6\n4 1\n255\n" + b" \n\t" * 4)
            f.flush()
            self.assertEqual(screenshot.read_ppm(f.name), (4, 1, b" \n\t" * 4))


class Weather(unittest.TestCase):
    reply = {"current": {
        "time": 1795600000, "cloud_cover": 75, "precipitation": 0.2,
        "wind_speed_10m": 18.4, "wind_direction_10m": 250,
        "temperature_2m": -3.5, "is_day": 1}}

    def config(self, text):
        with tempfile.NamedTemporaryFile("w") as f:
            f.write(text)
            f.flush()
            return weather.read_config(f.name)

    def test_config_is_rounded(self):
        self.assertEqual(
            self.config("# home\nlatitude = 12.3456\nlongitude=-98.7654\n"),
            (12.3, -98.8))

    def test_bad_config(self):
        for text in ("", "latitude=1\n", "latitude=91\nlongitude=0\n",
                     "latitude=nan\nlongitude=0\n", "latitude=x\nlongitude=0\n"):
            with self.assertRaises((KeyError, ValueError)):
                self.config(text)

    def test_lines(self):
        self.assertEqual(weather.to_lines(self.reply).splitlines(), [
            "time=1795600000", "cloud_cover=75", "rain=0.2", "wind=18.4",
            "wind_from=250", "temperature=-3.5", "is_day=1"])

    def test_bad_replies(self):
        for bad in ("nan", "x\ny=1", None):
            reply = {"current": dict(self.reply["current"], cloud_cover=bad)}
            with self.assertRaises((TypeError, ValueError)):
                weather.to_lines(reply)
        with self.assertRaises(KeyError):
            weather.to_lines({"error": True})


class Cursors(unittest.TestCase):
    def test_file_layout(self):
        data = cursors.xcursor([cursors.ARROW], 3, 2)
        magic, header, _, count = struct.unpack_from("<4sIII", data)
        self.assertEqual((magic, header, count), (b"Xcur", 16, len(cursors.SIZES)))
        end = 0
        for i, size in enumerate(cursors.SIZES):
            kind, nominal, at = struct.unpack_from("<III", data, 16 + 12 * i)
            chunk = struct.unpack_from("<IIIIIIIII", data, at)
            self.assertEqual(chunk[1:3], (kind, nominal))
            self.assertEqual(chunk[4:6], (size, size))
            self.assertTrue(chunk[6] < size and chunk[7] < size)
            end = at + 36 + size * size * 4
        self.assertEqual(end, len(data))


if __name__ == "__main__":
    unittest.main()
