// MusicStation V3 — offline microSD music station for ESP32-S3 + 2.9" e-paper,
// playing through MAX98357A I2S amplifiers.
//
// The card holds the music, the lyrics and the artwork; the station indexes it,
// browses it, and plays it with no phone, no network and no cloud service.
//
// Hold BACK while powering on for the bring-up self-test, which reports the
// panel, the card, the amplifier and each button independently. Follow the
// bring-up order in V3/WIRING.md and add one peripheral at a time.
//
// Set AUDIO_HARDWARE to 0 in audio.h to fall back to V2's silent simulated
// playhead, for screen work with no amplifier attached.
//
// No BLE, no Wi-Fi, no TLS, no JSON, no JPEG. See V3/README.md.

#include "audio.h"
#include "input.h"
#include "library.h"
#include "panel.h"
#include "pins.h"
#include "player.h"
#include "playlist.h"
#include "power.h"
#include "settings.h"
#include "storage.h"
#include "ui.h"

namespace
{

bool bringUpMode = false;
bool libraryReady = false;
bool screenDirty = true;
bool fullRefreshPending = false;

// A clock tick that has been deferred, so it is not lost while it waits.
bool transportOwed = false;

// A partial refresh blocks the panel for its measured duration. Starting a
// clock tick closer than that to a lyric would make the lyric wait behind it.
// The clock skips a second instead; nobody notices a skipped second,
// everybody notices a late line. The margin covers loop latency.
constexpr uint32_t LYRIC_GUARD_MARGIN_MS = 150;

// ---------------------------------------------------------------- bring-up

struct BringUpState
{
  bool sdMounted = false;
  int rootEntries = -1;
  String lastEvent = "none";
  uint32_t eventCount = 0;
  uint8_t tonesPlayed = 0;
};

// Test tones for the bring-up screen: UP plays the next one. Three distinct
// pitches make it obvious that each press reached the amp, rather than one
// long tone that might be a stuck buffer.
const uint16_t TEST_TONES_HZ[] = {440, 660, 880};
constexpr uint8_t TEST_TONE_COUNT = 3;
constexpr uint16_t TEST_TONE_MS = 700;

BringUpState bringUp;

void refreshCardStatus()
{
  storageEnd(); // release the old mount before re-running SD.begin()
  bringUp.sdMounted = storageBegin();
  bringUp.rootEntries = bringUp.sdMounted ? storageCountEntries("/") : -1;
}

void drawStatusRow(int16_t baseline, const char *label, const String &value)
{
  panelText(4, baseline, label, LIST_FONT);
  panelText(86, baseline, panelFitText(value, LIST_FONT, PANEL_W - 90),
            LIST_FONT);
}

// A held button inverts its box, which is the fastest way to confirm that
// every switch reaches its own GPIO and that none are shorted or floating.
void drawButtonBoxes(int16_t top)
{
  constexpr int16_t boxW = 56;
  constexpr int16_t boxH = 18;
  constexpr int16_t gap = 3;
  for (uint8_t i = 0; i < ACT_COUNT; i++)
  {
    int16_t x = 2 + i * (boxW + gap);
    Action action = (Action)i;
    bool held = inputHeld(action);
    if (held)
      display.fillRect(x, top, boxW, boxH, GxEPD_BLACK);
    else
      display.drawRect(x, top, boxW, boxH, GxEPD_BLACK);

    String name = actionName(action);
    int16_t textX = x + (boxW - panelTextWidth(name, SMALL_FONT)) / 2;
    if (held)
      unicodeText.setForegroundColor(GxEPD_WHITE);
    panelText(textX, top + 12, name, SMALL_FONT);
    if (held)
      unicodeText.setForegroundColor(GxEPD_BLACK);
  }
}

void drawBringUpScreen()
{
  panelText(4, 12, "MUSICSTATION V3", LIST_FONT);
  panelTextRight(PANEL_W - 4, 12, "BRING-UP", SMALL_FONT);
  display.drawFastHLine(0, 16, PANEL_W, GxEPD_BLACK);

  drawStatusRow(32, "Display",
                String("OK  ") + PANEL_W + "x" + PANEL_H + " SSD1680");

  if (bringUp.sdMounted)
    drawStatusRow(48, "microSD",
                  String("OK  ") + storageCardTypeName() + " " +
                      storageFormatBytes(storageCard().sizeBytes) + ", " +
                      (bringUp.rootEntries >= 0
                           ? String(bringUp.rootEntries) + " in /"
                           : String("root unreadable")));
  else
    drawStatusRow(48, "microSD", "FAIL  MENU retries");

  // The panel cannot hear, so this row only says what was sent; the speaker
  // is the actual test.
  String audioRow = audioBackendName();
  if (AUDIO_HARDWARE)
    audioRow += bringUp.tonesPlayed
                    ? String("  ") + bringUp.tonesPlayed + " tones sent"
                    : String("  UP plays a tone");
  drawStatusRow(64, "Audio", audioRow);

  display.drawFastHLine(0, 72, PANEL_W, GxEPD_BLACK);
  drawButtonBoxes(78);

  panelText(4, 110, panelFitText(bringUp.lastEvent, SMALL_FONT, PANEL_W - 60),
            SMALL_FONT);
  panelTextRight(PANEL_W - 4, 110, String(bringUp.eventCount) + " ev",
                 SMALL_FONT);
  panelTextRight(PANEL_W - 4, 122, String("partials ") + panelPartialCount(),
                 SMALL_FONT);
}

// ----------------------------------------------------------------- library

bool openLibrary()
{
  libraryReady = false;
  if (!storageMounted())
  {
    uiShowMessage("NO CARD", "Insert a card, MENU retries");
    return false;
  }

  bool opened = libraryOpen();
  if (!opened || libraryNeedsRebuild())
  {
    // Blocks for as long as the scan takes. uiBuildProgress throttles its own
    // refreshes so drawing does not dominate the scan.
    if (!libraryBuild(uiBuildProgress))
    {
      uiShowMessage("NO LIBRARY", "Add /Music to the card");
      return false;
    }
  }
  if (libraryTrackCount() == 0)
  {
    uiShowMessage("EMPTY", "No tracks under /Music");
    return false;
  }

  libraryReady = true;
  playlistRefresh();
  return true;
}

void logBanner()
{
  Serial.println();
  Serial.println("MusicStation V3");
  Serial.printf("  e-paper  BUSY=%d RST=%d DC=%d CS=%d MOSI=%d SCK=%d\n",
                EPD_BUSY, EPD_RST, EPD_DC, EPD_CS, EPD_MOSI, EPD_SCK);
  Serial.printf("  microSD  SDIO 1-bit CLK=%d CMD=%d DAT0=%d (DAT3=%d held high)\n",
                SD_CLK, SD_CMD, SD_DAT0, SD_DAT3);
  Serial.printf("  buttons  UP=%d DOWN=%d ENTER=%d BACK=%d MENU=%d\n", BTN_UP,
                BTN_DOWN, BTN_ENTER, BTN_BACK, BTN_MENU);
  Serial.printf("  audio    %s  BCLK=%d LRC=%d DIN=%d SD=%d\n",
                audioBackendName(), I2S_BCLK, I2S_LRCLK, I2S_DIN, AMP_SD);
  Serial.println("  hold BACK at boot for the bring-up self-test");
  Serial.println();
}

// ---------------------------------------------------------------- renderer

// Picks the smallest region that covers what actually changed. Region size
// barely affects refresh time, but a smaller window leaves the rest of the
// frame untouched, so the artwork is not driven again for a lyric change and
// neither is driven for a clock tick.
void renderIfNeeded()
{
  if (!inputSettled())
    return;

  // A due cleanup turns the next refresh into a whole-screen one, so it can
  // run without blanking whatever a partial window would have left out.
  bool full = fullRefreshPending || uiWantsFullRefresh() || panelCleanupDue();
  bool lyricChanged = playerTakeLyricChanged();
  // Both are one-shot flags, so take both rather than short-circuiting.
  bool playerTransport = playerTakeTransportChanged();
  bool uiTransport = uiTakeTransportChanged();
  bool transportChanged = playerTransport || uiTransport;
  bool trackChanged = playerTakeTrackChanged();

  if (bringUpMode)
  {
    if (!screenDirty)
      return;
    screenDirty = false;
    fullRefreshPending = false;
    panelDraw(full ? PANEL_FULL : panelRect(0, 0, PANEL_W, PANEL_H),
              drawBringUpScreen);
    return;
  }

  bool wholeScreen = screenDirty || trackChanged || full;
  if (!wholeScreen && !lyricChanged && !transportChanged && !transportOwed)
    return;

  // Away from Now Playing there are no partial regions to choose between.
  if (!uiOnNowPlaying())
  {
    if (!wholeScreen)
      return;
    screenDirty = false;
    fullRefreshPending = false;
    uiClearFullRefresh();
    panelDraw(full ? PANEL_FULL : panelRect(0, 0, PANEL_W, PANEL_H), uiDraw);
    return;
  }

  transportOwed = transportOwed || transportChanged;

  if (wholeScreen)
  {
    screenDirty = false;
    fullRefreshPending = false;
    transportOwed = false;
    uiClearFullRefresh();
    panelDraw(full ? PANEL_FULL : panelRect(0, 0, PANEL_W, PANEL_H), uiDraw);
  }
  else if (lyricChanged)
  {
    // The lyric and the clock share the right column, so one refresh brings
    // both up to date. Two back to back would hold the next line for 780 ms.
    transportOwed = false;
    panelDraw(uiRightColumnWindow(), uiDrawRightColumn);
  }
  else if (transportOwed)
  {
    if (playerMsUntilNextLyric() <
        panelPartialRefreshMs() + LYRIC_GUARD_MARGIN_MS)
      return; // keep the panel free; the lyric refresh will draw the clock
    transportOwed = false;
    panelDraw(uiTransportWindow(), uiDrawTransport);
  }
}

} // namespace

