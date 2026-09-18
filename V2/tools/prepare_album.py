#!/usr/bin/env python3
"""Prepare an album for a MusicStation V2 microSD card.

Point this at a folder of MP3s and it writes the card layout described in
V2/SD_LAYOUT.md: the audio renamed into /Music/<Artist>/<Album>/, a .lrc
sidecar of synchronized lyrics, and cover.bin as a pre-baked 1-bit bitmap.

Everything expensive or fallible happens here rather than on the device:
HTTPS, JSON, JPEG decoding, sharpening and dithering run once, on a machine
with memory to spare, instead of on every track change on a microcontroller
that is also decoding audio. That is what lets the firmware drop JPEGDEC,
ArduinoJson and the whole TLS stack.

Usage:
    python3 prepare_album.py ~/Music/Untrue --dest /Volumes/MUSICSTATION
    python3 prepare_album.py ~/Music/Untrue --dest . --artist Burial --album Untrue

Requires Pillow for artwork (pip install Pillow). Without it, everything else
still runs and the cover is skipped.
"""

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request

USER_AGENT = "MusicStation/2.0 (https://github.com/ralihachev/MusicStation)"
# The firmware decodes MP3 only. Anything else is transcoded on the way in,
# which matters because iTunes Store purchases arrive as .m4a.
CONVERTIBLE = (".m4a", ".aac", ".flac", ".wav", ".aiff", ".aif", ".ogg", ".opus", ".wma")
COVER_SIZE = 96
COVER_FULL = (296, 128)

try:
    from PIL import Image, ImageEnhance, ImageFilter, ImageOps
    HAVE_PIL = True
except ImportError:
    HAVE_PIL = False


# --------------------------------------------------------------------- mp3

def _syncsafe(data):
    return (data[0] & 0x7F) << 21 | (data[1] & 0x7F) << 14 | \
           (data[2] & 0x7F) << 7 | (data[3] & 0x7F)


def read_id3(path):
    """Minimal ID3v2 text-frame reader. Returns {} when there is no tag."""
    tags = {}
    with open(path, "rb") as handle:
        header = handle.read(10)
        if len(header) < 10 or header[:3] != b"ID3":
            return tags
        version = header[3]
        size = _syncsafe(header[6:10])
        body = handle.read(size)

    offset = 0
    name_length = 4 if version >= 3 else 3
    while offset + name_length + 4 <= len(body):
        name = body[offset:offset + name_length].decode("latin-1", "ignore")
        if not name.strip("\x00"):
            break
        if version >= 3:
            frame_size = struct.unpack(">I", body[offset + 4:offset + 8])[0]
            if version >= 4:
                frame_size = _syncsafe(body[offset + 4:offset + 8])
            offset += 10
        else:
            frame_size = int.from_bytes(body[offset + 3:offset + 6], "big")
            offset += 6
        if frame_size <= 0 or offset + frame_size > len(body):
            break
        raw = body[offset:offset + frame_size]
        offset += frame_size
        if not name.startswith("T") or not raw:
            continue
        encoding, text = raw[0], raw[1:]
        try:
            if encoding == 0:
                value = text.decode("latin-1")
            elif encoding == 1:
                value = text.decode("utf-16")
            elif encoding == 2:
                value = text.decode("utf-16-be")
            else:
                value = text.decode("utf-8")
        except UnicodeDecodeError:
            continue
        tags[name] = value.split("\x00")[0].strip()
    return tags


BITRATE_V1_L3 = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0]
BITRATE_V2_L3 = [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0]
SAMPLE_RATES = {3: [44100, 48000, 32000], 2: [22050, 24000, 16000], 0: [11025, 12000, 8000]}


