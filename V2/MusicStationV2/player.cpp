#include "player.h"

#include <algorithm>
#include <vector>

#include "audio.h"
#include "cover.h"
#include "library.h"
#include "lyrics.h"
#include "mp3meta.h"
#include "playlist.h"
#include "storage.h"

namespace
{

// Library-sourced queues hold 16-bit record indices: 2000 tracks cost 4 KB.
// Playlist-sourced queues hold paths, because a playlist entry is
// self-describing and needs no index lookup at all. They are capped at
// PLAYLIST_MAX_ENTRIES, so the arena stays small.
std::vector<uint16_t> queueIndices;
std::vector<uint32_t> queuePathOffsets;
String queuePathArena;
bool queueFromPlaylist = false;

uint16_t position = 0;
NowTrack current = {};
int16_t lastLyricIndex = -2;
uint32_t lastTransportSecond = 0xFFFFFFFF;

bool trackChanged = false;
bool lyricChanged = false;
bool transportChanged = false;

uint16_t queueSize()
{
  return queueFromPlaylist ? (uint16_t)queuePathOffsets.size()
                           : (uint16_t)queueIndices.size();
}

String queuePathAt(uint16_t index)
{
  if (index >= queueSize())
    return String();
  if (queueFromPlaylist)
    return String(queuePathArena.c_str() + queuePathOffsets[index]);
  return libraryTrackPath(queueIndices[index]);
}

void clearQueue()
{
  queueIndices.clear();
  queueIndices.shrink_to_fit();
  queuePathOffsets.clear();
  queuePathOffsets.shrink_to_fit();
  queuePathArena = String();
  queueFromPlaylist = false;
  position = 0;
}

// Seeded Fisher-Yates. The queue is permuted once rather than a random track
// being chosen at each advance, so "previous" stays meaningful.
void shuffleQueue(uint32_t seed, uint16_t keepAtFront)
{
  uint16_t count = queueSize();
  if (count < 2)
    return;

  randomSeed(seed);
  for (uint16_t i = count - 1; i > 0; i--)
  {
    uint16_t j = (uint16_t)random(i + 1);
    if (queueFromPlaylist)
      std::swap(queuePathOffsets[i], queuePathOffsets[j]);
    else
      std::swap(queueIndices[i], queueIndices[j]);
    // Follow the track the user actually picked so it still plays first.
    if (keepAtFront == i)
      keepAtFront = j;
    else if (keepAtFront == j)
      keepAtFront = i;
  }
  position = keepAtFront;
}

void describeTrack(uint16_t queueIndex, NowTrack &out)
{
  out = NowTrack();
  out.path = queuePathAt(queueIndex);
  if (out.path.length() == 0)
    return;

  if (!queueFromPlaylist)
  {
    uint16_t index = queueIndices[queueIndex];
    TrackRecord record;
    if (libraryTrack(index, record))
    {
      out.valid = true;
      out.libraryIndex = index;
      out.title = libraryString(record.titleOffset);
      out.artist = libraryArtistName(record.artistIndex);
      out.album = libraryAlbumName(record.albumIndex);
      out.durationSeconds = record.durationSeconds;
      out.trackNumber = record.trackNumber;
      return;
    }
  }

  // Playlist entry, or an index record that has gone stale. The folder layout
  // carries everything except duration.
  out.libraryIndex = -1;
  libraryParseTrackPath(out.path, out.artist, out.album, out.trackNumber,
                        out.title);
  out.valid = out.title.length() > 0;
}

// Duration is the one field a path cannot carry, and everything visible
// depends on it: the remaining time, the progress bar, and the simulated
// backend's end-of-track. A playlist entry has no index record, so probe the
// file. It costs two short reads and happens once per track change.
void ensureDuration(NowTrack &track)
{
  if (track.durationSeconds > 0 || track.path.length() == 0)
    return;
  StorageGuard guard;
  if (!guard)
    return;
  File audio = CARD.open(track.path.c_str(), FILE_READ);
  if (!audio)
    return;
  track.durationSeconds = mp3Probe(audio).durationSeconds;
  audio.close();
  if (track.durationSeconds == 0)
    Serial.printf("[player] no duration for %s\n", track.path.c_str());
}

bool loadCurrent(bool startPlaying)
{
  describeTrack(position, current);
  if (!current.valid)
    return false;
  ensureDuration(current);

  // Release the lyric arena before anything else allocates. V1 learned this
  // the hard way with JPEGDEC; the same discipline keeps the decoder's 32 KB
  // buffer able to find a contiguous block.
  lyricsClear();
  coverClear();

  lyricsLoad(libraryLyricPath(current.path));
  coverLoad(libraryCoverPath(current.path));

  lastLyricIndex = -2;
  lastTransportSecond = 0xFFFFFFFF;
  trackChanged = true;
  lyricChanged = true;
  transportChanged = true;

  if (!audioPlay(current.path, current.durationSeconds))
    return false;
  if (!startPlaying)
    audioPause();

  settings().position = position;
  settings().elapsedMs = 0;
  settingsSaveThrottled(true);
  return true;
}

void rememberSource(QueueSource source, uint16_t context,
                    const String &playlistPath)
{
  settings().source = source;
  settings().context = context;
  settings().playlistPath = playlistPath;
}

bool buildFromAlbum(uint16_t albumIndex)
{
  AlbumRecord album;
  if (!libraryAlbum(albumIndex, album) || album.trackCount == 0)
    return false;
  clearQueue();
  queueIndices.reserve(album.trackCount);
  for (uint16_t i = 0; i < album.trackCount; i++)
    queueIndices.push_back(album.firstTrack + i);
  return true;
}

bool buildFromArtist(uint16_t artistIndex)
{
  ArtistRecord artist;
  if (!libraryArtist(artistIndex, artist) || artist.trackCount == 0)
    return false;
  clearQueue();
  queueIndices.reserve(artist.trackCount);
  for (uint16_t i = 0; i < artist.trackCount; i++)
    queueIndices.push_back(artist.firstTrack + i);
  return true;
}

bool buildFromAll()
{
  uint16_t count = libraryTrackCount();
  if (count == 0)
    return false;
  clearQueue();
  queueIndices.reserve(count);
  for (uint16_t i = 0; i < count; i++)
    queueIndices.push_back(i);
  return true;
}

bool buildFromPlaylist(const String &path)
{
  clearQueue();
  queueFromPlaylist = true;
  if (!playlistRead(path, queuePathArena, queuePathOffsets))
  {
    clearQueue();
    return false;
  }
  return true;
}

bool start(uint16_t startRow, QueueSource source, uint16_t context,
           const String &playlistPath)
{
  if (queueSize() == 0)
    return false;
  position = startRow < queueSize() ? startRow : 0;
  rememberSource(source, context, playlistPath);

  if (settings().shuffle)
  {
    settings().shuffleSeed = (uint32_t)millis();
    shuffleQueue(settings().shuffleSeed, position);
  }
  return loadCurrent(true);
}

// Moves to another queue slot. `wrap` separates a user pressing Next, which
// should wrap, from a track ending under REPEAT_OFF, which should stop.
bool advance(int16_t delta, bool wrap)
{
  uint16_t count = queueSize();
  if (count == 0)
    return false;
  int32_t next = (int32_t)position + delta;
  if (next < 0)
    next = wrap ? count - 1 : 0;
  else if (next >= (int32_t)count)
  {
    if (!wrap)
      return false;
    next = 0;
  }
  position = (uint16_t)next;
  return loadCurrent(true);
}

} // namespace

