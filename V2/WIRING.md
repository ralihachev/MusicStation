# MusicStation V2 wiring

Target hardware:

- ESP32-S3-WROOM-1U module or carrier board
- WeAct Studio 2.9" black/white 296x128 e-paper (DEPG0290BS, SSD1680)
- Adafruit 4682 Micro SD SPI/SDIO breakout, **3V only**
- 5 tactile buttons
- MAX98357A I2S amplifier and speaker (M4, not yet fitted)

Everything runs at 3.3 V logic. No level shifters are needed anywhere in this
build. The one exception is the amplifier's *power* rail, which wants 5 V for
full output while its logic inputs stay at 3.3 V.

## Quick reference: every connection

One row per physical wire. Component silkscreen label on the left, ESP32-S3 pin
on the right.

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
| | CLK | **GPIO38** (SPI clock) |
| | CMD | **GPIO39** (SPI MOSI) |
| | DAT0 | **GPIO40** (SPI MISO) |
| | DAT3 | **GPIO41** (SPI CS) |
| | DAT1 | not connected |
| | DAT2 | not connected |
| | CD | **GPIO42** (optional card detect) |
| **Button UP** | leg 1 | **GPIO4** |
| | leg 2 | **GND** |
| **Button DOWN** | leg 1 | **GPIO5** |
| | leg 2 | **GND** |
| **Button ENTER** | leg 1 | **GPIO6** |
| | leg 2 | **GND** |
| **Button BACK** | leg 1 | **GPIO15** |
| | leg 2 | **GND** |
| **Button MENU** | leg 1 | **GPIO16** |
| | leg 2 | **GND** |
| **MAX98357A** (M4) | VIN | **5V** |
| | GND | **GND** |
| | BCLK | **GPIO17** |
| | LRC | **GPIO18** |
| | DIN | **GPIO21** |
| | SD | **GPIO2** |
| | GAIN | not connected (9 dB) |
| | +, - | speaker, 4-8 ohm (never to GND) |

If the card proves unreliable, fit **10 uF + 100 nF** between the breakout's `3V`
and `GND`, as close to the board as possible. See the decoupling notes in the
microSD section; try it without them first.

On a 4-leg tactile switch the legs are internally paired, so use one leg from
each pair; if the button reads as permanently pressed, you picked two legs from
the same pair, and moving one wire 90 degrees round fixes it.

## Complete pin map

| GPIO | Assignment | Peripheral | Notes |
|---:|---|---|---|
| 1 | Battery sense | Analog | ADC1_CH0, M6 |
| 2 | AMP_SD | Audio | Amplifier enable/shutdown, M4 |
| 4 | BTN_UP | Buttons | RTC, wake capable |
| 5 | BTN_DOWN | Buttons | RTC, wake capable |
| 6 | BTN_ENTER | Buttons | RTC, wake capable |
| 7 | EPD_BUSY | Display | From V1 |
| 8 | EPD_RST | Display | From V1 |
| 9 | EPD_DC | Display | From V1 |
| 10 | EPD_CS | Display | From V1 |
| 11 | EPD_MOSI | Display | From V1, WeAct silkscreen says `SDA` |
| 12 | EPD_SCK | Display | From V1, WeAct silkscreen says `SCL` |
| 13 | *spare* | | |
| 14 | *spare* | | |
| 15 | BTN_BACK | Buttons | RTC, wake capable |
| 16 | BTN_MENU | Buttons | RTC, wake capable |
| 17 | I2S_BCLK | Audio | M4 |
| 18 | I2S_LRCLK | Audio | M4 |
| 21 | I2S_DIN | Audio | M4 |
| 38 | SD_CLK | microSD | |
| 39 | SD_CMD | microSD | SPI MOSI |
| 40 | SD_DAT0 | microSD | SPI MISO |
| 41 | SD_DAT3 | microSD | SPI CS |
| 42 | SD_CD | microSD | Card detect, optional |
| 48 | *spare* | | Drives the RGB LED on many devkits |