def mp3_duration(path):
    """Seconds, preferring the Xing frame count. Mirrors mp3meta.cpp so the
    device and this script never disagree about a track's length."""
    size = os.path.getsize(path)
    with open(path, "rb") as handle:
        header = handle.read(10)
        start = 0
        if header[:3] == b"ID3":
            start = 10 + _syncsafe(header[6:10])
            if header[5] & 0x10:
                start += 10
        handle.seek(start)
        window = handle.read(4096)

    for i in range(len(window) - 4):
        if window[i] != 0xFF or (window[i + 1] & 0xE0) != 0xE0:
            continue
        version = (window[i + 1] >> 3) & 3
        layer = (window[i + 1] >> 1) & 3
        if version == 1 or layer != 1:
            continue
        bitrate_index = (window[i + 2] >> 4) & 0xF
        sample_index = (window[i + 2] >> 2) & 3
        mono = ((window[i + 3] >> 6) & 3) == 3
        if bitrate_index in (0, 15) or sample_index == 3:
            continue
        table = BITRATE_V1_L3 if version == 3 else BITRATE_V2_L3
        bitrate = table[bitrate_index] * 1000
        sample_rate = SAMPLE_RATES[version][sample_index]
        if not bitrate or not sample_rate:
            continue
        samples = 1152 if version == 3 else 576

        xing_at = i + 4 + ((17 if mono else 32) if version == 3 else (9 if mono else 17))
        if xing_at + 12 <= len(window) and window[xing_at:xing_at + 4] in (b"Xing", b"Info"):
            flags = struct.unpack(">I", window[xing_at + 4:xing_at + 8])[0]
            if flags & 1:
                frames = struct.unpack(">I", window[xing_at + 8:xing_at + 12])[0]
                return round(frames * samples / sample_rate)
        return int((size - start - i) * 8 / bitrate)
    return 0


# ----------------------------------------------------------------- network

def _get(url, timeout=15):
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read()


def fetch_lyrics(artist, title, album, duration):
    """LRCLIB, exact match first then a search. Returns LRC text or None.

    A record with only plain lyrics and no timestamps is treated as a miss:
    the device needs timestamps, and a plain-text sidecar would display as a
    single unchanging line."""
    query = urllib.parse.urlencode({
        "artist_name": artist, "track_name": title,
        "album_name": album, "duration": duration,
    })
    for url in (f"https://lrclib.net/api/get?{query}",
                "https://lrclib.net/api/search?" + urllib.parse.urlencode(
                    {"artist_name": artist, "track_name": title})):
        try:
            payload = json.loads(_get(url))
        except Exception:
            continue
        records = payload if isinstance(payload, list) else [payload]
        for record in records:
            synced = (record or {}).get("syncedLyrics")
            if synced and "[" in synced:
                return synced
    return None


def fetch_cover(artist, album):
    term = urllib.parse.quote(f"{artist} {album}")
    url = f"https://itunes.apple.com/search?term={term}&entity=album&limit=1"
    try:
        results = json.loads(_get(url)).get("results") or []
        if not results:
            return None
        art = results[0].get("artworkUrl100")
        if not art:
            return None
        return _get(art.replace("100x100bb", "600x600bb"))
    except Exception:
        return None


# ---------------------------------------------------------------- artwork

def bake_bitmap(jpeg_bytes, size):
    """JPEG -> MSBM 1-bit bitmap, sharpened and Floyd-Steinberg dithered.

    Width must be a multiple of 8: rows are byte-packed, so any other width
    would land every row after the first at the wrong offset. Bit 1 means
    black, matching Adafruit GFX drawBitmap with GxEPD_BLACK as the colour."""
    import io

    width, height = size
    if width % 8:
        raise ValueError("width must be a multiple of 8")

    image = Image.open(io.BytesIO(jpeg_bytes)).convert("L")
    image = ImageOps.fit(image, (width, height), Image.LANCZOS)
    image = ImageOps.autocontrast(image, cutoff=1)
    # Dithering throws away most of the tonal range, so detail has to be
    # exaggerated before it is discarded rather than after.
    image = image.filter(ImageFilter.UnsharpMask(radius=1.2, percent=150, threshold=2))
    image = ImageEnhance.Contrast(image).enhance(1.15)
    packed = image.convert("1")  # Floyd-Steinberg by default

    # PIL packs 1 = white; the panel wants 1 = black.
    data = bytes(~b & 0xFF for b in packed.tobytes())
    header = b"MSBM" + bytes([1, 0]) + struct.pack("<HH", width, height)
    return header + data


