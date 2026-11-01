// Host test: EPUB conversion of ordinary and hostile books stays bounded.
#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "reader/book_epub.h"

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; i++) {
        auto t0 = std::chrono::steady_clock::now();
        EpubMeta meta;
        std::vector<TocEntry> toc;
        bool m  = epub_read_meta(argv[i], meta);
        bool ok = epub_normalize(argv[i], "/tmp/out.txn", meta, toc, nullptr, nullptr);
        long sz = 0;
        if (FILE* f = fopen("/tmp/out.txn", "rb")) {
            fseek(f, 0, SEEK_END);
            sz = ftell(f);
            fclose(f);
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        printf("%-28s meta=%d norm=%d title=%zuB toc=%zu out=%ldB %.0fms\n", argv[i], m, ok, meta.title.size(),
               toc.size(), sz, ms);
        assert(meta.title.size() <= 4096);
        assert(sz < 64l * 1024 * 1024);
        remove("/tmp/out.txn");
    }
    puts("epub tests OK");
}