### Why the pins fall this way

**Buttons take RTC GPIOs.** Only GPIO0-21 can wake the ESP32-S3 from deep sleep,
so every button sits in that range. M6 depends on this and it is painful to
rewire later.

**microSD takes GPIO38-42.** These are not RTC pins and nothing else wants them.
They are the module's external JTAG pins, but the S3 debugs over its internal
USB-JTAG by default, so they are free.

**Avoided everywhere:** GPIO0, 3, 45, 46 (strapping), 19 and 20 (native USB, used
by USB CDC), 26-37 (SPI flash and octal PSRAM on `R8` module variants), and
43/44 (UART0, kept for serial). GPIO22-25 do not exist on the ESP32-S3.

That leaves 13, 14, and 48 spare, which is enough margin for a status LED, a
headphone-detect switch, or a sixth button.

## E-paper display

Unchanged from V1, on SPI2/FSPI. The WeAct silkscreen labels are misleading:
`SDA` and `SCL` are SPI, not I2C.

| WeAct pin | ESP32-S3 | Function |
|---|---:|---|
| VCC | 3V3 | Power, **3.3 V only** |
| GND | GND | Ground |
| SDA | GPIO11 | SPI MOSI |
| SCL | GPIO12 | SPI clock |
| CS | GPIO10 | Chip select |
| D/C | GPIO9 | Data/command |
| RES | GPIO8 | Reset |
| BUSY | GPIO7 | Busy output |

MISO is unused; the panel is write-only.

## microSD — Adafruit 4682

The 4682 is the 3V-only variant: no onboard regulator, no level shifter, direct
3.3 V logic. That is exactly right for the ESP32-S3 and means fewer parts, but
it also means **`3V` must go to 3V3 and never to 5 V.**

Wired for SPI mode:

| Breakout pin | ESP32-S3 | SPI function |
|---|---:|---|
| 3V | 3V3 | Power |
| GND | GND | Ground |
| CLK | GPIO38 | SPI clock |
| CMD | GPIO39 | SPI MOSI, data into the card |
| DAT0 | GPIO40 | SPI MISO, data out of the card |
| DAT3 | GPIO41 | SPI chip select |
| DAT1 | — | SDIO only, leave unconnected |
| DAT2 | — | SDIO only, leave unconnected |
| CD | GPIO42 | Card detect, optional |

The pin names are SDIO names. In SPI mode `CMD` is MOSI and `DAT0` is MISO,
which reads backwards the first time. `DAT3` doubles as chip select.

Card detect is worth wiring if your board breaks it out. The index rebuild
triggers in `SD_LAYOUT.md` include "card was removed and reinserted", and a CD
pin detects that directly instead of inferring it. Configure it as
`INPUT_PULLUP`; most sockets close the contact to ground when a card is present,
so confirm the polarity with a meter before trusting it.

Two practical notes:

- **Decoupling, if the card misbehaves.** An SD card idles near 0.2 mA and then
  pulls 100 mA+ for a few microseconds during a read burst or an internal erase.
  Dupont wire has enough resistance and inductance that the rail sags at the far
  end before the regulator can respond, which shows up as random `SD.begin()`
  failures, read errors once the panel starts refreshing, or
  `Brownout detector was triggered` reboots.

  The fix is local reservoir capacitance across `3V` and `GND`: **10 uF** for the
  bulk demand and **100 nF** beside it for the fast edges, which the bulk cap is
  too inductive to catch. Values are not critical; 4.7-47 uF and 10 nF-1 uF work,
  and one of the two beats neither. A 100 nF ceramic is marked `104`, not
  "100nF". An electrolytic is polarized: the stripe is negative, to GND.

  The Adafruit board likely carries some decoupling already, so try it without
  first. Short wires matter more than the capacitors do, and dropping SPI to
  10 MHz costs nothing given the throughput headroom. Do not disable the brownout
  detector; that hides the sag while it still corrupts the card.
