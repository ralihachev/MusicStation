// Audio output, behind one interface so the rest of the firmware does not know
// whether a decoder exists.
//
// V3 defaults to the real backend: MP3 decoded on core 0 and written over I2S
// to one or two MAX98357A amplifiers. Building with AUDIO_HARDWARE 0 brings
// back V2's simulated playhead, driven by millis(), for working on screens
// without the amplifier attached.
#pragma once

#include <Arduino.h>

#ifndef AUDIO_HARDWARE
#define AUDIO_HARDWARE 1
#endif

// 0: the decoder mixes to mono and writes it to both I2S slots, so every
//    amplifier plays the same signal whichever channel its SD pin selects.
//    One amp, or two amps wired in parallel for two speakers. Recommended.
// 1: true stereo. Needs the second amplifier's SD pin biased into its
//    right-channel window with a resistor; see V3/WIRING.md before enabling.
#ifndef AUDIO_STEREO
#define AUDIO_STEREO 0
#endif

constexpr uint8_t AUDIO_VOLUME_MAX = 20;

bool audioBegin();

// Loads `path` at `startMs`, playing or paused. durationSeconds is what the
// simulated backend counts against and what a VBR seek is scaled by. Loading
// paused never unmutes the amp, so restoring a track at boot is silent.
bool audioPlay(const String &path, uint16_t durationSeconds,
               uint32_t startMs = 0, bool startPlaying = true);
void audioStop();
void audioPause();
void audioResume();

bool audioIsPlaying();
bool audioIsPaused();
bool audioIsActive(); // playing or paused, i.e. a track is loaded

// The hardware backend counts samples actually handed to I2S, so the
// position follows what the speaker is playing even if the card stalls.
uint32_t audioPositionMs();
void audioSeekMs(uint32_t positionMs);

// True once, when the current track reached its end on its own.
bool audioTakeTrackEnded();

void audioSetVolume(uint8_t level); // 0..AUDIO_VOLUME_MAX
void audioSetMuted(bool muted);

// A short sine through the amplifier with no card involved, for the bring-up
// self-test. Refused while a track is loaded. False on the simulated backend.
bool audioPlayTestTone(uint16_t hz, uint16_t durationMs);
bool audioToneActive();

// Must be called from the main loop. The hardware backend decodes on its own
// task, so this only reaps end-of-track and finishes the test tone.
void audioTick();

const char *audioBackendName();
