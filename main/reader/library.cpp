/*
 * paperdiy-reader — library scanning and book sessions.
 *
 * SPDX-License-Identifier: MIT
 */
#include "reader/library.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/hal.h"
#include "reader/studio.h"

static const char* TAG = "library";

namespace {

/** Hard ceiling so a corrupt file cannot paginate forever. */
constexpr size_t MAX_PAGES = 20000;

constexpr uint32_t IDX_MAGIC   = 0x58444952u; /* 'RIDX' */
constexpr uint32_t IDX_VERSION = 2u;

struct IdxHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t layout_hash;
    uint32_t stream_size;
    uint32_t page_count;
    uint32_t reserved;
};

uint32_t layout_hash_for(const layout::Style& st)
{
    // Only things that change text metrics belong here. The colour theme does
    // not move a single glyph, so it deliberately does not invalidate the
    // index — repaginating a novel just to recolour a heading is a poor trade.
    return layout::styleHash(st) | (library::LAYOUT_REVISION << 16) | ((uint32_t)layout::TEXT_BOTTOM << 22);
}

/* Index caches are kept per style ("<hash>.i<style>") so switching back to a
 * previously used font size is instant. */
std::string idx_ext_for(const layout::Style& st)
{
    char buf[16];
    snprintf(buf, sizeof(buf), ".i%04lx", (unsigned long)(layout::styleHash(st) & 0xFFFF));
    return std::string(buf);
}

/* ---- .toc sidecar ------------------------------------------------- */

constexpr uint32_t TOC_MAGIC = 0x434F5452u; /* 'RTOC' */

bool toc_read(const std::string& path, uint32_t stream_size, std::vector<TocEntry>& out)
{
    out.clear();
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    uint32_t hdr[3] = {0, 0, 0};
    bool ok         = fread(hdr, sizeof(uint32_t), 3, f) == 3 && hdr[0] == TOC_MAGIC && hdr[1] == stream_size &&
              hdr[2] <= 4000;
    for (uint32_t i = 0; ok && i < hdr[2]; i++) {
        TocEntry e;
        uint8_t lv_len[2];
        if (fread(&e.off, sizeof(uint32_t), 1, f) != 1 || fread(lv_len, 1, 2, f) != 2) {
            ok = false;
            break;
        }
        e.level = lv_len[0];
        e.title.resize(lv_len[1]);
        if (lv_len[1] && fread(&e.title[0], 1, lv_len[1], f) != lv_len[1]) {
            ok = false;
            break;
        }
        out.push_back(e);
    }
    fclose(f);
    if (!ok) {
        out.clear();
    }
    return ok;
}

void toc_write(const std::string& path, uint32_t stream_size, const std::vector<TocEntry>& toc)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        return;
    }
    uint32_t hdr[3] = {TOC_MAGIC, stream_size, (uint32_t)toc.size()};
    fwrite(hdr, sizeof(uint32_t), 3, f);
    for (const TocEntry& e : toc) {
        uint8_t lv_len[2] = {e.level, (uint8_t)std::min<size_t>(e.title.size(), 255)};
        fwrite(&e.off, sizeof(uint32_t), 1, f);
        fwrite(lv_len, 1, 2, f);
        fwrite(e.title.data(), 1, lv_len[1], f);
    }
    fclose(f);
}

/* Strips in-band face markers for display strings. */
std::string plain_text(const std::string& s, size_t max_len)
{
    std::string out;
    for (char c : s) {
        if (c == layout::FACE_MARK_ITALIC || c == layout::FACE_MARK_BOLD || c == layout::FACE_MARK_BOLDITAL) {
            continue;
        }
        if (out.size() >= max_len) {
            break;
        }
        out.push_back(c);
    }
    return out;
}

bool ends_with_ci(const char* s, const char* suffix)
{
    if (!s || !suffix) {
        return false;
    }
    size_t ls = strlen(s);
    size_t lx = strlen(suffix);
    return ls >= lx && strcasecmp(s + ls - lx, suffix) == 0;
}

