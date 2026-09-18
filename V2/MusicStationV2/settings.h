// Persistent settings and playback state, in NVS.
//
// Resume matters more on this device than on most: the panel keeps showing the
// last frame while powered off, so coming back to a different track makes the
// display look like it lied.
#pragma once

#include <Arduino.h>

enum RepeatMode : uint8_t
{
  REPEAT_OFF = 0,
  REPEAT_ALL,
  REPEAT_ONE,
};

// How a queue was built, so it can be rebuilt on boot without storing it.
enum QueueSource : uint8_t
{
  QUEUE_NONE = 0,
  QUEUE_ALBUM,
  QUEUE_ARTIST,
  QUEUE_ALL,
  QUEUE_PLAYLIST,
};

struct PersistentState
{
  QueueSource source;
  uint16_t context;      // album or artist index
  String playlistPath;   // for QUEUE_PLAYLIST
  uint16_t position;     // index within the queue
  uint32_t elapsedMs;
  bool shuffle;
  uint32_t shuffleSeed;
  RepeatMode repeat;
  uint8_t volume;        // 0-20
  uint16_t sleepMinutes; // 0 disables
};

void settingsBegin();
PersistentState &settings();
void settingsSave();

// Saving on every tick would wear the flash; the player calls this and it
// writes at most once every few seconds, and always on pause or track change.
void settingsSaveThrottled(bool force = false);
