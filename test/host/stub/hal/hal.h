#pragma once
#include <cstdint>
#include <M5GFX.h>
enum { FONT_SIZE_1=0,FONT_SIZE_2,FONT_SIZE_3,FONT_SIZE_4,FONT_SIZE_5,FONT_SIZE_COUNT };
enum { FONT_SERIF=0, FONT_SANS, FONT_FAMILY_COUNT };
enum { SPACING_TIGHT=0, SPACING_NORMAL, SPACING_LOOSE, SPACING_COUNT };
enum { MARGIN_NARROW=0, MARGIN_NORMAL, MARGIN_WIDE, MARGIN_COUNT };
struct S { uint8_t font_size=2, font_family=0, spacing=1, margins=1; };
struct Hal { S settings; };
extern Hal hal;
