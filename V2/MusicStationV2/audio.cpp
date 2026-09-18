#include "audio.h"

#include "pins.h"
#include "storage.h"

#if AUDIO_HARDWARE
#include <AudioFileSource.h>
#include <AudioFileSourceBuffer.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
#endif

namespace
{

// Playback position is wall-clock while playing. That is exactly right for a
// real-time player, it is identical for both backends, and it avoids asking
// the decoder for a sample count it does not cheaply track.
uint32_t positionBaseMs = 0;
uint32_t playingSinceMs = 0;
bool playing = false;
bool active = false;
uint16_t trackDurationSeconds = 0;
bool trackEnded = false;
uint8_t volumeLevel = 12;
bool muted = false;

uint32_t elapsedMs()
{
  uint32_t total = positionBaseMs;
  if (playing)
    total += millis() - playingSinceMs;
  return total;
}

#if AUDIO_HARDWARE

// The decoder reads the card from its own core while the UI reads index
// records from the other, and the Arduino SD library is not thread-safe, so
// every read here takes the storage lock.
class LockedSdSource : public AudioFileSource
{
public:
  bool open(const char *filename) override
  {
    StorageGuard guard;
    if (!guard)
      return false;
    file = CARD.open(filename, FILE_READ);
    return (bool)file;
  }

  uint32_t read(void *data, uint32_t length) override
  {
    StorageGuard guard;
    if (!guard || !file)
      return 0;
    return (uint32_t)file.read((uint8_t *)data, length);
  }

  bool seek(int32_t position, int direction) override
  {
    StorageGuard guard;
    if (!guard || !file)
      return false;
    uint32_t target = direction == SEEK_SET   ? (uint32_t)position
                      : direction == SEEK_CUR ? file.position() + position
                                              : file.size() + position;
    return file.seek(target);
  }

  bool close() override
  {
    StorageGuard guard;
    if (file)
      file.close();
    return true;
  }

