#!/usr/bin/env python3
"""Move the VM's mouse pointer to a screen position, and optionally click.

Usage: mouse.py QMP_SOCKET X Y [click]

X and Y are pixels on a 1024x768 screen. This talks QMP, QEMU's JSON
control protocol, because the simpler text monitor can only send relative
mouse movement. The VM has a tablet (virtio-tablet), which reports absolute
positions on a 0..32767 scale, so a pixel is converted to that scale.
"""
import json
import socket
import sys
import time

SCREEN_WIDTH, SCREEN_HEIGHT = 1024, 768


def main():
    path, x, y = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    click = len(sys.argv) > 4 and sys.argv[4] == "click"

    sock = socket.socket(socket.AF_UNIX)
    sock.connect(path)
    stream = sock.makefile("rw")

    def command(name, arguments=None):
        message = {"execute": name}
        if arguments is not None:
            message["arguments"] = arguments
        stream.write(json.dumps(message) + "\n")
        stream.flush()
        # Skip asynchronous event messages until the reply arrives.
        while True:
            reply = json.loads(stream.readline())
            if "return" in reply or "error" in reply:
                assert "return" in reply, reply
                return

    stream.readline()                  # QEMU's greeting
    command("qmp_capabilities")        # required before any other command

    def send(events):
        command("input-send-event", {"events": events})

    send([
        {"type": "abs", "data": {"axis": "x", "value": x * 32767 // SCREEN_WIDTH}},
        {"type": "abs", "data": {"axis": "y", "value": y * 32767 // SCREEN_HEIGHT}},
    ])
    if click:
        time.sleep(0.3)
        send([{"type": "btn", "data": {"button": "left", "down": True}}])
        time.sleep(0.1)
        send([{"type": "btn", "data": {"button": "left", "down": False}}])
    time.sleep(0.3)


if __name__ == "__main__":
    main()