- If the card enumerates unreliably, pull `DAT1` and `DAT2` up to 3V3 through
  10k. The SD specification expects those lines high even when SPI mode ignores
  them, and some cards care.

### Separate SPI bus

The SD card gets its own SPI host rather than sharing the display's:

```cpp
SPIClass sdSPI(HSPI);                       // SPI3, display keeps SPI2/FSPI
sdSPI.begin(SD_CLK, SD_DAT0, SD_CMD, SD_DAT3);
SD.begin(SD_DAT3, sdSPI, 20000000);         // 20 MHz
```

Sharing one bus does work — the display's 780 ms cost is almost entirely the
BUSY wait, not SPI transfer, so the bus is idle during it. But M4 has an audio
decoder reading the card continuously on one core while the UI drives the panel
on the other, and a bus neither has to arbitrate for removes a whole category of
intermittent bug. The S3 has the pins to spare.

Start at 20 MHz. It can usually go to 40 MHz, but there is no throughput reason
to: MP3 at 320 kbps needs about 40 KB/s, and 20 MHz SPI delivers roughly 2 MB/s.

## Buttons

Five tactile switches, each wired between its GPIO and GND, with the internal
pull-up enabled. No external resistors.

| Button | ESP32-S3 | Other side |
|---|---:|---|
| UP | GPIO4 | GND |
| DOWN | GPIO5 | GND |
| ENTER | GPIO6 | GND |
| BACK | GPIO15 | GND |
| MENU | GPIO16 | GND |

```cpp
pinMode(BTN_UP, INPUT_PULLUP);   // idle HIGH, pressed LOW
```

Active low is deliberate: it needs no external parts, and it matches the
any-low ext1 deep-sleep wake source the ESP32-S3 provides, so M6 wakes on any
button without rewiring. Confirm the ext1 wake mode name against your core
version when you get there.

### Identifying the legs on a 4-pin switch

The four legs are two pairs that are **already connected to each other**;
pressing joins the pairs. The two legs on the same side of the body are the
same pair, so wiring both of them shorts the GPIO to ground permanently and the
firmware sees that button held down forever.

```
        1 ---|~~~~~~~~~|--- 3        1===2  always connected
             |  ( o )  |             3===4  always connected
        2 ---|_________|--- 4        pressing joins 1-2 to 3-4
```

**Use two diagonally opposite legs** (1 and 4, or 2 and 3). A diagonal pair is
always one from each side, whichever way round the switch sits. On a
breadboard, straddle the centre channel and each column is one pair.

### On a breadboard

A 6x6 mm switch has 0.2 inch spacing along each side and 0.3 inch across, and
0.3 inch is the width of the centre channel, so it only seats one way:
straddling the gutter with two legs each side. That places one internally
connected pair entirely left of the channel and the other entirely right.

```
         a  b  c  d  e  |gutter|  f  g  h  i  j
 row 1   .  .  .  *-----+------+-----*  .  .  .
 row 2   .  .  .  .     |      |     .  .  .  .
 row 3   .  .  .  *-----+------+-----*  .  .  .
                  \-- pair A --/   \-- pair B --/
```

**One wire left of the gutter, one wire right.** Any rows. Both wires on the
same side is the breadboard version of the same-pair mistake: that side is one
connected pair, so the GPIO sits permanently at ground.

For five buttons, jumper an ESP32 GND pin to the board's negative rail once,
then take each button's right-hand side to that rail. Five GPIO wires, five
short ground jumpers, one rail wire.

To confirm with a multimeter on continuity:

1. Touch the probes together first, so a silent result later means something.
2. Probe two legs without pressing. A beep means the same pair: move a probe to
   the other side. Silence means different pairs.
3. Press the button with the probes still on the silent pair. It should beep
   while pressed and go quiet on release. Those are the two legs to use.

