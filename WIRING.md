# MusicStation e-paper wiring

Target hardware:

- ESP32-S3 module or development board using ESP32-S3-WROOM-1U
- WeAct Studio 2.9-inch **black/white** 296x128 e-paper module
  (DEPG0290BS, SSD1680)

## Connections

| WeAct pin | ESP32-S3 GPIO | Purpose |
|---|---:|---|
| VCC | 3V3 | 3.3 V power |
| GND | GND | Common ground |
| SDA | GPIO11 | SPI MOSI (not I2C SDA) |
| SCL | GPIO12 | SPI clock (not I2C SCL) |
| CS | GPIO10 | SPI chip select |
| D/C | GPIO9 | Data/command select |
| RES | GPIO8 | Display reset |
| BUSY | GPIO7 | Display busy output |

The display does not use MISO. Power the WeAct module from **3.3 V**, not 5 V.
The six signal lines use 3.3 V logic, so no level shifter is needed.

If this is a bare WROOM-1U module rather than a development board, it also
needs a correct 3.3 V supply, enable/boot circuitry, USB/UART programming
connection, and the hardware design specified by Espressif. The table above
only covers the e-paper connection.

## Arduino setup

1. Select the board profile appropriate to the carrier board. `ESP32S3 Dev
   Module` works for many generic boards.
2. Install `GxEPD2` by Jean-Marc Zingg. Also install `ArduinoJson`, `JPEGDEC`,
   and `U8g2_for_Adafruit_GFX`; Adafruit GFX is installed as a dependency.
3. Copy `config.example.h` to the ignored `config.h` and enter the Wi-Fi
   credentials used by MusicStation.
4. Compile and upload `MusicStation.ino`.
5. If an older LightBlue bond exists, remove/forget it and restart both the
   ESP32 and iPhone Bluetooth.
6. Pair with `MusicStation` from iOS **Settings > Bluetooth**, then enter the
   passkey shown on the e-paper display. LightBlue is no longer required.

MusicStation advertises a genuine BLE HID Consumer Control service alongside
the Apple Media Service solicitation. This lets iOS own the bonded connection
and keep it alive when the screen is locked. The HID report is idle for now;
play/pause and track controls can be connected to inputs later.

Album art and synchronized LRCLIB lyrics are fetched after BLE pairing from
the Wi-Fi network configured in local `config.h`. When using an iPhone hotspot,
keep Personal Hotspot enabled while testing. If
the ESP32-S3 cannot see it, enable **Maximize Compatibility** on the iPhone so
the hotspot uses 2.4 GHz.

The added Wi-Fi, HTTPS, JSON, and JPEG libraries make the sketch substantially
larger. If Arduino reports that the sketch is too large, select a partition
scheme with a larger application slot, such as **Huge APP**.

The active screen uses the SSD1680 fast differential partial waveform about
every 800 ms while music is playing. A slow full waveform is used at startup
and after 300 partial updates to clean accumulated ghosting.

For a three-color WeAct panel, the driver class is different; do not use the
black/white configuration in the current sketch unchanged.