std::string hash8_of(const std::string& path)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%08lx", (unsigned long)Hal::pathHash(path.c_str()));
    return std::string(buf);
}

/* ---- .met sidecar ------------------------------------------------- */

bool met_read(const std::string& path, uint32_t expect_size, EpubMeta& meta)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }

    char line[512];
    uint32_t stored_size = 0;
    int version          = 0;
    meta.title.clear();
    meta.author.clear();
    meta.paragraphs = 0;

    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        if (strncmp(line, "v=", 2) == 0) {
            version = atoi(line + 2);
        } else if (strncmp(line, "size=", 5) == 0) {
            stored_size = (uint32_t)strtoul(line + 5, nullptr, 10);
        } else if (strncmp(line, "title=", 6) == 0) {
            meta.title = line + 6;
        } else if (strncmp(line, "author=", 7) == 0) {
            meta.author = line + 7;
        } else if (strncmp(line, "paras=", 6) == 0) {
            meta.paragraphs = (uint32_t)strtoul(line + 6, nullptr, 10);
        }
    }
    fclose(f);

    return version == 1 && stored_size == expect_size;
}

void met_write(const std::string& path, uint32_t src_size, const EpubMeta& meta)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        ESP_LOGW(TAG, "cannot write %s", path.c_str());
        return;
    }
    fprintf(f, "v=1\n");
    fprintf(f, "size=%lu\n", (unsigned long)src_size);
    fprintf(f, "title=%s\n", meta.title.c_str());
    fprintf(f, "author=%s\n", meta.author.c_str());
    fprintf(f, "paras=%lu\n", (unsigned long)meta.paragraphs);
    fclose(f);
}

/* ---- directory scan ----------------------------------------------- */

/* Collects supported files under @p dir, descending @p depth folder levels.
 * Hidden entries (".cache", ".Trashes", "._foo.epub" from macOS) are skipped. */
void collect(const std::string& dir, int depth, std::vector<std::string>& paths, bool epub_only = false)
{
    DIR* d = opendir(dir.c_str());
    if (!d) {
        return;
    }
    std::vector<std::string> subdirs;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') {
            continue;
        }
        std::string full = dir + "/" + e->d_name;
        if (e->d_type == DT_DIR) {
            if (depth > 0 && strcasecmp(e->d_name, "System Volume Information") != 0) {
                subdirs.push_back(full);
            }
            continue;
        }
        if (epub_only ? ends_with_ci(e->d_name, ".epub") : library::isSupportedFile(e->d_name)) {
            paths.push_back(full);
        }
        if (paths.size() >= 2000) {
            break;
        }
    }
    closedir(d);
    for (const std::string& sd : subdirs) {
        collect(sd, depth - 1, paths);
    }
}

void scan_volume(store_id_t store, std::vector<BookEntry>& out, const LibraryProgressCb& cb)
{
    const char* dir = hal_storage_books_dir(store);
    if (!dir) {
        return;
    }

    std::vector<std::string> paths;
    collect(dir, 2, paths);
    if (store == STORE_SD) {
        // EPUBs dropped straight onto the card root count too. Loose .txt
        // files there are usually not books (e.g. a Raspberry Pi config.txt).
        collect(STORAGE_SD_BASE, 0, paths, true);
    }
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());

    for (size_t i = 0; i < paths.size(); i++) {
        if (cb) {
            cb((int)((i * 100) / (paths.size() ? paths.size() : 1)), "Reading library");
        }

        BookEntry b;
        b.path     = paths[i];
        size_t sl  = b.path.find_last_of('/');
        b.filename = b.path.substr(sl + 1);
        b.store    = store;
        b.is_epub  = ends_with_ci(b.filename.c_str(), ".epub");

        struct stat st;
        if (stat(b.path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        b.size = (uint32_t)st.st_size;

        if (b.is_epub) {
            std::string met = library::cachePath(b, ".met");
            EpubMeta meta;
            if (!met.empty() && met_read(met, b.size, meta)) {
                b.title  = meta.title;
                b.author = meta.author;
            } else if (epub_read_meta(b.path.c_str(), meta)) {
                b.title  = meta.title;
                b.author = meta.author;
                if (!met.empty()) {
                    met_write(met, b.size, meta);
                }
            }
            if (b.title.empty()) {
                b.title = txt_title_from_filename(b.path.c_str());
            }
        } else {
            b.title = txt_title_from_filename(b.path.c_str());
        }

        hal.progressGet(b.path.c_str(), &b.page, &b.total);
        b.recent = hal.recentGet(b.path.c_str());
        ESP_LOGI(TAG, "  %s  [%s]", b.path.c_str(), b.title.c_str());
        out.push_back(b);

        // Reading EPUB metadata is I/O heavy; keep the watchdog fed.
        vTaskDelay(1);
    }
}

}  // namespace

