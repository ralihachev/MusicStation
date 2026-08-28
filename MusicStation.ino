// MusicStation — ESP32-S3 + WeAct Studio 2.9" black/white e-paper.
// Displays iPhone/iPad now-playing information through Apple Media Service.
// Target panel: WeAct DEPG0290BS, 296x128, SSD1680.

#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <JPEGDEC.h>
#include <atomic>
#include <new>
#include <utility>
#include <vector>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEClient.h>
#include <BLEHIDDevice.h>
#include <BLESecurity.h>
#include <esp_gap_ble_api.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include "config.h"

// ESP32-S3 WROOM-1U -> WeAct e-paper (4-wire SPI). MISO is unused.
#define EPD_SCK 12
#define EPD_MOSI 11
#define EPD_CS 10
#define EPD_DC 9
#define EPD_RST 8
#define EPD_BUSY 7

#define DEVICE_NAME "MusicStation"
constexpr char HTTP_USER_AGENT[] =
    "MusicStation/1.0 (https://github.com/ralihachev/MusicStation)";
// Begin drawing the next lyric before its timestamp so the ~780 ms e-paper
// partial waveform finishes when the line is actually sung.
#define LYRIC_LEAD_MS 800
#define AMS_SERVICE_UUID "89D3502B-0F36-433A-8EF4-C502AD55F8DC"
#define AMS_ENTITY_UPDATE_UUID "2F7CABCE-808D-411F-9A0C-BB92BA96C102"
#define HID_SERVICE_UUID 0x1812
#define HID_GENERIC_APPEARANCE 0x03C0

#define ENTITY_PLAYER 0
#define ENTITY_TRACK 2
#define PLAYER_ATTR_PLAYBACK_INFO 1
#define TRACK_ATTR_ARTIST 0
#define TRACK_ATTR_ALBUM 1
#define TRACK_ATTR_TITLE 2
#define TRACK_ATTR_DURATION 3

// Serial-only profiling. Set to 0 for a release build after the optimization
// work is complete. These diagnostics do not alter display or network policy.
#ifndef MUSICSTATION_PROFILE
#define MUSICSTATION_PROFILE 1
#endif

#if MUSICSTATION_PROFILE
struct PerformanceSnapshot
{
  size_t free8Bit;
  size_t largest8Bit;
  size_t freeInternal;
  size_t largestInternal;
  size_t minimumInternal;
  size_t loopStackFree;
};

PerformanceSnapshot capturePerformance()
{
  constexpr uint32_t internal8Bit = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  PerformanceSnapshot snapshot;
  snapshot.free8Bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  snapshot.largest8Bit = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  snapshot.freeInternal = heap_caps_get_free_size(internal8Bit);
  snapshot.largestInternal = heap_caps_get_largest_free_block(internal8Bit);
  snapshot.minimumInternal = heap_caps_get_minimum_free_size(internal8Bit);
  snapshot.loopStackFree =
      (size_t)uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);
  return snapshot;
}

void logPerformance(const char *stage)
{
  PerformanceSnapshot snapshot = capturePerformance();
  Serial.printf(
      "[perf] %s | free8=%u largest8=%u internal=%u largestInt=%u "
      "minInt=%u loopStack=%u\n",
      stage, (unsigned)snapshot.free8Bit, (unsigned)snapshot.largest8Bit,
      (unsigned)snapshot.freeInternal, (unsigned)snapshot.largestInternal,
      (unsigned)snapshot.minimumInternal, (unsigned)snapshot.loopStackFree);
}

class PerformanceScope
{
public:
  explicit PerformanceScope(const char *name)
      : label(name), startedAt(millis()), before(capturePerformance()) {}

  ~PerformanceScope()
  {
    PerformanceSnapshot after = capturePerformance();
    Serial.printf(
        "[perf] %s | %lu ms free8 %u->%u (%ld) largest8 %u->%u (%ld) "
        "internal %u->%u (%ld) loopStack=%u\n",
        label, (unsigned long)(millis() - startedAt),
        (unsigned)before.free8Bit, (unsigned)after.free8Bit,
        (long)after.free8Bit - (long)before.free8Bit,
        (unsigned)before.largest8Bit, (unsigned)after.largest8Bit,
        (long)after.largest8Bit - (long)before.largest8Bit,
        (unsigned)before.freeInternal, (unsigned)after.freeInternal,
        (long)after.freeInternal - (long)before.freeInternal,
        (unsigned)after.loopStackFree);
  }

private:
  const char *label;
  unsigned long startedAt;
  PerformanceSnapshot before;
};

#define PERF_SCOPE(label) PerformanceScope performanceScope(label)
#define PERF_LOG(stage) logPerformance(stage)
#else
#define PERF_SCOPE(label) ((void)0)
#define PERF_LOG(stage) ((void)0)
#endif

// GxEPD2 normally sets SSD1680 Border Waveform Control (0x3C) to 0x05,
// making the physical border follow a LUT transition on every partial update.
// Expose the protected command helpers so live updates can leave VBD in HiZ.
class QuietBorder290BS : public GxEPD2_290_BS
{
public:
  using GxEPD2_290_BS::GxEPD2_290_BS;

  void setQuietBorder(bool quiet)
  {
    _writeCommand(0x3C);
    _writeData(quiet ? 0xC0 : 0x05);
  }
};

GxEPD2_BW<QuietBorder290BS, QuietBorder290BS::HEIGHT> display(
    QuietBorder290BS(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));
U8G2_FOR_ADAFRUIT_GFX unicodeText;

// Dynamic metadata arrives as UTF-8. These compact fonts include printable
// ASCII and Cyrillic, unlike Adafruit GFX's bundled Latin-only FreeSans fonts.
const uint8_t *const TITLE_FONT = u8g2_font_9x15_t_cyrillic;
const uint8_t *const SMALL_FONT = u8g2_font_5x8_t_cyrillic;
const uint8_t *const LYRIC_FONT = u8g2_font_unifont_t_cyrillic;

struct NowPlaying
{
  String artist;
  String title;
  String album;
  float duration = 0;
  float elapsed = 0;
  float rate = 0;
  int state = 0;
  unsigned long elapsedAtMs = 0;
  bool haveTrack = false;
};

NowPlaying np;
BLEServer *server = nullptr;
BLEClient *client = nullptr;
BLEHIDDevice *hidDevice = nullptr;
BLECharacteristic *mediaInput = nullptr;
BLEAddress phoneAddress("00:00:00:00:00:00");

// A real HID Consumer Control report makes MusicStation a system-supported
// BLE accessory instead of a connection owned by LightBlue. Reports remain
// released in this display-only build; the three controls can be used later.
static const uint8_t mediaReportMap[] = {
    0x05, 0x0C, // Usage Page (Consumer)
    0x09, 0x01, // Usage (Consumer Control)
    0xA1, 0x01, // Collection (Application)
    0x85, 0x01, //   Report ID (1)
    0x15, 0x00, //   Logical Minimum (0)
    0x25, 0x01, //   Logical Maximum (1)
    0x75, 0x01, //   Report Size (1)
    0x95, 0x03, //   Report Count (3)
    0x09, 0xCD, //   Usage (Play/Pause)
    0x09, 0xB5, //   Usage (Scan Next Track)
    0x09, 0xB6, //   Usage (Scan Previous Track)
    0x81, 0x02, //   Input (Data, Variable, Absolute)
    0x75, 0x05, //   Report Size (5)
    0x95, 0x01, //   Report Count (1)
    0x81, 0x03, //   Input (Constant, Variable, Absolute)
    0xC0        // End Collection
};

std::atomic<bool> phoneConnected{false};
std::atomic<bool> authComplete{false};
std::atomic<unsigned long> authenticatedAt{0};
std::atomic<uint32_t> pairingPasskey{0};
std::atomic<bool> screenDirty{true};
bool transportDirty = true;
bool pauseRefreshPending = false;
// E-paper retains its pixels without power. After a disconnect, leave the
// last now-playing page exactly as it was until AMS supplies fresh metadata.
bool displayHeld = false;
bool playbackInfoKnown = false;
std::atomic<bool> failedBondPending{false};
esp_bd_addr_t failedBondAddress = {0};

bool wasConnected = false;
bool amsReady = false;
bool displayReady = false;
unsigned long lastRefreshStartedAt = 0;
uint16_t partialRefreshCount = 0;

// The DEPG0290BS fast differential waveform takes about 735-750 ms. Starting
// a new update every 800 ms gives the controller a short idle gap while still
// keeping playback changes visibly sub-second.
constexpr unsigned long PARTIAL_REFRESH_INTERVAL_MS = 800;
constexpr uint16_t FULL_REFRESH_AFTER_PARTIALS = 300;

// The cover uses almost the full height below the metadata. Transport updates
// live only in the bottom-right half, so they never touch this 96px square.
constexpr int COVER_X = 4;
constexpr int COVER_Y = 31;
constexpr int COVER_SIZE = 96;
uint8_t coverGray[COVER_SIZE * COVER_SIZE];
uint8_t coverBitmap[COVER_SIZE * COVER_SIZE / 8];
uint8_t *coverDecodeTarget = nullptr;
bool coverValid = false;
uint32_t coverForGeneration = 0;
String coverStatus;
bool wifiStarted = false;

