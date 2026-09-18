# MusicStation V2 microSD layout

This layout is the contract between the card and the firmware. Anything that can
write it works: a card reader today, USB mass storage or a Wi-Fi upload later.
Nothing in the firmware assumes how the files arrived.

Format the card as **FAT32** (exFAT support on ESP32 Arduino is inconsistent).

## Folder layout

```
/Music/<Artist>/<Album>/01 Title.mp3     audio
                       /01 Title.lrc     synchronized lyrics, optional
                       /cover.bin        1-bit album art, optional
                       /album.txt        metadata override, optional
/Playlists/Focus.m3u
/Visuals/<name>.bin                      optional full-screen art
/.station/                               device-owned, safe to delete
```

Two decisions worth stating explicitly.

**Sidecars sit next to the audio file and share its basename.** A track's lyrics
and its artwork are derivable from its path alone. That means a playlist entry
needs nothing but a path: no index lookup, no database join, no resolution step
that can fail. It is the simplest thing that works and it removes a whole class
of "playlist points at a track that moved" bugs.

**Artist and album come from the folder structure, not from ID3 tags.** Parsing
ID3 costs a seek and a variable-length read per track during indexing, and tags
in real libraries are inconsistent. Folders are already how the music is
organized on the Mac. `album.txt` exists for the cases where the folder name is
wrong, and ID3 parsing can be added later as a fallback for loose files.

Track numbers come from a leading `NN ` or `NN - ` in the filename. Without one,
tracks sort alphabetically within the album.

## Artwork: `cover.bin`

A pre-baked 1-bit bitmap, not a JPEG. This is what lets V2 drop `JPEGDEC`
entirely, and with it the large contiguous heap allocation that V1 fights for.
The same block is what the audio ring buffer will want.

```
offset  size  field
0       4     magic "MSBM"
4       1     version = 1
5       1     flags        bit0: 1 = inverted
6       2     width        pixels, must be a multiple of 8
8       2     height       pixels
10      ...   packed 1-bit rows, MSB first, (width/8) bytes per row
```

Two sizes are used:

- `cover.bin` at 96x96 for the Now Playing screen, matching V1's layout.
- `cover_full.bin` at 296x128, optional, for the full-screen visual mode.

Dithering happens on the Mac, where there is floating point, no heap pressure,
and the freedom to compare algorithms against the real panel. V1's sharpen-then-
error-diffuse approach is the starting point, since it is already tuned for this
display.

## Lyrics: `.lrc`

Standard LRC. Reuse V1's `parseLrcLine()` against a file stream instead of an
HTTP stream, which deletes the JSON, TLS, and LRCLIB fallback machinery.

```
[ti:In McDonalds]
[00:12.40]I can't take my eyes off you
[00:18.10]
[00:21.75]Holding on for dear life
```

UTF-8, with the same ASCII + Cyrillic glyph coverage as V1. Empty timestamps
clear the lyric line, which is how instrumental gaps are expressed.

The lyric arena from V1 Phase 3 still applies, but a local file makes it easier:
the file size is known before reading, so the arena is sized once with no growth
and no reallocation. The `LYRIC_LEAD_MS = 800` waveform compensation carries over
unchanged.

## Playlists: `.m3u`

```
#EXTM3U
#EXTINF:127,Burial - In McDonalds
/Music/Burial/Untrue/06 In McDonalds.mp3
```

Absolute card paths. `#EXTINF` is read when present and ignored otherwise. The
playlist name is the filename. Playlists written by the device use this same
format, so a playlist made on the station opens on the Mac and the reverse.

## Device index: `/.station/`

Rebuilt from the card whenever it is missing or stale. Never edit it by hand;
deleting it is always safe.

```
/.station/index.hdr      magic, version, card id, counts, build timestamp
          strings.bin    deduplicated UTF-8 blob, NUL separated
          tracks.bin     fixed 32-byte records
          albums.bin     fixed 16-byte records
          artists.bin    fixed 12-byte records
          state.bin      resume position, queue identity, shuffle seed
```

