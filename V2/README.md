# MusicStation V2

*Offline music station: microSD library, physical buttons, browsable e-paper UI,
local synchronized lyrics and pre-baked artwork.*

V2 is the step from "iPhone companion display" to "self-contained player".
V1 stays in the repository root and remains the working prototype; V2 is a new
sketch rather than an edit of `MusicStation.ino`, because the two have almost
opposite dependency profiles.

> **Current status:** M0-M6 written, not yet run on hardware. The sketch is
> `MusicStationV2/MusicStationV2.ino`. Audio runs on a simulated playhead until
> `AUDIO_HARDWARE` is set to 1 in `audio.h`, which is what lets every screen
> and the lyric scheduler be finished before the amplifier is fitted.

## What changes from V1

V1 is a *display* that reads state from a phone over BLE and fetches assets over
Wi-Fi. V2 *owns* the music, the metadata, and the playback state. That inverts
nearly every architectural decision.

| Concern | V1 | V2 |
|---|---|---|
| Source of truth | iPhone via Apple Media Service | microSD card |
| Metadata | AMS notifications | On-card index built by the device |
| Artwork | iTunes API -> JPEGDEC -> sharpen -> dither | Pre-baked 1-bit bitmap on the card |
| Lyrics | LRCLIB over HTTPS | `.lrc` sidecar file next to the track |
| Audio | Phone speakers | ESP32 I2S (planned) |
| Input | None wired | Physical buttons |
| Network | BLE + Wi-Fi + TLS required | None |

### Dropped in V2

BLE/AMS, Wi-Fi, `WiFiClientSecure`, `HTTPClient`, `ArduinoJson`, `JPEGDEC`, and
all LRCLIB/iTunes network code. This frees a large amount of flash and, more
importantly, the large contiguous internal heap block that `JPEGDEC` and TLS
currently compete for. Audio decoding and I2S buffers need exactly that block.

### Carried over from V1 and DeskPomodoro

- `QuietBorder290BS` driver: SSD1680 border driver in HiZ during partial refresh.
- Region-based partial refresh discipline and the full-cleanup waveform counter.
- `U8g2_for_Adafruit_GFX` UTF-8 rendering with ASCII + Cyrillic glyphs.
- `fitUtf8Text()`, `wrapLyric()`, and UTF-8 codepoint-boundary text handling.
- The LRC timestamp parser, reading from SD instead of an HTTP stream.
- DeskPomodoro's ISR button queue, which survives the blocking e-paper waveform.

## Hardware

| Part | Status |
|---|---|
| ESP32-S3-WROOM-1U + external antenna | In hand (from V1) |
| WeAct 2.9" black/white e-paper, 296x128, SSD1680 | In hand (from V1) |
| Adafruit 4682 microSD breakout, 3V only | In hand |
| 5 tactile buttons | In hand |
| MAX98357A I2S amplifier | Not yet |
| Speaker, power, enclosure | Not yet |

The antenna is no longer functionally required once BLE and Wi-Fi are gone, but
leave it connected while V1 and V2 share a board.

## Getting music onto the station

Eject the microSD card and write to it from a Mac with a card reader. No USB
firmware is required for this, and a reader is 20-80 MB/s against roughly
0.5-1 MB/s for the ESP32-S3's Full Speed USB.

The card layout in [SD_LAYOUT.md](SD_LAYOUT.md) is the contract. Anything that
writes that layout works, so USB mass storage can be added later as a
convenience without changing the firmware's model of the card.

`tools/prepare_album.py` does the preparation on the Mac: point it at an album
and it fetches synchronized lyrics and cover art, bakes the art to a 1-bit
bitmap at exact panel dimensions, and writes the folder structure.

```
python3 tools/prepare_album.py ~/Music/Untrue --dest /Volumes/MUSICSTATION
```

This is what moves JPEG decoding and dithering off the device permanently, and
with them the large contiguous heap allocation that the audio buffers need.

## Milestones

| Milestone | Contents | Gate | State |
|---|---|---|---|
| M0 Bring-up | Panel, button input layer, SD mount, self-test screen | Each peripheral reports independently | Written |
| M1 Card | SD mount, folder scan, on-card index build with progress screen | Index survives a power cycle and a card swap | Written |
| M2 Browse | List rendering, selection, navigation, letter jump, input abstraction | Full library navigable with 4 buttons | Written |
| M3 Playback model | Queue, shuffle/repeat, resume, simulated clock, `.lrc` sync, cover display | Lyrics track a simulated playhead correctly | Written |
| M4 Audio | MP3 decode on the second core, ring buffer, I2S to MAX98357A | No dropout across a full e-paper refresh | Written, needs the amp |
| M5 Playlists | `.m3u` reading, on-device playlist edits, settings screen | Playlist created on the device survives a rescan | Written |
| M6 Power | Deep sleep while paused, wake on button, battery reporting | Measured idle and playback current | Written, needs a battery |

