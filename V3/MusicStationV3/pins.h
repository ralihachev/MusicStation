// MusicStation V3 pin map. See V3/WIRING.md for the reasoning behind each
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

// microSD over SDIO in 1-bit mode, on the S3's SDMMC peripheral. SPI mode
// could not read any file over 1 KB on this card (see storage.h), so V3 uses
// the card's native bus on the same wires. DAT3 is unused by 1-bit SDIO but is
// held high so the card never powers up into SPI mode.
#define SD_CLK 38
#define SD_CMD 39
#define SD_DAT0 40
#define SD_DAT3 41
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

// I2S to the MAX98357A. With two amps, every one of these lines is shared:
// both boards hang off the same BCLK, LRC, DIN and SD wires.
#define I2S_BCLK 17
#define I2S_LRCLK 18 // the breakout labels it LRC
#define I2S_DIN 21
#define AMP_SD 2     // low = shutdown, high = on (and "left", see audio.cpp)

// Battery sense, ADC1_CH0. Reserved for M6.
#define BATTERY_SENSE 1
