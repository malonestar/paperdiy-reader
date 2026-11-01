# paperdiy-reader

E-reader firmware for the **M5Stack PaperDIY** (ESP32-S3, 4.7" 540×960 e-paper with 16 greys, two keys, microSD).

It reads EPUB and TXT books from the SD card. Text is set in anti-aliased Literata or Atkinson Hyperlegible, and page turns refresh only what changed. Your place in every book is saved. A Wi-Fi portal handles uploads, file management and screensavers.

## Features

- **Library home screen:** a 3×3 grid of covers taken from the EPUBs, with a progress bar on each book. Sort by recently read, title or author. Books without a cover get a typeset cover instead.
- **Reading:**
  - Proper typography: 5 sizes, serif or sans, line spacing and margins, with a live preview while you change them.
  - Justified text and chapter headings.
  - Italic and bold.
  - Code blocks in JetBrains Mono.
  - Characters: Latin-1, Latin Extended-A and Greek, plus curly quotes and dashes.
- **Navigation:** contents, bookmarks, *go to %*, and "N pages left in chapter".
- **Saved progress:** stored per book. It is font-independent, so changing the size keeps your place. Powering on resumes the book you were reading.
- **Fast refresh:** each screen is diffed against what the panel shows, and only changed 16×16 blocks are refreshed. A page turn takes about 0.2 s, and moving the library cursor refreshes only two frames. A full clean-up refresh runs every N pages (configurable).
- **Sleep screens:**
  - Your book's cover.
  - One of five procedural 16-grey designs: Contours, Stack, Ridges, Circuit, Moiré.
  - Your own images from the SD card.
  - Random.
- **Studio:** two extras on the library shelf: Conway's Game of Life, animated with partial refresh, and a gallery of the sleep-screen designs.
- **Wi-Fi & files portal:**
  - Upload books.
  - A full SD card file manager: browse, upload, download, rename/move, delete, new folder.
  - A screensaver maker that crops, adjusts and dithers photos to the panel's 16 greys in your browser.
  - Wi-Fi setup through a captive access point.
- **Hardened input handling:** malformed or hostile EPUB, ZIP and TXT files, and hostile HTTP requests, are bounded. See *Security notes* and `test/host`.

## Controls

The **top key** is A and the **bottom key** is B. Hold means about 0.6 s.

| Where | Top click | Bottom click | Hold top | Hold bottom |
|---|---|---|---|---|
| Reading | next page | previous page | menu | library |
| Library | previous book | next book | open | library menu |
| Menus and lists | up | down | select | back |
| Studio | Life: reseed. Gallery: next | Life: frame-rate counter. Gallery: previous | full refresh (Gallery: reshuffle) | back |

- **Both keys for 1.5 s:** sleep (power off), from anywhere except the Wi-Fi screen.
- **Power button:** one click turns the reader on (it resumes your book); a double click turns it off.
- **On USB power** the PMIC stays on, so "sleep" shows the sleep screen and waits for a key.

## Install a release (no toolchain needed)

1. Download `paperdiy-reader-vX.Y.Z-full.bin` from [Releases](https://github.com/malonestar/paperdiy-reader/releases).
2. Install esptool: `pip install esptool`.
3. Connect the PaperDIY with USB-C and flash:

   ```sh
   esptool --chip esp32s3 --baud 921600 write-flash 0x0 paperdiy-reader-vX.Y.Z-full.bin
   ```

   esptool finds the port by itself, or add `-p COM6` / `-p /dev/ttyACM0`. With esptool older than v5, write `write_flash` instead of `write-flash`.

To update later, flash only the firmware, which keeps your settings, progress and fonts:

```sh
esptool --chip esp32s3 write-flash 0x10000 paperdiy-reader-vX.Y.Z-app.bin
```

## Getting books on

- **SD card:** copy `.epub` or `.txt` files into `books/`. Sub-folders work, up to two levels deep. EPUBs at the top level of the card also show up.
- **Wi-Fi:** open *Library menu (hold B) → Wi-Fi & files*.
  - **First time:** the reader starts an open network called `PaperDIY-XXXXXX` and shows a QR code to join it. The setup page opens by itself. Choose your home Wi-Fi there.
  - **After that:** the reader joins your network and shows its address (also `http://reader.local`). If it can't join, it reopens the setup network.
  - **Hold A** on that screen to go back to setup.
  - **Hold B, or "Done" in the browser,** leaves the screen and switches the radio off.
- **Books saved to internal storage** (no card inserted) move to the card automatically at the next boot.

### SD card layout

```
books/            your books (any sub-folders)
books/.cache/     covers, page indexes, converted text (safe to delete)
screensaver/      sleep-screen images (PNG/JPEG; the web UI makes 4-bit PNGs)
screensaver/logo*.png   optional logo shown on the procedural designs
```

## Building

You need ESP-IDF **5.5.x**, Python 3, and for the font pack `pip install freetype-py fonttools`.

```sh
python fetch_repos.py          # pinned M5GFX + M5Unified (with PaperDIY support) into components/
python tools/make_fonts.py     # fonts/fonts.bin (8.1 MB; reproducible)
idf.py build
idf.py -p PORT flash           # firmware + partition table
esptool --chip esp32s3 -p PORT write-flash 0x410000 fonts/fonts.bin   # first time / font changes
python tools/make_release.py   # dist/: full, app and fonts images + SHA256SUMS
```

On Windows, `tools\build.ps1 [build|flash COMx|release]` wraps all of this.

### Partition map (16 MB flash)

| Name | Offset | Size | Contents |
|---|---|---|---|
| nvs | 0x9000 | 24 KB | settings, reading progress |
| factory | 0x10000 | 4 MB | firmware |
| fonts | 0x410000 | 9 MB | VLW font pack (memory-mapped) |
| storage | 0xD10000 | 2.9 MB | internal FAT (fallback when there's no card) |

### Releases

The release workflow is `.github/workflows/build.yml`:
- On every push it builds the firmware and font pack in the `espressif/idf:v5.5.1` container and runs the host tests.
- Pushing a tag `vX.Y.Z` attaches the `dist/` files to a GitHub release.
- Bump `version.txt` first. The version shows in the web UI and in the boot log.

## Tests

`test/host/run.sh` builds the portable modules for the PC with AddressSanitizer and UndefinedBehaviorSanitizer, and runs:
- layout and pagination on a synthetic 250-page book;
- the portal's path resolver and URL decoding;
- the captive DNS reply builder, with 200k random packets;
- a ZIP fuzzer;
- EPUB conversion of hostile books: a megabyte title, a paragraph with no breaks, a repeated spine, an oversized manifest, and injected style markers.

## Code map

| Path | What |
|---|---|
| `main/hal/` | board bring-up, settings and progress in NVS, internal FAT + SD |
| `main/fonts/vlw_fonts.*` | font pack loader with fast width tables |
| `main/reader/layout.*` | text normalisation and pagination |
| `main/reader/book_epub.*` | XHTML scanner and EPUB-to-paragraph-stream conversion |
| `main/reader/book_txt.*` | TXT source (UTF-8 or Windows-1252) |
| `main/third_party/miniz_lite.*` | ZIP and inflate |
| `main/reader/library.*` | library scan, sort, book sessions, page index, contents, bookmarks |
| `main/reader/render.*` | screens and the diff-based refresh policy |
| `main/reader/cover.*` | cover extraction and thumbnail cache |
| `main/reader/art.*` | procedural 16-grey artwork |
| `main/reader/studio.*` | animations |
| `main/reader/reader_app.*` | state machine and key handling |
| `main/net/` | Wi-Fi (station / setup AP) and captive DNS |
| `main/portal/` | HTTP API, path resolver |
| `web/index.html` | the web UI, embedded in the firmware |

## Security notes

The portal is for a trusted home network.
- The setup access point is open and only runs while the Wi-Fi screen is open on the device.
- On your LAN the API has no password. Every state-changing request needs an `X-Reader: 1` header, so other websites can't drive the portal (CSRF).
- All paths are validated against the volume root.
- Uploads are written to a temporary file and swapped in only when complete.
- The radio is off whenever the Wi-Fi screen is closed.

## Credits

- [M5GFX](https://github.com/m5stack/M5GFX) and [M5Unified](https://github.com/m5stack/M5Unified) (MIT), M5Stack. `fetch_repos.py` is adapted from the M5PaperDIY UserDemo (MIT).
- The inflate core in `miniz_lite.c` follows Mark Adler's *puff* (zlib license).
- Fonts: Literata, Atkinson Hyperlegible and JetBrains Mono (SIL OFL 1.1), in `fonts/src`.
- [espressif/qrcode](https://components.espressif.com/components/espressif/qrcode) and [espressif/mdns](https://components.espressif.com/components/espressif/mdns) (Apache-2.0).

MIT licensed. See `LICENSE`.