// Right column: 296 - 4 left - 96 cover - 4 gap - 8 right = 184px.
// X and width are byte-aligned for exact SSD1680 partial windows.
constexpr int RIGHT_X = 104;
constexpr int RIGHT_W = 184;

struct LyricLine
{
  uint32_t ms;
  uint32_t textOffset;
  uint32_t textLength;
};
std::vector<LyricLine> lyrics;
String lyricTextArena;
bool lyricStorageAllocationFailed = false;
uint32_t lyricsForGeneration = 0;
String lyricStatus;
bool lyricsDirty = false;
int lyricCursor = -1;
uint32_t lyricCursorPlaybackMs = 0;
bool lyricCursorValid = false;
uint32_t appliedTrackGeneration = 0;
uint64_t activeTrackKey = 0;
unsigned long trackMetadataChangedAt = 0;

void clearLyrics()
{
  lyrics.clear();
  lyricTextArena.remove(0);
  lyricStorageAllocationFailed = false;
  lyricCursor = -1;
  lyricCursorPlaybackMs = 0;
  lyricCursorValid = false;
}

// Return a previous track's retained lyric capacity before the next artwork
// decode. JPEGDEC needs one relatively large contiguous allocation, so keeping
// an obsolete lyric arena here can turn otherwise healthy free heap into an
// allocation failure. Same-track lookup fallbacks continue to use clearLyrics()
// and reuse their storage.
void releaseLyricStorage()
{
  std::vector<LyricLine>().swap(lyrics);
  String releasedArena(std::move(lyricTextArena));
  lyricStorageAllocationFailed = false;
  lyricCursor = -1;
  lyricCursorPlaybackMs = 0;
  lyricCursorValid = false;
}

bool appendLyricLine(uint32_t ms, const char *text, size_t length)
{
  while (length && isspace((unsigned char)*text))
  {
    text++;
    length--;
  }
  while (length && isspace((unsigned char)text[length - 1]))
    length--;
  if (!length)
    return false;
  if (lyrics.capacity() == 0)
    lyrics.reserve(64);
  LyricLine line;
  line.ms = ms;
  line.textOffset = lyricTextArena.length();
  line.textLength = length;
  if (!lyricTextArena.concat(text, length))
  {
    lyricStorageAllocationFailed = true;
    return false;
  }
  lyrics.push_back(line);
  return true;
}

String lyricTextAt(size_t index)
{
  const LyricLine &line = lyrics[index];
  return lyricTextArena.substring(line.textOffset,
                                  line.textOffset + line.textLength);
}

// BLE notifications run outside the Arduino loop task. Keep callback work
// bounded and allocation-free: callbacks only update this fixed-size inbox,
// and loop() remains the sole owner of NowPlaying and all Arduino Strings.
constexpr size_t AMS_TEXT_CAPACITY = 192;
enum AmsDirty : uint8_t
{
  AMS_DIRTY_PLAYBACK = 1 << 0,
  AMS_DIRTY_ARTIST = 1 << 1,
  AMS_DIRTY_ALBUM = 1 << 2,
  AMS_DIRTY_TITLE = 1 << 3,
  AMS_DIRTY_DURATION = 1 << 4
};

struct AmsInbox
{
  char artist[AMS_TEXT_CAPACITY];
  char album[AMS_TEXT_CAPACITY];
  char title[AMS_TEXT_CAPACITY];
  float duration;
  float elapsed;
  float rate;
  int state;
  unsigned long elapsedAtMs;
  uint32_t trackGeneration;
  uint8_t dirty;
};

AmsInbox amsInbox = {};
uint32_t publishedTrackGeneration = 0;
portMUX_TYPE amsInboxMux = portMUX_INITIALIZER_UNLOCKED;

class ServerCallbacks : public BLEServerCallbacks
{
  void onConnect(BLEServer *, esp_ble_gatts_cb_param_t *param) override
  {
    phoneAddress = BLEAddress(param->connect.remote_bda);
    phoneConnected = true;
  }

  void onDisconnect(BLEServer *, esp_ble_gatts_cb_param_t *) override
  {
    phoneConnected = false;
    authComplete = false;
    authenticatedAt = 0;
    pairingPasskey = 0;
  }
};

class SecurityCallbacks : public BLESecurityCallbacks
{
  uint32_t onPassKeyRequest() override { return 123456; }

  void onPassKeyNotify(uint32_t key) override
  {
    pairingPasskey = key;
    screenDirty = true;
  }

  bool onSecurityRequest() override { return true; }
  bool onConfirmPIN(uint32_t) override { return true; }

  void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) override
  {
    pairingPasskey = 0;
    screenDirty = true;
    if (cmpl.success)
    {
      authenticatedAt = millis();
      // Publish completion last so loop() cannot observe a successful auth
      // with the previous timestamp.
      authComplete = true;
    }
    else
    {
      authComplete = false;
      authenticatedAt = 0;
      memcpy(failedBondAddress, cmpl.bd_addr, sizeof(esp_bd_addr_t));
      failedBondPending = true;
      Serial.printf("BLE auth failed, reason %u\n", cmpl.fail_reason);
    }
  }
};

// iOS exposes AMS only to peripherals that solicit its service UUID.
void addAmsSolicitation(BLEAdvertisementData &adv)
{
  BLEUUID uuid(AMS_SERVICE_UUID);
  String data;
  data.reserve(18);
  data += (char)17;
  data += (char)ESP_BLE_AD_TYPE_128SOL_SRV_UUID;
  data.concat((const char *)uuid.getNative()->uuid.uuid128, 16);
  adv.addData(data);
}

void onEntityUpdate(BLERemoteCharacteristic *, uint8_t *data, size_t len, bool)
{
  if (len < 3)
    return;
  uint8_t entity = data[0];
  uint8_t attr = data[1];
  char value[192];
  size_t count = min(len - 3, sizeof(value) - 1);
  memcpy(value, data + 3, count);
  value[count] = 0;

  if (entity == ENTITY_PLAYER && attr == PLAYER_ATTR_PLAYBACK_INFO)
  {
    int state = 0;
    float rate = 0, elapsed = 0;
    if (sscanf(value, "%d,%f,%f", &state, &rate, &elapsed) >= 1)
    {
      portENTER_CRITICAL(&amsInboxMux);
      amsInbox.state = state;
      amsInbox.rate = rate;
      amsInbox.elapsed = elapsed;
      amsInbox.elapsedAtMs = millis();
      amsInbox.dirty |= AMS_DIRTY_PLAYBACK;
      portEXIT_CRITICAL(&amsInboxMux);
    }
  }
  else if (entity == ENTITY_TRACK)
  {
    uint8_t dirty = 0;
    char *destination = nullptr;
    switch (attr)
    {
    case TRACK_ATTR_ARTIST:
      dirty = AMS_DIRTY_ARTIST;
      destination = amsInbox.artist;
      break;
    case TRACK_ATTR_ALBUM:
      dirty = AMS_DIRTY_ALBUM;
      destination = amsInbox.album;
      break;
    case TRACK_ATTR_TITLE:
      dirty = AMS_DIRTY_TITLE;
      destination = amsInbox.title;
      break;
    case TRACK_ATTR_DURATION:
    {
      float duration = atof(value);
      portENTER_CRITICAL(&amsInboxMux);
      if (amsInbox.duration != duration)
      {
        amsInbox.duration = duration;
        amsInbox.trackGeneration = ++publishedTrackGeneration;
        amsInbox.dirty |= AMS_DIRTY_DURATION;
      }
      portEXIT_CRITICAL(&amsInboxMux);
      break;
    }
    }
    if (destination)
    {
      portENTER_CRITICAL(&amsInboxMux);
      if (strcmp(destination, value) != 0)
      {
        memcpy(destination, value, count + 1);
        amsInbox.trackGeneration = ++publishedTrackGeneration;
        amsInbox.dirty |= dirty;
      }
      portEXIT_CRITICAL(&amsInboxMux);
    }
  }
}

uint32_t latestPublishedTrackGeneration()
{
  portENTER_CRITICAL(&amsInboxMux);
  uint32_t generation = publishedTrackGeneration;
  portEXIT_CRITICAL(&amsInboxMux);
  return generation;
}

bool trackRequestIsCurrent(uint32_t generation)
{
  return generation != 0 && latestPublishedTrackGeneration() == generation;
}

void clearPendingAmsUpdates()
{
  portENTER_CRITICAL(&amsInboxMux);
  amsInbox.artist[0] = 0;
  amsInbox.album[0] = 0;
  amsInbox.title[0] = 0;
  amsInbox.duration = -1;
  amsInbox.dirty = 0;
  portEXIT_CRITICAL(&amsInboxMux);
}

uint64_t hashTrackField(uint64_t hash, const String &field)
{
  constexpr uint64_t fnvPrime = 1099511628211ULL;
  for (size_t i = 0; i < field.length(); i++)
  {
    hash ^= (uint8_t)field[i];
    hash *= fnvPrime;
  }
  hash ^= 0xFF;
  return hash * fnvPrime;
}

