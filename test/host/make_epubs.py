"""Builds test EPUBs: one ordinary book and several hostile ones."""
import zipfile, sys, os
out = sys.argv[1]; os.makedirs(out, exist_ok=True)
CONT = '<?xml version="1.0"?><container><rootfiles><rootfile full-path="OEBPS/c.opf"/></rootfiles></container>'
def opf(title, items, spine):
    return f'<package><metadata><dc:title>{title}</dc:title><dc:creator>Tester</dc:creator></metadata><manifest>{items}</manifest><spine>{spine}</spine></package>'
def book(name, title, chapters, spine_ids=None, extra=None):
    z = zipfile.ZipFile(os.path.join(out, name), "w", zipfile.ZIP_DEFLATED)
    z.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
    z.writestr("META-INF/container.xml", CONT)
    items = "".join(f'<item id="c{i}" href="c{i}.xhtml" media-type="application/xhtml+xml"/>' for i in range(len(chapters)))
    spine = "".join(f'<itemref idref="c{i}"/>' for i in (spine_ids if spine_ids is not None else range(len(chapters))))
    z.writestr("OEBPS/c.opf", opf(title, items + (extra or ""), spine))
    for i, c in enumerate(chapters):
        z.writestr(f"OEBPS/c{i}.xhtml", c)
    z.close()
normal = ["<html><body><h1>Chapter %d</h1>%s</body></html>" % (i, "".join(f"<p>Para {k} with <i>italic</i> &amp; caf&#233; text.</p>" for k in range(200))) for i in range(5)]
book("normal.epub", "A Normal Book", normal)
book("huge_title.epub", "A" * 900_000, normal[:1])
book("huge_para.epub", "Runaway", ["<html><body><p>" + "word " * 400_000 + "</body></html>"])
book("tab_pre.epub", "Tabs", ["<html><body><pre>" + "\t" * 1_000_000 + "</pre></body></html>"])
book("repeat_spine.epub", "Repeat", normal[:1], spine_ids=[0] * 20_000)
book("markers.epub", "Markers", ["<html><body><p>\x01evil &#2;bold &#3;x</p></body></html>"])
many = "".join(f'<item id="x{i}" href="x{i}.xhtml" media-type="application/xhtml+xml"/>' for i in range(9_000))
book("many_items.epub", "Many", normal[:1], extra=many)
print("ok")