  bool isOpen() override { return (bool)file; }
  uint32_t getSize() override { return file ? file.size() : 0; }
  uint32_t getPos() override { return file ? file.position() : 0; }

private:
  File file;
};

// Buffering compressed data rather than PCM is far cheaper per second of
// cushion. 32 KB is roughly two seconds at 128 kbps, which comfortably rides
// out the panel's ~780 ms blocking waveform.
constexpr uint32_t DECODE_BUFFER_BYTES = 32 * 1024;

LockedSdSource *source = nullptr;
AudioFileSourceBuffer *buffered = nullptr;
AudioGeneratorMP3 *decoder = nullptr;
AudioOutputI2S *output = nullptr;
TaskHandle_t decoderTask = nullptr;
volatile bool decoderShouldRun = false;
volatile bool decoderReportedEnd = false;
portMUX_TYPE audioMux = portMUX_INITIALIZER_UNLOCKED;

void applyGain()
{
  if (output == nullptr)
    return;
  float gain = muted ? 0.0f : (float)volumeLevel / (float)AUDIO_VOLUME_MAX;
  output->SetGain(gain);
  // The MAX98357A's SD pin selects the channel as well as enabling the amp, so
  // a GPIO high means "left channel", not "mono mix". Output is mixed to mono
  // in software instead, which makes the channel irrelevant and leaves this
  // pin as a clean mute for seeks and track changes.
  digitalWrite(AMP_SD, (muted || volumeLevel == 0) ? LOW : HIGH);
}

void teardownChain()
{
  if (decoder)
  {
    if (decoder->isRunning())
      decoder->stop();
    delete decoder;
    decoder = nullptr;
  }
  if (buffered)
  {
    delete buffered;
    buffered = nullptr;
  }
  if (source)
  {
    delete source;
    source = nullptr;
  }
}

// Pinned to core 0; the Arduino loop and the panel own core 1. A ~780 ms
// refresh on the UI core must never starve this one.
void decoderLoop(void *)
{
  for (;;)
  {
    if (decoderShouldRun && decoder != nullptr && decoder->isRunning())
    {
      if (!decoder->loop())
      {
        decoder->stop();
        portENTER_CRITICAL(&audioMux);
        decoderReportedEnd = true;
        decoderShouldRun = false;
        portEXIT_CRITICAL(&audioMux);
      }
    }
    else
    {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
}

#endif // AUDIO_HARDWARE

} // namespace

bool audioBegin()
{
  pinMode(AMP_SD, OUTPUT);
  digitalWrite(AMP_SD, LOW); // stay muted until a track actually starts

#if AUDIO_HARDWARE
  output = new AudioOutputI2S();
  output->SetPinout(I2S_BCLK, I2S_LRCLK, I2S_DIN);
  output->SetOutputModeMono(true);
  applyGain();
  xTaskCreatePinnedToCore(decoderLoop, "mp3", 8192, nullptr, 2, &decoderTask,
                          0);
#endif
  return true;
}

bool audioPlay(const String &path, uint16_t durationSeconds)
{
  audioStop();
  trackDurationSeconds = durationSeconds;
  positionBaseMs = 0;
  trackEnded = false;

#if AUDIO_HARDWARE
  source = new LockedSdSource();
  if (!source->open(path.c_str()))
  {
    teardownChain();
    return false;
  }
  buffered = new AudioFileSourceBuffer(source, DECODE_BUFFER_BYTES);
  decoder = new AudioGeneratorMP3();
  decoderReportedEnd = false;
  if (!decoder->begin(buffered, output))
  {
    teardownChain();
    return false;
  }
  decoderShouldRun = true;
#else
  (void)path;
#endif

  playing = true;
  active = true;
  playingSinceMs = millis();
  audioSetMuted(false);
  return true;
}

void audioStop()
{
#if AUDIO_HARDWARE
  decoderShouldRun = false;
  // Let the decoder task finish the frame it is inside before the chain under
  // it is deleted.
  vTaskDelay(pdMS_TO_TICKS(20));
  teardownChain();
#endif
  playing = false;
  active = false;
  positionBaseMs = 0;
  trackDurationSeconds = 0;
  audioSetMuted(true);
}

void audioPause()
{
  if (!playing)
    return;
  positionBaseMs = elapsedMs();
  playing = false;
#if AUDIO_HARDWARE
  decoderShouldRun = false;
#endif
  audioSetMuted(true);
}

void audioResume()
{
  if (playing || !active)
    return;
  playingSinceMs = millis();
  playing = true;
#if AUDIO_HARDWARE
  decoderShouldRun = true;
#endif
  audioSetMuted(false);
}

bool audioIsPlaying() { return playing; }
bool audioIsPaused() { return active && !playing; }
bool audioIsActive() { return active; }
uint32_t audioPositionMs() { return elapsedMs(); }

void audioSeekMs(uint32_t positionMs)
{
  positionBaseMs = positionMs;
  playingSinceMs = millis();
#if AUDIO_HARDWARE
  // Frame-accurate seeking needs either a Xing table or a scan, so it is left
  // to a later milestone. Resume still lands on the right track, at its start.
#endif
}

bool audioTakeTrackEnded()
{
  bool ended = trackEnded;
  trackEnded = false;
  return ended;
}

void audioSetVolume(uint8_t level)
{
  volumeLevel = level > AUDIO_VOLUME_MAX ? AUDIO_VOLUME_MAX : level;
#if AUDIO_HARDWARE
  applyGain();
#endif
}

void audioSetMuted(bool value)
{
  muted = value;
#if AUDIO_HARDWARE
  applyGain();
#else
  digitalWrite(AMP_SD, muted ? LOW : HIGH);
#endif
}

void audioTick()
{
  if (!active)
    return;

#if AUDIO_HARDWARE
  bool ended = false;
  portENTER_CRITICAL(&audioMux);
  if (decoderReportedEnd)
  {
    decoderReportedEnd = false;
    ended = true;
  }
  portEXIT_CRITICAL(&audioMux);
  if (ended)
  {
    trackEnded = true;
    playing = false;
  }
#else
  // Simulated: the track ends when the playhead reaches its indexed duration.
  if (playing && trackDurationSeconds > 0 &&
      elapsedMs() >= (uint32_t)trackDurationSeconds * 1000)
  {
    trackEnded = true;
    playing = false;
    positionBaseMs = (uint32_t)trackDurationSeconds * 1000;
  }
#endif
}

const char *audioBackendName()
{
#if AUDIO_HARDWARE
  return "I2S MAX98357A";
#else
  return "simulated";
#endif
}
