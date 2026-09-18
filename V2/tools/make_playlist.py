#!/usr/bin/env python3
"""Build a .m3u playlist from tracks already on a MusicStation card.

Playlist entries are absolute card paths and nothing else. Because lyrics and
artwork are sidecars sharing each track's basename, and artist and album come
from the folder structure, a path is self-describing: the firmware needs no
index lookup to play a playlist entry, so there is nothing to go stale.

Usage:
    python3 make_playlist.py --card /Volumes/MUSIC --list
    python3 make_playlist.py --card /Volumes/MUSIC --name Favourites \\
        --match Creep --match "Blow out"
    python3 make_playlist.py --card /Volumes/MUSIC --name "All Radiohead" \\
        --match Radiohead/
"""

import argparse
import os
import shutil
import subprocess
import sys


def find_tracks(card):
    """Every .mp3 under /Music, in the same order the firmware indexes them:
    artist, then album, then filename."""
    root = os.path.join(card, "Music")
    if not os.path.isdir(root):
        sys.exit(f"no /Music folder on {card}")
    found = []
    for base, dirs, files in os.walk(root):
        dirs[:] = sorted(d for d in dirs if not d.startswith("."))
        for name in sorted(files):
            if name.lower().endswith(".mp3") and not name.startswith("."):
                full = os.path.join(base, name)
                found.append("/" + os.path.relpath(full, card))
    return found


def describe(card_path):
    parts = card_path.strip("/").split("/")
    # /Music/<Artist>/<Album>/<NN Title>.mp3
    if len(parts) >= 4:
        return parts[1], parts[2], os.path.splitext(parts[3])[0]
    return "", "", os.path.splitext(parts[-1])[0]


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--card", required=True, help="card root, e.g. /Volumes/MUSIC")
    parser.add_argument("--name", help="playlist name, written as <name>.m3u")
    parser.add_argument("--match", action="append", default=[],
                        help="case-insensitive substring of the track path; repeatable, order is kept")
    parser.add_argument("--list", action="store_true", help="list every track and exit")
    parser.add_argument("--dry-run", action="store_true", help="report, write nothing")
    args = parser.parse_args()

    tracks = find_tracks(args.card)
    if not tracks:
        sys.exit("no .mp3 files found under /Music")

    if args.list or not args.name:
        for path in tracks:
            artist, album, title = describe(path)
            print(f"  {title:<34} {artist} / {album}")
        print(f"\n{len(tracks)} track(s)")
        if not args.name:
            return

    if not args.match:
        sys.exit("--name needs at least one --match (or use --list first)")

    # One entry per --match, in the order given, so a playlist reads as an
    # intentional running order rather than whatever the walk happened to find.
    selected = []
    for needle in args.match:
        hits = [t for t in tracks if needle.lower() in t.lower()]
        if not hits:
            print(f"  no match for {needle!r}")
            continue
        for hit in hits:
            if hit not in selected:
                selected.append(hit)

    if not selected:
        sys.exit("nothing matched; run with --list to see what is on the card")

    lines = ["#EXTM3U"]
    for path in selected:
        artist, _, title = describe(path)
        lines.append(f"#EXTINF:-1,{artist} - {title}")
        lines.append(path)
        print(f"  + {title}")

    destination = os.path.join(args.card, "Playlists", args.name + ".m3u")
    print(f"\n{len(selected)} track(s) -> {destination}")
    if args.dry_run:
        return
    os.makedirs(os.path.dirname(destination), exist_ok=True)
    with open(destination, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")

    # macOS writes a ._ sidecar per file on FAT. The firmware skips them, but
    # they waste a cluster each.
    if shutil.which("dot_clean"):
        subprocess.run(["dot_clean", "-m", args.card], check=False)


if __name__ == "__main__":
    main()
