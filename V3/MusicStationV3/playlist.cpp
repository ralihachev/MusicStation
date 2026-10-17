#include "playlist.h"

#include <algorithm>

#include "library.h"
#include "storage.h"

namespace
{
String namesArena;
std::vector<uint32_t> nameOffsets;

bool endsWithM3u(const char *name)
{
  size_t length = strlen(name);
  return length > 4 && strcasecmp(name + length - 4, ".m3u") == 0;
}
} // namespace

bool playlistRefresh()
{
  namesArena = String();
  nameOffsets.clear();

  StorageGuard guard;
  if (!guard || !storageMounted())
    return false;
  if (!CARD.exists(PLAYLIST_DIR))
    return true; // no playlists is a normal state, not an error

  File dir = CARD.open(PLAYLIST_DIR);
  if (!dir || !dir.isDirectory())
  {
    if (dir)
      dir.close();
    return false;
  }

  File entry = dir.openNextFile();
  while (entry)
  {
    const char *raw = entry.name();
    const char *slash = strrchr(raw, '/');
    const char *name = slash ? slash + 1 : raw;
    if (!entry.isDirectory() && name[0] != '.' && endsWithM3u(name))
    {
      nameOffsets.push_back(namesArena.length());
      namesArena += name;
      namesArena += '\0';
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();

  const char *base = namesArena.c_str();
  std::sort(nameOffsets.begin(), nameOffsets.end(),
            [base](uint32_t a, uint32_t b) {
              return strcasecmp(base + a, base + b) < 0;
            });
  return true;
}

uint16_t playlistCount() { return (uint16_t)nameOffsets.size(); }

String playlistName(uint16_t index)
{
  if (index >= nameOffsets.size())
    return String();
  String name = namesArena.c_str() + nameOffsets[index];
  int dot = name.lastIndexOf('.');
  if (dot > 0)
    name.remove(dot);
  return name;
}

String playlistPathAt(uint16_t index)
{
  if (index >= nameOffsets.size())
    return String();
  return String(PLAYLIST_DIR) + "/" + (namesArena.c_str() + nameOffsets[index]);
}

bool playlistRead(const String &path, String &arena,
                  std::vector<uint32_t> &offsets)
{
  arena = String();
  offsets.clear();

  StorageGuard guard;
  if (!guard || !storageMounted())
    return false;
  File file = CARD.open(path.c_str(), FILE_READ);
  if (!file)
    return false;

  String line;
  line.reserve(160);
  auto commit = [&]() {
    line.trim();
    // '#' covers #EXTM3U and #EXTINF, which are read for nothing here: the
    // duration and title are already derivable from the track itself.
    if (line.length() == 0 || line[0] == '#')
      return;
    if (offsets.size() >= PLAYLIST_MAX_ENTRIES)
      return;
    offsets.push_back(arena.length());
    arena += line;
    arena += '\0';
  };

  while (file.available())
  {
    int byte = file.read();
    if (byte < 0)
      break;
    if (byte == '\n')
    {
      commit();
      line = "";
    }
    else if (byte != '\r')
    {
      line += (char)byte;
    }
  }
  commit();
  file.close();
  return !offsets.empty();
}

bool playlistAppend(const String &playlistPath, const String &trackPath)
{
  StorageGuard guard;
  if (!guard || !storageMounted())
    return false;
  File file = CARD.open(playlistPath.c_str(), FILE_APPEND);
  if (!file)
    return false;
  file.print(trackPath);
  file.print('\n');
  file.close();
  return true;
}

bool playlistCreate(const String &name)
{
  StorageGuard guard;
  if (!guard || !storageMounted())
    return false;
  if (!CARD.exists(PLAYLIST_DIR))
    CARD.mkdir(PLAYLIST_DIR);
  String path = String(PLAYLIST_DIR) + "/" + name + ".m3u";
  if (CARD.exists(path.c_str()))
    return true;
  File file = CARD.open(path.c_str(), FILE_WRITE);
  if (!file)
    return false;
  file.println("#EXTM3U");
  file.close();
  return true;
}
