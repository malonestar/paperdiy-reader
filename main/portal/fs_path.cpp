/*
 * paperdiy-reader — safe path handling for the web file manager.
 *
 * SPDX-License-Identifier: MIT
 */
#include "portal/fs_path.h"

#include <cstdio>
#include <cstring>
#include <strings.h>

namespace fs_path {

namespace {

bool validUtf8(const std::string& s)
{
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        int extra       = 0;
        if (c < 0x80) {
            i++;
            continue;
        } else if (c >= 0xC2 && c <= 0xDF) {
            extra = 1;
        } else if (c >= 0xE0 && c <= 0xEF) {
            extra = 2;
        } else if (c >= 0xF0 && c <= 0xF4) {
            extra = 3;
        } else {
            return false;
        }
        if (i + (size_t)extra >= s.size()) {
            return false;
        }
        for (int k = 1; k <= extra; k++) {
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) {
                return false;
            }
        }
        i += (size_t)extra + 1;
    }
    return true;
}

bool fail(std::string* err, const char* why)
{
    if (err) {
        *err = why;
    }
    return false;
}

}  // namespace

bool resolve(const std::string& root, const std::string& rel, unsigned flags, std::string& abs, std::string* err)
{
    abs.clear();
    if (!validUtf8(rel)) {
        return fail(err, "invalid characters");
    }
    size_t b = 0;
    size_t e = rel.size();
    while (b < e && rel[b] == '/') {
        b++;
    }
    while (e > b && rel[e - 1] == '/') {
        e--;
    }
    std::string out = root;
    if (b == e) {
        if (!(flags & ALLOW_ROOT)) {
            return fail(err, "path required");
        }
        abs = out;
        return true;
    }

    size_t i = b;
    while (i <= e) {
        size_t j = rel.find('/', i);
        if (j == std::string::npos || j > e) {
            j = e;
        }
        std::string c = rel.substr(i, j - i);
        if (c.empty()) {
            return fail(err, "empty path component");
        }
        if (c == "." || c == "..") {
            return fail(err, "relative components are not allowed");
        }
        if (c.size() > 255) {
            return fail(err, "name too long");
        }
        for (unsigned char ch : c) {
            if (ch < 0x20 || ch == 0x7F || strchr("\\\"*:<>?|", ch)) {
                return fail(err, "name contains a reserved character");
            }
        }
        if (c.back() == '.' || c.back() == ' ') {
            return fail(err, "names cannot end with a dot or space");
        }
        if (c[0] == '.' && !(flags & ALLOW_HIDDEN)) {
            return fail(err, "hidden files are not accessible");
        }
        if (strcasecmp(c.c_str(), "System Volume Information") == 0) {
            return fail(err, "system folder");
        }
        out += '/';
        out += c;
        i = j + 1;
    }
    if (out.size() > 255) {
        return fail(err, "path too long");
    }
    abs = out;
    return true;
}

bool urlDecode(const std::string& in, std::string& out)
{
    out.clear();
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        char c = in[i];
        if (c == '+') {
            out.push_back(' ');
        } else if (c == '%') {
            if (i + 2 >= in.size()) {
                return false;
            }
            auto hex = [](char h) -> int {
                if (h >= '0' && h <= '9') return h - '0';
                if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                return -1;
            };
            int hi = hex(in[i + 1]);
            int lo = hex(in[i + 2]);
            if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) {
                return false;
            }
            out.push_back((char)(hi * 16 + lo));
            i += 2;
        } else {
            out.push_back(c);
        }
    }
    return true;
}

std::string baseName(const std::string& path)
{
    size_t s = path.find_last_of('/');
    return (s == std::string::npos) ? path : path.substr(s + 1);
}

std::string dirName(const std::string& path)
{
    size_t s = path.find_last_of('/');
    return (s == std::string::npos) ? std::string() : path.substr(0, s);
}

std::string jsonEscape(const std::string& s)
{
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':
                o += "\\\"";
                break;
            case '\\':
                o += "\\\\";
                break;
            case '\n':
                o += "\\n";
                break;
            case '\r':
                o += "\\r";
                break;
            case '\t':
                o += "\\t";
                break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", c);
                    o += b;
                } else {
                    o.push_back((char)c);
                }
        }
    }
    return o;
}

}  // namespace fs_path
