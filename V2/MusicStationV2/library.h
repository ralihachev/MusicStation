// On-card music library index.
//
// The index exists so that browsing costs a seek rather than a scan. Every
// record is fixed width, so row N of any list is one seek to N * sizeof(record)
// and the list view never holds more than the seven rows on screen. There is
// no PSRAM on this board and M4's audio buffers will claim most of what is
// left, so nothing here may scale with library size.
//
// Records are written pre-sorted. Walking /Music in sorted order visits
// artists, then their albums, then their tracks, which is exactly the output
// order required, so the build needs no sort pass and no second file. It also
// deduplicates structurally: an artist owns one directory, so its name is
// written once.
//
// Layout and record definitions are documented in V2/SD_LAYOUT.md.
#pragma once

#include <Arduino.h>

#include "storage.h"

// 2: indexes built before the SD max_files fix recorded every duration as
// zero. 3: so did indexes built at 20 MHz, where the audio read itself failed.
// zero, because the audio file could not be opened during the build. Bumping
// this forces those indexes to be rebuilt rather than trusted.
// 5: every index before this recorded zero durations, because SPI-mode reads
// of the audio files silently failed. SDIO reads them.
constexpr uint16_t INDEX_VERSION = 5;
constexpr const char *INDEX_DIR = "/.station";
constexpr const char *MUSIC_DIR = "/Music";
constexpr const char *PLAYLIST_DIR = "/Playlists";

// Track and album indices are 16-bit throughout, which caps a library at 65535
// tracks. At roughly 4 MB a track that is far past any card this device will
// carry, and it halves the size of every record.
constexpr uint16_t LIBRARY_MAX_TRACKS = 65535;

enum TrackFlags : uint8_t
{
  TRACK_HAS_LRC = 0x01,
  TRACK_HAS_COVER = 0x02,
};

struct __attribute__((packed)) TrackRecord
{
  uint32_t pathOffset;
  uint32_t titleOffset;
  uint16_t artistIndex;
  uint16_t albumIndex;
  uint16_t trackNumber;
  uint16_t durationSeconds;
  uint32_t fileSize;
  uint32_t modifiedTime;
  uint8_t flags;
  uint8_t reserved[7];
};

struct __attribute__((packed)) AlbumRecord
{
  uint32_t nameOffset;
  uint16_t artistIndex;
  uint16_t firstTrack;
  uint16_t trackCount;
  uint16_t year;
  uint8_t flags;
  uint8_t reserved[3];
};

struct __attribute__((packed)) ArtistRecord
{
  uint32_t nameOffset;
  uint16_t firstAlbum;
  uint16_t albumCount;
  uint16_t firstTrack;
  uint16_t trackCount;
};

static_assert(sizeof(TrackRecord) == 32, "track record must stay 32 bytes");
static_assert(sizeof(AlbumRecord) == 16, "album record must stay 16 bytes");
static_assert(sizeof(ArtistRecord) == 12, "artist record must stay 12 bytes");

// Reported during a build so the UI can show progress. Refresh from this at
// most every 2 s or 50 tracks: at ~780 ms a refresh, drawing per file would
// make indexing several times slower than the scan itself.
typedef void (*BuildProgress)(uint16_t tracksFound, const char *artist,
                              const char *album);

bool libraryOpen();
void libraryClose();
bool libraryValid();

// True when the index is missing, written by another firmware version, built
// from a different card, or when /Music has gained or lost an artist folder.
// The artist-folder count is one directory read, so it is cheap enough for
// every boot; it does not notice an album added inside an existing artist,
// which is what the Settings rescan is for.
bool libraryNeedsRebuild();

bool libraryBuild(BuildProgress progress);

uint16_t libraryArtistCount();
uint16_t libraryAlbumCount();
uint16_t libraryTrackCount();

bool libraryArtist(uint16_t index, ArtistRecord &out);
bool libraryAlbum(uint16_t index, AlbumRecord &out);
bool libraryTrack(uint16_t index, TrackRecord &out);

String libraryString(uint32_t offset);
String libraryArtistName(uint16_t index);
String libraryAlbumName(uint16_t index);
String libraryTrackTitle(uint16_t index);
String libraryTrackPath(uint16_t index);

// Sidecar paths, derived from the track path alone. Nothing needs the index to
// find a track's lyrics or artwork, which is what lets a playlist entry be
// nothing but a path.
// Metadata for a track that is not in the index — a playlist entry pointing
// somewhere unusual. The folder layout carries artist and album, so a path is
// self-describing and a playlist needs no index lookup at all.
void libraryParseTrackPath(const String &path, String &artist, String &album,
                           uint16_t &trackNumber, String &title);

String libraryLyricPath(const String &trackPath);
String libraryCoverPath(const String &trackPath);
