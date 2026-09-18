// Synchronized lyrics, read from a .lrc sidecar next to the track.
//
// V1 fetched these over HTTPS from LRCLIB and needed TLS, JSON, streaming
// parsers and a fallback chain. A local file removes all of it, and makes the
// arena easier too: the file size is known before reading, so storage is sized
// once with no growth and no reallocation.
#pragma once

#include <Arduino.h>

// Begin drawing the next line before its timestamp so the ~780 ms partial
// waveform finishes around when the line is actually sung. Carried over from
// V1, where it was tuned against this panel.
constexpr uint32_t LYRIC_LEAD_MS = 800;

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
