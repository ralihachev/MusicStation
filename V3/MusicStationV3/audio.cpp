#include "audio.h"

#include <driver/gpio.h>

#include "pins.h"
#include "storage.h"

#if AUDIO_HARDWARE
#include <AudioFileSource.h>
#include <AudioFileSourceBuffer.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>

#include "mp3meta.h"
#endif

namespace
{

uint32_t positionBaseMs = 0;
uint32_t playingSinceMs = 0; // simulated backend only
bool playing = false;
bool active = false;
uint16_t trackDurationSeconds = 0;
bool trackEnded = false;
uint8_t volumeLevel = 12;
bool muted = false;

// Loudness is roughly logarithmic in amplitude, so equal steps in gain sound
// like nothing at the top and a cliff at the bottom. These are the library's
// 2.6 fixed-point gain values (64 = unity) on an approximately quadratic
// curve. Unity is the ceiling: above it the samples clip before the amp does.
const uint8_t VOLUME_GAIN[AUDIO_VOLUME_MAX + 1] = {
    0, 1, 2, 3, 4, 5, 6, 7, 9, 11, 13, 15, 18, 21, 25, 29, 34, 40, 47, 55, 64,
};

#if !AUDIO_HARDWARE

uint32_t simulatedElapsedMs()
{
  uint32_t total = positionBaseMs;
  if (playing)
    total += millis() - playingSinceMs;
  return total;
}

#else

// The decoder reads the card from core 0 while the UI reads index records,
// lyrics and covers from core 1, and the SD library is not thread-safe, so
// every transfer here takes the storage lock.
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

  // Blocking read: the decoder genuinely has nothing to play without it.
  uint32_t read(void *data, uint32_t length) override
  {
    StorageGuard guard;
    if (!guard || !file)
      return 0;
    return (uint32_t)file.read((uint8_t *)data, length);
  }

  // The buffer's opportunistic top-up. If the UI holds the card right now,
  // skip this round rather than wait: the buffer still holds seconds of audio,
  // and stalling here is what would actually starve the speaker.
  uint32_t readNonBlock(void *data, uint32_t length) override
  {
    if (!storageLock(0))
      return 0;
    uint32_t got = file ? (uint32_t)file.read((uint8_t *)data, length) : 0;
    storageUnlock();
    return got;
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

  Mp3Info probe()
  {
    StorageGuard guard;
    Mp3Info info = {};
    if (guard && file)
      info = mp3Probe(file);
    return info;
  }

private:
  File file;
};

// Voicing for the 3525 cavity speakers. A 35 x 25 mm driver reproduces almost
// nothing below ~300 Hz; fed full-range bass, the cone overdrives and smears
// distortion into the midrange, which is what made V3's first sound "heavy".
//
// - High-pass: 4th-order Butterworth, two cascaded biquads. Steep enough that
//   the bass the speaker cannot play is never sent to it.
// - Low-mid dip: 300-600 Hz is where a small sealed cavity rings, and it
//   reads as "boxy" or "muddy" once the deep bass is gone.
// - Presence dip: the cavity resonance makes these speakers shouty around
//   2-4 kHz. A gentle cut there is most of what makes long listening bearable.
// - Headroom: modern masters sit at 0 dBFS, and the amp adds 9 dB on top.
//   Taking a little off keeps volume 20 out of clipping.
//
// Tune these by ear on the actual speaker. Raising AUDIO_HIGHPASS_HZ gives a
// thinner but cleaner and louder-sounding result; lowering it, more body
// until the cone starts to complain.
constexpr float AUDIO_HIGHPASS_HZ = 250.0f;
constexpr float AUDIO_LOWMID_HZ = 450.0f;
constexpr float AUDIO_LOWMID_DB = -3.0f;
constexpr float AUDIO_LOWMID_Q = 1.0f;
constexpr float AUDIO_PRESENCE_HZ = 3000.0f;
constexpr float AUDIO_PRESENCE_DB = -3.0f;
constexpr float AUDIO_PRESENCE_Q = 1.0f;
constexpr float AUDIO_HEADROOM_DB = -3.0f;

// One biquad, transposed direct form II, with state for two channels. Float
// is cheap on the S3's FPU: three of these per sample at 44.1 kHz is a few
// percent of one core.
struct Biquad
{
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  float z1[2] = {0, 0}, z2[2] = {0, 0};

