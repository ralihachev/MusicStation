#include "lyrics.h"

#include <algorithm>
#include <ctype.h>
#include <vector>

#include "storage.h"

namespace
{

struct LyricLine
{
  uint32_t ms;
  uint32_t offset; // into arena
  uint16_t length;
};

std::vector<LyricLine> lines;
String arena;
const char *status = "NO LYRICS";
int16_t cursor = -1;
uint32_t cursorMs = 0;

// [offset:+250] shifts every timestamp; some files need it and it costs
// nothing to honour.
int32_t fileOffsetMs = 0;

bool parseTimestamp(const char *text, size_t length, uint32_t &ms)
{
  // [mm:ss.xx] or [mm:ss.xxx] or [mm:ss]
  uint32_t minutes = 0, seconds = 0, fraction = 0;
  size_t i = 0;
  if (i >= length || !isdigit((unsigned char)text[i]))
    return false;
  while (i < length && isdigit((unsigned char)text[i]))
    minutes = minutes * 10 + (uint32_t)(text[i++] - '0');
  if (i >= length || text[i] != ':')
    return false;
  i++;
  if (i >= length || !isdigit((unsigned char)text[i]))
    return false;
  while (i < length && isdigit((unsigned char)text[i]))
    seconds = seconds * 10 + (uint32_t)(text[i++] - '0');

  uint32_t scale = 0;
  if (i < length && (text[i] == '.' || text[i] == ':'))
  {
    i++;
    size_t digits = 0;
    while (i < length && isdigit((unsigned char)text[i]))
    {
      fraction = fraction * 10 + (uint32_t)(text[i++] - '0');
      digits++;
    }
    scale = digits == 1 ? 100 : digits == 2 ? 10 : 1;
  }
  if (i != length)
    return false;

  ms = minutes * 60000 + seconds * 1000 + fraction * scale;
  return true;
}

void addLine(uint32_t ms, const char *text, size_t length)
{
  // Trim; a line that is only whitespace is a deliberate gap and is kept as an
  // empty line so instrumental passages clear the display.
  while (length > 0 && (text[0] == ' ' || text[0] == '\t'))
  {
    text++;
    length--;
  }
  while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t' ||
                        text[length - 1] == '\r'))
    length--;

  LyricLine line;
  line.ms = ms;
  line.offset = arena.length();
  line.length = (uint16_t)length;
  arena.concat(text, length);
  lines.push_back(line);
}

// One physical line may carry several timestamps: "[00:12.00][01:44.00]text".
void parseLine(const String &raw)
{
  if (raw.length() == 0)
    return;

  std::vector<uint32_t> stamps;
  size_t cursorAt = 0;
  while (cursorAt < raw.length() && raw[cursorAt] == '[')
  {
    int close = raw.indexOf(']', cursorAt);
    if (close < 0)
      break;
    size_t innerLength = (size_t)close - cursorAt - 1;
    uint32_t ms = 0;
    if (parseTimestamp(raw.c_str() + cursorAt + 1, innerLength, ms))
    {
      stamps.push_back(ms);
    }
    else
    {
      // Metadata tag. [offset:] is the only one that changes playback.
      String tag = raw.substring(cursorAt + 1, close);
      tag.toLowerCase();
      if (tag.startsWith("offset:"))
        fileOffsetMs = tag.substring(7).toInt();
      if (stamps.empty())
        return; // a pure metadata line has no text to keep
    }
    cursorAt = (size_t)close + 1;
  }

  if (stamps.empty())
    return;
  const char *text = raw.c_str() + cursorAt;
  size_t length = raw.length() - cursorAt;
  for (uint32_t ms : stamps)
    addLine(ms, text, length);
}

} // namespace

void lyricsClear()
{
  lines.clear();
  lines.shrink_to_fit();
  arena = String();
  status = "NO LYRICS";
  cursor = -1;
  cursorMs = 0;
  fileOffsetMs = 0;
}