uint64_t currentTrackKey()
{
  if (!np.haveTrack)
    return 0;
  uint64_t hash = 14695981039346656037ULL;
  hash = hashTrackField(hash, np.artist);
  hash = hashTrackField(hash, np.title);
  hash = hashTrackField(hash, np.album);
  uint32_t roundedDuration = np.duration > 0 ? (uint32_t)(np.duration + 0.5f) : 0;
  for (size_t i = 0; i < sizeof(roundedDuration); i++)
  {
    hash ^= (roundedDuration >> (i * 8)) & 0xFF;
    hash *= 1099511628211ULL;
  }
  return hash ? hash : 1;
}

void applyPendingAmsUpdates()
{
  AmsInbox pending;
  portENTER_CRITICAL(&amsInboxMux);
  uint8_t dirty = amsInbox.dirty;
  if (dirty)
  {
    pending = amsInbox;
    amsInbox.dirty = 0;
  }
  portEXIT_CRITICAL(&amsInboxMux);
  if (!dirty)
    return;

  if (dirty & AMS_DIRTY_PLAYBACK)
  {
    bool hadPlaybackInfo = playbackInfoKnown;
    int previousState = np.state;
    np.state = pending.state;
    np.rate = pending.rate;
    np.elapsed = pending.elapsed;
    np.elapsedAtMs = pending.elapsedAtMs;
    playbackInfoKnown = true;
    if (displayHeld && phoneConnected && np.haveTrack && np.state != 0)
    {
      displayHeld = false;
      screenDirty = true;
    }
    if (np.state == 0)
    {
      // A real transition into pause gets one final transport refresh so the
      // action glyph changes to Play. Repeated paused notifications and a
      // reconnect that is already paused remain completely silent.
      if (hadPlaybackInfo && previousState != 0 && !displayHeld)
        pauseRefreshPending = true;
      transportDirty = pauseRefreshPending;
      lyricsDirty = false;
    }
    else if (!displayHeld)
    {
      pauseRefreshPending = false;
      transportDirty = true;
    }
  }

  bool metadataChanged = dirty & (AMS_DIRTY_ARTIST | AMS_DIRTY_ALBUM |
                                  AMS_DIRTY_TITLE | AMS_DIRTY_DURATION);
  if (dirty & AMS_DIRTY_ARTIST)
    np.artist = pending.artist;
  if (dirty & AMS_DIRTY_ALBUM)
    np.album = pending.album;
  if (dirty & AMS_DIRTY_TITLE)
  {
    np.title = pending.title;
    np.haveTrack = np.title.length() > 0;
    // A reconnect stays visually silent until fresh title and playback state
    // confirm that AMS is no longer showing stale connection state.
    if (displayHeld && phoneConnected && np.haveTrack &&
        playbackInfoKnown && np.state != 0)
      displayHeld = false;
  }
  if (dirty & AMS_DIRTY_DURATION)
    np.duration = pending.duration;
  if (metadataChanged)
  {
    appliedTrackGeneration = pending.trackGeneration;
    activeTrackKey = currentTrackKey();
    trackMetadataChangedAt = millis();
    if (!displayHeld)
      screenDirty = true;
  }
}

float currentElapsed()
{
  float value = np.elapsed;
  if (np.state == 1)
    value += np.rate * (millis() - np.elapsedAtMs) / 1000.0f;
  if (np.duration > 0 && value > np.duration)
    value = np.duration;
  return value < 0 ? 0 : value;
}

void formatTime(float seconds, char *out, size_t size)
{
  int whole = max(0, (int)seconds);
  snprintf(out, size, "%d:%02d", whole / 60, whole % 60);
}

String urlEncode(const String &input)
{
  String encoded;
  encoded.reserve(input.length() * 3);
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < input.length(); i++)
  {
    uint8_t c = (uint8_t)input[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
      encoded += (char)c;
    else
    {
      encoded += '%';
      encoded += hex[c >> 4];
      encoded += hex[c & 0x0F];
    }
  }
  return encoded;
}

void parseLrcLine(const char *text, size_t length)
{
  const char *bracket = (const char *)memchr(text, ']', length);
  int minutes, seconds, fraction;
  if (length > 9 && text[0] == '[' && bracket &&
      sscanf(text, "[%d:%d.%d]", &minutes, &seconds, &fraction) == 3)
  {
    size_t textLength = length - (bracket + 1 - text);
    const char *dot = (const char *)memchr(text, '.', bracket - text);
    int fractionDigits = dot ? (int)(bracket - dot - 1) : 0;
    uint32_t fractionMs = fractionDigits == 1 ? fraction * 100UL : fractionDigits == 2 ? fraction * 10UL
                                                                                       : fraction;
    uint32_t lineMs = (uint32_t)minutes * 60000UL +
                      (uint32_t)seconds * 1000UL + fractionMs;
    appendLyricLine(lineMs, bracket + 1, textLength);
  }
}

class LrcStreamSink
{
public:
  LrcStreamSink() { line.reserve(256); }

  bool write(char c)
  {
    if (c == '\n')
    {
      flush();
      return true;
    }
    if (c == '\r')
      return true;
    if (line.concat(c))
      return true;
    overflow = true;
    return false;
  }

  void finish() { flush(); }

private:
  String line;
  bool overflow = false;

  void flush()
  {
    if (!overflow && line.length())
      parseLrcLine(line.c_str(), line.length());
    line.remove(0);
    overflow = false;
  }
};

// Allocate the response only after TLS connects, keeping that buffer out of
// the handshake's heap peak. HTTP/1.0 avoids chunked response bodies.
int httpDownload(const String &url, uint8_t **output, int maxLength)
{
  PERF_SCOPE("httpDownload");
  *output = nullptr;
  bool tls = url.startsWith("https://");
  WiFiClientSecure secureClient;
  WiFiClient plainClient;
  if (tls)
    secureClient.setInsecure();

  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(15000);
  http.setUserAgent("MusicStation/2.0 (ESP32-S3 e-paper)");
  WiFiClient &networkClient = tls ? (WiFiClient &)secureClient : plainClient;
  if (!http.begin(networkClient, url))
    return -1;

  int code = http.GET();
  PERF_LOG(tls ? "HTTPS GET connected" : "HTTP GET connected");
  int total = -1;
  int expected = -1;
  if (code == HTTP_CODE_OK)
  {
    expected = http.getSize();
    int capacity = (expected > 0 && expected + 1 < maxLength)
                       ? expected + 1
                       : maxLength;
    uint8_t *buffer = (uint8_t *)malloc(capacity);
    if (!buffer)
    {
      http.end();
      return -1;
    }
    WiFiClient *stream = http.getStreamPtr();
    total = 0;
    unsigned long lastDataAt = millis();
    while (total < capacity - 1 && millis() - lastDataAt < 20000)
    {
      size_t available = stream->available();
      if (available)
      {
        int count = stream->readBytes(
            buffer + total, min((int)available, capacity - 1 - total));
        if (count > 0)
        {
          total += count;
          lastDataAt = millis();
        }
      }
      else if (!http.connected())
        break;
      else
        delay(20);
      if (expected > 0 && total >= expected)
        break;
    }
    buffer[total] = 0;
    *output = buffer;
    if (total == capacity - 1 && (expected < 0 || total < expected))
      Serial.printf("HTTP response truncated at %d bytes\n", total);
  }
  http.end();
  if (code != HTTP_CODE_OK)
    Serial.printf("HTTP download failed: %d\n", code);
  return total;
}

int downloadWithRetry(const String &url, uint8_t **output, int maxLength)
{
  int size = httpDownload(url, output, maxLength);
  if (size <= 0)
  {
    delay(800);
    size = httpDownload(url, output, maxLength);
  }
  return size;
}

// All HTTP work is synchronous, so one shared read buffer removes thousands
// of one-byte virtual calls without adding another large task-stack object.
uint8_t networkReadBuffer[512];

enum LyricsJsonField : uint8_t
{
  LYRICS_FIELD_NONE,
  LYRICS_FIELD_SYNCED,
  LYRICS_FIELD_FILE
};

class BufferedHttpReader
{
public:
  BufferedHttpReader(WiFiClient *source, HTTPClient &request)
      : stream(source), http(request), position(0), availableBytes(0),
        lastDataAt(millis()) {}

  int read()
  {
    if (position < availableBytes)
      return networkReadBuffer[position++];

    while (millis() - lastDataAt < 20000)
    {
      size_t available = stream->available();
      if (available)
      {
        int count = stream->readBytes(
            networkReadBuffer, min(available, sizeof(networkReadBuffer)));
        if (count <= 0)
          continue;
        position = 1;
        availableBytes = count;
        lastDataAt = millis();
        return networkReadBuffer[0];
      }
      if (!http.connected())
        return -1;
      delay(20);
    }
    return -1;
  }

