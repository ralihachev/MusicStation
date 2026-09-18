// E-paper panel: driver, refresh discipline, and UTF-8 text helpers.
//
// The panel costs roughly 780 ms per refresh regardless of how much of it is
// redrawn, because the SSD1680 partial waveform is a fixed frame sequence. The
// whole UI is therefore built to minimise how OFTEN this module is called, not
// how much each call draws. See V2/UI_DESIGN.md.
#pragma once

#include <Arduino.h>
#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>

#include "pins.h"

// GxEPD2 rewrites the SSD1680 border waveform on every update, which makes the
// physical border flash. Exposing the protected command helpers lets partial
// updates leave VBD in HiZ instead. Carried over from V1 unchanged.
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

extern GxEPD2_BW<QuietBorder290BS, QuietBorder290BS::HEIGHT> display;
extern U8G2_FOR_ADAFRUIT_GFX unicodeText;

// Landscape after setRotation(1).
constexpr int16_t PANEL_W = 296;
constexpr int16_t PANEL_H = 128;

// Differential partial updates lose contrast as they stack and the black
// drifts toward grey, so a full-LUT cleanup waveform periodically restores it.
//
// How fast that happens depends on how much of the panel each update drives,
// not on how many updates there are. V1 gets away with 300 because its partials
// are a 184x20 transport strip; DeskPomodoro needs 25 because it redraws most
// of the screen. V2 does both, so the counter is charged by area: a full-screen
// update costs 8, the lyric column 2, the transport strip 1. The budget below
// then works out at roughly 25 full-screen updates or 200 transport ticks,
// matching each project's proven figure.
constexpr uint16_t FULL_REFRESH_COST_BUDGET = 200;
constexpr uint16_t FULL_SCREEN_COST = 8;

// Adafruit GFX's bundled fonts are Latin-only. These U8g2 fonts carry
// printable ASCII and Cyrillic, so metadata renders as glyphs rather than as
// raw UTF-8 bytes. Row pitch in the list views is sized around LIST_FONT.
extern const uint8_t *const LIST_FONT;  // 9x15
extern const uint8_t *const SMALL_FONT; // 5x8
extern const uint8_t *const LYRIC_FONT; // unifont

struct PanelWindow
{
  bool full;
  int16_t x, y, w, h;
};

constexpr PanelWindow PANEL_FULL = {true, 0, 0, 0, 0};
PanelWindow panelRect(int16_t x, int16_t y, int16_t w, int16_t h);

bool panelBegin();
bool panelReady();

// Runs `draw` inside GxEPD2's paged loop against `window`, handling the quiet
// border, the cleanup-waveform counter, and refresh bookkeeping. This is the
// only function that may block for a waveform, which keeps the cost of a
// refresh visible at every call site.
void panelDraw(const PanelWindow &window, void (*draw)());

uint32_t panelLastRefreshStartedAt();
uint16_t panelPartialCount();

// Text. Widths are measured in UTF-8 codepoints, never bytes, so multi-byte
// glyphs are neither split nor counted twice.
int panelTextWidth(const String &text, const uint8_t *font);
String panelFitText(const String &text, const uint8_t *font, int maxWidth);
void panelText(int16_t x, int16_t baseline, const String &text,
               const uint8_t *font);
void panelTextRight(int16_t right, int16_t baseline, const String &text,
                    const uint8_t *font);
void panelTextCentered(int16_t baseline, const String &text,
                       const uint8_t *font);
