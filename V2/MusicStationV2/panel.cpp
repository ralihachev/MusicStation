#include "panel.h"

#include <SPI.h>

GxEPD2_BW<QuietBorder290BS, QuietBorder290BS::HEIGHT> display(
    QuietBorder290BS(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));
U8G2_FOR_ADAFRUIT_GFX unicodeText;

const uint8_t *const LIST_FONT = u8g2_font_9x15_t_cyrillic;
const uint8_t *const SMALL_FONT = u8g2_font_5x8_t_cyrillic;
const uint8_t *const LYRIC_FONT = u8g2_font_unifont_t_cyrillic;

namespace
{
// u8g2_SetFont() resets is_transparent to 0 whenever the font pointer changes,
// which silently undoes setFontMode(1). In solid mode every glyph's bounding
// box is filled with bg_color first: invisible on white, but it paints white
// rectangles over an inverted selection bar. Every font change must therefore
// re-assert transparent mode.
void useFont(const uint8_t *font)
{
  unicodeText.setFont(font);
  unicodeText.setFontMode(1);
}

bool ready = false;
uint16_t partialCount = 0;
uint32_t lastRefreshStartedAt = 0;
} // namespace

PanelWindow panelRect(int16_t x, int16_t y, int16_t w, int16_t h)
{
  PanelWindow window;
  window.full = false;
  window.x = x;
  window.y = y;
  window.w = w;
  window.h = h;
  return window;
}

bool panelBegin()
{
  // MISO is unused; the panel is write-only.
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init(0, true, 50, false);
  display.setRotation(1); // landscape: 296 wide x 128 high
  display.setTextWrap(false);
  unicodeText.begin(display);
  unicodeText.setFontMode(1);
  unicodeText.setFontDirection(0);
  unicodeText.setForegroundColor(GxEPD_BLACK);
  unicodeText.setBackgroundColor(GxEPD_WHITE);
  ready = true;
  return ready;
}

bool panelReady() { return ready; }

void panelDraw(const PanelWindow &window, void (*draw)())
{
  if (!ready || draw == nullptr)
    return;

  bool full = window.full;
  if (partialCount >= FULL_REFRESH_COST_BUDGET)
    full = true;

  // Full cleaning updates use the panel's normal border waveform. Differential
  // partial updates hold the border driver in HiZ to prevent its thin flash.
  display.epd2.setQuietBorder(!full);

  // Stamp the start of the blocking waveform rather than its completion, so
  // cadence is measured against when the panel became busy.
  lastRefreshStartedAt = millis();

  if (full)
    display.setFullWindow();
  else
    display.setPartialWindow(window.x, window.y, window.w, window.h);

  display.firstPage();
  do
  {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    draw();
  } while (display.nextPage());

  // Keep the border quiet between updates and prepare the next partial cycle,
  // exactly as V1 does.
  display.epd2.setQuietBorder(true);

  if (full)
  {
    partialCount = 0;
  }
  else
  {
    uint32_t area = (uint32_t)window.w * window.h;
    uint32_t cost = (area * FULL_SCREEN_COST) /
                    ((uint32_t)PANEL_W * PANEL_H);
    partialCount += (uint16_t)(cost < 1 ? 1 : cost);
  }
}

uint32_t panelLastRefreshStartedAt() { return lastRefreshStartedAt; }
uint16_t panelPartialCount() { return partialCount; }

int panelTextWidth(const String &text, const uint8_t *font)
{
  useFont(font);
  return unicodeText.getUTF8Width(text.c_str());
}

namespace
{
// Length of the UTF-8 sequence beginning at `start`, clamped to the string.
// Malformed input advances one byte so a caller cannot loop forever.
size_t nextUtf8Boundary(const String &text, size_t start)
{
  if (start >= text.length())
    return text.length();
  uint8_t first = (uint8_t)text[start];
  size_t bytes = first < 0x80 ? 1 : first < 0xE0 ? 2 : first < 0xF0 ? 3 : 4;
  size_t end = start + bytes;
  if (end > text.length())
    end = text.length();
  for (size_t i = start + 1; i < end; i++)
    if (((uint8_t)text[i] & 0xC0) != 0x80)
      return start + 1;
  return end;
}

int utf8GlyphWidth(const String &text, size_t start, size_t end)
{
  char glyph[5];
  size_t length = end - start;
  if (length > sizeof(glyph) - 1)
    length = sizeof(glyph) - 1;
  memcpy(glyph, text.c_str() + start, length);
  glyph[length] = 0;
  return unicodeText.getUTF8Width(glyph);
}
} // namespace

String panelFitText(const String &text, const uint8_t *font, int maxWidth)
{
  useFont(font);
  if (unicodeText.getUTF8Width(text.c_str()) <= maxWidth)
    return text;

  int available = maxWidth - unicodeText.getUTF8Width("...");
  if (available < 0)
    available = 0;

  int width = 0;
  size_t boundary = 0;
  while (boundary < text.length())
  {
    size_t next = nextUtf8Boundary(text, boundary);
    int glyphWidth = utf8GlyphWidth(text, boundary, next);
    if (width + glyphWidth > available)
      break;
    width += glyphWidth;
    boundary = next;
  }

  String fitted;
  fitted.reserve(boundary + 3);
  fitted.concat(text.c_str(), boundary);
  fitted += "...";
  return fitted;
}

void panelText(int16_t x, int16_t baseline, const String &text,
               const uint8_t *font)
{
  useFont(font);
  unicodeText.setCursor(x, baseline);
  unicodeText.print(text);
}

void panelTextRight(int16_t right, int16_t baseline, const String &text,
                    const uint8_t *font)
{
  panelText(right - panelTextWidth(text, font), baseline, text, font);
}

void panelTextCentered(int16_t baseline, const String &text,
                       const uint8_t *font)
{
  panelText((PANEL_W - panelTextWidth(text, font)) / 2, baseline, text, font);
}