/* ================================================================== */
/* library namespace                                                   */
/* ================================================================== */

/* Sort key that ignores a leading "The " / "A " / "An ". */
static const char* title_key(const std::string& t)
{
    const char* s = t.c_str();
    if (strncasecmp(s, "the ", 4) == 0) {
        return s + 4;
    }
    if (strncasecmp(s, "an ", 3) == 0) {
        return s + 3;
    }
    if (strncasecmp(s, "a ", 2) == 0) {
        return s + 2;
    }
    return s;
}

/* Surname for "First Last" authors; "Last, First" already sorts. */
static std::string author_key(const std::string& a)
{
    if (a.empty()) {
        return std::string("\x7f");
    }
    if (a.find(',') != std::string::npos) {
        return a;
    }
    size_t sp = a.find_last_of(' ');
    return (sp == std::string::npos) ? a : a.substr(sp + 1) + " " + a.substr(0, sp);
}

void library::sortBooks(std::vector<BookEntry>& books, int mode)
{
    std::stable_sort(books.begin(), books.end(), [mode](const BookEntry& a, const BookEntry& b) {
        const bool sa = studio::isStudio(a), sb = studio::isStudio(b);
        if (sa != sb) {
            return sb;  // the Studio shelf always comes after the books
        }
        if (sa) {
            return studio::pieceOf(a) < studio::pieceOf(b);
        }
        if (mode == SORT_RECENT && a.recent != b.recent) {
            return a.recent > b.recent;  // never-opened books (0) fall to the end
        }
        if (mode == SORT_AUTHOR) {
            int c = strcasecmp(author_key(a.author).c_str(), author_key(b.author).c_str());
            if (c != 0) {
                return c < 0;
            }
        }
        int c = strcasecmp(title_key(a.title), title_key(b.title));
        if (c != 0) {
            return c < 0;
        }
        return a.path < b.path;
    });
}

bool library::isSupportedFile(const char* filename)
{
    return ends_with_ci(filename, ".txt") || ends_with_ci(filename, ".epub");
}

bool library::entryForPath(const std::string& path, BookEntry& out)
{
    if (path.empty()) {
        return false;
    }

    hal_storage_lock();
    struct stat st;
    bool exists = (stat(path.c_str(), &st) == 0) && S_ISREG(st.st_mode);
    hal_storage_unlock();

    if (!exists) {
        return false;
    }

    out          = BookEntry();
    out.path     = path;
    out.size     = (uint32_t)st.st_size;
    out.is_epub  = ends_with_ci(path.c_str(), ".epub");
    out.store    = storeOf(path);

    size_t slash = path.find_last_of('/');
    out.filename = (slash == std::string::npos) ? path : path.substr(slash + 1);

    if (out.is_epub) {
        EpubMeta meta;
        std::string met = cachePath(out, ".met");
        if (!met.empty() && met_read(met, out.size, meta)) {
            out.title  = meta.title;
            out.author = meta.author;
        }
    }
    if (out.title.empty()) {
        out.title = txt_title_from_filename(path.c_str());
    }

    hal.progressGet(out.path.c_str(), &out.page, &out.total);
    out.recent = hal.recentGet(out.path.c_str());
    return true;
}

