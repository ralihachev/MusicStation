# MusicStation V3 interface design

V3 keeps V2's interface. The changes are volume on Now Playing and a real
audio backend; everything else below is unchanged from V2.

Panel: 296x128 landscape, black/white, SSD1680, no greyscale, no backlight.

## The constraint everything follows from

V1 measured roughly **780 ms** for a partial refresh of a 184x20 strip. The
SSD1680 partial waveform is a fixed frame sequence, so that cost is dominated by
the waveform rather than by the redrawn area: a two-row update and a full-screen
update cost close to the same. Confirm this on the panel early, because the
whole design depends on it.

The consequence is the opposite of a normal display. Do not optimize how much is
redrawn. **Optimize how many times the panel refreshes at all.** A list that
refreshes once per keypress is unusable; a list that refreshes once per *burst*
of keypresses feels fine.

Three rules follow:

1. **Model and renderer are decoupled.** Input mutates UI state immediately and
   never draws. A render pass runs separately, draws the latest state, and is
   allowed to skip intermediate states entirely. Scrolling past eight items
   produces one refresh, not eight.
2. **Input is coalesced by a settle timer.** After the last input event, wait
   `SCROLL_SETTLE_MS` before drawing. Holding a key scans the list internally at
   full speed and the panel catches up when the key is released.
3. **Never gesture where a button will do.** A 400 ms multi-tap window stacked on
   a 780 ms refresh is over a second before anything visibly happens. Discrete
   buttons express intent immediately.

### Timing constants

```
SCROLL_SETTLE_MS      150   // quiet period before the list is drawn
KEY_REPEAT_DELAY_MS   450   // hold this long before auto-repeat starts
KEY_REPEAT_RATE_MS     90   // auto-repeat interval once running
FAST_SCROLL_AFTER_MS 1500   // repeat this long switches to page-at-a-time
FULL_REFRESH_AFTER_PARTIALS 300  // cleanup waveform, as V1
```

`FAST_SCROLL_AFTER_MS` matters for a 2000-track library: hold DOWN, and after a
second and a half the cursor starts moving a page per repeat instead of a row.

## Input model

V3 uses **five tactile buttons**, active low, wired per [WIRING.md](WIRING.md).
Tactile rather than capacitive, because the panel says nothing for 780 ms after a
press and the click is the only confirmation the user gets in that window; a
silent pad invites a second press and a double scroll.

Even so, the firmware does not name buttons. It names **logical actions**, and a
single mapping table binds hardware to them, so a later change to four buttons or
an encoder is a one-table edit.

```
UP      previous item / previous track
DOWN    next item / next track
ENTER   open / play / pause
BACK    up one level / leave Now Playing
MENU    context actions (optional fifth action)
```

Four actions are the floor and are enough for the entire interface, because
Now Playing reuses UP/DOWN/ENTER for transport. A fifth MENU action is worth a
button if you have one: it carries "add to playlist", "shuffle", and "play next"
without hiding them behind holds.

| Hardware | Mapping |
|---|---|
| 5 buttons | Direct: UP, DOWN, ENTER, BACK, MENU |
| 4 buttons | Direct: UP, DOWN, ENTER, BACK; MENU = hold ENTER |
| 3 buttons | PREV=UP, NEXT=DOWN, OK tap=ENTER, OK hold=BACK, PREV hold=top of list |
| Rotary encoder | Turn = UP/DOWN, push = ENTER, side button = BACK |

Five is chosen because with a 780 ms refresh, every gesture you have to
disambiguate is latency the user feels directly. The alternate rows above remain
supported by the mapping table.

An encoder browses long lists best but emits detents far faster than the panel
can follow. It works, but only because of `SCROLL_SETTLE_MS` above; it makes the
settle timer mandatory rather than merely advisable.

### Reliability

Reuse DeskPomodoro's pattern exactly. An ISR timestamps edges with
`esp_timer_get_time()` and pushes them into a small ring buffer, so presses
during the roughly 0.75 s blocking waveform are queued rather than lost. Debounce
at 80 ms. This is the single most important input detail on this hardware: the
main loop is *not* responsive while the panel is updating, and a player whose
buttons drop presses mid-refresh feels broken.

## Screen map

Now Playing is home, not the library. The device's resting state is playing music
and showing lyrics, exactly as V1 does, and browsing is somewhere you go.

```
        Now Playing  <-- home, and where playback returns
             |
     BACK    |  ENTER on a track
             v
          Library
             |
   +---------+----------+-----------+
   |         |          |           |
Playlists  Artists    Albums    All Tracks
   |         |          |           |
 Tracks   Albums      Tracks     Tracks
             |
          Tracks

  Library also holds: Settings
```

