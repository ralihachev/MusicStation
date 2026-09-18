// MP3 duration extraction.
//
// Duration is the one track field that cannot be derived from the filesystem,
// and both the lyric scheduler and the progress bar depend on it, so a wrong
// value is visible on every screen. It is read once during the index build
// rather than at play time: a few short reads per file at index time, none at
// all while audio is decoding.
#pragma once

#include <Arduino.h>
#include <FS.h>

struct Mp3Info
{
  bool valid;
  uint16_t durationSeconds;
  uint32_t bitrateBps;   // nominal; for VBR this is the average
  uint32_t sampleRate;
  uint32_t audioStart;   // first byte after any ID3v2 tag
  bool variableBitrate;
};

// Reads only the ID3v2 header and the first MPEG frame, so cost is a couple of
// short reads regardless of file size.
Mp3Info mp3Probe(File &file);