std::string library::cachePath(const BookEntry& book, const char* extension)
{
    const char* dir = hal_storage_cache_dir(book.store);
    if (!dir) {
        return std::string();
    }
    hal_storage_mkdir(dir);
    return std::string(dir) + "/" + hash8_of(book.path) + extension;
}

void library::clearCache(const BookEntry& book)
{
    // Every sidecar starts with the book's path hash: .met .txn .toc .bmk and
    // one .iXXXX page index per typography style used.
    const char* dir = hal_storage_cache_dir(book.store);
    if (!dir) {
        return;
    }
    const std::string prefix = hash8_of(book.path) + ".";
    DIR* d                   = opendir(dir);
    if (!d) {
        return;
    }
    std::vector<std::string> victims;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (strncmp(e->d_name, prefix.c_str(), prefix.size()) == 0) {
            victims.push_back(std::string(dir) + "/" + e->d_name);
        }
    }
    closedir(d);
    for (const std::string& v : victims) {
        remove(v.c_str());
    }
}

store_id_t library::storeOf(const std::string& path)
{
    return (path.compare(0, strlen(STORAGE_SD_BASE), STORAGE_SD_BASE) == 0) ? STORE_SD : STORE_INTERNAL;
}

static BookEntry probeFor(const std::string& path)
{
    BookEntry b;
    b.path  = path;
    b.store = library::storeOf(path);
    return b;
}

void library::bookMoved(const std::string& from, const std::string& to)
{
    if (!isSupportedFile(from.c_str())) {
        return;
    }
    hal_storage_lock();
    clearCache(probeFor(from));
    hal_storage_unlock();
    hal.progressMove(from.c_str(), to.c_str());
}

void library::bookForgotten(const std::string& path)
{
    if (!isSupportedFile(path.c_str())) {
        return;
    }
    hal_storage_lock();
    clearCache(probeFor(path));
    hal_storage_unlock();
    hal.progressClear(path.c_str());
    char rpath[256];
    if (hal.resumeLoad(rpath, sizeof(rpath)) && path == rpath) {
        hal.resumeClear();
    }
}

bool library::removeBook(const BookEntry& book)
{
    hal_storage_lock();
    clearCache(book);
    bool ok = (remove(book.path.c_str()) == 0);
    hal_storage_unlock();

    if (ok) {
        hal.progressClear(book.path.c_str());
        ESP_LOGI(TAG, "deleted %s", book.path.c_str());
    } else {
        ESP_LOGW(TAG, "failed to delete %s", book.path.c_str());
    }
    return ok;
}

void library::scan(std::vector<BookEntry>& out, const LibraryProgressCb& cb)
{
    out.clear();

    hal_storage_lock();

    // SD first so that a book present on both volumes resolves to the card.
    if (hal_storage_sd_available()) {
        scan_volume(STORE_SD, out, cb);
    }

    std::vector<BookEntry> internal_books;
    scan_volume(STORE_INTERNAL, internal_books, cb);

    hal_storage_unlock();

    for (const BookEntry& ib : internal_books) {
        bool shadowed = false;
        for (const BookEntry& sb : out) {
            if (strcasecmp(sb.filename.c_str(), ib.filename.c_str()) == 0) {
                shadowed = true;
                break;
            }
        }
        if (!shadowed) {
            out.push_back(ib);
        }
    }

    if (hal.settings.show_studio) {
        studio::addEntries(out);
    }
    library::sortBooks(out, hal.settings.sort_mode);

    ESP_LOGI(TAG, "library: %u book(s)", (unsigned)out.size());

}

