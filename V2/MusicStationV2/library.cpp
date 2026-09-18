#include "library.h"

#include <algorithm>
#include <ctype.h>
#include <strings.h>
#include <vector>

#include "mp3meta.h"

namespace
{

struct __attribute__((packed)) IndexHeader
{
  char magic[4]; // "MSIX"
  uint16_t version;
  uint16_t artistCount;
  uint16_t albumCount;
  uint16_t trackCount;
  uint16_t sourceArtistDirs;
  uint32_t stringBytes;
  uint64_t cardSize;
};

const char *HDR_PATH = "/.station/index.hdr";
const char *STRINGS_PATH = "/.station/strings.bin";
const char *TRACKS_PATH = "/.station/tracks.bin";
const char *ALBUMS_PATH = "/.station/albums.bin";
const char *ARTISTS_PATH = "/.station/artists.bin";

IndexHeader header = {};
bool indexOpen = false;
File stringsFile, tracksFile, albumsFile, artistsFile;

// macOS writes AppleDouble sidecars (._Track.mp3) and metadata directories
// (.Spotlight-V100, .fseventsd, .Trashes) onto FAT volumes. Indexing those
// would double the apparent track count with unplayable files, so every
// dot-prefixed entry is skipped.
bool isHiddenName(const char *name)
{
  return name == nullptr || name[0] == '.' || name[0] == '\0';
}

bool hasExtension(const char *name, const char *extension)
{
  size_t nameLength = strlen(name);
  size_t extLength = strlen(extension);
  if (nameLength <= extLength)
    return false;
  return strcasecmp(name + nameLength - extLength, extension) == 0;
}

const char *baseName(const char *path)
{
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

// One directory's entries, sorted. Memory scales with a single directory, not
// with the library, and it is released before the next directory is opened.
class DirListing
{
public:
  bool load(const char *path, bool wantDirectories)
  {
    clear();
    File dir = CARD.open(path);
    if (!dir || !dir.isDirectory())
    {
      if (dir)
        dir.close();
      return false;
    }

    File entry = dir.openNextFile();
    while (entry)
    {
      const char *name = baseName(entry.name());
      bool isDir = entry.isDirectory();
      if (!isHiddenName(name) && isDir == wantDirectories &&
          (wantDirectories || hasExtension(name, ".mp3")))
      {
        if (offsets.size() < MAX_ENTRIES && arena.length() < MAX_ARENA)
        {
          offsets.push_back(arena.length());
          arena += name;
          arena += '\0';
          sizes.push_back(entry.size());
        }
        else
        {
          overflowed = true;
        }
      }
      entry.close();
      entry = dir.openNextFile();
    }
    dir.close();

    // Sort after the arena is complete: growing a String can reallocate, and
    // the offsets must be stable before anything dereferences them.
    std::vector<uint16_t> order(offsets.size());
    for (uint16_t i = 0; i < order.size(); i++)
      order[i] = i;
    const char *base = arena.c_str();
    std::sort(order.begin(), order.end(),
              [base, this](uint16_t a, uint16_t b) {
                return strcasecmp(base + offsets[a], base + offsets[b]) < 0;
              });

    std::vector<uint32_t> sortedOffsets(offsets.size());
    std::vector<uint32_t> sortedSizes(sizes.size());
    for (uint16_t i = 0; i < order.size(); i++)
    {
      sortedOffsets[i] = offsets[order[i]];
      sortedSizes[i] = sizes[order[i]];
    }
    offsets.swap(sortedOffsets);
    sizes.swap(sortedSizes);
    return true;
  }

  void clear()
  {
    arena = String();
    offsets.clear();
    sizes.clear();
    overflowed = false;
  }

  uint16_t count() const { return (uint16_t)offsets.size(); }
  const char *name(uint16_t i) const { return arena.c_str() + offsets[i]; }
  uint32_t size(uint16_t i) const { return sizes[i]; }
  bool didOverflow() const { return overflowed; }

private:
  static constexpr size_t MAX_ENTRIES = 2048;
  static constexpr size_t MAX_ARENA = 96 * 1024;
  String arena;
  std::vector<uint32_t> offsets;
  std::vector<uint32_t> sizes;
  bool overflowed = false;
};

// Records are 12 to 32 bytes. Writing them individually would force a
// read-modify-write of a 512-byte sector per record, so they are batched.
class BufferedWriter
{
public:
  bool open(const char *path)
  {
    CARD.remove(path);
    if (buffer == nullptr)
      buffer = (uint8_t *)malloc(BUFFER_BYTES);
    if (buffer == nullptr)
      return false;
    file = CARD.open(path, FILE_WRITE);
    used = 0;
    total = 0;
    return (bool)file;
  }

  bool write(const void *data, size_t length)
  {
    const uint8_t *bytes = (const uint8_t *)data;
    while (length > 0)
    {
      size_t room = BUFFER_BYTES - used;
      size_t chunk = length < room ? length : room;
      memcpy(buffer + used, bytes, chunk);
      used += chunk;
      bytes += chunk;
      length -= chunk;
      total += chunk;
      if (used == BUFFER_BYTES && !flush())
        return false;
    }
    return true;
  }

  bool flush()
  {
    if (used == 0)
      return true;
    bool ok = file.write(buffer, used) == used;
    used = 0;
    return ok;
  }

  void close()
  {
    flush();
    if (file)
      file.close();
    free(buffer);
    buffer = nullptr;
  }

  ~BufferedWriter() { free(buffer); }

  uint32_t written() const { return total; }
  explicit operator bool() const { return (bool)file; }

private:
  // Heap, not stack: four of these are alive at once inside libraryBuild, and
  // the loop task only has 8 KB.
  static constexpr size_t BUFFER_BYTES = 512;
  File file;
  uint8_t *buffer = nullptr;
  size_t used = 0;
  uint32_t total = 0;
};

// "07 - Dog Shelter.mp3" -> number 7, title "Dog Shelter".
//
// The extension is stripped first, and a leading number only counts when a
// separator follows it. Otherwise "1979.mp3" would be read as track 1979 with
// the title "mp3".
void splitTrackName(const char *fileName, uint16_t &number, String &title)
{
  number = 0;
  title = fileName;
  int dot = title.lastIndexOf('.');
  if (dot > 0)
    title.remove(dot);

  size_t digits = 0;
  uint32_t parsed = 0;
  while (digits < title.length() && isdigit((unsigned char)title[digits]))
  {
    parsed = parsed * 10 + (uint32_t)(title[digits] - '0');
    digits++;
  }
  if (digits == 0 || digits >= title.length() || parsed > 9999)
    return; // no number, or the name is nothing but digits

  size_t cursor = digits;
  while (cursor < title.length() &&
         (title[cursor] == ' ' || title[cursor] == '-' ||
          title[cursor] == '.' || title[cursor] == '_'))
    cursor++;
  if (cursor == digits || cursor >= title.length())
    return; // digits not followed by a separator and a title

  number = (uint16_t)parsed;
  title.remove(0, cursor);
}

uint16_t countArtistDirs()
{
  StorageGuard guard;
  if (!guard)
    return 0;
  File dir = CARD.open(MUSIC_DIR);
  if (!dir || !dir.isDirectory())
  {
    if (dir)
      dir.close();
    return 0;
  }
  uint16_t count = 0;
  File entry = dir.openNextFile();
  while (entry)
  {
    if (entry.isDirectory() && !isHiddenName(baseName(entry.name())))
      count++;
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();
  return count;
}

bool readRecord(File &file, uint32_t index, void *out, size_t size)
{
  StorageGuard guard;
  if (!guard || !file)
    return false;
  if (!file.seek((uint32_t)(index * size)))
    return false;
  return file.read((uint8_t *)out, size) == (int)size;
}

} // namespace

bool libraryOpen()
{
  libraryClose();
  StorageGuard guard;
  if (!guard || !storageMounted())
    return false;

  File headerFile = CARD.open(HDR_PATH, FILE_READ);
  if (!headerFile)
    return false;
  bool ok = headerFile.read((uint8_t *)&header, sizeof(header)) ==
            (int)sizeof(header);
  headerFile.close();
  if (!ok || memcmp(header.magic, "MSIX", 4) != 0 ||
      header.version != INDEX_VERSION)
    return false;

  stringsFile = CARD.open(STRINGS_PATH, FILE_READ);
  tracksFile = CARD.open(TRACKS_PATH, FILE_READ);
  albumsFile = CARD.open(ALBUMS_PATH, FILE_READ);
  artistsFile = CARD.open(ARTISTS_PATH, FILE_READ);
  indexOpen = stringsFile && tracksFile && albumsFile && artistsFile;
  if (!indexOpen)
    libraryClose();
  return indexOpen;
}

void libraryClose()
{
  if (stringsFile)
    stringsFile.close();
  if (tracksFile)
    tracksFile.close();
  if (albumsFile)
    albumsFile.close();
  if (artistsFile)
    artistsFile.close();
  indexOpen = false;
}

bool libraryValid() { return indexOpen; }

bool libraryNeedsRebuild()
{
  if (!indexOpen)
    return true;
  if (header.cardSize != storageCard().sizeBytes)
    return true;
  return header.sourceArtistDirs != countArtistDirs();
}

bool libraryBuild(BuildProgress progress)
{
  // Held across the whole scan. Playback is stopped before a rebuild, so
  // nothing is waiting on the card, and taking the lock per file would cost
  // more than it protects.
  StorageGuard guard;
  if (!guard || !storageMounted())
    return false;
  libraryClose();

  if (!CARD.exists(MUSIC_DIR))
  {
    Serial.println("[lib] /Music is missing");
    return false;
  }
  if (!CARD.exists(INDEX_DIR))
    CARD.mkdir(INDEX_DIR);

  BufferedWriter strings, tracks, albums, artists;
  if (!strings.open(STRINGS_PATH) || !tracks.open(TRACKS_PATH) ||
      !albums.open(ALBUMS_PATH) || !artists.open(ARTISTS_PATH))
  {
    Serial.println("[lib] could not open index files for writing");
    return false;
  }

  auto addString = [&strings](const String &value) -> uint32_t {
    uint32_t offset = strings.written();
    strings.write(value.c_str(), value.length() + 1);
    return offset;
  };

  uint16_t artistCount = 0, albumCount = 0, trackCount = 0;
  bool truncated = false;

  DirListing artistDirs;
  if (!artistDirs.load(MUSIC_DIR, true))
  {
    Serial.println("[lib] /Music is not readable");
    return false;
  }

  for (uint16_t a = 0; a < artistDirs.count(); a++)
  {
    String artistName = artistDirs.name(a);
    String artistPath = String(MUSIC_DIR) + "/" + artistName;

    ArtistRecord artistRecord = {};
    artistRecord.nameOffset = addString(artistName);
    artistRecord.firstAlbum = albumCount;
    artistRecord.firstTrack = trackCount;

    DirListing albumDirs;
    albumDirs.load(artistPath.c_str(), true);
    for (uint16_t b = 0; b < albumDirs.count() && !truncated; b++)
    {
      String albumName = albumDirs.name(b);
      String albumPath = artistPath + "/" + albumName;

      AlbumRecord albumRecord = {};
      albumRecord.nameOffset = addString(albumName);
      albumRecord.artistIndex = artistCount;
      albumRecord.firstTrack = trackCount;
      if (CARD.exists((albumPath + "/cover.bin").c_str()))
        albumRecord.flags |= TRACK_HAS_COVER;

      DirListing trackFiles;
      trackFiles.load(albumPath.c_str(), false);

      // Alphabetical order puts "10 Homeless" before "2 Archangel", so the
      // album is re-ordered by parsed track number here. Unnumbered files keep
      // their alphabetical order and follow the numbered ones.
      std::vector<uint16_t> order(trackFiles.count());
      std::vector<uint16_t> numbers(trackFiles.count());
      for (uint16_t t = 0; t < trackFiles.count(); t++)
      {
        String ignored;
        splitTrackName(trackFiles.name(t), numbers[t], ignored);
        order[t] = t;
      }
      std::stable_sort(order.begin(), order.end(),
                       [&numbers](uint16_t a, uint16_t b) {
                         bool aNumbered = numbers[a] > 0;
                         bool bNumbered = numbers[b] > 0;
                         if (aNumbered != bNumbered)
                           return aNumbered;
                         return aNumbered ? numbers[a] < numbers[b] : false;
                       });

      for (uint16_t slot = 0; slot < order.size(); slot++)
      {
        uint16_t t = order[slot];
        if (trackCount >= LIBRARY_MAX_TRACKS)
        {
          truncated = true;
          break;
        }
        const char *fileName = trackFiles.name(t);
        String trackPath = albumPath + "/" + fileName;

        uint16_t number = 0;
        String title;
        splitTrackName(fileName, number, title);

        TrackRecord record = {};
        record.pathOffset = addString(trackPath);
        record.titleOffset = addString(title);
        record.artistIndex = artistCount;
        record.albumIndex = albumCount;
        record.trackNumber = number;
        record.fileSize = trackFiles.size(t);

        File audio = CARD.open(trackPath.c_str(), FILE_READ);
        if (audio)
        {
          Mp3Info info = mp3Probe(audio);
          record.durationSeconds = info.durationSeconds;
          record.modifiedTime = (uint32_t)audio.getLastWrite();
          audio.close();
        }

        if (CARD.exists(libraryLyricPath(trackPath).c_str()))
          record.flags |= TRACK_HAS_LRC;
        if (albumRecord.flags & TRACK_HAS_COVER)
          record.flags |= TRACK_HAS_COVER;

        tracks.write(&record, sizeof(record));
        trackCount++;

        if (progress && (trackCount % 25 == 0))
          progress(trackCount, artistName.c_str(), albumName.c_str());
      }

      albumRecord.trackCount = trackCount - albumRecord.firstTrack;
      // An album folder with no playable files is not worth a row in the list.
      if (albumRecord.trackCount > 0)
      {
        albums.write(&albumRecord, sizeof(albumRecord));
        albumCount++;
      }
    }

    artistRecord.albumCount = albumCount - artistRecord.firstAlbum;
    artistRecord.trackCount = trackCount - artistRecord.firstTrack;
    if (artistRecord.trackCount > 0)
    {
      artists.write(&artistRecord, sizeof(artistRecord));
      artistCount++;
    }
    if (truncated)
      break;
  }

  strings.close();
  tracks.close();
  albums.close();
  artists.close();

  IndexHeader built = {};
  memcpy(built.magic, "MSIX", 4);
  built.version = INDEX_VERSION;
  built.artistCount = artistCount;
  built.albumCount = albumCount;
  built.trackCount = trackCount;
  built.sourceArtistDirs = countArtistDirs();
  built.stringBytes = strings.written();
  built.cardSize = storageCard().sizeBytes;

  // The header is written last and validated first, so an interrupted build
  // leaves an index that simply fails to open and is rebuilt, rather than one
  // that opens and reads past the end of a truncated record file.
  CARD.remove(HDR_PATH);
  File headerFile = CARD.open(HDR_PATH, FILE_WRITE);
  if (!headerFile)
    return false;
  headerFile.write((const uint8_t *)&built, sizeof(built));
  headerFile.close();

  if (truncated)
    Serial.printf("[lib] stopped at the %u track limit\n",
                  (unsigned)LIBRARY_MAX_TRACKS);
  Serial.printf("[lib] indexed %u tracks, %u albums, %u artists\n",
                (unsigned)trackCount, (unsigned)albumCount,
                (unsigned)artistCount);

  return libraryOpen();
}

uint16_t libraryArtistCount() { return indexOpen ? header.artistCount : 0; }
uint16_t libraryAlbumCount() { return indexOpen ? header.albumCount : 0; }
uint16_t libraryTrackCount() { return indexOpen ? header.trackCount : 0; }

bool libraryArtist(uint16_t index, ArtistRecord &out)
{
  if (!indexOpen || index >= header.artistCount)
    return false;
  return readRecord(artistsFile, index, &out, sizeof(out));
}

bool libraryAlbum(uint16_t index, AlbumRecord &out)
{
  if (!indexOpen || index >= header.albumCount)
    return false;
  return readRecord(albumsFile, index, &out, sizeof(out));
}

bool libraryTrack(uint16_t index, TrackRecord &out)
{
  if (!indexOpen || index >= header.trackCount)
    return false;
  return readRecord(tracksFile, index, &out, sizeof(out));
}

String libraryString(uint32_t offset)
{
  if (!indexOpen || offset >= header.stringBytes)
    return String();
  StorageGuard guard;
  if (!guard || !stringsFile.seek(offset))
    return String();
  String value;
  // Strings are NUL separated, so read until the terminator. Names are short;
  // the cap only stops a corrupted offset from consuming the heap.
  for (uint16_t i = 0; i < 512; i++)
  {
    int byte = stringsFile.read();
    if (byte <= 0)
      break;
    value += (char)byte;
  }
  return value;
}

String libraryArtistName(uint16_t index)
{
  ArtistRecord record;
  return libraryArtist(index, record) ? libraryString(record.nameOffset)
                                      : String();
}

String libraryAlbumName(uint16_t index)
{
  AlbumRecord record;
  return libraryAlbum(index, record) ? libraryString(record.nameOffset)
                                     : String();
}

String libraryTrackTitle(uint16_t index)
{
  TrackRecord record;
  return libraryTrack(index, record) ? libraryString(record.titleOffset)
                                     : String();
}

String libraryTrackPath(uint16_t index)
{
  TrackRecord record;
  return libraryTrack(index, record) ? libraryString(record.pathOffset)
                                     : String();
}

void libraryParseTrackPath(const String &path, String &artist, String &album,
                           uint16_t &trackNumber, String &title)
{
  artist = String();
  album = String();
  trackNumber = 0;
  title = String();

  int lastSlash = path.lastIndexOf('/');
  if (lastSlash < 0)
  {
    splitTrackName(path.c_str(), trackNumber, title);
    return;
  }
  splitTrackName(path.c_str() + lastSlash + 1, trackNumber, title);

  int albumSlash = path.lastIndexOf('/', lastSlash - 1);
  if (albumSlash < 0)
    return;
  album = path.substring(albumSlash + 1, lastSlash);

  int artistSlash = path.lastIndexOf('/', albumSlash - 1);
  if (artistSlash < 0)
    return;
  artist = path.substring(artistSlash + 1, albumSlash);
}

String libraryLyricPath(const String &trackPath)
{
  int dot = trackPath.lastIndexOf('.');
  return (dot > 0 ? trackPath.substring(0, dot) : trackPath) + ".lrc";
}

String libraryCoverPath(const String &trackPath)
{
  int slash = trackPath.lastIndexOf('/');
  return (slash > 0 ? trackPath.substring(0, slash) : trackPath) + "/cover.bin";
}
