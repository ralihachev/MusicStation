// Sleep and battery.
//
// E-paper retains its image with no power at all, so a sleeping station still
// shows what it was playing. That makes deep sleep unusually cheap here: there
// is nothing to redraw on wake.
#pragma once

#include <Arduino.h>

void powerBegin();

// Any button edge or playback activity postpones sleep.
void powerNoteActivity();

// Call every loop. Sleeps when paused and idle past the configured timeout.
void powerTick(bool playing);

void powerSleepNow();

// -1 when no divider is fitted or the reading is implausible, so the UI can
// say "unknown" rather than invent a number.
int8_t powerBatteryPercent();
float powerBatteryVolts();