bool playerBegin()
{
  audioBegin();
  audioSetVolume(settings().volume);
  return true;
}

bool playerRestore()
{
  PersistentState &state = settings();
  bool built = false;
  switch (state.source)
  {
  case QUEUE_ALBUM:
    built = buildFromAlbum(state.context);
    break;
  case QUEUE_ARTIST:
    built = buildFromArtist(state.context);
    break;
  case QUEUE_ALL:
    built = buildFromAll();
    break;
  case QUEUE_PLAYLIST:
    built = buildFromPlaylist(state.playlistPath);
    break;
  default:
    return false;
  }
  if (!built)
    return false;

  // Reuse the stored seed so the restored queue is the same permutation the
  // user was listening to, not a new one.
  if (state.shuffle)
    shuffleQueue(state.shuffleSeed, state.position);

  position = state.position < queueSize() ? state.position : 0;
  // Restored paused: resuming mid-track without frame-accurate seeking would
  // start from the beginning while the screen showed the old position.
  return loadCurrent(false);
}

bool playerPlayAlbum(uint16_t albumIndex, uint16_t startRow)
{
  return buildFromAlbum(albumIndex) &&
         start(startRow, QUEUE_ALBUM, albumIndex, String());
}

bool playerPlayArtist(uint16_t artistIndex, uint16_t startRow)
{
  return buildFromArtist(artistIndex) &&
         start(startRow, QUEUE_ARTIST, artistIndex, String());
}

bool playerPlayAll(uint16_t startIndex)
{
  return buildFromAll() && start(startIndex, QUEUE_ALL, 0, String());
}