After wiring, with the board **unpowered**, probe between the GPIO header pin
and GND: silent released, beeping pressed. A constant beep is a same-pair
mistake or a solder bridge; no beep at all is a dead joint or the wrong header
pin. Powered and flashed, the same points read about 3.3 V released and 0 V
pressed, because `INPUT_PULLUP` only exists once `pinMode` has run.

The bring-up self-test then confirms it visually: each button's box inverts
while held. A box inverted with nothing pressed is a short on that GPIO; one
that never inverts is a dead joint.

A 100 nF capacitor across each switch is optional. It suppresses contact bounce
in hardware and does no harm, but the 80 ms software debounce carried over from
DeskPomodoro is the primary defence and is sufficient on its own.

**Reuse DeskPomodoro's ISR ring buffer.** The main loop does not run during the
roughly 0.75 s e-paper waveform, so presses have to be captured by an interrupt
and queued. Polling in `loop()` silently drops any press that lands during a
refresh, which on this device is most of them.

Suggested physical arrangement, which keeps UP and DOWN under the thumb and puts
the destructive-ish MENU furthest from ENTER:

```
              [ UP ]
   [ BACK ]  [ ENTER ]  [ MENU ]
             [ DOWN ]
```

## I2S audio (M4, not yet fitted)

Not required for M1 through M3. The pins are reserved now so the amplifier drops
in without disturbing anything.

| MAX98357A | ESP32-S3 | Notes |
|---|---:|---|
| VIN | 5V | 3.3 V works at reduced output |
| GND | GND | |
| BCLK | GPIO17 | Bit clock |
| LRC | GPIO18 | Word select / left-right clock |
| DIN | GPIO21 | Serial data |
| SD | GPIO2 | Enable and channel select, see below |
| GAIN | — | Leave floating for 9 dB |

**The `SD` pin is not a plain enable.** Its voltage selects the channel as well
as powering the amp down:

| SD voltage | Result |
|---|---|
| below 0.16 V | Shutdown |
| 0.16 - 0.77 V | Right channel |
| 0.77 - 1.4 V | (Left + Right) / 2 |
| above 1.4 V | Left channel |

Driving it from a GPIO therefore gives you shutdown at LOW and *left channel* at
HIGH, not the mono mix. The clean resolution is to mix to mono in software and
write the same sample to both I2S channels, after which channel selection stops
mattering and the GPIO becomes a straightforward mute. Muting during seeks and
track changes is worth having, since an unmuted amplifier pops.

The output is bridged. **Do not connect either speaker terminal to ground.**
Use a 4-8 ohm speaker rated for at least 3 W.

## Power

| Load | Draw |
|---|---|
| ESP32-S3, radios off | ~40 mA |
| e-paper during refresh | ~15 mA for about 1 s |
| microSD active | 20-100 mA in bursts |
| MAX98357A at volume | 500 mA+ peaks into 4 ohm |

V2 has no BLE and no Wi-Fi, so the idle draw is far below V1's. The amplifier
dominates once fitted: budget a 5 V supply of at least 1 A, and do not run the
speaker from a laptop USB port you care about.

All grounds are common. The external 2.4 GHz antenna is no longer functionally
required once BLE and Wi-Fi are gone, but leave it connected while the board is
still being used for V1.

## Bring-up order

Wire and verify one peripheral at a time. Debugging all four at once is how a
weekend disappears.

1. **Display alone.** Flash V1 or DeskPomodoro; a working panel confirms the
   board, the supply, and the SPI2 pins before anything is added.
2. **Buttons alone.** Print edges to serial. Confirm all five read HIGH at idle
   and LOW when pressed, and that none of them float.
3. **microSD alone.** `SD.begin()`, then list the root directory. Do this before
   any display code runs, so a failure cannot be blamed on bus contention.
4. **Display plus microSD.** Refresh the panel in a loop while reading a large
   file. Any brownout or card dropout here is a decoupling problem, not a
   software one.
5. **Amplifier last**, at low volume, with the speaker disconnected on the first
   power-up so a wiring mistake is quiet rather than expensive.
