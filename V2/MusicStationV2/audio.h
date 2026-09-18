// Audio output, behind one interface so the rest of the firmware does not know
// whether a decoder exists yet.
//
// With AUDIO_HARDWARE 0 this is a simulated playhead driven by millis(). That
// is deliberate, not a stub: it exercises the queue, the lyric scheduler and
// every screen, so the interface can be finished and tested before the
// MAX98357A is fitted. Setting AUDIO_HARDWARE to 1 swaps in the real decoder
// with no changes anywhere else.
#pragma once

#include <Arduino.h>

#ifndef AUDIO_HARDWARE
#define AUDIO_HARDWARE 0
#endif

constexpr uint8_t AUDIO_VOLUME_MAX = 20;

bool audioBegin();

// durationSeconds is what the simulated backend counts against; the hardware
// backend uses it only for reporting.
bool audioPlay(const String &path, uint16_t durationSeconds);
void audioStop();
void audioPause();
void audioResume();

bool audioIsPlaying();
bool audioIsPaused();
bool audioIsActive(); // playing or paused, i.e. a track is loaded

uint32_t audioPositionMs();
void audioSeekMs(uint32_t positionMs);

// True once, when the current track reached its end on its own.
bool audioTakeTrackEnded();

void audioSetVolume(uint8_t level); // 0..AUDIO_VOLUME_MAX
void audioSetMuted(bool muted);

// Must be called from the main loop. The hardware backend decodes on its own
// task, so this only moves the playhead and reaps end-of-track.
void audioTick();

const char *audioBackendName();
