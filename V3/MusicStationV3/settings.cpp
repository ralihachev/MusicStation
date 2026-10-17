#include "settings.h"

#include <Preferences.h>

#include "lyrics.h"

namespace
{
Preferences prefs;
PersistentState state;
uint32_t lastSaveAt = 0;
constexpr uint32_t SAVE_INTERVAL_MS = 15000;
} // namespace

void settingsBegin()
{
  prefs.begin("station", false);
  state.source = (QueueSource)prefs.getUChar("src", QUEUE_NONE);
  state.context = prefs.getUShort("ctx", 0);
  state.playlistPath = prefs.getString("pl", "");
  state.position = prefs.getUShort("pos", 0);
  state.elapsedMs = prefs.getUInt("ms", 0);
  state.shuffle = prefs.getBool("shuf", false);
  state.shuffleSeed = prefs.getUInt("seed", 1);
  state.repeat = (RepeatMode)prefs.getUChar("rep", REPEAT_OFF);
  state.volume = prefs.getUChar("vol", 12);
  state.sleepMinutes = prefs.getUShort("sleep", 10);
  state.lyricOffsetMs = prefs.getShort("lyoff", LYRIC_OFFSET_MS);
}

PersistentState &settings() { return state; }

void settingsSave()
{
  prefs.putUChar("src", (uint8_t)state.source);
  prefs.putUShort("ctx", state.context);
  prefs.putString("pl", state.playlistPath);
  prefs.putUShort("pos", state.position);
  prefs.putUInt("ms", state.elapsedMs);
  prefs.putBool("shuf", state.shuffle);
  prefs.putUInt("seed", state.shuffleSeed);
  prefs.putUChar("rep", (uint8_t)state.repeat);
  prefs.putUChar("vol", state.volume);
  prefs.putUShort("sleep", state.sleepMinutes);
  prefs.putShort("lyoff", state.lyricOffsetMs);
  lastSaveAt = millis();
}

void settingsSaveThrottled(bool force)
{
  if (!force && millis() - lastSaveAt < SAVE_INTERVAL_MS)
    return;
  settingsSave();
}