/* ================================================================== */
/* BookSession                                                         */
/* ================================================================== */

BookSession::~BookSession()
{
    close();
}

void BookSession::close()
{
    _src = nullptr;
    _txt.close();
    _norm.close();
    _index.clear();
    _index.shrink_to_fit();
    _toc.clear();
    _bookmarks.clear();
    _canvas = nullptr;
}

bool BookSession::ensureNormalised(const LibraryProgressCb& cb)
{
    std::string txn = library::cachePath(_book, ".txn");
    std::string met = library::cachePath(_book, ".met");
    if (txn.empty()) {
        return false;
    }

    EpubMeta meta;
    bool fresh = false;

    struct stat st;
    if (stat(txn.c_str(), &st) == 0 && st.st_size > 0 && !met.empty() && met_read(met, _book.size, meta) &&
        meta.paragraphs > 0) {
        fresh = true;
    }

    if (!fresh) {
        if (cb) {
            cb(0, "Extracting");
        }

        struct Ctx {
            const LibraryProgressCb* cb;
        } ctx{&cb};

        auto trampoline = [](int pct, const char* what, void* raw) {
            Ctx* c = (Ctx*)raw;
            if (c && c->cb && *c->cb) {
                (*c->cb)(pct, what);
            }
        };

        if (!epub_normalize(_book.path.c_str(), txn.c_str(), meta, _toc, trampoline, &ctx)) {
            return false;
        }
        if (!met.empty()) {
            met_write(met, _book.size, meta);
        }
        // A fresh extraction always invalidates every page index and the
        // stored contents; the new contents are saved once the stream opens.
        struct stat tst;
        uint32_t txn_size = (stat(txn.c_str(), &tst) == 0) ? (uint32_t)tst.st_size : 0;
        toc_write(library::cachePath(_book, ".toc"), txn_size, _toc);
    }

    if (!meta.title.empty()) {
        _book.title = meta.title;
    }
    if (!meta.author.empty()) {
        _book.author = meta.author;
    }

    return _norm.open(txn.c_str());
}

bool BookSession::open(const BookEntry& book, M5Canvas* canvas, const layout::Style& style,
                       const LibraryProgressCb& cb)
{
    close();

    _book   = book;
    _canvas = canvas;
    _style  = style;

    if (!canvas) {
        return false;
    }

    hal_storage_lock();

    bool ok = false;
    if (book.is_epub) {
        ok = ensureNormalised(cb);
        if (ok) {
            _src = &_norm;
        }
    } else {
        ok = _txt.open(book.path.c_str());
        if (ok) {
            _src = &_txt;
        }
    }

    if (!ok || !_src) {
        hal_storage_unlock();
        ESP_LOGW(TAG, "cannot open %s", book.path.c_str());
        close();
        return false;
    }

    const uint32_t stream_size = _src->size();
    std::string idx_path       = library::cachePath(_book, idx_ext_for(_style).c_str());

    loadToc(stream_size);
    loadBookmarks();

    if (idx_path.empty() || !loadIndex(idx_path, stream_size)) {
        if (!paginate(cb)) {
            hal_storage_unlock();
            close();
            return false;
        }
        if (!idx_path.empty()) {
            saveIndex(idx_path, stream_size);
        }
    }

    hal_storage_unlock();

    if (_index.empty()) {
        // A completely empty book still needs one page so the UI has something
        // to draw.
        _index.push_back(layout::Pos{0, 0});
    }

    uint16_t start_page = ((size_t)_book.page < _index.size()) ? _book.page : 0;
    hal.progressSet(_book.path.c_str(), start_page, (uint16_t)_index.size());

    ESP_LOGI(TAG, "opened '%s' (%d pages, style %04lx, %u toc, %u bookmarks)", _book.title.c_str(),
             (int)_index.size(), (unsigned long)layout::styleHash(_style), (unsigned)_toc.size(),
             (unsigned)_bookmarks.size());
    return true;
}