`BACK` from Library root goes to Now Playing when something is loaded. Long-press
`BACK` anywhere returns straight to Now Playing.

## Layouts

### Now Playing

Unchanged from V1, so all of its layout and refresh work carries over.

```
+--------------------------------------------+
| Song Title                     Artist Name |
| Album Name                                 |
| +----------+   lyric line one              |
| |          |   lyric line two              |
| |  96x96   |   lyric line three            |
| |  cover   |                               |
| +----------+   1:23   [>]   -2:45          |
|                =========-------------      |
+--------------------------------------------+
```

Actions:

| Input | Effect |
|---|---|
| UP tap | Previous track; restarts the current one if more than 3 s in |
| DOWN tap | Next track |
| UP / DOWN hold | Volume up / down, one step per ~180 ms, 0-20 |
| ENTER | Play / pause |
| BACK | Open the Library |
| MENU | Track context menu |

A tap on UP or DOWN acts on **release**, not press, because only the release
proves the press was not the start of a hold. The delay is a few tenths of a
second inside a 780 ms refresh, so it is not perceptible.

Volume is applied to the amplifier on every step while the key is held, but the
panel does not refresh until the key is released: the settle timer sees the
repeats as activity. The transport strip then shows `VOLUME 12/20` and a bar,
in the same place and shape as the progress bar, for 2.5 s before reverting to
the clock. One gesture costs two strip refreshes and one NVS write, however far
the volume moved.

### List view

Used for artists, albums, tracks, and playlists.

```
+--------------------------------------------+
| ARTISTS                            12/48   |  header, y 0..15
+--------------------------------------------+  rule at y 16
|   Aphex Twin                               |  row 0, y 18
|   Boards of Canada                         |  row 1, y 33
|###Burial###################################|  row 2, y 48  <- selected
|   Caribou                                  |  row 3, y 63
|   Clark                                    |  row 4, y 78
|   Four Tet                                 |  row 5, y 93
|   Jon Hopkins                              |  row 6, y 108
+--------------------------------------------+
```

Seven rows of 15 px starting at y=18, header 16 px, one rule. The selection is a
full-width inverted bar with white text. A thin caret would ghost less, but the
inverted bar stays legible as contrast decays between cleanup waveforms, and
legibility wins on a panel with no backlight.

Rows span x=0..295. 296 is a multiple of 8, so the partial window is byte-aligned
for SSD1680 without the padding V1 needed for its x=104 right column.

The `12/48` position counter replaces a scrollbar. It costs nothing to draw and
tells you where you are without scrolling to find out.

### Track list

Track lists earn two extra columns, since track order and length are exactly what
you want when choosing within an album.

```
+--------------------------------------------+
| UNTRUE                    Burial   6/11    |
+--------------------------------------------+
| 4  Endorphin                         2:59  |
| 5  Etched Headplate                  5:25  |
|###6##In##McDonalds###################2:07##|
| 7  Untrue                            3:52  |
| 8  Shell Of Light                    4:44  |
| 9  Dog Shelter                       3:43  |
| 10 Homeless                          4:19  |
+--------------------------------------------+
```

A `*` before the track number marks a track missing its `.lrc` sidecar, so a
badly prepared album is visible from the device rather than discovered mid-song.

### Letter jump

Scrolling is the wrong primitive for a large library even with fast scroll. Any
list longer than one screen accepts a jump: press MENU, or hold UP from the top
of the list, and pick a letter.

```
+--------------------------------------------+
| JUMP TO                                    |
+--------------------------------------------+
|                                            |
|    A B C D E F G H I J K L M               |
|    N O P Q R S T U V W X Y Z #             |
|                                            |
|              -> B <-                       |
+--------------------------------------------+
```

UP/DOWN move through the alphabet, ENTER jumps to the first entry at that letter,
BACK cancels. `#` collects digits and symbols. Two refreshes gets you anywhere in
the library, against dozens for scrolling.

The grid is drawn once; moving the cursor redraws only the selected-letter line
at the bottom, which keeps the jump screen responsive.

### Context menu

Opened with MENU on any item. Contents depend on what is selected.

```
+--------------------------------------------+
| IN McDONALDS                               |
+--------------------------------------------+
|###Play#now#################################|
|   Play next                                |
|   Add to queue                             |
|   Add to playlist...                       |
|   Shuffle album                            |
+--------------------------------------------+
```

### Index build progress

A full card scan takes a while and must not look like a crash.

