// Synchronized lyrics, read from a .lrc sidecar next to the track.
//
// V1 fetched these over HTTPS from LRCLIB and needed TLS, JSON, streaming
// parsers and a fallback chain. A local file removes all of it, and makes the
// arena easier too: the file size is known before reading, so storage is sized
// once with no growth and no reallocation.
#pragma once

#include <Arduino.h>

// Default for the lyric offset: added to the measured refresh time
// (panelPartialRefreshMs, ~795 ms on this panel) to give how long before its
// timestamp a line starts drawing. Bench listening put the right total lead
// between ~890 ms (late) and ~1040 ms (early).
//
// This is only the default. Hold MENU on Now Playing to tune it by ear while
// a song plays; the result is kept in NVS (settings().lyricOffsetMs).
constexpr int16_t LYRIC_OFFSET_MS = 175;
constexpr int16_t LYRIC_OFFSET_MIN_MS = -1500;
constexpr int16_t LYRIC_OFFSET_MAX_MS = 2000;

// A lyric file larger than this is almost certainly not a lyric file.
constexpr size_t LYRIC_MAX_BYTES = 64 * 1024;

bool lyricsLoad(const String &path);
void lyricsClear();

bool lyricsAvailable();
uint16_t lyricsCount();
String lyricsText(uint16_t index);

// Status for the screen when there is nothing to show: "NO LYRICS", the file
// had no timestamps, or it failed to load.
const char *lyricsStatus();

// Index of the line that should be on screen at this playback position, or -1
// before the first line. The cursor advances incrementally, so the common case
// of time moving forward costs one comparison rather than a scan.
int16_t lyricsIndexAt(uint32_t playbackMs);

// Timestamp of the line after the one lyricsIndexAt() last returned, or
// UINT32_MAX when there is none. Lets the renderer keep the panel free for it.
uint32_t lyricsNextMs();
