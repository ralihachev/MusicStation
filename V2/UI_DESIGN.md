# MusicStation V2 interface design

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

V2 uses **five tactile buttons**, active low, wired per [WIRING.md](WIRING.md).
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

Actions: UP/DOWN skip track, ENTER play/pause, BACK opens Library,
MENU opens the track context menu.

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

## Audio boundary

Audio sits behind one interface from the start:

```
begin() / play(path) / pause() / resume() / stop()
seek(ms) / positionMs() / durationMs() / isPlaying()
```

M3 implements this as a simulated clock driven by `millis()`, which is enough to
develop and test the queue, lyric scheduling, and every screen before the
amplifier exists. M4 swaps in the real MP3 decoder with no UI changes.

The hard M4 requirement: **the e-paper refresh blocks for roughly 780 ms and must
never starve audio.** The decoder runs as its own task pinned to the core that
does not run the UI, feeding an I2S DMA ring buffer holding at least 1-2 seconds.
UI, SD index reads, and panel updates run on the other core. Both touch the SD
card, so card access needs a mutex, and the decoder's buffer has to be deep
enough to ride out a UI read that holds it.
