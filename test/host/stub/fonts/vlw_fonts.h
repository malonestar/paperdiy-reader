#pragma once
#include <cstddef>
#include <cstdint>
#include <M5GFX.h>
namespace vlw {
const lgfx::IFont* get(const char* name);
int width(const lgfx::IFont* f, const char* s, size_t len);
inline int width(const lgfx::IFont* f, const char* s){ size_t n=0; while(s[n]) n++; return width(f,s,n);} 
int height(const lgfx::IFont*);
bool has_glyph(const lgfx::IFont*, uint32_t cp);
uint32_t utf8_next(const char* s, size_t len, size_t& i);
}