bool BookSession::loadIndex(const std::string& idx_path, uint32_t stream_size)
{
    FILE* f = fopen(idx_path.c_str(), "rb");
    if (!f) {
        return false;
    }

    IdxHeader h{};
    if (fread(&h, 1, sizeof(h), f) != sizeof(h)) {
        fclose(f);
        return false;
    }

    if (h.magic != IDX_MAGIC || h.version != IDX_VERSION || h.layout_hash != layout_hash_for(_style) ||
        h.stream_size != stream_size || h.page_count == 0 || h.page_count > MAX_PAGES) {
        fclose(f);
        return false;
    }

    _index.assign(h.page_count, layout::Pos{});
    size_t want = (size_t)h.page_count * sizeof(uint32_t) * 2;

    std::vector<uint32_t> raw(h.page_count * 2);
    if (fread(raw.data(), 1, want, f) != want) {
        fclose(f);
        _index.clear();
        return false;
    }
    fclose(f);

    for (uint32_t i = 0; i < h.page_count; i++) {
        _index[i].off  = raw[i * 2 + 0];
        _index[i].word = raw[i * 2 + 1];
    }

    ESP_LOGI(TAG, "index cache hit: %u pages", (unsigned)h.page_count);
    return true;
}

void BookSession::saveIndex(const std::string& idx_path, uint32_t stream_size)
{
    FILE* f = fopen(idx_path.c_str(), "wb");
    if (!f) {
        ESP_LOGW(TAG, "cannot write %s", idx_path.c_str());
        return;
    }

    IdxHeader h{};
    h.magic       = IDX_MAGIC;
    h.version     = IDX_VERSION;
    h.layout_hash = layout_hash_for(_style);
    h.stream_size = stream_size;
    h.page_count  = (uint32_t)_index.size();
    h.reserved    = 0;

    fwrite(&h, 1, sizeof(h), f);

    std::vector<uint32_t> raw;
    raw.reserve(_index.size() * 2);
    for (const layout::Pos& p : _index) {
        raw.push_back(p.off);
        raw.push_back(p.word);
    }
    fwrite(raw.data(), sizeof(uint32_t), raw.size(), f);
    fclose(f);
}

bool BookSession::paginate(const LibraryProgressCb& cb)
{
    _index.clear();

    const uint32_t total_bytes = _src->size() ? _src->size() : 1;
    layout::Pos pos{0, 0};
    layout::Page page;

    int last_pct = -1;

    for (size_t n = 0; n < MAX_PAGES; n++) {
        _index.push_back(pos);

        if (!layout::layoutPage(_canvas, *_src, pos, _style, page)) {
            // Nothing left to place — drop the page we speculatively added.
            if (_index.size() > 1) {
                _index.pop_back();
            }
            break;
        }
        if (page.eof) {
            break;
        }
        if (page.next == pos) {
            ESP_LOGW(TAG, "pagination stalled at offset %lu", (unsigned long)pos.off);
            break;
        }
        pos = page.next;

        if (cb) {
            int pct = (int)((uint64_t)pos.off * 100u / total_bytes);
            if (pct > 100) {
                pct = 100;
            }
            if (pct != last_pct) {
                last_pct = pct;
                cb(pct, "Paginating");
            }
        }

        if ((n & 0x07) == 0) {
            vTaskDelay(1);  // keep the idle task alive during long books
        }
    }

    if (cb) {
        cb(100, "Paginating");
    }

    ESP_LOGI(TAG, "paginated into %u pages", (unsigned)_index.size());
    return !_index.empty();
}

bool BookSession::renderPage(int page_no, layout::Page& out)
{
    if (!_src || page_no < 0 || page_no >= (int)_index.size()) {
        return false;
    }
    return layout::layoutPage(_canvas, *_src, _index[(size_t)page_no], _style, out);
}

/* ------------------------------------------------------------------ */
/* Positions                                                           */
/* ------------------------------------------------------------------ */

