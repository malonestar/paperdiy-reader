// Host fuzz test for miniz_lite: mutated ZIPs must never crash or overrun.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "miniz_lite.h"
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        puts("usage: test_zip some.epub");
        return 0;
    }
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 1;
    std::vector<unsigned char> base;
    int c;
    while ((c = fgetc(f)) != EOF) base.push_back((unsigned char)c);
    fclose(f);
    unsigned s = 7;
    int opened = 0;
    for (int it = 0; it < 400; it++) {
        std::vector<unsigned char> b = base;
        int flips = 1 + it % 16;
        for (int k = 0; k < flips; k++) {
            s = s * 1103515245 + 12345;
            size_t pos = (it % 2) ? b.size() - 1 - (s >> 8) % std::min<size_t>(b.size(), 4096) : (s >> 8) % b.size();
            b[pos] = (unsigned char)(s >> 16);
        }
        FILE* o = fopen("/tmp/fuzz.zip", "wb");
        fwrite(b.data(), 1, b.size(), o);
        fclose(o);
        mz_zip_archive z;
        if (!mz_zip_reader_init_file(&z, "/tmp/fuzz.zip")) continue;
        opened++;
        for (uint32_t i = 0; i < mz_zip_reader_get_num_files(&z) && i < 200; i++) {
            size_t sz = 0;
            void* p = mz_zip_reader_extract_to_heap_max(&z, i, 4u << 20, &sz);
            if (p) mz_free(p);
        }
        mz_zip_reader_end(&z);
    }
    printf("zip fuzz OK (%d archives opened)\n", opened);
}