  float process(float x, int ch)
  {
    float y = b0 * x + z1[ch];
    z1[ch] = b1 * x - a1 * y + z2[ch];
    z2[ch] = b2 * x - a2 * y;
    return y;
  }

  void reset()
  {
    z1[0] = z1[1] = z2[0] = z2[1] = 0;
  }

  // RBJ audio-EQ cookbook.
  void highpass(float fs, float f, float q)
  {
    float w = 2.0f * PI * f / fs;
    float alpha = sinf(w) / (2.0f * q);
    float c = cosf(w);
    float a0 = 1.0f + alpha;
    b0 = (1.0f + c) / 2.0f / a0;
    b1 = -(1.0f + c) / a0;
    b2 = b0;
    a1 = -2.0f * c / a0;
    a2 = (1.0f - alpha) / a0;
  }

  void peaking(float fs, float f, float q, float db)
  {
    float A = powf(10.0f, db / 40.0f);
    float w = 2.0f * PI * f / fs;
    float alpha = sinf(w) / (2.0f * q);
    float c = cosf(w);
    float a0 = 1.0f + alpha / A;
    b0 = (1.0f + alpha * A) / a0;
    b1 = -2.0f * c / a0;
    b2 = (1.0f - alpha * A) / a0;
    a1 = b1;
    a2 = (1.0f - alpha / A) / a0;
  }
};

// Counts every stereo frame accepted by the I2S driver. That count, not the
// wall clock, is the playhead: if the card stalls and the decoder falls
// behind, the lyrics wait for the music instead of running ahead of it.
class CountingI2S : public AudioOutputI2S
{
public:
  bool ConsumeSample(int16_t sample[2]) override
  {
    if (hertz != designedRate)
      design(hertz);

    float left = sample[LEFTCHANNEL];
    float right = channels == 1 ? left : (float)sample[RIGHTCHANNEL];
    int16_t shaped[2];
#if AUDIO_STEREO
    shaped[LEFTCHANNEL] = voice(left, 0);
    shaped[RIGHTCHANNEL] = voice(right, 1);
#else
    // Mixed here rather than by the library, so the filters run once.
    shaped[LEFTCHANNEL] = shaped[RIGHTCHANNEL] = voice((left + right) * 0.5f, 0);
#endif

    if (!AudioOutputI2S::ConsumeSample(shaped))
    {
      // The DMA ring was full. The caller retries this same sample, so put
      // the filters back exactly where they were.
      restoreState();
      return false;
    }
    frames = frames + 1; // only this task writes it; the UI core only reads
    return true;
  }

  void resetVoicing()
  {
    for (Biquad &stage : stages)
      stage.reset();
  }

  // Not counted: silence written on pause is not part of the track.
  bool consumeSilence()
  {
    int16_t zero[2] = {0, 0};
    return AudioOutputI2S::ConsumeSample(zero);
  }

  void setGainStep(uint8_t f2p6) { gainF2P6 = f2p6; }
  uint32_t rate() const { return hertz ? hertz : 44100; }
  uint32_t dmaFrames() const { return _buffers * _bufferWords; }
  bool running() const { return i2sOn; }

  volatile uint32_t frames = 0;

private:
  static constexpr int STAGES = 4;
  Biquad stages[STAGES];
  Biquad saved[STAGES];
  uint16_t designedRate = 0;
  float headroom = 1.0f;