bool BookSession::previewPage(const layout::Pos& pos, const layout::Style& style, layout::Page& out)
{
    if (!_src) {
        return false;
    }
    hal_storage_lock();
    bool ok = layout::layoutPage(_canvas, *_src, pos, style, out);
    hal_storage_unlock();
    return ok;
}

static bool pos_less(const layout::Pos& a, const layout::Pos& b)
{
    return a.off < b.off || (a.off == b.off && a.word < b.word);
}

layout::Pos BookSession::pagePos(int page_no) const
{
    if (_index.empty()) {
        return layout::Pos{};
    }
    if (page_no < 0) {
        page_no = 0;
    }
    if (page_no >= (int)_index.size()) {
        page_no = (int)_index.size() - 1;
    }
    return _index[(size_t)page_no];
}

int BookSession::pageForPos(const layout::Pos& pos) const
{
    if (_index.empty()) {
        return 0;
    }
    // Last page whose start is <= pos.
    auto it = std::upper_bound(_index.begin(), _index.end(), pos,
                               [](const layout::Pos& p, const layout::Pos& e) { return pos_less(p, e); });
    int page = (int)(it - _index.begin()) - 1;
    return page < 0 ? 0 : page;
}

/* ------------------------------------------------------------------ */
/* Contents                                                            */
/* ------------------------------------------------------------------ */

void BookSession::loadToc(uint32_t stream_size)
{
    std::string path = library::cachePath(_book, ".toc");
    if (!path.empty() && toc_read(path, stream_size, _toc)) {
        return;
    }

    // No (valid) sidecar: build one by walking the paragraph stream. This
    // covers plain-text books and caches from older firmware.
    _toc.clear();
    if (!_src || !_src->seek(0)) {
        return;
    }
    layout::Para para;
    uint32_t n = 0;
    for (;;) {
        uint32_t off = _src->tell();
        if (!_src->next(para)) {
            break;
        }
        if ((para.style == layout::PARA_H1 || para.style == layout::PARA_H2) && _toc.size() < 2000) {
            TocEntry e;
            e.off   = off;
            e.level = (para.style == layout::PARA_H1) ? 1 : 2;
            e.title = plain_text(para.text, 96);
            _toc.push_back(e);
        }
        if ((++n & 0xFF) == 0) {
            vTaskDelay(1);
        }
    }
    if (!path.empty()) {
        toc_write(path, stream_size, _toc);
    }
}

int BookSession::tocPage(int i) const
{
    if (i < 0 || i >= (int)_toc.size()) {
        return 0;
    }
    return pageForPos(layout::Pos{_toc[(size_t)i].off, 0});
}

int BookSession::tocIndexForPage(int page_no) const
{
    if (_toc.empty() || _index.empty()) {
        return -1;
    }
    // A heading that starts anywhere on this page counts as "here", so the
    // contents list highlights the chapter whose title is on screen.
    layout::Pos end_pos = (page_no + 1 < (int)_index.size()) ? _index[(size_t)page_no + 1]
                                                               : layout::Pos{UINT32_MAX, UINT32_MAX};
    int found = -1;
    for (int i = 0; i < (int)_toc.size(); i++) {
        if (pos_less(layout::Pos{_toc[(size_t)i].off, 0}, end_pos)) {
            found = i;
        } else {
            break;
        }
    }
    return found;
}

