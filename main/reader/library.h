/*
 * paperdiy-reader — library scanning and book sessions.
 *
 * On-disk layout, per volume:
 *
 *   <books>/Title.epub            the book itself
 *   <books>/.cache/<hash8>.met    cached EPUB metadata (title/author/size)
 *   <books>/.cache/<hash8>.txn    normalised paragraph stream (EPUB only)
 *   <books>/.cache/<hash8>.idx    page-offset index for the current font size
 *
 * <hash8> is the FNV-1a hash of the book's full path, so caches follow the
 * book and are cheap to invalidate.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "hal/storage/hal_storage.h"
#include "reader/book_epub.h"
#include "reader/book_txt.h"
#include "reader/layout.h"

/** @brief One book as shown in the library menu. */
struct BookEntry {
    std::string path;     /*!< full path, e.g. /sdcard/books/Dracula.epub */
    std::string filename; /*!< Dracula.epub */
    std::string title;
    std::string author;
    store_id_t store   = STORE_INTERNAL;
    bool is_epub       = false;
    uint32_t size      = 0;
    uint16_t page      = 0; /*!< last read page (0-based) */
    uint16_t total     = 0; /*!< pages at the last-used font size, 0 = unknown */
    uint32_t recent    = 0; /*!< "last opened" stamp, 0 = never opened */
    int8_t cover       = 0; /*!< 0 = not looked for yet, 1 = thumbnail cached, -1 = none */

    /** @brief Percent read, 0..100. Returns -1 when not yet paginated. */
    int percent() const
    {
        if (total == 0) {
            return -1;
        }
        uint32_t p = (uint32_t)page * 100u / (uint32_t)total;
        return (int)(p > 100 ? 100 : p);
    }
};

/** @brief Progress reporting during long operations. */
using LibraryProgressCb = std::function<void(int percent, const char* what)>;

namespace library {

/** @brief Bumped whenever page geometry or font metrics change. */
static constexpr uint32_t LAYOUT_REVISION = 5;

/**
 * @brief Scans both volumes for .txt / .epub books.
 *
 * SD entries win over identically-named internal ones. Results are sorted by
 * title, case-insensitively. EPUB metadata is read (and cached) on first sight
 * of a book, so the first scan after adding books is the slow one.
 */
void scan(std::vector<BookEntry>& out, const LibraryProgressCb& cb = nullptr);

enum SortMode { SORT_RECENT = 0, SORT_TITLE = 1, SORT_AUTHOR = 2 };

/** @brief Sorts in place: most recently opened first, by title, or by author surname. */
void sortBooks(std::vector<BookEntry>& books, int mode);

/** @brief Removes a book plus its caches and stored progress. */
bool removeBook(const BookEntry& book);

/**
 * @brief A book file was renamed or moved: carry its reading progress over and
 *        drop the caches keyed by the old path. Safe for non-book files.
 */
void bookMoved(const std::string& from, const std::string& to);

/** @brief A book file is gone (deleted elsewhere): drop its caches and progress. */
void bookForgotten(const std::string& path);

/** @brief Store a path lives on (SD or internal) — from its prefix. */
store_id_t storeOf(const std::string& path);

/** @brief Cache file path for a book, e.g. ".idx". Empty if unavailable. */
std::string cachePath(const BookEntry& book, const char* extension);

/** @brief Deletes every cache file belonging to a book. */
void clearCache(const BookEntry& book);

/** @brief True when the filename has a supported extension. */
bool isSupportedFile(const char* filename);

/**
 * @brief Builds a BookEntry for one known path without scanning a directory.
 *
 * Used by the instant-resume fast path, which must not pay for a full library
 * scan before it can draw the page the reader was last on.
 *
 * @return false when the file no longer exists.
 */
bool entryForPath(const std::string& path, BookEntry& out);

}  // namespace library

/** @brief A saved place in a book. Stored as a stream position so it
 *  survives font and layout changes. */
struct Bookmark {
    layout::Pos pos;
    std::string snippet; /*!< first words of the page, for the list */
};

/**
 * @brief An open book: source stream, page-offset index, contents, bookmarks.
 *
 * Opening paginates the whole book once (reporting progress) and caches the
 * result per typography style, so flipping between two font sizes only
 * paginates each once.
 */
class BookSession {
public:
    BookSession() = default;
    ~BookSession();

    BookSession(const BookSession&)            = delete;
    BookSession& operator=(const BookSession&) = delete;

    /**
     * @brief Opens @p book for reading with typography @p style.
     * @param canvas used for font metrics during pagination
     * @return false when the book cannot be read.
     */
    bool open(const BookEntry& book, M5Canvas* canvas, const layout::Style& style,
              const LibraryProgressCb& cb = nullptr);

    /** @brief Releases the source stream and index. */
    void close();

    /** @brief True while a book is open. */
    bool isOpen() const
    {
        return _src != nullptr;
    }

    /** @brief Number of pages at the current font size. */
    int pageCount() const
    {
        return (int)_index.size();
    }

    /** @brief Lays out page @p page_no. Returns false when out of range. */
    bool renderPage(int page_no, layout::Page& out);

    /** @brief The book this session was opened from. */
    const BookEntry& book() const
    {
        return _book;
    }

    /** @brief Lays out one page from @p pos in another style (live Aa preview). */
    bool previewPage(const layout::Pos& pos, const layout::Style& style, layout::Page& out);

    /** @brief Typography the index was built for. */
    const layout::Style& style() const
    {
        return _style;
    }

    /** @brief Start position of page @p page_no (clamped). */
    layout::Pos pagePos(int page_no) const;

    /** @brief Page that contains stream position @p pos. */
    int pageForPos(const layout::Pos& pos) const;

    /* ---- contents ---- */

    /** @brief Headings (h1 chapters, h2 sections) in reading order. */
    const std::vector<TocEntry>& toc() const
    {
        return _toc;
    }

    /** @brief Page on which contents entry @p i starts. */
    int tocPage(int i) const;

    /** @brief Index of the contents entry the page falls under, or -1. */
    int tocIndexForPage(int page_no) const;

    /**
     * @brief Chapter boundaries around a page, for "pages left in chapter".
     *
     * Chapters are the top heading level present (h1, or h2 when a book has
     * no h1s). @return false when the book has no headings.
     */
    bool chapterSpan(int page_no, int* first_page, int* end_page, std::string* title) const;

    /* ---- bookmarks ---- */

    const std::vector<Bookmark>& bookmarks() const
    {
        return _bookmarks;
    }

    /** @brief True when a bookmark falls on page @p page_no. */
    bool isBookmarked(int page_no) const;

    /** @brief Adds or removes a bookmark on a page. @return true if added. */
    bool toggleBookmark(int page_no, const std::string& snippet);

    /** @brief Removes bookmark @p i. */
    void removeBookmark(int i);

private:
    bool ensureNormalised(const LibraryProgressCb& cb);
    bool loadIndex(const std::string& idx_path, uint32_t stream_size);
    void saveIndex(const std::string& idx_path, uint32_t stream_size);
    bool paginate(const LibraryProgressCb& cb);
    void loadToc(uint32_t stream_size);
    void loadBookmarks();
    void saveBookmarks();

    BookEntry _book;
    M5Canvas* _canvas = nullptr;
    layout::Style _style;
    std::vector<TocEntry> _toc;
    std::vector<Bookmark> _bookmarks;

    TxtParaSource _txt;
    NormParaSource _norm;
    layout::ParaSource* _src = nullptr;

    std::vector<layout::Pos> _index;
};