  void design(uint16_t rate)
  {
    float fs = rate ? (float)rate : 44100.0f;
    // Butterworth 4th order as two 2nd-order sections with these Qs.
    stages[0].highpass(fs, AUDIO_HIGHPASS_HZ, 0.5412f);
    stages[1].highpass(fs, AUDIO_HIGHPASS_HZ, 1.3066f);
    stages[2].peaking(fs, AUDIO_LOWMID_HZ, AUDIO_LOWMID_Q, AUDIO_LOWMID_DB);
    stages[3].peaking(fs, AUDIO_PRESENCE_HZ, AUDIO_PRESENCE_Q,
                      AUDIO_PRESENCE_DB);
    resetVoicing();
    headroom = powf(10.0f, AUDIO_HEADROOM_DB / 20.0f);
    designedRate = rate;
  }

  int16_t voice(float x, int ch)
  {
    for (int i = 0; i < STAGES; i++)
    {
      saved[i].z1[ch] = stages[i].z1[ch];
      saved[i].z2[ch] = stages[i].z2[ch];
      x = stages[i].process(x, ch);
    }
    x *= headroom;
    if (x > 32767.0f)
      x = 32767.0f;
    else if (x < -32768.0f)
      x = -32768.0f;
    return (int16_t)x;
  }