bool BookSession::chapterSpan(int page_no, int* first_page, int* end_page, std::string* title) const
{
    if (_toc.empty()) {
        return false;
    }
    uint8_t top = 2;
    for (const TocEntry& e : _toc) {
        if (e.level < top) {
            top = e.level;
        }
    }

    layout::Pos here = pagePos(page_no);
    int cur          = -1;
    int next         = -1;
    for (int i = 0; i < (int)_toc.size(); i++) {
        if (_toc[(size_t)i].level != top) {
            continue;
        }
        if (!pos_less(here, layout::Pos{_toc[(size_t)i].off, 0})) {
            cur = i;
        } else {
            next = i;
            break;
        }
    }
    if (cur < 0) {
        return false;  // front matter before the first chapter
    }
    if (first_page) {
        *first_page = tocPage(cur);
    }
    if (end_page) {
        *end_page = (next >= 0) ? tocPage(next) : (int)_index.size();
    }
    if (title) {
        *title = _toc[(size_t)cur].title;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Bookmarks                                                           */
/* ------------------------------------------------------------------ */

void BookSession::loadBookmarks()
{
    _bookmarks.clear();
    std::string path = library::cachePath(_book, ".bmk");
    FILE* f          = path.empty() ? nullptr : fopen(path.c_str(), "rb");
    if (!f) {
        return;
    }
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        unsigned long off  = 0;
        unsigned long word = 0;
        int consumed       = 0;
        if (sscanf(line, "%lu %lu %n", &off, &word, &consumed) < 2) {
            continue;
        }
        Bookmark b;
        b.pos.off  = (uint32_t)off;
        b.pos.word = (uint32_t)word;
        b.snippet  = line + consumed;
        while (!b.snippet.empty() && (b.snippet.back() == '\n' || b.snippet.back() == '\r')) {
            b.snippet.pop_back();
        }
        _bookmarks.push_back(b);
    }
    fclose(f);
    std::sort(_bookmarks.begin(), _bookmarks.end(),
              [](const Bookmark& a, const Bookmark& b) { return pos_less(a.pos, b.pos); });
}

void BookSession::saveBookmarks()
{
    std::string path = library::cachePath(_book, ".bmk");
    if (path.empty()) {
        return;
    }
    hal_storage_lock();
    if (_bookmarks.empty()) {
        remove(path.c_str());
    } else if (FILE* f = fopen(path.c_str(), "wb")) {
        for (const Bookmark& b : _bookmarks) {
            fprintf(f, "%lu %lu %s\n", (unsigned long)b.pos.off, (unsigned long)b.pos.word, b.snippet.c_str());
        }
        fclose(f);
    }
    hal_storage_unlock();
}

bool BookSession::isBookmarked(int page_no) const
{
    if (_index.empty()) {
        return false;
    }
    layout::Pos start = pagePos(page_no);
    layout::Pos end   = (page_no + 1 < (int)_index.size()) ? _index[(size_t)page_no + 1]
                                                             : layout::Pos{UINT32_MAX, UINT32_MAX};
    for (const Bookmark& b : _bookmarks) {
        if (!pos_less(b.pos, start) && pos_less(b.pos, end)) {
            return true;
        }
    }
    return false;
}

bool BookSession::toggleBookmark(int page_no, const std::string& snippet)
{
    layout::Pos start = pagePos(page_no);
    layout::Pos end   = (page_no + 1 < (int)_index.size()) ? _index[(size_t)page_no + 1]
                                                             : layout::Pos{UINT32_MAX, UINT32_MAX};
    bool removed = false;
    for (size_t i = 0; i < _bookmarks.size();) {
        if (!pos_less(_bookmarks[i].pos, start) && pos_less(_bookmarks[i].pos, end)) {
            _bookmarks.erase(_bookmarks.begin() + (long)i);
            removed = true;
        } else {
            i++;
        }
    }
    if (!removed) {
        Bookmark b;
        b.pos     = start;
        b.snippet = plain_text(snippet, 60);
        _bookmarks.push_back(b);
        std::sort(_bookmarks.begin(), _bookmarks.end(),
                  [](const Bookmark& a, const Bookmark& c) { return pos_less(a.pos, c.pos); });
    }
    saveBookmarks();
    return !removed;
}

void BookSession::removeBookmark(int i)
{
    if (i < 0 || i >= (int)_bookmarks.size()) {
        return;
    }
    _bookmarks.erase(_bookmarks.begin() + i);
    saveBookmarks();
}