void setup()
{
  Serial.begin(115200);
  // USB CDC discards anything written before the host opens the port, so the
  // boot banner is lost without this. Bounded, because the station must still
  // boot with nothing attached.
  uint32_t serialWaitStart = millis();
  while (!Serial && millis() - serialWaitStart < 2000)
    delay(10);
  delay(200);
  logBanner();

  panelBegin();
  inputBegin();

  // inputBegin() seeds held state from the pins, so a button already down at
  // boot is visible here without waiting for an edge.
  bringUpMode = inputHeld(ACT_BACK);
  if (bringUpMode)
  {
    Serial.println("[boot] bring-up self-test");
    // The amp is tested without the card, so a dead card cannot hide a
    // working speaker or the other way round.
    audioBegin();
    audioSetVolume(8);
    refreshCardStatus();
    panelDraw(PANEL_FULL, drawBringUpScreen);
    return;
  }

  settingsBegin();
  storageBegin();
  playerBegin();
  powerBegin();

  if (openLibrary())
  {
    uiBegin();
    // The queue is restored so resume still works, but boot lands on the
    // library rather than dropping straight into a track nobody asked for.
    playerRestore();
    uiStartInLibrary();
  }

  panelDraw(PANEL_FULL, uiDraw);
  uiClearFullRefresh();
  playerTakeTrackChanged();
  playerTakeLyricChanged();
  playerTakeTransportChanged();
}