  void restoreState()
  {
    for (int i = 0; i < STAGES; i++)
      for (int ch = 0; ch < 2; ch++)
      {
        stages[i].z1[ch] = saved[i].z1[ch];
        stages[i].z2[ch] = saved[i].z2[ch];
      }
  }
};

// Buffering compressed data rather than PCM is far cheaper per second of
// cushion. 32 KB is about two seconds at 128 kbps and 0.8 s at 320 kbps. The
// panel never blocks this core, so the cushion only has to cover the UI
// holding the card lock, which it does for milliseconds at a time.
constexpr uint32_t DECODE_BUFFER_BYTES = 32 * 1024;

// Eight DMA buffers of 576 frames, about 100 ms at 44.1 kHz. Deeper than the
// library default so a slow card read on the blocking path does not click.
constexpr int DMA_BUFFER_COUNT = 8;
constexpr int DMA_BUFFER_BYTES = 576 * 4;

// libmad keeps its large state on the heap; the stack only has to cover
// synthesis and the call chain down through the card read.
constexpr uint32_t DECODER_STACK_BYTES = 10240;

// Quiet enough not to startle with the speaker on the bench, loud enough to
// hear across a room at the default gain.
constexpr int16_t TONE_AMPLITUDE = 6000;
constexpr uint32_t TONE_RATE = 44100;
constexpr uint32_t TONE_FADE_FRAMES = TONE_RATE / 100; // 10 ms, no clicks

LockedSdSource *source = nullptr;
AudioFileSourceBuffer *buffered = nullptr;
AudioGeneratorMP3 *decoder = nullptr;
CountingI2S *output = nullptr;
TaskHandle_t decoderTask = nullptr;
String currentPath;

// Everything below is shared with the decoder task and guarded by
// chainMutex. The task holds it for one decoder->loop() at a time, so
// stopping or seeking from the UI core waits at most one DMA refill and can
// never free the chain underneath a running decode.
SemaphoreHandle_t chainMutex = nullptr;
bool decoderRun = false;
bool decoderEnded = false;
uint32_t silenceOwed = 0;
uint32_t toneFramesLeft = 0;
uint32_t toneFramesTotal = 0;
float tonePhase = 0.0f;
float toneStep = 0.0f;
bool toneDraining = false; // tone written, silence flushing it out of DMA
bool toneDone = false;

void lockChain() { xSemaphoreTake(chainMutex, portMAX_DELAY); }
void unlockChain() { xSemaphoreGive(chainMutex); }

void applyGain()
{
  if (output != nullptr)
    output->setGainStep(muted ? 0 : VOLUME_GAIN[volumeLevel]);
  // The MAX98357A's SD pin selects a channel as well as enabling the amp:
  // GPIO high is "left", not "mono mix". The decoder already writes the mono
  // mix to both slots, so the channel is irrelevant and this pin is a clean
  // mute that also stops the amp's idle hiss.
  digitalWrite(AMP_SD, (muted || volumeLevel == 0) ? LOW : HIGH);
}

// Caller holds chainMutex.
void teardownChainLocked()
{
  decoderRun = false;
  silenceOwed = 0;
  if (decoder)
  {
    if (decoder->isRunning())
      decoder->stop(); // also stops I2S and closes the file
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
  if (output && output->running())
    output->stop();
}

// Caller holds chainMutex. Leaves the decoder ready but not running; the
// caller decides whether it plays.
bool openChainLocked(const String &path, uint32_t startMs)
{
  teardownChainLocked();

  source = new LockedSdSource();
  if (!source->open(path.c_str()))
  {
    Serial.printf("[audio] cannot open %s\n", path.c_str());
    teardownChainLocked();
    return false;
  }

  Mp3Info info = source->probe();
  uint32_t offset = mp3SeekOffset(info, startMs,
                                  (uint32_t)trackDurationSeconds * 1000);
  if (!source->seek((int32_t)offset, SEEK_SET))
  {
    Serial.printf("[audio] seek to %u failed\n", (unsigned)offset);
    teardownChainLocked();
    return false;
  }

  buffered = new AudioFileSourceBuffer(source, DECODE_BUFFER_BYTES);
  decoder = new AudioGeneratorMP3();
  output->frames = 0;
  output->resetVoicing();
  decoderEnded = false;
  if (!decoder->begin(buffered, output))
  {
    Serial.printf("[audio] decoder refused %s (free heap %u, largest %u)\n",
                  path.c_str(), (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMaxAllocHeap());
    teardownChainLocked();
    return false;
  }

  Serial.printf("[audio] %s @%lu ms, byte %u, %s, heap %u/%u\n", path.c_str(),
                (unsigned long)startMs, (unsigned)offset,
                info.hasToc ? "VBR toc" : "CBR", (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMaxAllocHeap());
  positionBaseMs = startMs;
  currentPath = path;
  return true;
}

// Caller holds chainMutex.
void pumpToneLocked()
{
  int16_t sample[2];
  while (toneFramesLeft > 0)
  {
    uint32_t done = toneFramesTotal - toneFramesLeft;
    float envelope = 1.0f;
    if (done < TONE_FADE_FRAMES)
      envelope = (float)done / TONE_FADE_FRAMES;
    else if (toneFramesLeft < TONE_FADE_FRAMES)
      envelope = (float)toneFramesLeft / TONE_FADE_FRAMES;
    int16_t value = (int16_t)(sinf(tonePhase) * TONE_AMPLITUDE * envelope);
    sample[0] = sample[1] = value;
    if (!output->ConsumeSample(sample))
      return; // DMA full; come back next tick
    tonePhase += toneStep;
    if (tonePhase > 2.0f * PI)
      tonePhase -= 2.0f * PI;
    toneFramesLeft--;
  }
  // The last 100 ms of the tone, fade included, is still in the DMA ring.
  // Push it out with silence before the channel stops, or the fade is cut
  // and the speaker clicks.
  silenceOwed = output->dmaFrames();
  toneDraining = true;
}

// Pinned to core 0; the Arduino loop and the panel own core 1, so a ~780 ms
// blocking refresh never touches this task.
//
// The I2S write is non-blocking: decoder->loop() fills the DMA ring and
// returns as soon as it is full. Without the delay below this task would spin
// at priority 2 and starve core 0's idle task until the watchdog fired.
void decoderLoop(void *)
{
  for (;;)
  {
    bool busy = false;
    lockChain();
    if (decoderRun && decoder != nullptr && decoder->isRunning())
    {
      busy = true;
      if (!decoder->loop())
      {
        // End of file, or an unrecoverable stream: either way the track is
        // over, and the player decides what comes next.
        if (decoder->isRunning())
          decoder->stop();
        decoderRun = false;
        decoderEnded = true;
      }
    }
    else if (toneFramesLeft > 0)
    {
      busy = true;
      pumpToneLocked();
    }
    else if (silenceOwed > 0 && output->running())
    {
      // With nothing written, the DMA ring keeps replaying its last contents.
      // The amp is already muted, but overwriting it means resume starts
      // clean instead of with a 100 ms echo of where pause happened.
      busy = true;
      while (silenceOwed > 0 && output->consumeSilence())
        silenceOwed--;
      if (silenceOwed == 0 && toneDraining)
      {
        toneDraining = false;
        toneDone = true;
      }
    }
    unlockChain();
    vTaskDelay(busy ? 1 : pdMS_TO_TICKS(10));
  }
}

#endif // AUDIO_HARDWARE

} // namespace

bool audioBegin()
{
  // Muted until a track actually starts. Configured before the hold that
  // sleep left on this pin is released, so the amp never sees it float.
  pinMode(AMP_SD, OUTPUT);
  digitalWrite(AMP_SD, LOW);
  gpio_hold_dis((gpio_num_t)AMP_SD);

#if AUDIO_HARDWARE
  chainMutex = xSemaphoreCreateMutex();
  output = new CountingI2S();
  output->SetPinout(I2S_BCLK, I2S_LRCLK, I2S_DIN);
  output->SetBuffers(DMA_BUFFER_COUNT, DMA_BUFFER_BYTES);
  // CountingI2S does the mono mix itself, ahead of its filters.
  output->SetOutputModeMono(false);
  applyGain();
  if (xTaskCreatePinnedToCore(decoderLoop, "mp3", DECODER_STACK_BYTES, nullptr,
                              2, &decoderTask, 0) != pdPASS)
  {
    Serial.println("[audio] could not start the decoder task");
    return false;
  }
#endif
  return true;
}

bool audioPlay(const String &path, uint16_t durationSeconds, uint32_t startMs,
               bool startPlaying)
{
  audioStop();
  trackDurationSeconds = durationSeconds;
  positionBaseMs = startMs;
  trackEnded = false;

#if AUDIO_HARDWARE
  lockChain();
  bool opened = openChainLocked(path, startMs);
  decoderRun = opened && startPlaying;
  unlockChain();
  if (!opened)
    return false;
#else
  (void)path;
#endif

  active = true;
  playing = startPlaying;
  playingSinceMs = millis();
  if (startPlaying)
    audioSetMuted(false);
  return true;
}

void audioStop()
{
  audioSetMuted(true);
#if AUDIO_HARDWARE
  lockChain();
  toneFramesLeft = 0;
  toneDraining = false;
  toneDone = false;
  teardownChainLocked();
  unlockChain();
  currentPath = String();
#endif
  playing = false;
  active = false;
  positionBaseMs = 0;
  trackDurationSeconds = 0;
}

void audioPause()
{
  if (!playing)
    return;
#if AUDIO_HARDWARE
  audioSetMuted(true);
  lockChain();
  decoderRun = false;
  silenceOwed = output->dmaFrames();
  unlockChain();
#else
  positionBaseMs = simulatedElapsedMs();
  audioSetMuted(true);
#endif
  playing = false;
}

void audioResume()
{
  if (playing || !active)
    return;
#if AUDIO_HARDWARE
  lockChain();
  silenceOwed = 0;
  bool ready = decoder != nullptr && decoder->isRunning();
  // The decoder stops itself at end of file. Resuming a track that already
  // finished, at the end of a queue with repeat off, plays it again from the
  // start rather than sitting "playing" with nothing to decode.
  if (!ready && currentPath.length() > 0)
  {
    String path = currentPath;
    ready = openChainLocked(path, 0);
  }
  decoderRun = ready;
  unlockChain();
  if (!ready)
    return;
#endif
  playingSinceMs = millis();
  playing = true;
  audioSetMuted(false);
}

bool audioIsPlaying() { return playing; }
bool audioIsPaused() { return active && !playing; }
bool audioIsActive() { return active; }

uint32_t audioPositionMs()
{
#if AUDIO_HARDWARE
  if (!active || output == nullptr)
    return positionBaseMs;
  // Frames still sitting in the DMA ring have been counted but not heard.
  uint32_t frames = output->frames;
  uint32_t queued = output->dmaFrames();
  frames = frames > queued ? frames - queued : 0;
  return positionBaseMs + (uint32_t)((uint64_t)frames * 1000 / output->rate());
#else
  return simulatedElapsedMs();
#endif
}

void audioSeekMs(uint32_t positionMs)
{
#if AUDIO_HARDWARE
  if (!active || currentPath.length() == 0)
    return;
  // Seeking restarts the decoder at a byte offset. That is the only reliable
  // seek libmad offers, and it costs one file open, which is nothing next to
  // the panel refresh the seek is about to cause anyway.
  bool resume = playing;
  audioSetMuted(true);
  String path = currentPath;
  lockChain();
  bool opened = openChainLocked(path, positionMs);
  decoderRun = opened && resume;
  unlockChain();
  if (!opened)
  {
    // The file was readable a moment ago; treat this like the track ending
    // so the queue moves on rather than sitting on a silent screen.
    trackEnded = true;
    playing = false;
    return;
  }
  if (resume)
    audioSetMuted(false);
#else
  positionBaseMs = positionMs;
  playingSinceMs = millis();
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
  if (playing || audioToneActive())
    audioSetMuted(false);
}

void audioSetMuted(bool value)
{
  muted = value;
#if AUDIO_HARDWARE
  applyGain();
#else
  digitalWrite(AMP_SD, (muted || volumeLevel == 0) ? LOW : HIGH);
#endif
}

bool audioPlayTestTone(uint16_t hz, uint16_t durationMs)
{
#if AUDIO_HARDWARE
  if (active || hz == 0 || durationMs == 0)
    return false;
  lockChain();
  if (!output->running())
  {
    output->SetRate(TONE_RATE);
    output->begin();
  }
  toneFramesTotal = toneFramesLeft = (uint32_t)TONE_RATE * durationMs / 1000;
  toneStep = 2.0f * PI * (float)hz / (float)TONE_RATE;
  tonePhase = 0.0f;
  toneDraining = false;
  toneDone = false;
  unlockChain();
  // A volume of zero would make the test say nothing; never test below 6.
  if (volumeLevel < 6)
    volumeLevel = 6;
  audioSetMuted(false);
  Serial.printf("[audio] test tone %u Hz for %u ms at volume %u\n", hz,
                durationMs, volumeLevel);
  return true;
#else
  (void)hz;
  (void)durationMs;
  return false;
#endif
}

bool audioToneActive()
{
#if AUDIO_HARDWARE
  return toneFramesLeft > 0 || toneDraining || toneDone;
#else
  return false;
#endif
}

void audioTick()
{
#if AUDIO_HARDWARE
  bool ended = false;
  bool toneFinished = false;
  lockChain();
  if (decoderEnded)
  {
    decoderEnded = false;
    ended = true;
  }
  if (toneDone)
  {
    toneDone = false;
    toneFinished = true;
    if (output->running())
      output->stop();
  }
  unlockChain();

  if (toneFinished)
    audioSetMuted(true);
  if (ended && active)
  {
    trackEnded = true;
    playing = false;
    audioSetMuted(true);
  }
#else
  if (!active)
    return;
  // Simulated: the track ends when the playhead reaches its indexed duration.
  if (playing && trackDurationSeconds > 0 &&
      simulatedElapsedMs() >= (uint32_t)trackDurationSeconds * 1000)
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
#if AUDIO_STEREO
  return "I2S stereo";
#else
  return "I2S mono";
#endif
#else
  return "simulated";
#endif
}
