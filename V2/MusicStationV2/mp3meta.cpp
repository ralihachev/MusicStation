#include "mp3meta.h"

namespace
{

// Layer III only; V2 does not index other layers.
const uint16_t BITRATE_V1_L3[16] = {0,   32,  40,  48,  56,  64,  80,  96,
                                    112, 128, 160, 192, 224, 256, 320, 0};
const uint16_t BITRATE_V2_L3[16] = {0,  8,  16, 24, 32,  40,  48,  56,
                                    64, 80, 96, 112, 128, 144, 160, 0};
const uint32_t SAMPLE_RATE_V1[4] = {44100, 48000, 32000, 0};
const uint32_t SAMPLE_RATE_V2[4] = {22050, 24000, 16000, 0};
const uint32_t SAMPLE_RATE_V25[4] = {11025, 12000, 8000, 0};

// An ID3v2 size is four bytes with the top bit of each cleared, so a size can
// never contain a false frame sync.
uint32_t syncSafe(const uint8_t *bytes)
{
  return ((uint32_t)(bytes[0] & 0x7F) << 21) |
         ((uint32_t)(bytes[1] & 0x7F) << 14) |
         ((uint32_t)(bytes[2] & 0x7F) << 7) | (uint32_t)(bytes[3] & 0x7F);
}

uint32_t skipId3v2(File &file)
{
  uint8_t header[10];
  if (!file.seek(0) || file.read(header, 10) != 10)
    return 0;
  if (header[0] != 'I' || header[1] != 'D' || header[2] != '3')
    return 0;
  uint32_t size = 10 + syncSafe(header + 6);
  if (header[5] & 0x10)
    size += 10; // footer present
  return size;
}

struct FrameHeader
{
  bool valid;
  uint8_t versionId; // 0 = MPEG2.5, 2 = MPEG2, 3 = MPEG1
  bool mono;
  uint32_t bitrateBps;
  uint32_t sampleRate;
  uint16_t samplesPerFrame;
  uint32_t frameBytes;
};

FrameHeader parseFrameHeader(const uint8_t *b)
{
  FrameHeader frame = {};
  if (b[0] != 0xFF || (b[1] & 0xE0) != 0xE0)
    return frame;

  uint8_t versionId = (b[1] >> 3) & 0x03;
  uint8_t layer = (b[1] >> 1) & 0x03;
  if (versionId == 1 || layer != 0x01) // reserved version, or not Layer III
    return frame;

  uint8_t bitrateIndex = (b[2] >> 4) & 0x0F;
  uint8_t sampleIndex = (b[2] >> 2) & 0x03;
  uint8_t padding = (b[2] >> 1) & 0x01;
  uint8_t channelMode = (b[3] >> 6) & 0x03;
  if (bitrateIndex == 0 || bitrateIndex == 15 || sampleIndex == 3)
    return frame; // free-format or reserved

  bool version1 = versionId == 3;
  uint16_t bitrateKbps =
      version1 ? BITRATE_V1_L3[bitrateIndex] : BITRATE_V2_L3[bitrateIndex];
  uint32_t sampleRate = versionId == 3   ? SAMPLE_RATE_V1[sampleIndex]
                        : versionId == 2 ? SAMPLE_RATE_V2[sampleIndex]
                                         : SAMPLE_RATE_V25[sampleIndex];
  if (bitrateKbps == 0 || sampleRate == 0)
    return frame;

  frame.valid = true;
  frame.versionId = versionId;
  frame.mono = channelMode == 3;
  frame.bitrateBps = (uint32_t)bitrateKbps * 1000;
  frame.sampleRate = sampleRate;
  frame.samplesPerFrame = version1 ? 1152 : 576;
  frame.frameBytes =
      (frame.samplesPerFrame / 8) * frame.bitrateBps / sampleRate + padding;
  return frame;
}

// Xing (VBR) and Info (CBR) headers sit in the first frame's side-information
// area, at an offset that depends on version and channel count.
uint16_t xingOffset(const FrameHeader &frame)
{
  if (frame.versionId == 3)
    return frame.mono ? 17 : 32;
  return frame.mono ? 9 : 17;
}

} // namespace

Mp3Info mp3Probe(File &file)
{
  Mp3Info info = {};
  uint32_t fileSize = file.size();
  uint32_t audioStart = skipId3v2(file);
  if (audioStart >= fileSize)
  {
    Serial.printf("[mp3] bail: size=%u id3=%u\n", (unsigned)fileSize,
                  (unsigned)audioStart);
    return info;
  }
  info.audioStart = audioStart;

  // The first frame usually begins immediately, but padding and stray bytes
  // are common, so scan a window rather than trusting the offset.
  // Static rather than stack: this runs inside libraryBuild's call chain, which
  // is already deep, and only ever from the one task.
  constexpr size_t WINDOW = 1024;
  static uint8_t window[WINDOW];
  if (!file.seek(audioStart))
  {
    Serial.printf("[mp3] bail: seek to %u failed\n", (unsigned)audioStart);
    return info;
  }
  int read = (int)file.read(window, WINDOW);
  if (read < 40)
  {
    Serial.printf("[mp3] bail: size=%u id3=%u read=%d\n", (unsigned)fileSize,
                  (unsigned)audioStart, read);
    return info;
  }

  FrameHeader frame = {};
  size_t frameAt = 0;
  for (size_t i = 0; i + 4 <= (size_t)read; i++)
  {
    frame = parseFrameHeader(window + i);
    if (frame.valid)
    {
      frameAt = i;
      break;
    }
  }
  if (!frame.valid)
  {
    Serial.printf("[mp3] bail: no frame sync in %d bytes, first=%02X %02X %02X %02X\n",
                  read, window[0], window[1], window[2], window[3]);
    return info;
  }

  info.valid = true;
  info.sampleRate = frame.sampleRate;
  info.bitrateBps = frame.bitrateBps;
  uint32_t audioBytes = fileSize - (audioStart + frameAt);

  // Prefer the Xing frame count: it is exact, and for VBR the first frame's
  // bitrate says nothing useful about the rest of the file.
  size_t xingAt = frameAt + 4 + xingOffset(frame);
  if (xingAt + 12 <= (size_t)read)
  {
    const uint8_t *tag = window + xingAt;
    bool isXing = memcmp(tag, "Xing", 4) == 0;
    bool isInfo = memcmp(tag, "Info", 4) == 0;
    if (isXing || isInfo)
    {
      info.variableBitrate = isXing;
      uint32_t flags = ((uint32_t)tag[4] << 24) | ((uint32_t)tag[5] << 16) |
                       ((uint32_t)tag[6] << 8) | tag[7];
      if (flags & 0x0001)
      {
        uint32_t frames = ((uint32_t)tag[8] << 24) | ((uint32_t)tag[9] << 16) |
                          ((uint32_t)tag[10] << 8) | tag[11];
        uint64_t samples = (uint64_t)frames * frame.samplesPerFrame;
        info.durationSeconds =
            (uint16_t)((samples + frame.sampleRate / 2) / frame.sampleRate);
        if (info.durationSeconds > 0)
          info.bitrateBps = (uint32_t)((uint64_t)audioBytes * 8 /
                                       info.durationSeconds);
        return info;
      }
    }
  }

  // Constant bitrate fallback.
  info.durationSeconds =
      (uint16_t)((uint64_t)audioBytes * 8 / frame.bitrateBps);
  return info;
}