  uint8_t findNextLyricsField()
  {
    constexpr char syncedToken[] = "\"syncedLyrics\"";
    constexpr char fileToken[] = "\"lyricsfile\"";
    size_t syncedMatched = 0;
    size_t fileMatched = 0;
    int value;
    while ((value = read()) >= 0)
    {
      char c = (char)value;
      syncedMatched = c == syncedToken[syncedMatched]
                          ? syncedMatched + 1
                          : (c == syncedToken[0] ? 1 : 0);
      fileMatched = c == fileToken[fileMatched]
                        ? fileMatched + 1
                        : (c == fileToken[0] ? 1 : 0);
      if (syncedMatched == sizeof(syncedToken) - 1)
        return LYRICS_FIELD_SYNCED;
      if (fileMatched == sizeof(fileToken) - 1)
        return LYRICS_FIELD_FILE;
    }
    return LYRICS_FIELD_NONE;
  }

private:
  WiFiClient *stream;
  HTTPClient &http;
  size_t position;
  size_t availableBytes;
  unsigned long lastDataAt;
};

template <typename Sink>
bool appendUtf8(Sink &output, uint16_t codepoint)
{
  if (codepoint < 0x80)
    return output.write((char)codepoint);
  else if (codepoint < 0x800)
  {
    bool ok = output.write((char)(0xC0 | (codepoint >> 6)));
    return output.write((char)(0x80 | (codepoint & 0x3F))) && ok;
  }
  bool ok = output.write((char)(0xE0 | (codepoint >> 12)));
  ok = output.write((char)(0x80 | ((codepoint >> 6) & 0x3F))) && ok;
  return output.write((char)(0x80 | (codepoint & 0x3F))) && ok;
}

template <typename Sink>
bool readJsonStringValue(BufferedHttpReader &reader, Sink &output,
                         bool &truncated)
{
  truncated = false;
  int value;
  do
    value = reader.read();
  while (value >= 0 && value != ':');
  do
    value = reader.read();
  while (value >= 0 && isspace(value));
  if (value != '"')
    return false; // includes a JSON null value

  bool escaped = false;
  while ((value = reader.read()) >= 0)
  {
    char c = (char)value;
    if (!escaped)
    {
      if (c == '"')
      {
        output.finish();
        return true;
      }
      if (c == '\\')
        escaped = true;
      else
        truncated = !output.write(c) || truncated;
      continue;
    }

    escaped = false;
    if (c == 'n')
    {
      truncated = !output.write('\n') || truncated;
    }
    else if (c == 'r')
    {
    }
    else if (c == 't')
    {
      truncated = !output.write(' ') || truncated;
    }
    else if (c == 'u')
    {
      uint16_t codepoint = 0;
      bool valid = true;
      for (int digit = 0; digit < 4; digit++)
      {
        int hexChar = reader.read();
        if (hexChar < 0)
        {
          valid = false;
          break;
        }
        codepoint <<= 4;
        if (hexChar >= '0' && hexChar <= '9')
          codepoint |= hexChar - '0';
        else if (hexChar >= 'a' && hexChar <= 'f')
          codepoint |= hexChar - 'a' + 10;
        else if (hexChar >= 'A' && hexChar <= 'F')
          codepoint |= hexChar - 'A' + 10;
        else
        {
          valid = false;
          break;
        }
      }
      if (valid)
        truncated = !appendUtf8(output, codepoint) || truncated;
    }
    else
      truncated = !output.write(c) || truncated;
  }
  return false;
}

String decodeYamlText(String value)
{
  value.trim();
  if (value.length() >= 2 && value[0] == '\'' &&
      value[value.length() - 1] == '\'')
  {
    value = value.substring(1, value.length() - 1);
    value.replace("''", "'");
  }
  else if (value.length() >= 2 && value[0] == '"' &&
           value[value.length() - 1] == '"')
  {
    value = value.substring(1, value.length() - 1);
    value.replace("\\\"", "\"");
    value.replace("\\\\", "\\");
  }
  return value;
}

// Minimal streaming Lyricsfile parser: only top-level lines[*].text and
// start_ms are retained. Deeper word timing entries are ignored.
class LyricsfileStreamSink
{
public:
  LyricsfileStreamSink() { rawLine.reserve(256); }

  bool write(char c)
  {
    if (c == '\n')
    {
      processLine();
      return true;
    }
    if (c == '\r')
      return true;
    if (rawLine.concat(c))
      return true;
    overflow = true;
    return false;
  }

  void finish()
  {
    processLine();
    commitLine();
  }

private:
  bool inLines = false;
  int itemIndent = -1;
  String rawLine;
  String lineText;
  uint32_t startMs = 0;
  bool haveText = false, haveStart = false;
  bool overflow = false;

  void commitLine()
  {
    if (haveText && haveStart && lineText.length())
      appendLyricLine(startMs, lineText.c_str(), lineText.length());
    lineText.remove(0);
    startMs = 0;
    haveText = haveStart = false;
  }

  void processLine()
  {
    if (overflow)
    {
      rawLine.remove(0);
      overflow = false;
      return;
    }
    int indent = 0;
    while (indent < (int)rawLine.length() && rawLine[indent] == ' ')
      indent++;
    String content = rawLine.substring(indent);
    rawLine.remove(0);
    content.trim();

    if (!inLines)
    {
      if (content == "lines:")
        inLines = true;
      return;
    }
    // YAML permits a sequence to be indentationless beneath its mapping key:
    // lines:\n- text: ...  LRCLIB's serializer uses this valid compact form.
    if (indent == 0 && content.length() && content != "lines:" &&
        !content.startsWith("- "))
    {
      commitLine();
      inLines = false;
      return;
    }

    if (itemIndent < 0 && content.startsWith("- "))
      itemIndent = indent;
    if (indent == itemIndent && content.startsWith("- "))
    {
      commitLine();
      content = content.substring(2);
    }
    else if (indent != itemIndent + 2)
      return;

    if (content.startsWith("text:"))
    {
      lineText = decodeYamlText(content.substring(5));
      haveText = true;
    }
    else if (content.startsWith("start_ms:"))
    {
      startMs = (uint32_t)strtoul(content.substring(9).c_str(), nullptr, 10);
      haveStart = true;
    }
  }
};

