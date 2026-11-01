// Host tests for the portal's path resolver and the captive DNS reply builder.
#include <arpa/inet.h>
#include <cassert>
#include <cstdio>
#include <cstring>

#include "net/captive_dns.h"
#include "portal/fs_path.h"

using fs_path::resolve;

static bool ok(const char* rel, unsigned flags = 0)
{
    std::string abs;
    return resolve("/sdcard", rel, flags, abs, nullptr);
}

int main()
{
    std::string abs;
    assert(resolve("/sdcard", "books/Dracula.epub", 0, abs, nullptr) && abs == "/sdcard/books/Dracula.epub");
    assert(resolve("/sdcard", "/books/", 0, abs, nullptr) && abs == "/sdcard/books");
    assert(resolve("/sdcard", "", fs_path::ALLOW_ROOT, abs, nullptr) && abs == "/sdcard");
    assert(ok("books/Café [1].epub"));
    assert(!ok(""));
    assert(!ok("../etc"));
    assert(!ok("books/../../x"));
    assert(!ok("books/./x"));
    assert(!ok("books//x"));
    assert(!ok("a\\b"));
    assert(!ok("a:b"));
    assert(!ok("a\x01"));
    assert(!ok("name."));
    assert(!ok("name "));
    assert(!ok(".cache/x"));
    assert(ok(".cache/x", fs_path::ALLOW_HIDDEN));
    assert(!ok("System Volume Information/x"));
    assert(!ok("\xC3"));      // truncated UTF-8
    assert(!ok("\xC0\xAF"));  // overlong '/'
    assert(!ok(std::string(300, 'a').c_str()));

    std::string d;
    assert(fs_path::urlDecode("a%20b+c%2Fd", d) && d == "a b c/d");
    assert(!fs_path::urlDecode("a%00b", d));
    assert(!fs_path::urlDecode("a%2", d));
    assert(!fs_path::urlDecode("a%zz", d));
    assert(fs_path::jsonEscape("a\"b\\c\n\x01") == "a\\\"b\\\\c\\n\\u0001");

    // DNS: A query for "example.com"
    uint8_t q[64] = {0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0};
    int n = 12;
    const char* labels[] = {"example", "com"};
    for (const char* l : labels) {
        q[n++] = (uint8_t)strlen(l);
        memcpy(q + n, l, strlen(l));
        n += (int)strlen(l);
    }
    q[n++] = 0;
    q[n++] = 0; q[n++] = 1; q[n++] = 0; q[n++] = 1;
    uint8_t out[600];
    uint32_t ip = inet_addr("192.168.4.1");
    int r = net::dnsBuildReply(q, n, ip, out, sizeof(out));
    assert(r == n + 16 && out[7] == 1 && (out[2] & 0x80) && memcmp(out + r - 4, &ip, 4) == 0);
    // AAAA: no answer but a valid reply
    q[n - 3] = 28;
    r = net::dnsBuildReply(q, n, ip, out, sizeof(out));
    assert(r == n && out[7] == 0);
    // malformed: label running past the end, compression pointer, short packet
    uint8_t bad[20] = {0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 60, 'a', 'b'};
    assert(net::dnsBuildReply(bad, 15, ip, out, sizeof(out)) == 0);
    bad[12] = 0xC0;
    assert(net::dnsBuildReply(bad, 20, ip, out, sizeof(out)) == 0);
    assert(net::dnsBuildReply(q, 10, ip, out, sizeof(out)) == 0);
    // fuzz: random packets never crash or overrun
    unsigned s = 1;
    for (int i = 0; i < 200000; i++) {
        uint8_t pkt[512];
        int len = 12 + (int)((s = s * 1103515245 + 12345) >> 16) % 500;
        for (int k = 0; k < len; k++) pkt[k] = (uint8_t)((s = s * 1103515245 + 12345) >> 16);
        pkt[2] &= 0x07;
        int rr = net::dnsBuildReply(pkt, len, ip, out, sizeof(out));
        assert(rr >= 0 && rr <= (int)sizeof(out));
    }
    puts("portal tests OK");
}
