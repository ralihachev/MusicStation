// Album artwork: pre-baked 1-bit bitmaps read straight off the card.
//
// V1 downloaded a JPEG, decoded it, sharpened it and error-diffused it on the
// device, which needed one large contiguous heap allocation. That block is
// exactly what the M4 audio buffers want, so V2 does the work on the Mac
// instead and reads a bitmap it can blit unchanged. Format in V2/SD_LAYOUT.md.
#pragma once

#include <Arduino.h>

constexpr uint16_t COVER_SIZE = 96;
constexpr int16_t COVER_X = 4;
constexpr int16_t COVER_Y = 31;

// 96x96 at one bit per pixel. Static, because the alternative is a heap
// allocation on every track change competing with the decoder.
constexpr size_t COVER_MAX_BYTES = (COVER_SIZE / 8) * COVER_SIZE;

bool coverLoad(const String &path);
void coverClear();
bool coverValid();
uint16_t coverWidth();
uint16_t coverHeight();
const uint8_t *coverBitmap();