// Scan either synchronized field in response order and decode it directly
// into the pooled lyric representation without buffering the full payload.
bool fetchLyricsFrom(const String &url)
{
  PERF_SCOPE("fetchLyricsFrom");
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(15000);
  http.setUserAgent(HTTP_USER_AGENT);
  if (!http.begin(secureClient, url))
    return false;
  int code = http.GET();
  PERF_LOG("LRCLIB GET connected");
  if (code != HTTP_CODE_OK)
  {
    http.end();
    Serial.printf("lyrics get: HTTP %d\n", code);
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  BufferedHttpReader reader(stream, http);
  if (lyrics.capacity() < 64)
    lyrics.reserve(64);
  lyricTextArena.reserve(2048);
  bool sawSynced = false;
  bool sawLyricsfile = false;
  while (lyrics.empty() && (!sawSynced || !sawLyricsfile))
  {
    uint8_t field = reader.findNextLyricsField();
    if (field == LYRICS_FIELD_NONE)
      break;
    if (field == LYRICS_FIELD_SYNCED)
      sawSynced = true;
    else
      sawLyricsfile = true;

    bool truncated = false;
    bool haveValue = false;
    if (field == LYRICS_FIELD_SYNCED)
    {
      LrcStreamSink sink;
      haveValue = readJsonStringValue(reader, sink, truncated);
    }
    else
    {
      LyricsfileStreamSink sink;
      haveValue = readJsonStringValue(reader, sink, truncated);
    }
    if (truncated)
      Serial.println("An oversized lyric line was skipped");
    (void)haveValue;
  }
  http.end();
  if (lyricStorageAllocationFailed)
    Serial.println("Not enough memory for the complete lyrics response");
  if (!lyrics.empty())
    Serial.printf("Lyrics ready: %u lines, %u text B, line cap %u\n",
                  (unsigned)lyrics.size(), (unsigned)lyricTextArena.length(),
                  (unsigned)lyrics.capacity());
  return !lyrics.empty();
}

int searchLyricsId(const String &artist, const String &title,
                   const String &album, float duration)
{
  PERF_SCOPE("searchLyricsId");
  String url;
  url.reserve(96 + artist.length() * 3 + title.length() * 3 +
              album.length() * 3);
  url = "https://lrclib.net/api/search?artist_name=";
  url += urlEncode(artist);
  url += "&track_name=";
  url += urlEncode(title);
  if (album.length())
    url += "&album_name=" + urlEncode(album);
  url += "&duration=" + String((int)duration);
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(15000);
  http.setUserAgent(HTTP_USER_AGENT);
  if (!http.begin(secureClient, url))
    return -1;
  int code = http.GET();
  PERF_LOG("LRCLIB search connected");
  int bestId = -1;
  if (code == HTTP_CODE_OK)
  {
    // The structured query is already narrowly matched. Read just the first
    // result's ID and stop before its embedded lyrics/lyricsfile payload.
    WiFiClient *stream = http.getStreamPtr();
    BufferedHttpReader reader(stream, http);
    const char token[] = "\"id\"";
    size_t matched = 0;
    int value;
    while ((value = reader.read()) >= 0)
    {
      char c = (char)value;
      if (c == token[matched])
        matched++;
      else
        matched = c == token[0] ? 1 : 0;
      if (matched == sizeof(token) - 1)
        break;
    }
    do
      value = reader.read();
    while (value >= 0 && value != ':');
    do
      value = reader.read();
    while (value >= 0 && isspace(value));
    if (value >= '0' && value <= '9')
    {
      bestId = 0;
      do
      {
        bestId = bestId * 10 + value - '0';
        value = reader.read();
      } while (value >= '0' && value <= '9');
    }
  }
  http.end();
  if (code != HTTP_CODE_OK)
    Serial.printf("Lyrics search failed: HTTP %d\n", code);
  return bestId;
}

void fetchLyrics()
{
  PERF_SCOPE("fetchLyrics total");
  uint32_t requestedGeneration = appliedTrackGeneration;
  if (!trackRequestIsCurrent(requestedGeneration))
    return;
  const String &requestedTitle = np.title;
  const String &requestedArtist = np.artist;
  const String &requestedAlbum = np.album;
  float requestedDuration = np.duration;
  clearLyrics();
  lyricsForGeneration = requestedGeneration;
  lyricStatus = "LYRICS...";
  if (WiFi.status() != WL_CONNECTED)
    return;

  String exactUrl;
  exactUrl.reserve(112 + requestedArtist.length() * 3 +
                   requestedTitle.length() * 3 + requestedAlbum.length() * 3);
  exactUrl = "https://lrclib.net/api/get?artist_name=";
  exactUrl += urlEncode(requestedArtist);
  exactUrl += "&track_name=";
  exactUrl += urlEncode(requestedTitle);
  exactUrl += "&album_name=";
  exactUrl += urlEncode(requestedAlbum);
  exactUrl += "&duration=";
  exactUrl += String((int)requestedDuration);
  bool found = fetchLyricsFrom(exactUrl);
  if (!found && trackRequestIsCurrent(requestedGeneration))
  {
    delay(300); // LRCLIB asks clients to space sequential requests slightly
    int id = searchLyricsId(requestedArtist, requestedTitle,
                            requestedAlbum, requestedDuration);
    if (id <= 0 && requestedAlbum.length() &&
        trackRequestIsCurrent(requestedGeneration))
    {
      delay(300);
      id = searchLyricsId(requestedArtist, requestedTitle, "",
                          requestedDuration);
    }
    if (id > 0 && trackRequestIsCurrent(requestedGeneration))
    {
      clearLyrics();
      delay(300);
      found = fetchLyricsFrom(String("https://lrclib.net/api/get/") + id);
    }
  }

  if (!trackRequestIsCurrent(requestedGeneration))
  {
    clearLyrics();
    return;
  }
  lyricStatus = found ? "" : "NO SYNCED LYRICS";
  lyricsDirty = true;
  if (!found)
    Serial.printf("No synchronized lyrics for '%s'\n", requestedTitle.c_str());
}

int drawDecodedCover(JPEGDRAW *draw)
{
  uint8_t *pixels = (uint8_t *)draw->pPixels;
  for (int y = 0; y < draw->iHeight; y++)
  {
    int targetY = draw->y + y;
    if (targetY >= COVER_SIZE)
      break;
    for (int x = 0; x < draw->iWidth; x++)
    {
      int targetX = draw->x + x;
      if (targetX < COVER_SIZE)
        coverDecodeTarget[targetY * COVER_SIZE + targetX] =
            pixels[y * draw->iWidth + x];
    }
  }
  return 1;
}

void sharpenCover()
{
  PERF_SCOPE("sharpenCover");
  // Preserve only the three original source rows needed by the 3x3 box blur.
  // This produces the same sharpened pixels as the old full 96x96 blur image
  // while reducing temporary storage from 9,216 bytes to 288 stack bytes.
  uint8_t rowStorage[3][COVER_SIZE];
  uint8_t *previous = rowStorage[0];
  uint8_t *current = rowStorage[1];
  uint8_t *next = rowStorage[2];
  memcpy(current, coverGray, COVER_SIZE);
  if (COVER_SIZE > 1)
    memcpy(next, coverGray + COVER_SIZE, COVER_SIZE);

  for (int y = 0; y < COVER_SIZE; y++)
  {
    for (int x = 0; x < COVER_SIZE; x++)
    {
      int sum = 0, count = 0;
      for (int dx = -1; dx <= 1; dx++)
      {
        int xx = x + dx;
        if (xx >= 0 && xx < COVER_SIZE)
        {
          if (y > 0)
          {
            sum += previous[xx];
            count++;
          }
          sum += current[xx];
          count++;
          if (y + 1 < COVER_SIZE)
          {
            sum += next[xx];
            count++;
          }
        }
      }
      int sharpened = current[x] + 2 * (current[x] - sum / count);
      coverGray[y * COVER_SIZE + x] = constrain(sharpened, 0, 255);
    }

    if (y + 1 < COVER_SIZE)
    {
      uint8_t *recycled = previous;
      previous = current;
      current = next;
      next = recycled;
      if (y + 2 < COVER_SIZE)
        memcpy(next, coverGray + (y + 2) * COVER_SIZE, COVER_SIZE);
    }
  }
}

// Contrast-stretch then Atkinson-dither. Set bits are black pixels in the
// bitmap passed to GFX; the page itself is already filled white.
void ditherCover()
{
  PERF_SCOPE("ditherCover");
  constexpr int pixelCount = COVER_SIZE * COVER_SIZE;
  int histogram[256] = {0};
  for (int i = 0; i < pixelCount; i++)
    histogram[coverGray[i]]++;

  int low = 0, high = 255, accumulated = 0;
  for (int value = 0; value < 256; value++)
  {
    accumulated += histogram[value];
    if (accumulated >= pixelCount / 20)
    {
      low = value;
      break;
    }
  }
  accumulated = 0;
  for (int value = 255; value >= 0; value--)
  {
    accumulated += histogram[value];
    if (accumulated >= pixelCount / 20)
    {
      high = value;
      break;
    }
  }
  if (high - low < 32)
  {
    low = 0;
    high = 255;
  }
  for (int i = 0; i < pixelCount; i++)
    coverGray[i] = constrain((coverGray[i] - low) * 255 / (high - low), 0, 255);

  memset(coverBitmap, 0, sizeof(coverBitmap));
  for (int y = 0; y < COVER_SIZE; y++)
  {
    for (int x = 0; x < COVER_SIZE; x++)
    {
      int index = y * COVER_SIZE + x;
      int oldValue = coverGray[index];
      int newValue = oldValue < 128 ? 0 : 255;
      if (newValue == 0)
        coverBitmap[y * (COVER_SIZE / 8) + x / 8] |= 0x80 >> (x & 7);
      int error = (oldValue - newValue) / 8;
      if (!error)
        continue;
      if (x + 1 < COVER_SIZE)
        coverGray[index + 1] = constrain(coverGray[index + 1] + error, 0, 255);
      if (x + 2 < COVER_SIZE)
        coverGray[index + 2] = constrain(coverGray[index + 2] + error, 0, 255);
      if (y + 1 < COVER_SIZE)
      {
        if (x > 0)
          coverGray[index + COVER_SIZE - 1] =
              constrain(coverGray[index + COVER_SIZE - 1] + error, 0, 255);
        coverGray[index + COVER_SIZE] =
            constrain(coverGray[index + COVER_SIZE] + error, 0, 255);
        if (x + 1 < COVER_SIZE)
          coverGray[index + COVER_SIZE + 1] =
              constrain(coverGray[index + COVER_SIZE + 1] + error, 0, 255);
      }
      if (y + 2 < COVER_SIZE)
        coverGray[index + 2 * COVER_SIZE] =
            constrain(coverGray[index + 2 * COVER_SIZE] + error, 0, 255);
    }
  }
  coverValid = true;
}

bool fetchArtworkUrl(const String &url, String &artworkUrl)
{
  PERF_SCOPE("fetchArtworkUrl");
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(15000);
  http.setUserAgent("MusicStation/2.0 (ESP32-S3 e-paper)");
  if (!http.begin(secureClient, url))
    return false;
  int code = http.GET();
  PERF_LOG("iTunes search connected");
  bool found = false;
  if (code == HTTP_CODE_OK)
  {
    JsonDocument filter;
    filter["results"][0]["artworkUrl100"] = true;
    JsonDocument document;
    DeserializationError error = deserializeJson(
        document, http.getStream(), DeserializationOption::Filter(filter));
    if (!error)
    {
      artworkUrl = String(
          (const char *)(document["results"][0]["artworkUrl100"] | ""));
      found = artworkUrl.length() > 0;
    }
  }
  else
    Serial.printf("Artwork search failed: HTTP %d\n", code);
  http.end();
  return found;
}

void fetchCover()
{
  PERF_SCOPE("fetchCover total");
  uint32_t requestedGeneration = appliedTrackGeneration;
  if (!trackRequestIsCurrent(requestedGeneration))
    return;
  const String &requestedTitle = np.title;
  const String &requestedArtist = np.artist;
  coverForGeneration = requestedGeneration;
  coverValid = false;
  coverStatus = "NO ART";
  if (WiFi.status() != WL_CONNECTED)
    return;

  String searchTerm = requestedArtist + " " + requestedTitle;
  String searchUrl;
  searchUrl.reserve(88 + searchTerm.length() * 3);
  searchUrl =
      "https://itunes.apple.com/search?media=music&entity=song&limit=1&term=";
  searchUrl += urlEncode(searchTerm);
  uint8_t *buffer = nullptr;
  String artworkUrl;
  if (!fetchArtworkUrl(searchUrl, artworkUrl))
    coverStatus = "ART SEARCH";

  if (artworkUrl.length() && trackRequestIsCurrent(requestedGeneration))
  {
    artworkUrl.replace("100x100", "96x96");
    int length = downloadWithRetry(artworkUrl, &buffer, 24576);
    if (!trackRequestIsCurrent(requestedGeneration))
    {
      free(buffer);
      return;
    }
    if (length > 0)
    {
      memset(coverGray, 255, sizeof(coverGray));
      coverDecodeTarget = coverGray;
      // JPEGDEC is a large object. A normal throwing new terminates this build
      // when heap fragmentation leaves no sufficiently large block.
      JPEGDEC *jpeg = new (std::nothrow) JPEGDEC();
      bool decoded = false;
      if (jpeg && jpeg->openRAM(buffer, length, drawDecodedCover))
      {
        jpeg->setPixelType(EIGHT_BIT_GRAYSCALE);
        decoded = jpeg->decode(0, 0, 0);
        jpeg->close();
      }
      else if (!jpeg)
      {
        Serial.printf("Artwork decoder allocation failed: need %u B, "
                      "largest block %u B\n",
                      (unsigned)sizeof(JPEGDEC),
                      (unsigned)heap_caps_get_largest_free_block(
                          MALLOC_CAP_8BIT));
      }
      delete jpeg;
      if (decoded && trackRequestIsCurrent(requestedGeneration))
      {
        sharpenCover();
        ditherCover();
        coverStatus = "";
      }
      else
        coverStatus = "ART JPEG";
    }
    else
      coverStatus = "ART IMAGE";
    free(buffer);
  }
  bool requestCurrent = trackRequestIsCurrent(requestedGeneration);
  if (requestCurrent)
    screenDirty = true;
  if (!coverValid && requestCurrent)
    Serial.printf("Cover failed for '%s': %s\n", requestedTitle.c_str(),
                  coverStatus.c_str());
}

int textWidth(const String &text, const GFXfont *font)
{
  int16_t x1, y1;
  uint16_t width, height;
  display.setFont(font);
  display.getTextBounds(text, 0, 0, &x1, &y1, &width, &height);
  return width;
}

size_t nextUtf8Boundary(const String &text, size_t start)
{
  if (start >= text.length())
    return text.length();
  uint8_t first = (uint8_t)text[start];
  size_t bytes = first < 0x80   ? 1
                 : first < 0xE0 ? 2
                 : first < 0xF0 ? 3
                                : 4;
  size_t end = min(start + bytes, text.length());
  for (size_t i = start + 1; i < end; i++)
    if (((uint8_t)text[i] & 0xC0) != 0x80)
      return start + 1; // malformed UTF-8: advance without splitting again
  return end;
}

int utf8GlyphWidth(const String &text, size_t start, size_t end)
{
  char glyph[5];
  size_t length = min(end - start, sizeof(glyph) - 1);
  memcpy(glyph, text.c_str() + start, length);
  glyph[length] = 0;
  return unicodeText.getUTF8Width(glyph);
}

int utf8TextWidth(const String &text, const uint8_t *font)
{
  unicodeText.setFont(font);
  return unicodeText.getUTF8Width(text.c_str());
}

String fitUtf8Text(const String &input, const uint8_t *font, int maxWidth)
{
  unicodeText.setFont(font);
  if (unicodeText.getUTF8Width(input.c_str()) <= maxWidth)
    return input;
  const int available = max(0, maxWidth - unicodeText.getUTF8Width("..."));
  int width = 0;
  size_t boundary = 0;
  while (boundary < input.length())
  {
    size_t next = nextUtf8Boundary(input, boundary);
    int glyphWidth = utf8GlyphWidth(input, boundary, next);
    if (width + glyphWidth > available)
      break;
    width += glyphWidth;
    boundary = next;
  }
  String fitted;
  fitted.reserve(boundary + 3);
  fitted.concat(input.c_str(), boundary);
  fitted += "...";
  return fitted;
}

void drawCentered(const String &text, int baseline, const GFXfont *font)
{
  String fitted = text;
  if (textWidth(fitted, font) > 286)
  {
    while (fitted.length() > 1 && textWidth(fitted + "...", font) > 286)
      fitted.remove(fitted.length() - 1);
    fitted += "...";
  }
  int width = textWidth(fitted, font);
  display.setFont(font);
  display.setCursor((296 - width) / 2, baseline);
  display.print(fitted);
}

void drawStatusPage()
{
  if (pairingPasskey)
  {
    drawCentered("ENTER THIS CODE ON iPHONE", 29, &FreeSans9pt7b);
    char code[8];
    snprintf(code, sizeof(code), "%06u", (unsigned)pairingPasskey);
    drawCentered(code, 72, &FreeSansBold12pt7b);
    drawCentered("Keep this device connected", 111, &FreeSans9pt7b);
  }
  else if (phoneConnected)
  {
    drawCentered(amsReady ? "Connected - nothing playing" : "Connecting to Apple Media Service...",
                 53, &FreeSansBold12pt7b);
    drawCentered("Start playback on the iPhone or iPad", 91, &FreeSans9pt7b);
  }
  else
  {
    drawCentered("WAITING FOR iPHONE / iPAD", 38, &FreeSansBold12pt7b);
    drawCentered("PAIR IN IPHONE BLUETOOTH SETTINGS", 76, &FreeSans9pt7b);
    drawCentered("The pairing code will appear here", 108, &FreeSans9pt7b);
  }
}

int currentLyricIndex()
{
  if (lyrics.empty())
  {
    lyricCursor = -1;
    lyricCursorValid = false;
    return -1;
  }
  uint32_t playbackMs = (uint32_t)(currentElapsed() * 1000.0f);
  playbackMs += LYRIC_LEAD_MS;

  // Normal playback advances one or a few records. A backwards seek uses a
  // logarithmic lookup instead of rescanning every lyric from the beginning.
  if (!lyricCursorValid || playbackMs < lyricCursorPlaybackMs)
  {
    size_t low = 0, high = lyrics.size();
    while (low < high)
    {
      size_t middle = low + (high - low) / 2;
      if (lyrics[middle].ms <= playbackMs)
        low = middle + 1;
      else
        high = middle;
    }
    lyricCursor = (int)low - 1;
  }
  else
  {
    while (lyricCursor + 1 < (int)lyrics.size() &&
           lyrics[lyricCursor + 1].ms <= playbackMs)
      lyricCursor++;
  }
  lyricCursorPlaybackMs = playbackMs;
  lyricCursorValid = true;
  return lyricCursor;
}

int wrapLyric(const String &text, String *rows, int maxRows)
{
  unicodeText.setFont(LYRIC_FONT);
  size_t start = 0;
  while (start < text.length() && isspace((unsigned char)text[start]))
    start++;
  int count = 0;
  const int maxWidth = RIGHT_W - 8;
  while (start < text.length() && count < maxRows)
  {
    int width = 0;
    size_t cursor = start;
    size_t lastFit = start;
    size_t lastSpace = text.length();
    while (cursor < text.length())
    {
      size_t next = nextUtf8Boundary(text, cursor);
      int glyphWidth = utf8GlyphWidth(text, cursor, next);
      if (width + glyphWidth > maxWidth)
        break;
      width += glyphWidth;
      lastFit = next;
      if (text[cursor] == ' ')
        lastSpace = cursor;
      cursor = next;
    }
    if (cursor == text.length())
    {
      rows[count++] = text.substring(start);
      start = text.length();
      break;
    }
    size_t split = lastSpace != text.length() && lastSpace > start
                       ? lastSpace
                       : lastFit;
    if (split == start) // one glyph is wider than the row
      split = nextUtf8Boundary(text, start);
    rows[count++] = text.substring(start, split);
    start = split;
    while (start < text.length() && isspace((unsigned char)text[start]))
      start++;
  }
  if (start < text.length() && count > 0)
  {
    String last = rows[count - 1];
    last += "...";
    rows[count - 1] = fitUtf8Text(last, LYRIC_FONT, maxWidth);
  }
  return count;
}

void drawLyrics()
{
  // Three lines keep the lyric block comfortably below the album metadata.
  // Longer lyrics are ellipsized instead of growing upward into that row.
  String rows[3];
  int rowCount = 0;
  int index = currentLyricIndex();
  if (index >= 0)
    rowCount = wrapLyric(lyricTextAt(index), rows, 3);
  else if (lyrics.empty() && lyricStatus.length())
    rowCount = wrapLyric(lyricStatus, rows, 3);

  unicodeText.setFont(LYRIC_FONT);
  const int lineHeight = 16;
  const int areaTop = 35;
  const int areaHeight = 70;
  int baseline = areaTop + (areaHeight - rowCount * lineHeight) / 2 + 13;
  for (int row = 0; row < rowCount; row++)
  {
    int width = utf8TextWidth(rows[row], LYRIC_FONT);
    unicodeText.drawUTF8(RIGHT_X + max(0, (RIGHT_W - width) / 2),
                         baseline + row * lineHeight, rows[row].c_str());
  }
}

// The glyph shows the action a tap would perform: pause while music is
// playing, play while it is paused. Touch control can be added later.
void drawTransportActionIcon()
{
  if (np.state == 1)
  {
    display.fillRect(190, 112, 4, 15, GxEPD_BLACK);
    display.fillRect(198, 112, 4, 15, GxEPD_BLACK);
  }
  else
  {
    display.fillTriangle(189, 112, 189, 126, 202, 119, GxEPD_BLACK);
  }
}

void drawTransportBar()
{
  float elapsed = currentElapsed();
  const int barX = RIGHT_X, barY = 123, barW = RIGHT_W;

  // Elapsed and remaining times sit above the bar, aligned to its ends.
  char elapsedText[12];
  char remainingClock[12];
  char remainingText[14];
  formatTime(elapsed, elapsedText, sizeof(elapsedText));
  display.setFont(nullptr);
  display.setTextSize(1);
  display.setCursor(barX, 109);
  display.print(elapsedText);
  if (np.duration > 0)
  {
    float remaining = max(0.0f, np.duration - elapsed);
    formatTime(remaining, remainingClock, sizeof(remainingClock));
    snprintf(remainingText, sizeof(remainingText), "-%s", remainingClock);
    display.setCursor(barX + barW - strlen(remainingText) * 6, 109);
    display.print(remainingText);
  }

  display.drawRect(barX, barY, barW, 3, GxEPD_BLACK);
  if (np.duration > 0)
  {
    int fill = constrain((int)((barW - 2) * elapsed / np.duration), 0, barW - 2);
    if (fill > 0)
      display.fillRect(barX + 1, barY + 1, fill, 1, GxEPD_BLACK);
  }

  // Open a small gap and center the action glyph in the right-hand toolbar.
  display.fillRect(184, 110, 24, 17, GxEPD_WHITE);
  drawTransportActionIcon();
}

void drawNowPlayingPage()
{
  // Compact title and artist share the top row. The album sits immediately
  // below, leaving nearly the entire middle available for artwork and lyrics.
  String title = fitUtf8Text(np.title, TITLE_FONT, 188);
  unicodeText.setFont(TITLE_FONT);
  unicodeText.drawUTF8(4, 16, title.c_str());

  String artist = fitUtf8Text(np.artist, SMALL_FONT, 92);
  int artistWidth = utf8TextWidth(artist, SMALL_FONT);
  unicodeText.drawUTF8(292 - artistWidth, 8, artist.c_str());

  if (np.album.length())
  {
    String album = fitUtf8Text(np.album, SMALL_FONT, 288);
    unicodeText.drawUTF8(4, 22, album.c_str());
  }

  if (coverValid)
  {
    display.drawBitmap(COVER_X, COVER_Y, coverBitmap,
                       COVER_SIZE, COVER_SIZE, GxEPD_BLACK);
  }
  else
  {
    // Keep the intended artwork area legible while Wi-Fi/search/decode runs.
    display.drawRect(COVER_X, COVER_Y, COVER_SIZE, COVER_SIZE, GxEPD_BLACK);
    String label = coverStatus.length() ? coverStatus : "ART...";
    while (textWidth(label, nullptr) > COVER_SIZE - 8 && label.length() > 1)
      label.remove(label.length() - 1);
    display.setFont(nullptr);
    display.setTextSize(1);
    display.setCursor(COVER_X + (COVER_SIZE - textWidth(label, nullptr)) / 2,
                      COVER_Y + COVER_SIZE / 2 + 3);
    display.print(label);
  }

  drawLyrics();
  drawTransportBar();
}

void refreshDisplay(bool fullRefresh, bool transportOnly = false,
                    bool rightColumnOnly = false)
{
  if (!displayReady)
    return;
  if (partialRefreshCount >= FULL_REFRESH_AFTER_PARTIALS)
    fullRefresh = true;
  if (fullRefresh)
  {
    transportOnly = false;
    rightColumnOnly = false;
  }
#if MUSICSTATION_PROFILE
  unsigned long profileRefreshStartedAt = millis();
  bool profileFullRefresh = fullRefresh;
  bool profileTransportOnly = transportOnly;
  bool profileRightColumnOnly = rightColumnOnly;
#endif

  // Full cleaning updates use the panel's normal border waveform. Differential
  // partial updates hold the border driver in HiZ to prevent its thin flash.
  display.epd2.setQuietBorder(!fullRefresh);

  // Measure cadence from the beginning of the blocking panel waveform, not
  // from its completion. Clear dirty first so a BLE update arriving while the
  // panel is busy remains queued for the next refresh.
  lastRefreshStartedAt = millis();
  if (transportOnly)
    transportDirty = false;
  else if (rightColumnOnly)
  {
    lyricsDirty = false;
    transportDirty = false;
  }
  else
  {
    screenDirty = false;
    lyricsDirty = false;
    transportDirty = false;
  }

  if (fullRefresh)
    display.setFullWindow();
  else if (transportOnly)
    // Only the bottom-right toolbar changes during normal playback. Artwork
    // and lyrics remain untouched until their own content changes.
    display.setPartialWindow(RIGHT_X, 107, RIGHT_W, 20);
  else if (rightColumnOnly)
    // Lyric changes redraw the right column and current transport state, but
    // never drive the album-art pixels again.
    display.setPartialWindow(RIGHT_X, 31, RIGHT_W, 96);
  else
    display.setPartialWindow(0, 0, display.width(), display.height());

  display.firstPage();
  do
  {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    if (transportOnly)
      drawTransportBar();
    else if (rightColumnOnly)
    {
      drawLyrics();
      drawTransportBar();
    }
    else if (amsReady && np.haveTrack)
      drawNowPlayingPage();
    else
      drawStatusPage();
  } while (display.nextPage());

  // Keep the border quiet between updates and prepare the next partial cycle.
  display.epd2.setQuietBorder(true);

  if (fullRefresh)
    partialRefreshCount = 0;
  else
    partialRefreshCount++;

#if MUSICSTATION_PROFILE
  // Transport updates are intentionally sampled to keep profiling itself from
  // adding a serial-write delay to every 800 ms panel cycle.
  static uint32_t transportRefreshes = 0;
  bool report = !profileTransportOnly || (transportRefreshes++ % 50 == 0);
  if (report)
  {
    const char *kind = profileFullRefresh       ? "full"
                       : profileTransportOnly   ? "transport"
                       : profileRightColumnOnly ? "lyrics"
                                                : "screen";
    Serial.printf("[perf] display %s | %lu ms partialCount=%u\n", kind,
                  (unsigned long)(millis() - profileRefreshStartedAt),
                  (unsigned)partialRefreshCount);
    PERF_LOG("after display refresh");
  }
#endif
}

bool startAmsClient()
{
  PERF_SCOPE("startAmsClient");
  // Called only after the HID link has finished pairing and encryption. Do
  // not request encryption here: overlapping SMP procedures make Bluedroid
  // reject the connection with "earlier enc was not done for same device".
  // A GATT-client interface shares the physical HID connection to iOS. Never
  // close that link merely because AMS discovery needs another attempt.
  if (client && !client->isConnected())
  {
    delete client;
    client = nullptr;
  }
  if (!client)
  {
    client = BLEDevice::createClient();
    if (!client->connect(phoneAddress))
    {
      Serial.println("client connect failed");
      return false;
    }
  }

  pairingPasskey = 0;
  screenDirty = true;
  if (!client->isConnected())
    return false;

  // AMS may be published after encryption. Force fresh service discovery on
  // every retry instead of reusing a cached pre-authentication service list.
  client->getServices();
  BLERemoteService *ams = client->getService(AMS_SERVICE_UUID);
  if (!ams)
  {
    Serial.println("AMS service not found (pairing rejected?)");
    return false;
  }
  BLERemoteCharacteristic *entityUpdate =
      ams->getCharacteristic(AMS_ENTITY_UPDATE_UUID);
  if (!entityUpdate)
  {
    Serial.println("Entity Update characteristic not found");
    return false;
  }

  entityUpdate->registerForNotify(onEntityUpdate);
  uint8_t playerSub[] = {ENTITY_PLAYER, PLAYER_ATTR_PLAYBACK_INFO};
  uint8_t trackSub[] = {ENTITY_TRACK, TRACK_ATTR_ARTIST, TRACK_ATTR_ALBUM,
                        TRACK_ATTR_TITLE, TRACK_ATTR_DURATION};
  entityUpdate->writeValue(playerSub, sizeof(playerSub), true);
  entityUpdate->writeValue(trackSub, sizeof(trackSub), true);
  return true;
}

void cleanupClient()
{
  clearPendingAmsUpdates();
  amsReady = false;
  authComplete = false;
  authenticatedAt = 0;
  np = NowPlaying();
  activeTrackKey = 0;
  appliedTrackGeneration = 0;
  trackMetadataChangedAt = 0;
  playbackInfoKnown = false;
  pauseRefreshPending = false;
  coverValid = false;
  coverForGeneration = 0;
  coverStatus = "";
  releaseLyricStorage();
  lyricsForGeneration = 0;
  lyricStatus = "";
  lyricsDirty = false;
  if (wifiStarted)
  {
    WiFi.disconnect();
    wifiStarted = false;
  }
  if (client)
  {
    if (client->isConnected())
      client->disconnect();
    delete client;
    client = nullptr;
  }
  screenDirty = !displayHeld;
  transportDirty = false;
  PERF_LOG("after BLE/WiFi cleanup");
}

void setup()
{
  Serial.begin(115200);
  PERF_LOG("boot");
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  // Keep GxEPD2's internal waveform diagnostics off; our sampled [perf]
  // timings contain the useful display information without serial noise.
  display.init(0, true, 50, false);
  display.setRotation(1); // landscape: 296 wide x 128 high
  display.setTextWrap(false);
  unicodeText.begin(display);
  unicodeText.setFontMode(1);
  unicodeText.setFontDirection(0);
  unicodeText.setForegroundColor(GxEPD_BLACK);
  unicodeText.setBackgroundColor(GxEPD_WHITE);
  displayReady = true;
  refreshDisplay(true);
  PERF_LOG("display initialized");

  // Joining is deliberately postponed until HID pairing and AMS discovery
  // are complete. BLE bonding gets the radio to itself during that phase.
  WiFi.mode(WIFI_STA);

  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT_MITM);
  BLEDevice::setSecurityCallbacks(new SecurityCallbacks());
  static BLESecurity security;
  // Keep the SMP authentication request consistent with the global MITM
  // encryption level. DisplayOnly + SC MITM Bond uses passkey entry on iOS.
  security.setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
  security.setCapability(ESP_IO_CAP_OUT);
  security.setKeySize(16);
  security.setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  security.setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

  server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  // Genuine HID-over-GATT service. The report is a media remote, not a
  // keyboard, so it won't interfere with the iPhone's on-screen keyboard.
  hidDevice = new BLEHIDDevice(server);
  // Populate the mandatory Device Information PnP value and readable HID
  // metadata. Leaving these characteristics empty makes iOS abandon HID
  // enumeration while pairing is still in progress.
  hidDevice->manufacturer()->setValue("MusicStation");
  hidDevice->pnp(0x02, 0x303A, 0x4001, 0x0100);
  hidDevice->hidInfo(0x00, 0x01);
  // BLEHIDDevice's legacy API is not const-correct; it copies this map into
  // the characteristic, so the immutable source can remain in flash.
  hidDevice->reportMap(const_cast<uint8_t *>(mediaReportMap),
                       sizeof(mediaReportMap));
  mediaInput = hidDevice->inputReport(1);
  uint8_t released = 0;
  mediaInput->setValue(&released, 1);
  hidDevice->startServices();
  // In Arduino-ESP32 3.2.x setBatteryLevel() may notify immediately when the
  // server is marked started. The battery service must be attached first or
  // BLECharacteristic::notify() asserts on a null server pointer.
  hidDevice->setBatteryLevel(100);
  PERF_LOG("BLE HID initialized");

  BLEAdvertising *advertising = server->getAdvertising();
  BLEAdvertisementData advertisement;
  advertisement.setFlags(ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
  // iOS Settings filters for supported profiles in the primary advertising
  // packet. Keep HID here (not only in the scan response) so MusicStation is
  // considered a system-pairable accessory. Flags + HID + AMS use 25/31 B.
  advertisement.setCompleteServices(BLEUUID((uint16_t)HID_SERVICE_UUID));
  addAmsSolicitation(advertisement);
  bool advDataOk = advertising->setAdvertisementData(advertisement);

  BLEAdvertisementData scanResponse;
  scanResponse.setAppearance(HID_GENERIC_APPEARANCE);
  scanResponse.setName(DEVICE_NAME);
  advertising->setScanResponse(true);
  bool scanDataOk = advertising->setScanResponseData(scanResponse);
  if (!advDataOk || !scanDataOk)
    Serial.printf("BLE advertising data failed: primary=%s scan=%s\n",
                  advDataOk ? "ok" : "FAILED", scanDataOk ? "ok" : "FAILED");

  // Raw advertising-data configuration completes asynchronously in Bluedroid.
  // Give both GAP events time to land before requesting advertising.
  delay(250);
  bool advertisingStarted = advertising->start();
  if (!advertisingStarted)
    Serial.println("BLE advertising failed to start");
  PERF_LOG("BLE advertising started");
}

void loop()
{
  static unsigned long lastAmsTry = 0;
  static unsigned long lastWifiTry = 0;
  static bool wifiWasUp = false;
  static uint64_t preparedTrackKey = 0;
  static int displayedLyricIndex = -2;
  if (phoneConnected && !wasConnected)
  {
    wasConnected = true;
    lastAmsTry = 0;
    if (!displayHeld)
    {
      screenDirty = true;
      refreshDisplay(false);
    }
  }
  else if (phoneConnected && wasConnected && authComplete && !amsReady &&
           millis() - authenticatedAt >= 500 &&
           (lastAmsTry == 0 || millis() - lastAmsTry > 10000))
  {
    lastAmsTry = millis();
    amsReady = startAmsClient();
    screenDirty = true;
    if (!amsReady)
      Serial.println("AMS setup failed; retrying in 10s");
  }
  else if (!phoneConnected && wasConnected)
  {
    bool keepNowPlayingFrame = amsReady && np.haveTrack;
    displayHeld = keepNowPlayingFrame;
    playbackInfoKnown = false;
    pauseRefreshPending = false;
    screenDirty = !keepNowPlayingFrame;
    transportDirty = false;
    lyricsDirty = false;
    wasConnected = false;
    lastAmsTry = 0;
    cleanupClient();
    delay(500);
    BLEDevice::startAdvertising();
  }

  if (phoneConnected)
    applyPendingAmsUpdates();

  // Authentication failure can leave a stale peer entry on the ESP side.
  // Remove only that failed peer after the link is down. The matching iPhone
  // entry must still be forgotten manually because firmware cannot erase it.
  if (!phoneConnected && failedBondPending.exchange(false))
  {
    esp_bd_addr_t address;
    memcpy(address, failedBondAddress, sizeof(esp_bd_addr_t));
    esp_err_t result = esp_ble_remove_bond_device(address);
    if (result != ESP_OK)
      Serial.println("Failed ESP bond could not be removed");
  }

  // Start the hotspot only after the BLE accessory is fully usable. Retry in
  // the background because an iPhone Personal Hotspot may appear after boot.
  if (amsReady && np.haveTrack && !wifiStarted)
  {
    wifiStarted = true;
    lastWifiTry = millis();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
  bool wifiUp = WiFi.status() == WL_CONNECTED;
  if (wifiUp != wifiWasUp)
  {
    wifiWasUp = wifiUp;
    if (wifiUp)
    {
      PERF_LOG("WiFi connected");
      coverForGeneration = 0; // retry after a late hotspot reconnect
      lyricsForGeneration = 0;
    }
  }
  if (wifiStarted && !wifiUp && millis() - lastWifiTry >= 15000)
  {
    lastWifiTry = millis();
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }

  // AMS sends artist/title/album/duration separately. Let those notifications
  // settle before searching, otherwise a new title can be paired with the old
  // artist. A failed lookup is attempted only once until Wi-Fi reconnects.
  if (activeTrackKey != preparedTrackKey)
  {
    preparedTrackKey = activeTrackKey;
    coverValid = false;
    coverForGeneration = 0;
    coverStatus = "ART...";
    releaseLyricStorage();
    lyricsForGeneration = 0;
    lyricStatus = "LYRICS...";
    lyricsDirty = true;
    displayedLyricIndex = -2;
    screenDirty = true;
  }
  else if (amsReady && np.haveTrack && np.duration > 0 && wifiUp &&
           activeTrackKey != 0 && appliedTrackGeneration != 0 &&
           (appliedTrackGeneration != coverForGeneration ||
            appliedTrackGeneration != lyricsForGeneration) &&
           millis() - trackMetadataChangedAt >= 1500)
  {
    if (appliedTrackGeneration != coverForGeneration)
      fetchCover();
    if (appliedTrackGeneration != lyricsForGeneration)
      fetchLyrics();
    PERF_LOG("track assets complete");
  }

  int lyricIndex = currentLyricIndex();
  if (lyricIndex != displayedLyricIndex)
  {
    displayedLyricIndex = lyricIndex;
    lyricsDirty = true;
  }

  unsigned long now = millis();
  // Metadata/status changes refresh the logical screen. Normal playback only
  // touches the inset bottom strip, avoiding a waveform around the perimeter.
  bool refreshDue = now - lastRefreshStartedAt >= PARTIAL_REFRESH_INTERVAL_MS;
  bool displayFrozen = displayHeld ||
                       (playbackInfoKnown && np.state == 0);
  if (!displayHeld && refreshDue && pauseRefreshPending)
  {
    refreshDisplay(false, true);
    pauseRefreshPending = false;
  }
  else if (!displayFrozen && refreshDue && screenDirty)
    refreshDisplay(false);
  else if (!displayFrozen && refreshDue && lyricsDirty)
    refreshDisplay(false, false, true);
  else if (!displayFrozen && refreshDue &&
           (transportDirty || (amsReady && np.haveTrack && np.state == 1)))
    refreshDisplay(false, true);

  delay(20);
}
