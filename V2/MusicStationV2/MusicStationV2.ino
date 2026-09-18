// MusicStation V2 — offline microSD music station for ESP32-S3 + 2.9" e-paper.
//
// The card holds the music, the lyrics and the artwork; the station indexes it,
// browses it, and plays it with no phone, no network and no cloud service.
//
// Hold BACK while powering on for the bring-up self-test, which reports the
// panel, the card and each button independently. Follow the bring-up order in
// V2/WIRING.md and add one peripheral at a time.
//
// Audio runs on a simulated playhead until AUDIO_HARDWARE is set to 1 in
// audio.h, which is what lets every screen and the lyric scheduler be finished
// before the amplifier is fitted.
//
// No BLE, no Wi-Fi, no TLS, no JSON, no JPEG. See V2/README.md.

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

// ---------------------------------------------------------------- bring-up

struct BringUpState
{
  bool sdMounted = false;
  int rootEntries = -1;
  String lastEvent = "none";
  uint32_t eventCount = 0;
};

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
  panelText(4, 12, "MUSICSTATION V2", LIST_FONT);
  panelTextRight(PANEL_W - 4, 12, "BRING-UP", SMALL_FONT);
  display.drawFastHLine(0, 16, PANEL_W, GxEPD_BLACK);

  drawStatusRow(32, "Display",
                String("OK  ") + PANEL_W + "x" + PANEL_H + " SSD1680");

  if (bringUp.sdMounted)
  {
    drawStatusRow(48, "microSD",
                  String("OK  ") + storageCardTypeName() + " " +
                      storageFormatBytes(storageCard().sizeBytes));
    drawStatusRow(64, "Root",
                  bringUp.rootEntries >= 0
                      ? String(bringUp.rootEntries) + " entries"
                      : String("unreadable"));
  }
  else
  {
    drawStatusRow(48, "microSD", "FAIL  check wiring / FAT32");
    drawStatusRow(64, "Root", "MENU retries mount");
  }

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
  Serial.println("MusicStation V2");
  Serial.printf("  e-paper  BUSY=%d RST=%d DC=%d CS=%d MOSI=%d SCK=%d\n",
                EPD_BUSY, EPD_RST, EPD_DC, EPD_CS, EPD_MOSI, EPD_SCK);
  Serial.printf("  microSD  CLK=%d CMD/MOSI=%d DAT0/MISO=%d DAT3/CS=%d\n",
                SD_CLK, SD_CMD, SD_DAT0, SD_DAT3);
  Serial.printf("  buttons  UP=%d DOWN=%d ENTER=%d BACK=%d MENU=%d\n", BTN_UP,
                BTN_DOWN, BTN_ENTER, BTN_BACK, BTN_MENU);
  Serial.printf("  audio    %s\n", audioBackendName());
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

  bool full = fullRefreshPending || uiWantsFullRefresh();
  bool lyricChanged = playerTakeLyricChanged();
  bool transportChanged = playerTakeTransportChanged();
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
  if (!wholeScreen && !lyricChanged && !transportChanged)
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

  if (wholeScreen)
  {
    screenDirty = false;
    fullRefreshPending = false;
    uiClearFullRefresh();
    panelDraw(full ? PANEL_FULL : panelRect(0, 0, PANEL_W, PANEL_H), uiDraw);
  }
  else if (lyricChanged)
  {
    panelDraw(uiLyricWindow(), uiDrawLyricColumn);
    // A lyric refresh redraws the transport strip's neighbour, not the strip
    // itself, so a pending tick still owes a refresh.
    if (transportChanged)
      panelDraw(uiTransportWindow(), uiDrawTransport);
  }
  else
  {
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

  if (!bringUpMode)
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