# -------------------------------------------------------------------- main

def safe_name(value):
    """FAT32 rejects these outright, and a stray '/' would invent a folder."""
    cleaned = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "_", value).strip(" .")
    return cleaned or "Unknown"


# Rippers name files in a handful of recognisable shapes. Untagged rips are
# common enough that falling back to the filename has to handle more than a
# leading number, or the whole filename ends up as the title.
FILENAME_PATTERNS = [
    # "01 - Creep", "01. Creep", "01 Creep"
    r"^\s*(\d{1,3})\s*[-._)]*\s+(.+)$",
    # "RADIOHEAD (PABLO HONEY) - Track  2 - Creep"
    r"[Tt]rack\s*(\d{1,3})\s*[-._]+\s*(.+)$",
    # "Radiohead - 02 - Creep"
    r"^.*?[-_]\s*(\d{1,3})\s*[-._]+\s*(.+)$",
]


def parse_filename(stem):
    """Returns (number, title) or None. Tries the shapes above in order."""
    for pattern in FILENAME_PATTERNS:
        # search, not match: the "Track N" shape appears mid-filename, after
        # the artist and album. Patterns that must anchor carry their own "^".
        match = re.search(pattern, stem)
        if match:
            title = re.sub(r"\s+", " ", match.group(2)).strip(" -_.")
            if title:
                return int(match.group(1)), title
    return None


def convert_to_mp3(source, workdir, bitrate="320k"):
    """Transcodes non-MP3 audio into workdir. Returns the folder to index.

    Lossy-to-lossy at 320k is wasteful in theory but the loss is inaudible
    against a 96x96 e-paper player, and it keeps one decoder in the firmware
    rather than three."""
    candidates = [f for f in sorted(os.listdir(source))
                  if f.lower().endswith(CONVERTIBLE) and not f.startswith(".")]
    if not candidates:
        return source
    if shutil.which("ffmpeg") is None:
        print("  non-MP3 audio found but ffmpeg is missing (brew install ffmpeg)")
        return source

    print(f"  converting {len(candidates)} file(s) to MP3")
    for name in candidates:
        target = os.path.join(workdir, os.path.splitext(name)[0] + ".mp3")
        subprocess.run(
            ["ffmpeg", "-loglevel", "error", "-y", "-i", os.path.join(source, name),
             "-map_metadata", "0", "-codec:a", "libmp3lame", "-b:a", bitrate, target],
            check=True)
    # Existing MP3s in the same folder are used as they are.
    for name in sorted(os.listdir(source)):
        if name.lower().endswith(".mp3") and not name.startswith("."):
            shutil.copy(os.path.join(source, name), os.path.join(workdir, name))
    return workdir


def strip_appledouble(root):
    """macOS writes a ._ sidecar per file on FAT when xattrs are preserved.
    The firmware skips them, but they waste a cluster each."""
    if shutil.which("dot_clean"):
        subprocess.run(["dot_clean", "-m", root], check=False)


