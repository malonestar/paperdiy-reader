/*
 * paperdiy-reader — captive DNS reply builder (pure, host-testable).
 *
 * SPDX-License-Identifier: MIT
 */
#include <cstring>

#include "net/captive_dns.h"

namespace net {

int dnsBuildReply(const uint8_t* req, int len, uint32_t ip, uint8_t* out, int out_cap)
{
    // Header (12) + at least a root name, type and class.
    if (!req || !out || len < 17 || len > 512 || out_cap < len + 16) {
        return 0;
    }
    const uint16_t flags = (uint16_t)((req[2] << 8) | req[3]);
    if (flags & 0x8000) {
        return 0;  // a response, not a query
    }
    if ((flags >> 11) & 0x0F) {
        return 0;  // only standard queries (opcode 0)
    }
    const uint16_t qd = (uint16_t)((req[4] << 8) | req[5]);
    if (qd == 0) {
        return 0;
    }

    // Walk the first question's name, bounds-checked; no compression pointers
    // are legal in a question.
    int p = 12;
    for (;;) {
        if (p >= len) {
            return 0;
        }
        uint8_t l = req[p];
        if (l == 0) {
            p++;
            break;
        }
        if (l & 0xC0) {
            return 0;
        }
        p += 1 + l;
    }
    if (p + 4 > len) {
        return 0;
    }
    const uint16_t qtype  = (uint16_t)((req[p] << 8) | req[p + 1]);
    const uint16_t qclass = (uint16_t)((req[p + 2] << 8) | req[p + 3]);
    const int qend        = p + 4;

    // Reply = header + the first question only (drops EDNS / extra records).
    memcpy(out, req, (size_t)qend);
    out[2]  = (uint8_t)(0x80 | (req[2] & 0x01) | 0x04);  // QR, AA, keep RD
    out[3]  = 0x80;                                      // RA, NOERROR
    out[4]  = 0;
    out[5]  = 1;  // QDCOUNT
    out[6]  = 0;
    out[7]  = 0;  // ANCOUNT (set below)
    out[8]  = 0;
    out[9]  = 0;  // NSCOUNT
    out[10] = 0;
    out[11] = 0;  // ARCOUNT
    int n   = qend;

    if (qtype == 1 && (qclass & 0x7FFF) == 1) {  // A / IN
        const uint8_t ans[] = {0xC0, 0x0C,             // name: pointer to the question
                               0x00, 0x01, 0x00, 0x01,  // A, IN
                               0x00, 0x00, 0x00, 0x3C,  // TTL 60 s
                               0x00, 0x04};
        memcpy(out + n, ans, sizeof(ans));
        n += (int)sizeof(ans);
        memcpy(out + n, &ip, 4);  // already network order
        n += 4;
        out[7] = 1;
    }
    // Other types (AAAA, HTTPS...): NOERROR with no answer, so clients fall
    // back to the A record instead of timing out.
    return n;
}

}  // namespace net
