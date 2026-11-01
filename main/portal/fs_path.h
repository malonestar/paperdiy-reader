/*
 * paperdiy-reader — safe path handling for the web file manager.
 *
 * Every path that arrives over HTTP goes through resolve(): it validates (it
 * never rewrites) a relative path and joins it to a volume root, so nothing
 * can escape the volume or touch the reader's own files. Pure C++, so it is
 * covered by the host tests.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <string>

namespace fs_path {

enum Flags : unsigned {
    ALLOW_ROOT   = 1u << 0, /*!< the volume root itself ("" or "/") is acceptable */
    ALLOW_HIDDEN = 1u << 1, /*!< components starting with '.' are acceptable */
};

/**
 * @brief Validates @p rel and joins it to @p root.
 *
 * Accepts "a/b/c.epub" (a leading or trailing '/' is ignored). Rejects empty
 * components, "." and "..", backslashes, control characters, the FAT-reserved
 * characters " * : < > ? |, components ending in '.' or ' ', invalid UTF-8,
 * components over 255 bytes, results over 255 bytes, hidden components unless
 * ALLOW_HIDDEN, and "System Volume Information".
 *
 * @param err receives a short reason on failure (may be null)
 */
bool resolve(const std::string& root, const std::string& rel, unsigned flags, std::string& abs, std::string* err);

/** @brief Decodes %XX and '+' from a URL query value. Rejects %00. */
bool urlDecode(const std::string& in, std::string& out);

/** @brief Last component of a path ("" for the root). */
std::string baseName(const std::string& path);

/** @brief Everything before the last '/'. */
std::string dirName(const std::string& path);

/** @brief JSON string escaping (quotes, backslash, control characters). */
std::string jsonEscape(const std::string& s);

}  // namespace fs_path
