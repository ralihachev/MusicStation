// Playback model: one queue, one position, and everything that follows from
// them.
//
// A queue is the single playback primitive. Playing an album, an artist, a
// playlist or one track all mean "replace the queue and start at index N", so
// there is one code path for advance, shuffle, repeat and resume rather than
// four.
#pragma once

#include <Arduino.h>

#include "settings.h"

struct NowTrack
{
  bool valid;
  String path;
  String title;
  String artist;
  String album;
  uint16_t durationSeconds;
  uint16_t trackNumber;
  int32_t libraryIndex; // -1 when the entry came from a playlist
};

bool playerBegin();

// Rebuilds the queue recorded in NVS and re-selects its track, paused. The
// panel keeps showing the last frame while powered off, so returning to a
// different track would make the display look like it lied.
bool playerRestore();

bool playerPlayAlbum(uint16_t albumIndex, uint16_t startRow);
bool playerPlayArtist(uint16_t artistIndex, uint16_t startRow);
bool playerPlayAll(uint16_t startIndex);
bool playerPlayPlaylist(const String &path, uint16_t startRow);

void playerToggle();
void playerNext();
void playerPrevious();
void playerStop();

void playerSetShuffle(bool shuffle);
void playerCycleRepeat();
void playerSetVolume(uint8_t level);

bool playerHasTrack();
const NowTrack &playerTrack();
bool playerIsPlaying();
uint32_t playerPositionMs();
uint16_t playerQueueSize();
uint16_t playerQueuePosition();

// Index of the lyric line that should be on screen now, already including the
// waveform lead. -1 before the first line.
int16_t playerLyricIndex();

// Must run every loop. Moves the playhead, advances at end of track, and
// persists state.
void playerTick();

// One-shot flags for the renderer, so it can pick a refresh region rather than
// redrawing the whole screen for a lyric change.
bool playerTakeTrackChanged();
bool playerTakeLyricChanged();
bool playerTakeTransportChanged();