### Why fixed-width records

**The index must never need RAM proportional to library size.** There is no
PSRAM on the current board, and M4's audio buffers will claim most of what is
left. Fixed-width records make row N of any list a single seek:
`offset = N * sizeof(record)`. Drawing seven visible rows costs fourteen short
SD reads, roughly a millisecond each, against a 780 ms refresh. The list view
never holds more than the seven rows on screen.

Records are stored pre-sorted by the build pass, so browsing never sorts at
runtime.

```
track record (32 bytes)
  uint32 pathOffset      into strings.bin
  uint32 titleOffset     into strings.bin
  uint16 artistIndex
  uint16 albumIndex
  uint16 trackNumber
  uint16 durationSeconds
  uint32 fileSize
  uint32 modifiedTime
  uint8  flags           bit0 hasLrc, bit1 hasCover
  uint8  reserved[7]

album record (16 bytes)
  uint32 nameOffset
  uint16 artistIndex
  uint16 firstTrack      index into tracks.bin
  uint16 trackCount
  uint16 year
  uint8  flags           bit0 hasCover
  uint8  reserved[3]

artist record (12 bytes)
  uint32 nameOffset
  uint16 firstAlbum
  uint16 albumCount
  uint16 firstTrack
  uint16 trackCount
```

Because tracks are sorted by artist, then album, then track number, an album's
tracks are contiguous: `firstTrack` plus `trackCount` is the whole track list,
and an artist's albums work the same way. No secondary lookup tables.

`durationSeconds` is the one field that requires reading into each file during
indexing. For constant-bitrate MP3 it is derivable from the file size and the
first frame header; VBR needs the Xing/VBRI header. Both are a single short read
at the start of the file. Getting this right at index time matters because the
lyric scheduler and the progress bar both depend on it, and a wrong duration is
visible on every screen.

### Rebuild triggers

- `/.station/index.hdr` missing or its version does not match the firmware
- Card id differs from the one recorded in the header
- `Rescan card` chosen in Settings
- Card was removed and reinserted

A file count and newest modification time are stored in the header as a cheap
staleness heuristic, so a card that gained an album is noticed at boot without
a full rescan being forced on every power-up. It is a heuristic, not a guarantee:
an edit that leaves both unchanged is missed, which is what `Rescan card` is for.

## Preparation script

`tools/prepare_album.py` takes an album folder and produces the layout above:
it reads the ID3 tags, computes each duration the same way `mp3meta.cpp` does,
fetches synchronized lyrics from LRCLIB and art from iTunes using the same
sources V1 already talks to, bakes the art to `cover.bin` at both sizes, and
writes the result to the mounted card.

```
python3 tools/prepare_album.py ~/Music/Untrue --dest /Volumes/MUSICSTATION
```

`--artist` and `--album` override bad tags, `--no-network` skips the lookups,
and `--dry-run` reports without writing. Always dry-run an untagged rip first:
with no ID3 tags the artist and album fall back to the folder name, and the
title is recovered from the filename. Three ripper naming shapes are
recognised, including the number appearing mid-filename as in
`ARTIST (ALBUM) - Track 02 - Creep.mp3`.

Files are copied without their macOS extended attributes. Preserving them on a
FAT volume makes the Finder write an AppleDouble `._` sidecar beside every
file; the firmware skips those, but they waste a cluster each. Pillow is needed for artwork; without
it everything else still runs and the cover is skipped.

A record that LRCLIB has only as plain text is treated as a miss, because the
device needs timestamps and a plain-text sidecar would display as one
unchanging line for the whole song.

Keeping this on the Mac is what makes the device simple. Every expensive or
fallible operation - HTTPS, JSON, JPEG decode, dithering, tag cleanup, character
set repair - happens once, on a machine with memory to spare, instead of on every
track change on a microcontroller that is also decoding audio.