def collect_tracks(source, artist_override, album_override):
    tracks = []
    for name in sorted(os.listdir(source)):
        if not name.lower().endswith(".mp3") or name.startswith("."):
            continue
        path = os.path.join(source, name)
        tags = read_id3(path)
        stem = os.path.splitext(name)[0]
        number = 0
        match = parse_filename(stem)
        title = tags.get("TIT2") or (match[1] if match else stem)
        if tags.get("TRCK"):
            try:
                number = int(str(tags["TRCK"]).split("/")[0])
            except ValueError:
                number = 0
        if not number and match:
            number = match[0]
        tracks.append({
            "path": path,
            "title": title.strip(),
            "number": number,
            "artist": artist_override or tags.get("TPE1") or "Unknown Artist",
            "album": album_override or tags.get("TALB")
                     or os.path.basename(os.path.abspath(source)),
            "duration": mp3_duration(path),
        })
    tracks.sort(key=lambda t: (t["number"] == 0, t["number"], t["title"].lower()))
    for position, track in enumerate(tracks, start=1):
        if not track["number"]:
            track["number"] = position
    return tracks


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source", nargs="+",
                        help="one or more album folders (audio is converted to MP3 if needed)")
    parser.add_argument("--dest", required=True, help="card root, e.g. /Volumes/MUSICSTATION")
    parser.add_argument("--artist", help="override the artist tag")
    parser.add_argument("--album", help="override the album tag")
    parser.add_argument("--no-network", action="store_true",
                        help="skip lyric and artwork lookup")
    parser.add_argument("--dry-run", action="store_true", help="report, write nothing")
    args = parser.parse_args()

    if len(args.source) > 1 and (args.artist or args.album):
        sys.exit("--artist/--album apply to a single album; run them one at a time")

    for folder in args.source:
        if not os.path.isdir(folder):
            sys.exit(f"not a folder: {folder}")

    failures = 0
    for folder in args.source:
        try:
            process_album(folder, args)
        except Exception as error:
            failures += 1
            print(f"  FAILED: {error}")
    if not args.dry_run:
        strip_appledouble(args.dest)
    print(f"\ndone: {len(args.source) - failures}/{len(args.source)} album(s)")


def process_album(folder, args):
    with tempfile.TemporaryDirectory(prefix="musicstation-") as workdir:
        indexed = folder if args.dry_run else convert_to_mp3(folder, workdir)
        _process(indexed, args)


def _process(source, args):
    tracks = collect_tracks(source, args.artist, args.album)
    if not tracks:
        print(f"no playable audio in {source}")
        return

    artist = safe_name(tracks[0]["artist"])
    album = safe_name(tracks[0]["album"])
    target = os.path.join(args.dest, "Music", artist, album)
    print(f"{artist} / {album}  ->  {target}")
    if not args.dry_run:
        os.makedirs(target, exist_ok=True)

    for track in tracks:
        base = f"{track['number']:02d} {safe_name(track['title'])}"
        destination = os.path.join(target, base + ".mp3")
        minutes, seconds = divmod(track["duration"], 60)
        print(f"  {base}  {minutes}:{seconds:02d}", end="", flush=True)

        if not args.dry_run:
            # copy, not copy2: preserving macOS extended attributes on a FAT
            # volume makes the Finder write an AppleDouble "._" sidecar beside
            # every file. The firmware skips them, but they are pure waste on
            # the card and nothing here needs the metadata.
            shutil.copy(track["path"], destination)

        if args.no_network:
            print()
            continue

        lyrics = fetch_lyrics(track["artist"], track["title"], track["album"],
                              track["duration"])
        if lyrics:
            print("  + lyrics")
            if not args.dry_run:
                with open(os.path.join(target, base + ".lrc"), "w",
                          encoding="utf-8") as handle:
                    handle.write(lyrics)
        else:
            print("  (no synced lyrics)")

    if args.no_network:
        return
    if not HAVE_PIL:
        print("  Pillow not installed: skipping cover (pip install Pillow)")
        return

    jpeg = fetch_cover(tracks[0]["artist"], tracks[0]["album"])
    if not jpeg:
        print("  no artwork found")
        return
    for filename, size in (("cover.bin", (COVER_SIZE, COVER_SIZE)),
                           ("cover_full.bin", COVER_FULL)):
        blob = bake_bitmap(jpeg, size)
        print(f"  {filename}  {size[0]}x{size[1]}  {len(blob)} bytes")
        if not args.dry_run:
            with open(os.path.join(target, filename), "wb") as handle:
                handle.write(blob)


if __name__ == "__main__":
    main()
