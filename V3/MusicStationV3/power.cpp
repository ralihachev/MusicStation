#include "power.h"

#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#include "audio.h"
#include "pins.h"
#include "settings.h"

namespace
{
uint32_t lastActivityAt = 0;

const gpio_num_t WAKE_PINS[] = {
    (gpio_num_t)BTN_UP, (gpio_num_t)BTN_DOWN, (gpio_num_t)BTN_ENTER,
    (gpio_num_t)BTN_BACK, (gpio_num_t)BTN_MENU,
};

// A single cell through a 2:1 divider. Adjust after measuring against a known
// voltage; an uncalibrated guess here would be worse than reporting nothing.
constexpr float DIVIDER_RATIO = 2.0f;
constexpr float EMPTY_VOLTS = 3.30f;
constexpr float FULL_VOLTS = 4.15f;
} // namespace

void powerBegin()
{
  lastActivityAt = millis();
  analogReadResolution(12);
}

void powerNoteActivity() { lastActivityAt = millis(); }

float powerBatteryVolts()
{
  uint32_t millivolts = analogReadMilliVolts(BATTERY_SENSE);
  return (float)millivolts / 1000.0f * DIVIDER_RATIO;
}

int8_t powerBatteryPercent()
{
  float volts = powerBatteryVolts();
  if (volts < 2.5f || volts > 5.0f)
    return -1; // nothing plausible connected to the divider
  float fraction = (volts - EMPTY_VOLTS) / (FULL_VOLTS - EMPTY_VOLTS);
  if (fraction < 0.0f)
    fraction = 0.0f;
  if (fraction > 1.0f)
    fraction = 1.0f;
  return (int8_t)(fraction * 100.0f);
}

void powerSleepNow()
{
  audioSetMuted(true);
  audioStop();
  settingsSave();

  // GPIO state is lost in deep sleep. Released, AMP_SD floats, the breakout's
  // pull-up to VIN brings the amp out of shutdown, and it idles at a few mA
  // with the I2S lines floating into it. Latch it low until audioBegin()
  // releases it on the next boot.
  digitalWrite(AMP_SD, LOW);
  gpio_hold_en((gpio_num_t)AMP_SD);
  gpio_deep_sleep_hold_en();

  // Buttons are active low with pull-ups, so any-low is the matching wake
  // source. The pull-ups must be held by the RTC domain, otherwise they are
  // released on sleep and every pin floats low immediately.
  uint64_t mask = 0;
  for (gpio_num_t pin : WAKE_PINS)
  {
    rtc_gpio_pullup_en(pin);
    rtc_gpio_pulldown_dis(pin);
    mask |= 1ULL << (uint32_t)pin;
  }

  // ANY_LOW is an enum, not a macro, so it cannot be feature-tested with #if;
  // V2's attempt always fell through to the deprecated ALL_LOW. Core 3.x on
  // the S3 has ANY_LOW, which is the condition a single pressed button meets.
  esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);

  Serial.println("[power] sleeping; press any button to wake");
  Serial.flush();
  esp_deep_sleep_start();
}

void powerTick(bool playing)
{
  if (playing)
  {
    lastActivityAt = millis();
    return;
  }
  uint16_t minutes = settings().sleepMinutes;
  if (minutes == 0)
    return;
  if (millis() - lastActivityAt >= (uint32_t)minutes * 60000UL)
    powerSleepNow();
}
