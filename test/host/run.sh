#!/bin/sh
# Builds and runs the host tests (ASan + UBSan).
set -e
cd "$(dirname "$0")"
make -s
python3 make_epubs.py epubs >/dev/null
./test_layout | tail -1
./test_portal
./test_zip epubs/normal.epub
./test_epub epubs/*.epub 2>/dev/null
