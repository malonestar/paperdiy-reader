# Font sources

| Font | Used for | Source | License |
|---|---|---|---|
| Literata (variable, opsz/wght) | book text | https://github.com/googlefonts/literata | SIL OFL 1.1 — `OFL-Literata.txt` |
| Atkinson Hyperlegible | menus, chrome, sans book text | https://brailleinstitute.org/freefont | SIL OFL 1.1 — `OFL-AtkinsonHyperlegible.txt` |
| JetBrains Mono (variable) | code blocks | https://github.com/JetBrains/JetBrainsMono | SIL OFL 1.1 — `OFL-JetBrainsMono.txt` |

`tools/make_fonts.py` renders these into `fonts/fonts.bin` (anti-aliased VLW
bitmaps), which is flashed to the `fonts` partition. Rendered bitmaps are a
derivative of the fonts and are distributed under the same license.