void loop()
{
  InputEvent event;

  while (inputNext(event))
  {
    powerNoteActivity();

    if (bringUpMode)
    {
      bringUp.eventCount++;
      bringUp.lastEvent = String(actionName(event.action)) + " " +
                          eventKindName(event.kind) +
                          (event.fast ? " fast" : "");
      Serial.printf("[input] %-5s %-7s%s at %lu\n", actionName(event.action),
                    eventKindName(event.kind), event.fast ? " fast" : "",
                    (unsigned long)event.at);
      if (event.kind == EV_PRESS)
      {
        if (event.action == ACT_ENTER)
          fullRefreshPending = true; // manual ghosting cleanup
        else if (event.action == ACT_MENU)
          refreshCardStatus();
        else if (event.action == ACT_UP && !audioToneActive() &&
                 audioPlayTestTone(
                     TEST_TONES_HZ[bringUp.tonesPlayed % TEST_TONE_COUNT],
                     TEST_TONE_MS))
          bringUp.tonesPlayed++;
      }
      screenDirty = true;
      continue;
    }

    // Retrying a failed mount has to work from the message screen, where the
    // navigation tree does not exist yet.
    if (event.kind == EV_PRESS && event.action == ACT_MENU && !libraryReady)
    {
      storageEnd();
      storageBegin();
      if (openLibrary())
      {
        uiBegin();
        playerRestore();
      }
      fullRefreshPending = true;
      screenDirty = true;
      continue;
    }

    if (uiHandleEvent(event))
      screenDirty = true;
  }

  if (bringUpMode)
    audioTick(); // finishes test tones
  else
  {
    playerTick();

    if (uiTakeRescanRequest())
    {
      playerStop();
      libraryBuild(uiBuildProgress);
      libraryReady = libraryTrackCount() > 0;
      playlistRefresh();
      uiBegin();
      fullRefreshPending = true;
      screenDirty = true;
    }
  }

  renderIfNeeded();

  if (!bringUpMode)
    powerTick(playerIsPlaying());

  delay(20);
}
