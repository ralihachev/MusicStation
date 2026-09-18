// MusicStation V2 pin map. See V2/WIRING.md for the reasoning behind each
// assignment and for the per-component wiring tables.
#pragma once

// E-paper, SPI2/FSPI. Unchanged from V1. The WeAct silkscreen labels the data
// and clock lines SDA and SCL, but the interface is SPI, not I2C.
#define EPD_SCK 12
#define EPD_MOSI 11
#define EPD_CS 10
#define EPD_DC 9
#define EPD_RST 8
#define EPD_BUSY 7

// microSD, SPI3/HSPI. The Adafruit 4682 uses SDIO pin names: in SPI mode CMD
// carries MOSI, DAT0 carries MISO, and DAT3 is chip select. The card gets its
// own SPI host so the M4 decoder never arbitrates with the panel for a bus.
#define SD_CLK 38
#define SD_CMD 39  // MOSI
#define SD_DAT0 40 // MISO
#define SD_DAT3 41 // CS
#define SD_CD 42

// Card detect is optional wiring. An unconnected pin floats to the internal
// pull-up and would read as "no card", so presence checks stay disabled until
// the pin is actually wired and its polarity confirmed with a meter.
#define SD_CD_ENABLED 0
#define SD_CD_ACTIVE_LOW 1

// Buttons, active low with internal pull-ups. Every one is an RTC GPIO
// (0-21) because only those can wake the ESP32-S3 from deep sleep in M6.
#define BTN_UP 4
#define BTN_DOWN 5
#define BTN_ENTER 6
#define BTN_BACK 15
#define BTN_MENU 16

// I2S to the MAX98357A. Reserved for M4; nothing drives these yet.
#define I2S_BCLK 17
#define I2S_LRCLK 18
#define I2S_DIN 21
#define AMP_SD 2

// Battery sense, ADC1_CH0. Reserved for M6.
#define BATTERY_SENSE 1
