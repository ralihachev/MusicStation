# MusicStation V3

*The offline station with sound: microSD library, physical buttons, browsable
e-paper UI, synchronized lyrics, and MP3 playback through MAX98357A I2S
amplifiers into 8 ohm speakers.*

V3 is V2 finished. V2 built every screen, the card index, the queue and the
lyric scheduler against a simulated playhead while the amplifier was in the
post; V3 makes the real audio path the default and fixes what running it for
real requires. V1 (repository root) and V2 (`V2/`) are left as they were.

> **Current status:** playing music on the wired hardware: ESP32-S3, the
> e-paper panel, microSD over SDIO, five buttons, and one MAX98357A into an
> 8 ohm cavity speaker. Lyric timing and speaker voicing are still being tuned
> by ear. The sketch is `MusicStationV3/MusicStationV3.ino`; wire it per
> [WIRING.md](WIRING.md), in its bring-up order.

## What changes from V2

| | V2 | V3 |
|---|---|---|
| Default backend | Simulated playhead, silent | MP3 over I2S to MAX98357A |
| Card bus | SPI in the docs, SDIO in the code | SDIO, documented; DAT3 held high |
| Playhead | Wall clock | Frames accepted by I2S |
| Volume | Settings screen only, +2 per press | Hold UP/DOWN on Now Playing; perceptual curve |
| Resume after power-off | Right track, from 0:00 | Right track, right position |
| Track start | Decoder scans from byte 0, through any ID3 tag | Seeks to the first real MPEG frame |
| Seek | Not implemented on hardware | CBR exact, VBR via Xing table |
| Pause | Stops feeding I2S; DMA loops its last 100 ms | Muted, then DMA overwritten with silence |
| Deep sleep | Amp's SD pin floats, so the amp wakes | SD latched low across sleep |
| Bring-up self-test | Panel, card, buttons | Plus an amplifier test tone on UP |
| Speaker voicing | None | High-pass, low-mid and presence EQ for small cavity speakers |
| Lyric timing | Fixed 800 ms lead | Lead from the measured refresh time, plus an offset tuned on the device |
| Lyrics vs. clock | Clock tick could hold a lyric for 780 ms | Clock skips a second instead; lyric and clock share one refresh |
| Cleanup waveform | Could upgrade a partial refresh and blank the screen | Only ever runs as a whole-screen draw |

Fixes that only show up with real audio:

- **Watchdog.** ESP8266Audio's I2S write is non-blocking. V2's decoder task
  called `loop()` back to back at priority 2, which would have starved core 0's
  idle task until the task watchdog reset the board. V3 yields one tick per
  call.
- **Use-after-free on stop.** V2 cleared a flag, waited 20 ms and deleted the
  decoder, while the other core could still be inside `loop()` waiting on the
  card. V3 guards the chain with a mutex both cores take.
- **Card contention.** The buffer's top-up read now skips a round instead of
  blocking when the UI holds the card. The 32 KB buffer covers it, and the
  speaker never waits on a lyric file load.
- **Deep-sleep wake mode.** V2 feature-tested `ESP_EXT1_WAKEUP_ANY_LOW` with
  `#if defined`, but it is an enum rather than a macro, so it always fell
  through to the deprecated `ALL_LOW`. It worked only because the S3 aliases
  the two.

