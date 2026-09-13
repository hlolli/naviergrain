#!/usr/bin/env python3
"""Independent little-endian fixture; no production encoder/constants imported."""
import pathlib
import struct
import subprocess
import sys
import tempfile

header = struct.pack(
    "<4sIIIQQQQdIIII6fQ24x", b"NGFL", 1, 128, 4096,
    0xfedcba9876543210, 0x1020304050607080, 0x20000000000001,
    0xf123456789abcdef, 1.25, 16, 16, 1, 1, 1, 2, 3, 4, 5, 6,
    0x8877665544332211,
)
assert len(header) == 128
values = [(a + i * .125) * (-1 if a == 1 else 1)
          for a in range(4) for i in range(256)]
packet = header + struct.pack("<1024f", *values)
with tempfile.TemporaryDirectory(prefix="naviergrain-wire-") as directory:
    path = pathlib.Path(directory) / "independent-field.bin"
    path.write_bytes(packet)
    subprocess.run([sys.argv[1], str(path)], check=True)
