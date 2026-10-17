# MusicStation V3 wiring

V3 is V2 plus sound. Everything V2 wired stays where it is; the new parts are
one or two MAX98357A I2S amplifier breakouts and 8 ohm speakers.

Target hardware:

- ESP32-S3-WROOM-1U module or devkit
- WeAct Studio 2.9" black/white 296x128 e-paper (DEPG0290BS, SSD1680)
- Adafruit 4682 microSD breakout, **3V only**, driven over **SDIO**
- 5 tactile buttons
- MAX98357A I2S class-D amplifier breakout (the purple 7-pin board with a
  green screw terminal), one per speaker
- 3525 cavity speaker, 8 ohm 2 W, on a 1.25 mm two-wire lead, one per amp

All logic is 3.3 V and no level shifters are needed. The amplifiers are the
only thing on **5 V**, and only on their `VIN` pin.

## Quick reference: every connection

One row per physical wire. Silkscreen label on the left, ESP32-S3 on the right.
Rows marked *new* are the only change from V2.

| Component | Its pin | Connects to |
|---|---|---|
| **E-paper** WeAct 2.9" | VCC | **3V3** |
| | GND | **GND** |
| | SDA | **GPIO11** (SPI MOSI, not I2C) |
| | SCL | **GPIO12** (SPI clock, not I2C) |
| | CS | **GPIO10** |
| | D/C | **GPIO9** |
| | RES | **GPIO8** |
| | BUSY | **GPIO7** |
| **microSD** Adafruit 4682 | 3V | **3V3** (never 5 V) |
| | GND | **GND** |
| | CLK | **GPIO38** (SDIO clock) |
| | CMD | **GPIO39** (SDIO command) |
| | DAT0 | **GPIO40** (SDIO data) |
| | DAT3 | **GPIO41** (held high by firmware) |
| | DAT1, DAT2 | not connected |
| | CD | **GPIO42** (optional card detect) |
| **Button UP** | one leg / diagonal leg | **GPIO4** / **GND** |
| **Button DOWN** | one leg / diagonal leg | **GPIO5** / **GND** |
| **Button ENTER** | one leg / diagonal leg | **GPIO6** / **GND** |
| **Button BACK** | one leg / diagonal leg | **GPIO15** / **GND** |
| **Button MENU** | one leg / diagonal leg | **GPIO16** / **GND** |
| **Amp** MAX98357A *new* | VIN | **5V** (devkit `5V` / `5V0` / `VBUS` pin) |
| | GND | **GND** |
| | BCLK | **GPIO17** |
| | LRC | **GPIO18** |
| | DIN | **GPIO21** |
| | SD | **GPIO2** |
| | GAIN | not connected (9 dB) |
| | screw `+` | speaker **red** |
| | screw `-` | speaker **black** |