bool lyricsLoad(const String &path)
{
  lyricsClear();

  StorageGuard guard;
  if (!guard || !storageMounted())
  {
    status = "CARD BUSY";
    return false;
  }
  if (!CARD.exists(path.c_str()))
  {
    // Normal, not a failure: the track simply has no sidecar.
    Serial.printf("[lyrics] no file at %s\n", path.c_str());
    return false; // status stays NO LYRICS
  }

  File file = CARD.open(path.c_str(), FILE_READ);
  if (!file)
  {
    Serial.printf("[lyrics] open failed: %s\n", path.c_str());
    status = "LYRICS UNREADABLE";
    return false;
  }
  size_t size = file.size();
  if (size == 0 || size > LYRIC_MAX_BYTES)
  {
    file.close();
    status = size == 0 ? "NO LYRICS" : "LYRICS TOO LARGE";
    return false;
  }

  // The file size is known, so the arena is sized once. Text is always shorter
  // than the file because timestamps are stripped.
  arena.reserve(size);

  String raw;
  raw.reserve(160);
  while (file.available())
  {
    int byte = file.read();
    if (byte < 0)
      break;
    if (byte == '\n')
    {
      parseLine(raw);
      raw = "";
    }
    else if (byte != '\r')
    {
      raw += (char)byte;
    }
  }
  parseLine(raw);
  file.close();

  if (lines.empty())
  {
    // Either the provider had only plain text, or the parser rejected every
    // line. Those are very different problems, so report enough to tell them
    // apart: a file with timestamps that yields no lines is a parser bug.
    Serial.printf("[lyrics] %u bytes, 0 lines parsed, first 48 bytes: ",
                  (unsigned)size);
    File probe = CARD.open(path.c_str(), FILE_READ);
    if (probe)
    {
      for (int i = 0; i < 48 && probe.available(); i++)
      {
        int byte = probe.read();
        Serial.printf(byte >= 32 && byte < 127 ? "%c" : "<%02X>", byte);
      }
      probe.close();
    }
    Serial.println();
    status = "NO SYNCED LYRICS";
    return false;
  }

  if (fileOffsetMs != 0)
    for (LyricLine &line : lines)
    {
      int64_t shifted = (int64_t)line.ms + fileOffsetMs;
      line.ms = shifted < 0 ? 0 : (uint32_t)shifted;
    }

  // Multi-timestamp lines arrive out of order, so sorting is required rather
  // than defensive.
  std::sort(lines.begin(), lines.end(),
            [](const LyricLine &a, const LyricLine &b) { return a.ms < b.ms; });

  status = "";
  Serial.printf("[lyrics] %s: %u lines, first at %u ms\n", path.c_str(),
                (unsigned)lines.size(), (unsigned)lines[0].ms);
  return true;
}

bool lyricsAvailable() { return !lines.empty(); }
uint16_t lyricsCount() { return (uint16_t)lines.size(); }
const char *lyricsStatus() { return status; }

String lyricsText(uint16_t index)
{
  if (index >= lines.size())
    return String();
  String text;
  text.concat(arena.c_str() + lines[index].offset, lines[index].length);
  return text;
}

int16_t lyricsIndexAt(uint32_t playbackMs)
{
  if (lines.empty())
    return -1;

  // Time normally moves forward by one tick, so walk from the current cursor.
  // A seek or a track change falls back to a binary search.
  if (playbackMs < cursorMs || cursor < -1)
  {
    int32_t low = 0, high = (int32_t)lines.size();
    while (low < high)
    {
      int32_t mid = low + (high - low) / 2;
      if (lines[mid].ms <= playbackMs)
        low = mid + 1;
      else
        high = mid;
    }
    cursor = (int16_t)(low - 1);
  }
  else
  {
    while (cursor + 1 < (int32_t)lines.size() &&
           lines[cursor + 1].ms <= playbackMs)
      cursor++;
  }
  cursorMs = playbackMs;
  return cursor;
}