```
+--------------------------------------------+
|                                            |
|            BUILDING INDEX                  |
|                                            |
|          312 tracks found                  |
|       ====================------            |
|                                            |
|            Burial - Untrue                 |
+--------------------------------------------+
```

Refresh this at most every 2 s or every 50 files, whichever is less frequent.
Refreshing per file would make indexing several times slower than the scan
itself, since each refresh costs 780 ms.

### Settings

Library size, card status, firmware version, output volume, shuffle and repeat
defaults, sleep timeout, and a `Rescan card` action.

## Refresh budget

| Event | Region | Notes |
|---|---|---|
| Selection moved | List area | One refresh per settled burst, not per press |
| Level entered/left | Whole screen | New header and new content |
| Letter cursor moved | Bottom line only | Grid is static |
| Lyric changed | Right column | As V1 |
| Transport tick | Bottom-right strip | See below |
| Volume changed | Bottom-right strip | Once on release, once when the overlay reverts |
| Track changed | Whole screen | New metadata and cover |
| Paused / disconnected | None | Panel retains the frame, as V1 |
| Cleanup | Full waveform | Every `FULL_REFRESH_AFTER_PARTIALS` |

V1's open Phase 2 item applies here and should be settled in V2 rather than
inherited: transport refreshes should be content-driven, not unconditionally
every 800 ms. Refresh the strip when the displayed *second* or the progress
*pixel* actually changes, and make the interval configurable so 3 s, 5 s, and
10 s cadences can be compared on the panel. Browsing is bursty and cheap; a
continuously ticking playback screen is what actually wears the panel.

## Playback state

A **queue** is the single playback primitive. Playing an album, a playlist, an
artist, or one track all mean "replace the queue and start at index N". Shuffle
permutes the queue rather than randomizing on each advance, so `previous` stays
meaningful.

Repeat is off / all / one. Resume position, queue identity, and shuffle state
persist to NVS, so a power cycle returns to the same place. The panel keeps
showing the last frame while powered off, which makes resume feel continuous.

## Audio

The interface is unchanged from V2's M3 boundary, so the UI does not know which
backend it is talking to:

```
begin() / play(path, startMs, playing) / pause() / resume() / stop()
seek(ms) / positionMs() / isPlaying()
```

`AUDIO_HARDWARE 1`, the V3 default, is ESP8266Audio's libmad decoder on core 0
writing to I2S. `AUDIO_HARDWARE 0` is V2's simulated playhead.

The requirements, and how they are met:

- **The e-paper refresh blocks core 1 for ~780 ms and must never starve audio.**
  The decoder is a task pinned to core 0, which nothing else uses now that BLE
  and Wi-Fi are gone. The panel never touches that core.
- **The decoder must not starve core 0's idle task either.** The library's I2S
  write is non-blocking, so `decoder->loop()` returns the moment the DMA ring is
  full. The task sleeps one tick after every call; without that it would spin
  at priority 2 until the task watchdog fired.
- **The card is shared.** The UI reads index records, lyrics and covers from
  core 1 while the decoder streams from core 0, behind the storage mutex. The
  decoder's opportunistic buffer top-up uses a zero-timeout lock: if the UI
  holds the card, it skips that round and plays from its 32 KB buffer rather
  than wait.
- **Stopping must not free a chain mid-decode.** Every touch of the decoder
  chain, from either core, holds a chain mutex. The task holds it for one
  `loop()` at a time, so `stop()` or `seek()` waits for at most one DMA refill.
- **Lyrics follow the speaker, not the clock.** The playhead is the count of
  frames accepted by I2S, minus what is still queued in DMA. If the card ever
  stalls, the lyric waits for the music.
- **Lyrics lead by the measured refresh.** The panel times every partial
  refresh; a line starts drawing that long before its timestamp, plus a user
  offset tuned on the device (hold MENU on Now Playing). A clock tick that
  would still be running when a line is due is skipped, and a lyric change
  redraws the lyric and the clock in one refresh rather than two.
- **Playback never starts inside an ID3 tag.** libmad hunts for a frame sync
  from wherever it starts reading, and embedded cover art is full of false
  ones. Every start, at 0 or mid-track, seeks to an offset at or past the first
  real frame.
- **Seeking is a decoder restart at a byte offset.** CBR offsets are exact from
  the bitrate; VBR uses the Xing table of contents. Resume after power-off and
  "restart this track" both go through it.
- **Pause is silent and stays silent.** The amp is muted through its SD pin and
  the DMA ring is overwritten with silence, so resume does not open with a
  100 ms echo of where pause happened.