The microSD wires are the same physical wires as V2. Only the protocol on them
changed, from SPI to SDIO; see [microSD](#microsd--adafruit-4682-over-sdio).

## Amplifier and speaker

```
                     MAX98357A breakout
                  +----------------------+
   GPIO18 ------- | LRC                  |
   GPIO17 ------- | BCLK            +  o-|------ speaker red
   GPIO21 ------- | DIN          [screw] |
   (open)         | GAIN            -  o-|------ speaker black
   GPIO2  ------- | SD                   |
   GND    ------- | GND                  |
   5V     ------- | VIN                  |
                  +----------------------+
```

| Breakout pin | ESP32-S3 | Notes |
|---|---:|---|
| LRC | GPIO18 | Word select, also called LRCLK or WS |
| BCLK | GPIO17 | Bit clock |
| DIN | GPIO21 | Audio data from the ESP32 |
| GAIN | — | Leave open: 9 dB, right for an 8 ohm speaker on 5 V |
| SD | GPIO2 | Shutdown/mute, driven by firmware |
| GND | GND | |
| VIN | 5V | 3.3 V also works, at roughly half the loudness |

### The speaker lead

The 3525 speakers come on a 1.25 mm two-pin plug, which does not fit anything
on the amp. Cut the plug off, leaving as much lead as you can, then strip 4-5 mm
of each wire, twist the strands and tin them, or crimp a small ferrule.
Untinned strands splay under a screw terminal and short across to the other
side.

- **Red to `+`, black to `-`.** With one speaker polarity is cosmetic. With two
  it matters: wire both the same way, or they partly cancel and the bass
  vanishes.
- **Never connect either speaker wire to GND.** The output is bridged: both
  terminals are driven, so neither one is ground. A speaker terminal tied to
  GND shorts an output stage.
- Do not put two speakers on one amp. Use one amp per speaker; that is what the
  spare boards are for.

### Two speakers: wire the second amp in parallel

The firmware mixes to mono and sends the same signal in both I2S slots, so a
second amplifier needs **no code change** and no extra GPIOs. Every header pin
on amp 2 goes to the same place as on amp 1:

```
   GPIO18 --+-- amp1 LRC        5V  --+-- amp1 VIN
            +-- amp2 LRC              +-- amp2 VIN
   GPIO17 --+-- amp1 BCLK       GND --+-- amp1 GND
            +-- amp2 BCLK             +-- amp2 GND
   GPIO21 --+-- amp1 DIN
            +-- amp2 DIN        amp1 +/- -> speaker 1
   GPIO2  --+-- amp1 SD         amp2 +/- -> speaker 2
            +-- amp2 SD
```

On a breadboard, put both amps on the same rows so the header pins line up, and
link them pin for pin. Both amps mute together because they share GPIO2.

The third board is a spare. Keep it that way until the first two work.

### Why GPIO2 drives SD, and what it actually does

`SD` is a combined shutdown and channel-select input, read as a voltage:

| SD voltage | MAX98357A output |
|---|---|
| below 0.16 V | Shutdown |
| 0.16 - 0.77 V | (Left + Right) / 2 |
| 0.77 - 1.4 V | Right channel |
| above 1.4 V | Left channel |

> V2's WIRING.md has the middle two rows swapped. The table above is the
> datasheet's order. The breakout ties SD to VIN through a 1 M resistor, which
> against the chip's internal 100 k pull-down sits at about 0.45 V on 5 V:
> that is the stereo mix, which is the breakout's documented default.

Driving SD from a GPIO therefore gives **shutdown at LOW** and **left channel at
HIGH**. Since the firmware writes the mono mix to both channels, "left" carries
the full signal and the GPIO becomes a clean mute. The firmware uses it to:

- stay silent through boot and while paused,
- cut the pop during track changes and seeks,
- keep the amp fully off in deep sleep. GPIO2 is latched low across sleep;
  otherwise it would float, the 1 M pull-up would wake the amp, and it would sit
  there at a few mA hissing into a sleeping station.

GPIO2 is not a strapping pin on the ESP32-S3 (on the original ESP32 it is), so
the amp on it cannot upset boot.

### Optional: true stereo

Two speakers in one small box sit too close together for stereo to be audible,
so this is not recommended, but it is supported. Build with `AUDIO_STEREO 1` in
`audio.h` and bias amp 2's SD into its right-channel window:

- amp 1 SD: GPIO2 directly (left)
- amp 2 SD: GPIO2 through a **330 k** resistor (right)

With GPIO2 high that lands near 1.05 V, inside 0.77-1.4 V, assuming this clone
has the same 1 M pull-up as the reference board. **Measure amp 2's SD pin with
a meter while a track plays before trusting it**, and adjust the resistor if it
is outside the window. The cost: with GPIO2 low, amp 2's SD sits near 0.36 V,
which is "mix", not "shutdown". It goes silent from the firmware's digital
silence rather than the mute, and it stays powered in deep sleep.

## Power

| Load | Draw |
|---|---|
| ESP32-S3, radios off | ~40 mA |
| e-paper during refresh | ~15 mA for about 1 s |
| microSD active | 20-100 mA in bursts |
| MAX98357A idle | ~2.5 mA each; ~0 in shutdown |
| MAX98357A at full volume into 8 ohm | ~300-400 mA peaks each |

An 8 ohm speaker on 5 V tops out around 1.4 W per amp, inside the speakers'
2 W rating. One amp is comfortable on a USB port. Two at full volume can peak
near 0.8 A, so:

- Power from a port or charger that can deliver **1 A at 5 V**. Most Mac USB-C
  ports and any phone charger manage it; unpowered hubs and USB 2 ports
  (500 mA) may not.
- **Fit a 220-1000 uF electrolytic across VIN and GND at the amps** if the
  panel flickers, the card drops out, or the board resets on loud passages.
  Class-D current comes in pulses, and Dupont leads cannot deliver them without
  the rail sagging. Stripe to GND.
- **Run the amps' GND as its own wire back to an ESP32 GND pin**, not daisy-
  chained through the SD card's ground. Speaker current flowing through the
  card's ground return is how audio turns into read errors.

## microSD — Adafruit 4682 over SDIO

Same breakout, same wires as V2, different protocol. SPI mode could not read
any file over 1 KB on this card: multi-block reads failed while directory
listings worked perfectly (see `storage.h` for the full story). SDIO is the
card's native protocol, runs on the S3's dedicated SDMMC peripheral, and reads
everything.

| Breakout pin | ESP32-S3 | SDIO function |
|---|---:|---|
| 3V | 3V3 | Power, **3.3 V only** |
| GND | GND | Ground |
| CLK | GPIO38 | Clock |
| CMD | GPIO39 | Command, bidirectional |
| DAT0 | GPIO40 | Data, bidirectional |
| DAT3 | GPIO41 | Unused in 1-bit mode; firmware holds it high |
| DAT1 | — | Leave unconnected |
| DAT2 | — | Leave unconnected |
| CD | GPIO42 | Card detect, optional, disabled in `pins.h` |

Why DAT3 stays wired: a card samples DAT3 when it powers up, and low there means
"SPI mode". Once a card is in SPI mode it refuses SDIO until its power is
removed. Holding DAT3 high from the firmware means the card always comes up in
SD mode. If you ever see a mount failure right after flashing SPI-based
firmware, **reseat the card**; a reset is not enough.

If the card mounts unreliably:

1. Reseat it (clears SPI mode).
2. Fit **10 k pull-ups from CMD and DAT0 to 3V3**. SDIO expects them, and the
   ESP32's internal pull-ups are weak (~45 k) for long Dupont runs.
3. Fit **10 uF + 100 nF** across the breakout's `3V` and `GND`.

Shorter wires help more than any of these.

## E-paper display

Unchanged from V1/V2, on SPI2/FSPI. The WeAct silkscreen labels are misleading:
`SDA` and `SCL` are SPI, not I2C. MISO is unused; the panel is write-only.

## Buttons

Five tactile switches, each between its GPIO and GND, with internal pull-ups.
No external resistors. Use **two diagonally opposite legs** of a 4-leg switch:
the two legs on the same side are internally joined, and wiring both of them
shorts the GPIO to ground permanently. On a breadboard, straddle the centre
gutter and take one wire from each side. V2's WIRING.md has the detailed leg
identification and multimeter checks.

Suggested layout:

```
              [ UP ]
   [ BACK ]  [ ENTER ]  [ MENU ]
             [ DOWN ]
```

## Complete pin map

| GPIO | Assignment | Peripheral | Notes |
|---:|---|---|---|
| 1 | Battery sense | Analog | ADC1_CH0, M6, not wired |
| 2 | AMP_SD | Audio | Mute/shutdown for every amp |
| 4 | BTN_UP | Buttons | RTC, wake capable |
| 5 | BTN_DOWN | Buttons | RTC, wake capable |
| 6 | BTN_ENTER | Buttons | RTC, wake capable |
| 7 | EPD_BUSY | Display | |
| 8 | EPD_RST | Display | |
| 9 | EPD_DC | Display | |
| 10 | EPD_CS | Display | |
| 11 | EPD_MOSI | Display | Silkscreen `SDA` |
| 12 | EPD_SCK | Display | Silkscreen `SCL` |
| 13 | *spare* | | |
| 14 | *spare* | | |
| 15 | BTN_BACK | Buttons | RTC, wake capable |
| 16 | BTN_MENU | Buttons | RTC, wake capable |
| 17 | I2S_BCLK | Audio | Shared by every amp |
| 18 | I2S_LRCLK | Audio | Shared by every amp, silkscreen `LRC` |
| 21 | I2S_DIN | Audio | Shared by every amp |
| 38 | SD_CLK | microSD | SDIO |
| 39 | SD_CMD | microSD | SDIO |
| 40 | SD_DAT0 | microSD | SDIO |
| 41 | SD_DAT3 | microSD | Held high |
| 42 | SD_CD | microSD | Optional |
| 48 | *spare* | | RGB LED on many devkits |

Avoided: GPIO0, 3, 45, 46 (strapping), 19 and 20 (native USB), 26-37 (flash and
octal PSRAM on `R8` modules), 43/44 (UART0). GPIO22-25 do not exist on the S3.

## Bring-up order

Add the audio last, one amp at a time.

1. **Everything V2 already had.** Display, buttons and card should all pass the
   self-test (hold BACK while powering on) before any amp is connected.
2. **Amp 1, no speaker.** Wire the header only. Power up and measure:
   - `VIN` to `GND`: about 5 V.
   - `SD` to `GND`: about 0 V. A few hundred millivolts instead means GPIO2 is
     not reaching it, and the breakout's own pull-up is holding the amp on.
   - In the self-test, press UP. `SD` should jump to 3.3 V for the length of
     the tone, then drop back.
3. **Speaker on amp 1.** Power off, connect the speaker, power on in self-test
   and press UP. You should hear 440 Hz, then 660 Hz and 880 Hz on the next
   presses. Silence while SD goes high points at the BCLK/LRC/DIN wires.
   Harsh noise or a distorted tone instead of a clean one usually means LRC and
   BCLK are swapped.
4. **Music.** Normal boot, open a track. Watch serial for the `[audio]` line,
   which reports the start byte and free heap.
5. **Amp 2**, in parallel, same checks. Then both together at high volume while
   the panel refreshes, to confirm the supply holds up (see [Power](#power)).