bool playerPlayPlaylist(const String &path, uint16_t startRow)
{
  return buildFromPlaylist(path) && start(startRow, QUEUE_PLAYLIST, 0, path);
}

void playerToggle()
{
  if (!current.valid)
    return;
  if (audioIsPlaying())
    audioPause();
  else
    audioResume();
  transportChanged = true;
  settings().elapsedMs = audioPositionMs();
  settingsSaveThrottled(true);
}

void playerNext() { advance(1, true); }

void playerPrevious()
{
  // Restart the current track first, as every other player does, and only step
  // back when already near the start.
  if (audioPositionMs() > 3000)
  {
    audioSeekMs(0);
    lastLyricIndex = -2;
    transportChanged = true;
    lyricChanged = true;
    return;
  }
  advance(-1, true);
}

void playerStop()
{
  audioStop();
  lyricsClear();
  coverClear();
  current = NowTrack();
  clearQueue();
  trackChanged = true;
}

void playerSetShuffle(bool shuffle)
{
  PersistentState &state = settings();
  if (state.shuffle == shuffle)
    return;
  state.shuffle = shuffle;
  if (shuffle && queueSize() > 1)
  {
    state.shuffleSeed = (uint32_t)millis();
    shuffleQueue(state.shuffleSeed, position);
  }
  else if (!shuffle)
  {
    // Rebuilding in natural order loses the current slot, so re-find it.
    String keep = current.path;
    switch (state.source)
    {
    case QUEUE_ALBUM:
      buildFromAlbum(state.context);
      break;
    case QUEUE_ARTIST:
      buildFromArtist(state.context);
      break;
    case QUEUE_ALL:
      buildFromAll();
      break;
    case QUEUE_PLAYLIST:
      buildFromPlaylist(state.playlistPath);
      break;
    default:
      break;
    }
    for (uint16_t i = 0; i < queueSize(); i++)
      if (queuePathAt(i) == keep)
      {
        position = i;
        break;
      }
  }
  settings().position = position;
  settingsSaveThrottled(true);
  transportChanged = true;
}

void playerCycleRepeat()
{
  PersistentState &state = settings();
  state.repeat = state.repeat == REPEAT_OFF   ? REPEAT_ALL
                 : state.repeat == REPEAT_ALL ? REPEAT_ONE
                                              : REPEAT_OFF;
  settingsSaveThrottled(true);
  transportChanged = true;
}

void playerSetVolume(uint8_t level)
{
  settings().volume = level > AUDIO_VOLUME_MAX ? AUDIO_VOLUME_MAX : level;
  audioSetVolume(settings().volume);
  settingsSaveThrottled(true);
  transportChanged = true;
}

bool playerHasTrack() { return current.valid; }
const NowTrack &playerTrack() { return current; }
bool playerIsPlaying() { return audioIsPlaying(); }
uint32_t playerPositionMs() { return audioPositionMs(); }
uint16_t playerQueueSize() { return queueSize(); }
uint16_t playerQueuePosition() { return position; }

int16_t playerLyricIndex()
{
  if (!lyricsAvailable())
    return -1;
  return lyricsIndexAt(playerPositionMs() + LYRIC_LEAD_MS);
}

void playerTick()
{
  audioTick();
  if (!current.valid)
    return;

  if (audioTakeTrackEnded())
  {
    switch (settings().repeat)
    {
    case REPEAT_ONE:
      loadCurrent(true);
      break;
    case REPEAT_ALL:
      advance(1, true);
      break;
    case REPEAT_OFF:
      if (!advance(1, false))
      {
        audioPause();
        transportChanged = true;
      }
      break;
    }
    return;
  }

  int16_t lyricIndex = playerLyricIndex();
  if (lyricIndex != lastLyricIndex)
  {
    lastLyricIndex = lyricIndex;
    lyricChanged = true;
  }

  // The transport strip is redrawn only when the displayed second actually
  // changes. V1 refreshed it unconditionally every 800 ms, which was the
  // largest single contributor to panel wear.
  uint32_t second = playerPositionMs() / 1000;
  if (second != lastTransportSecond)
  {
    lastTransportSecond = second;
    transportChanged = true;
  }

  if (audioIsPlaying())
  {
    settings().elapsedMs = playerPositionMs();
    settings().position = position;
    settingsSaveThrottled(false);
  }
}

bool playerTakeTrackChanged()
{
  bool value = trackChanged;
  trackChanged = false;
  return value;
}

bool playerTakeLyricChanged()
{
  bool value = lyricChanged;
  lyricChanged = false;
  return value;
}

bool playerTakeTransportChanged()
{
  bool value = transportChanged;
  transportChanged = false;
  return value;
}