See [UI_DESIGN.md](UI_DESIGN.md#audio) for how the audio path is built.

## Hardware

| Part | Status |
|---|---|
| ESP32-S3-WROOM-1U + external antenna | In hand |
| WeAct 2.9" black/white e-paper, 296x128, SSD1680 | In hand |
| Adafruit 4682 microSD breakout, 3V only | In hand, SDIO |
| 5 tactile buttons | In hand |
| MAX98357A I2S amplifier breakout | In hand, x3 |
| 3525 cavity speaker, 8 ohm 2 W | In hand |
| Battery, enclosure | Not yet |

One amp and one speaker is the whole audio build. A second pair is wired in
parallel with the first, no code change; see
[WIRING.md](WIRING.md#two-speakers-wire-the-second-amp-in-parallel).

## Build

Arduino Library Manager:

- GxEPD2
- Adafruit GFX (installed as a GxEPD2 dependency)
- U8g2_for_Adafruit_GFX
- **ESP8266Audio 2.4.1**, now required by the default build

Board: an ESP32-S3 profile, USB CDC on boot, **Huge APP** partition scheme,
**PSRAM disabled**. Same as V1 and V2.

> **ESP8266Audio 2.4.1 does not build against ESP32 core 3.2.1.** Its
> `AudioOutputPDM.cpp` refers to an `i2s_pdm_tx_slot_config_t::data_fmt` member
> that this core's ESP-IDF does not have. V3 uses I2S rather than PDM, so take
> that one file out of the build:
>
> ```
> mv ~/Documents/Arduino/libraries/ESP8266Audio/src/AudioOutputPDM.cpp \
>    ~/Documents/Arduino/libraries/ESP8266Audio/src/AudioOutputPDM.cpp.disabled
> ```
>
> Reverse the rename to restore it.

Or from the command line:

```
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=huge_app,PSRAM=disabled" \
  V3/MusicStationV3
```

### Verified builds

ESP32 core 3.2.1, GxEPD2 1.6.9, Adafruit GFX 1.12.6, U8g2_for_Adafruit_GFX
1.8.0, ESP8266Audio 2.4.1; no warnings from the sketch with `--warnings all`.

| Build | Flash | Global RAM | Free for locals |
|---|---|---|---|
| `AUDIO_HARDWARE 1` (default, I2S) | 770,182 (24%) | 32,156 (9%) | 295,524 |
| `AUDIO_HARDWARE 0` (simulated) | 617,010 (19%) | 31,900 (9%) | 295,780 |

### Build switches

In `audio.h`:

| Switch | Default | Meaning |
|---|---|---|
| `AUDIO_HARDWARE` | 1 | 0 for V2's silent simulated playhead |
| `AUDIO_STEREO` | 0 | 1 for true stereo; needs a resistor on amp 2, see WIRING.md |

## Controls

| | Now Playing | Lists |
|---|---|---|
| UP tap | Previous track (restart if > 3 s in) | Move up |
| DOWN tap | Next track | Move down |
| UP / DOWN hold | Volume up / down | Auto-repeat, then page at a time |
| ENTER | Play / pause | Open or play |
| BACK | Open the library | Up one level |
| BACK held | — | Return to Now Playing |
| MENU tap | Track actions | Letter jump, or track actions |
| MENU hold | Lyric sync: UP earlier, DOWN later, ENTER done | — |

Hold BACK while powering on for the bring-up self-test. In it, UP plays a test
tone through the amp (440, 660, 880 Hz in turn), ENTER runs a cleanup refresh,
and MENU retries the card.

## Tuning by ear

Two things depend on the actual panel and speaker, so both are exposed:

- **Lyric sync.** Hold MENU on Now Playing; UP makes lyrics earlier, DOWN
  later, in 50 ms steps, while the song keeps playing. ENTER saves. The value
  is stored in NVS and added to the panel's measured refresh time.
- **Speaker voicing.** The constants at the top of `audio.cpp`:
  `AUDIO_HIGHPASS_HZ` (250), `AUDIO_LOWMID_DB` (-3 at 450 Hz),
  `AUDIO_PRESENCE_DB` (-3 at 3 kHz) and `AUDIO_HEADROOM_DB` (-3). Raise the
  high-pass if it sounds muddy, lower it if thin.

## Getting music onto the station

Unchanged from V2. The card layout in [SD_LAYOUT.md](SD_LAYOUT.md) is the
contract; `tools/prepare_album.py` fetches lyrics and art and bakes the art on
the Mac:

```
python3 tools/prepare_album.py ~/Music/SomeAlbum --dest /Volumes/MUSIC
python3 tools/make_playlist.py --card /Volumes/MUSIC --list
```

Any MP3 the ESP32 can decode in real time plays: MPEG-1/2 Layer III, CBR or
VBR, up to 320 kbps, mono or stereo. Non-MP3 sources are transcoded by
`prepare_album.py` with ffmpeg.

## Source layout

| File | Contents |
|---|---|
| `MusicStationV3.ino` | Boot, bring-up self-test, render loop |
| `pins.h` | Pin map, mirroring WIRING.md |
| `panel.h/.cpp` | Quiet-border driver, refresh discipline, UTF-8 text |
| `input.h/.cpp` | ISR capture, debounce, logical actions, auto-repeat |
| `storage.h/.cpp` | microSD over SDIO, shared-card mutex |
| `mp3meta.h/.cpp` | ID3v2 skip, frame header, Xing duration and seek table |
| `library.h/.cpp` | Index build and fixed-width record reads |
| `lyrics.h/.cpp` | LRC parsing and the incremental playback cursor |
| `cover.h/.cpp` | 1-bit artwork read straight off the card |
| `audio.h/.cpp` | I2S decoder task and simulated backend behind one interface |
| `player.h/.cpp` | Queue, shuffle, repeat, resume, track loading |
| `playlist.h/.cpp` | .m3u reading, appending and creation |
| `settings.h/.cpp` | NVS-backed settings and resume state |
| `power.h/.cpp` | Deep sleep, button wake, amp held off in sleep |
| `ui.h/.cpp` | Navigation stack, all screens, volume gesture |

## Documents

- [WIRING.md](WIRING.md): every wire, amp and speaker details, bring-up order
- [UI_DESIGN.md](UI_DESIGN.md): screens, input model, refresh strategy, audio
  path
- [SD_LAYOUT.md](SD_LAYOUT.md): card layout and on-card index format
