#!/usr/bin/env python3
"""Packages a build into release files under dist/.

    python tools/make_release.py            # after `idf.py build` and make_fonts.py

Produces, with X.Y.Z from version.txt:
  paperdiy-reader-vX.Y.Z-full.bin   everything, flash at 0x0 (first install)
  paperdiy-reader-vX.Y.Z-app.bin    firmware only, flash at 0x10000 (updates)
  paperdiy-reader-vX.Y.Z-fonts.bin  font pack, flash at 0x410000
  SHA256SUMS.txt
"""
import hashlib
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
B = os.path.join(ROOT, "build")
VER = open(os.path.join(ROOT, "version.txt")).read().strip()
DIST = os.path.join(ROOT, "dist")
name = f"paperdiy-reader-v{VER}"

parts = [("0x0", os.path.join(B, "bootloader", "bootloader.bin")),
         ("0x8000", os.path.join(B, "partition_table", "partition-table.bin")),
         ("0x10000", os.path.join(B, "paperdiy_reader.bin")),
         ("0x410000", os.path.join(ROOT, "fonts", "fonts.bin"))]
for _, p in parts:
    if not os.path.exists(p):
        sys.exit(f"missing {p}: build first (idf.py build, tools/make_fonts.py)")

os.makedirs(DIST, exist_ok=True)
full = os.path.join(DIST, f"{name}-full.bin")
def merge(sub, mode_opts):
    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32s3", sub, "-o", full] + mode_opts
    for off, p in parts:
        cmd += [off, p]
    return subprocess.run(cmd).returncode == 0

# esptool v5 spells it merge-bin / --flash-mode; v4 (bundled with IDF 5.5) merge_bin / --flash_mode.
if not (merge("merge-bin", ["--flash-mode", "dio", "--flash-freq", "80m", "--flash-size", "16MB"]) or
        merge("merge_bin", ["--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", "16MB"])):
    sys.exit("esptool merge failed")
shutil.copy(parts[2][1], os.path.join(DIST, f"{name}-app.bin"))
shutil.copy(parts[3][1], os.path.join(DIST, f"{name}-fonts.bin"))

with open(os.path.join(DIST, "SHA256SUMS.txt"), "w") as f:
    for fn in sorted(os.listdir(DIST)):
        if fn.startswith(name):
            h = hashlib.sha256(open(os.path.join(DIST, fn), "rb").read()).hexdigest()
            f.write(f"{h}  {fn}\n")
print("release files in", DIST)
