#!/usr/bin/env python3
"""Tiny serial logger: python tools/mon.py COM6 [seconds] [--reset]"""
import sys
import time

import serial

port = sys.argv[1]
secs = float(sys.argv[2]) if len(sys.argv) > 2 and not sys.argv[2].startswith("-") else 1e9
s = serial.Serial(port, 115200, timeout=0.2)
if "--reset" in sys.argv:
    s.dtr = False
    s.rts = True
    time.sleep(0.1)
    s.rts = False
t0 = time.time()
while time.time() - t0 < secs:
    d = s.read(4096)
    if d:
        sys.stdout.write(d.decode("utf-8", "replace"))
        sys.stdout.flush()