M3 deliberately lands before audio hardware. A simulated playhead exercises the
queue, the lyric scheduler, and every screen, so the UI can be finished and
tested while the amplifier is still in the post.

## Source layout

| File | Contents |
|---|---|
| `MusicStationV2.ino` | Boot, bring-up self-test, render loop |
| `pins.h` | Pin map, mirroring WIRING.md |
| `panel.h/.cpp` | Quiet-border driver, refresh discipline, UTF-8 text |
| `input.h/.cpp` | ISR capture, debounce, logical actions, auto-repeat |
| `storage.h/.cpp` | microSD on its own SPI host |
| `mp3meta.h/.cpp` | ID3v2 skip, frame header and Xing parsing for duration |
| `library.h/.cpp` | Index build and fixed-width record reads |
| `lyrics.h/.cpp` | LRC parsing and the incremental playback cursor |
| `cover.h/.cpp` | 1-bit artwork read straight off the card |
| `audio.h/.cpp` | Simulated and I2S backends behind one interface |
| `player.h/.cpp` | Queue, shuffle, repeat, resume, track loading |
| `playlist.h/.cpp` | .m3u reading, appending and creation |
| `settings.h/.cpp` | NVS-backed settings and resume state |
| `power.h/.cpp` | Deep sleep, button wake, battery reporting |
| `ui.h/.cpp` | Navigation stack, all screens, region-based drawing |

## Dependencies

Through the Arduino Library Manager:

- GxEPD2
- Adafruit GFX (installed as a GxEPD2 dependency)
- U8g2_for_Adafruit_GFX

ESP8266Audio (2.4.1) is needed **only** once `AUDIO_HARDWARE` is set to 1; the
default build does not reference it. ArduinoJson, JPEGDEC and the TLS stack
that V1 needs are all gone.

> **ESP8266Audio 2.4.1 does not build against ESP32 core 3.2.1.** Its
> `AudioOutputPDM.cpp` refers to an `i2s_pdm_tx_slot_config_t::data_fmt` member
> that this core's ESP-IDF does not have. V2 uses I2S rather than PDM, so the
> fix is to take that one file out of the build:
>
> ```
> mv ~/Documents/Arduino/libraries/ESP8266Audio/src/AudioOutputPDM.cpp \
>    ~/Documents/Arduino/libraries/ESP8266Audio/src/AudioOutputPDM.cpp.disabled
> ```
>
> Nothing in V2 references PDM output. Reverse the rename to restore it.

## Verified builds

Both configurations compile against ESP32 core 3.2.1, GxEPD2 1.6.9, Adafruit
GFX 1.12.6 and U8g2_for_Adafruit_GFX 1.8.0, with USB CDC on boot, Huge APP and
PSRAM disabled:

| Build | Flash | Global RAM | Free for locals |
|---|---|---|---|
| `AUDIO_HARDWARE 0` (simulated) | 586,414 (18%) | 30,604 (9%) | 297,076 |
| `AUDIO_HARDWARE 1` (I2S) | 727,598 (23%) | 30,804 (9%) | 296,876 |

For comparison, V1 is 1,757,650 bytes of flash and 85,160 bytes of global RAM.
Dropping BLE, Wi-Fi, TLS, ArduinoJson and JPEGDEC is what accounts for the
difference, and the freed internal RAM is what M4's decode buffer needs.

Board settings as V1: an ESP32-S3 profile, USB CDC enabled on boot, Huge APP
partition scheme, PSRAM disabled.

## Controls

| | Now Playing | Lists |
|---|---|---|
| UP | Previous track | Move up |
| DOWN | Next track | Move down |
| ENTER | Play / pause | Open or play |
| BACK | Open the library | Up one level |
| BACK held | — | Return to Now Playing |
| MENU | Track actions | Letter jump, or track actions |

Hold UP or DOWN to auto-repeat; keep holding and it moves a page at a time.
Hold BACK while powering on for the bring-up self-test.

## Documents

- [CURRENT_STATE.md](CURRENT_STATE.md) - handoff: what is built, what is verified, what to do next
- [`tools/prepare_album.py`](tools/prepare_album.py) - Mac-side album preparation
- [`tools/make_playlist.py`](tools/make_playlist.py) - build .m3u playlists from the card
- [WIRING.md](WIRING.md) - complete pin map and per-peripheral wiring
- [UI_DESIGN.md](UI_DESIGN.md) - screens, input model, and refresh strategy
- [SD_LAYOUT.md](SD_LAYOUT.md) - card layout and on-card index format
