# Changelog

## 1.0.1

- Library: covers already cached are drawn in the same refresh as the grid, so
  returning to the library shows every cover at once (no tile-by-tile reload).
- First-time cover extraction updates the screen in batches instead of once per
  cover; the in-memory cover cache holds ~6 library pages.

## 1.0.0 — first public release

- **Library:** cover grid with reading progress, sorting, and a Studio shelf (Game of Life, design gallery).
- **Reader:** EPUB and TXT, anti-aliased Literata, Atkinson Hyperlegible and JetBrains Mono, a live Aa preview, contents, bookmarks, *go to %*, and resume on power-up.
- **Refresh:** diff-based partial refresh (16×16 blocks) with per-screen waveforms and a periodic clean-up.
- **Sleep screens:** book cover, five procedural 16-grey designs, card images, or random.
- **Wi-Fi & files portal:**
  - Setup access point with a captive page, then the home network (`reader.local`).
  - Book upload and an SD file manager.
  - Screensaver maker (crop, tone, Floyd-Steinberg dithering to a 4-bit PNG in the browser).
- **Hardening:**
  - Bounded EPUB, ZIP and TXT parsing (size caps, de-duplicated spine, paragraph splitting, write-error checks).
  - Atomic uploads, path validation, and a CSRF guard.
  - Captive DNS and Wi-Fi rewritten.
  - Safe internal-to-SD book migration that keeps reading progress.
