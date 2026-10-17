#include "ui.h"

#include <ctype.h>

#include "audio.h"
#include "cover.h"
#include "library.h"
#include "lyrics.h"
#include "player.h"
#include "playlist.h"
#include "power.h"
#include "settings.h"

namespace
{

enum ScreenKind : uint8_t
{
  SCREEN_NOW_PLAYING,
  SCREEN_LIBRARY,
  SCREEN_PLAYLISTS,
  SCREEN_ARTISTS,
  SCREEN_ALBUMS,
  SCREEN_TRACKS,
  SCREEN_ALL_TRACKS,
  SCREEN_CONTEXT,
  SCREEN_PLAYLIST_PICK,
  SCREEN_SETTINGS,
  SCREEN_JUMP,
  SCREEN_MESSAGE,
};

struct NavFrame
{
  ScreenKind kind;
  uint16_t context; // artist index for ALBUMS, album index for TRACKS
  uint16_t selected;
  uint16_t top;
};

constexpr uint8_t NAV_DEPTH = 7;
NavFrame stack[NAV_DEPTH];
uint8_t depth = 0;
bool wantFull = true;
bool rescanRequested = false;

String messageTitle, messageDetail;

// The track a context menu is acting on, kept as a path so it survives a
// rescan and works for playlist entries that are not in the index.
String contextTrackPath;
String contextTrackTitle;

const char *const LIBRARY_ITEMS[] = {"Playlists", "Artists", "Albums",
                                     "All Tracks", "Settings"};
constexpr uint16_t LIBRARY_ITEM_COUNT = 5;

const char *const CONTEXT_ITEMS[] = {"Play now", "Play album",
                                     "Add to playlist...", "Shuffle",
                                     "Repeat"};
constexpr uint16_t CONTEXT_ITEM_COUNT = 5;

const char *const SETTINGS_ITEMS[] = {"Volume",  "Shuffle",     "Repeat",
                                      "Sleep",   "Rescan card", "About"};
constexpr uint16_t SETTINGS_ITEM_COUNT = 6;

const char JUMP_LETTERS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ#";
constexpr uint16_t JUMP_COUNT = 27;

const uint16_t SLEEP_CHOICES[] = {0, 5, 10, 20, 30, 60};
constexpr uint8_t SLEEP_CHOICE_COUNT = 6;

// Now Playing UP/DOWN: a tap skips, a hold changes volume. The skip waits for
// the release, because only then is it known that the press was not a hold.
// That costs a fraction of the 780 ms refresh that follows either way.
Action pendingSkip = ACT_COUNT;
bool volumeGestureActive = false;
uint8_t volumeRepeats = 0;
// Repeats arrive every 90 ms; stepping on every other one crosses the whole
// range in under four seconds and still lets a short hold land on one level.
constexpr uint8_t VOLUME_REPEATS_PER_STEP = 2;

// The transport strip shows the volume for a moment after it changes, then
// reverts. Both cost one strip refresh, not a whole screen.
constexpr uint32_t VOLUME_OVERLAY_MS = 2500;
uint32_t volumeOverlayUntil = 0;
bool volumeOverlayDrawn = false;
bool transportDirty = false;

// Lyric sync mode shares the overlay slot: hold MENU on Now Playing, then
// UP/DOWN nudge the lyrics while they keep playing, so the offset is tuned
// against the song instead of by reflashing. Leaves on ENTER, BACK, or after
// SYNC_OVERLAY_MS without a press.
enum OverlayKind : uint8_t
{
  OVERLAY_VOLUME,
  OVERLAY_SYNC,
};
OverlayKind overlayKind = OVERLAY_VOLUME;
constexpr uint32_t SYNC_OVERLAY_MS = 4000;
constexpr int16_t SYNC_STEP_MS = 50;
bool pendingMenu = false;
uint8_t syncRepeats = 0;

bool volumeOverlayVisible()
{
  return volumeOverlayUntil != 0 &&
         (int32_t)(volumeOverlayUntil - millis()) > 0;
}

bool syncModeActive()
{
  return overlayKind == OVERLAY_SYNC && volumeOverlayVisible();
}

void showOverlay(OverlayKind kind, uint32_t forMs)
{
  overlayKind = kind;
  volumeOverlayUntil = millis() + forMs;
  if (volumeOverlayUntil == 0)
    volumeOverlayUntil = 1;
  transportDirty = true;
}

NavFrame &top() { return stack[depth - 1]; }

void push(ScreenKind kind, uint16_t context)
{
  if (depth >= NAV_DEPTH)
    return;
  stack[depth] = {kind, context, 0, 0};
  depth++;
  wantFull = true;
}

bool pop()
{
  if (depth <= 1)
    return false;
  depth--;
  wantFull = true;
  return true;
}

String formatDuration(uint32_t seconds)
{
  char text[10];
  snprintf(text, sizeof(text), "%u:%02u", (unsigned)(seconds / 60),
           (unsigned)(seconds % 60));
  return String(text);
}

String repeatName()
{
  switch (settings().repeat)
  {
  case REPEAT_ALL:
    return "All";
  case REPEAT_ONE:
    return "One";
  default:
    return "Off";
  }
}

// ------------------------------------------------------------------ lists

uint16_t rowCount(const NavFrame &frame)
{
  switch (frame.kind)
  {
  case SCREEN_LIBRARY:
    return LIBRARY_ITEM_COUNT;
  case SCREEN_PLAYLISTS:
    return playlistCount();
  case SCREEN_ARTISTS:
    return libraryArtistCount();
  case SCREEN_ALBUMS:
  {
    if (frame.context == 0xFFFF)
      return libraryAlbumCount();
    ArtistRecord artist;
    return libraryArtist(frame.context, artist) ? artist.albumCount : 0;
  }
  case SCREEN_TRACKS:
  {
    AlbumRecord album;
    return libraryAlbum(frame.context, album) ? album.trackCount : 0;
  }
  case SCREEN_ALL_TRACKS:
    return libraryTrackCount();
  case SCREEN_CONTEXT:
    return CONTEXT_ITEM_COUNT;
  case SCREEN_PLAYLIST_PICK:
    return playlistCount();
  case SCREEN_SETTINGS:
    return SETTINGS_ITEM_COUNT;
  case SCREEN_JUMP:
    return JUMP_COUNT;
  default:
    return 0;
  }
}

// Albums and tracks are stored contiguously per parent, so a child list is a
// base plus an offset with no secondary lookup table.
uint16_t recordIndex(const NavFrame &frame, uint16_t row)
{
  switch (frame.kind)
  {
  case SCREEN_ALBUMS:
  {
    if (frame.context == 0xFFFF)
      return row;
    ArtistRecord artist;
    return libraryArtist(frame.context, artist) ? artist.firstAlbum + row : row;
  }
  case SCREEN_TRACKS:
  {
    AlbumRecord album;
    return libraryAlbum(frame.context, album) ? album.firstTrack + row : row;
  }
  default:
    return row;
  }
}

String screenTitle(const NavFrame &frame)
{
  switch (frame.kind)
  {
  case SCREEN_LIBRARY:
    return "LIBRARY";
  case SCREEN_PLAYLISTS:
    return "PLAYLISTS";
  case SCREEN_ARTISTS:
    return "ARTISTS";
  case SCREEN_ALBUMS:
    return frame.context == 0xFFFF ? String("ALBUMS")
                                   : libraryArtistName(frame.context);
  case SCREEN_TRACKS:
    return libraryAlbumName(frame.context);
  case SCREEN_ALL_TRACKS:
    return "ALL TRACKS";
  case SCREEN_CONTEXT:
    return contextTrackTitle.length() ? contextTrackTitle : String("TRACK");
  case SCREEN_PLAYLIST_PICK:
    return "ADD TO";
  case SCREEN_SETTINGS:
    return "SETTINGS";
  case SCREEN_JUMP:
    return "JUMP TO";
  default:
    return messageTitle;
  }
}

struct Row
{
  String left;
  String right;
  bool warn; // missing lyrics
};

Row rowAt(const NavFrame &frame, uint16_t row)
{
  Row out = {String(), String(), false};
  switch (frame.kind)
  {
  case SCREEN_LIBRARY:
    out.left = LIBRARY_ITEMS[row];
    break;
  case SCREEN_PLAYLISTS:
  case SCREEN_PLAYLIST_PICK:
    out.left = playlistName(row);
    break;
  case SCREEN_ARTISTS:
  {
    ArtistRecord artist;
    if (libraryArtist(row, artist))
    {
      out.left = libraryString(artist.nameOffset);
      out.right = String(artist.albumCount);
    }
    break;
  }
  case SCREEN_ALBUMS:
  {
    AlbumRecord album;
    if (libraryAlbum(recordIndex(frame, row), album))
    {
      out.left = libraryString(album.nameOffset);
      out.right = String(album.trackCount);
    }
    break;
  }
  case SCREEN_TRACKS:
  case SCREEN_ALL_TRACKS:
  {
    TrackRecord track;
    if (libraryTrack(recordIndex(frame, row), track))
    {
      String title = libraryString(track.titleOffset);
      out.left = (frame.kind == SCREEN_TRACKS && track.trackNumber > 0)
                     ? String(track.trackNumber) + "  " + title
                     : title;
      out.right = track.durationSeconds ? formatDuration(track.durationSeconds)
                                        : String("--:--");
      out.warn = (track.flags & TRACK_HAS_LRC) == 0;
    }
    break;
  }
  case SCREEN_CONTEXT:
    out.left = CONTEXT_ITEMS[row];
    if (row == 3)
      out.right = settings().shuffle ? "On" : "Off";
    else if (row == 4)
      out.right = repeatName();
    break;
  case SCREEN_SETTINGS:
    out.left = SETTINGS_ITEMS[row];
    switch (row)
    {
    case 0:
      out.right = String(settings().volume) + "/" + AUDIO_VOLUME_MAX;
      break;
    case 1:
      out.right = settings().shuffle ? "On" : "Off";
      break;
    case 2:
      out.right = repeatName();
      break;
    case 3:
      out.right = settings().sleepMinutes
                      ? String(settings().sleepMinutes) + " min"
                      : String("Never");
      break;
    case 5:
      out.right = audioBackendName();
      break;
    default:
      break;
    }
    break;
  case SCREEN_JUMP:
    out.left = String(JUMP_LETTERS[row]);
    break;
  default:
    break;
  }
  return out;
}

// A jump must binary-search the key the list is actually ORDERED by, which is
// not always the text on screen. tracks.bin and albums.bin are artist-major,
// so an album list spanning all artists, and the all-tracks list, are both
// ordered by artist name rather than by their own titles.
String rowSortKey(const NavFrame &frame, uint16_t row)
{
  switch (frame.kind)
  {
  case SCREEN_ARTISTS:
    return libraryArtistName(row);
  case SCREEN_ALBUMS:
  {
    uint16_t index = recordIndex(frame, row);
    if (frame.context != 0xFFFF)
      return libraryAlbumName(index);
    AlbumRecord album;
    return libraryAlbum(index, album) ? libraryArtistName(album.artistIndex)
                                      : String();
  }
  case SCREEN_ALL_TRACKS:
  {
    TrackRecord track;
    return libraryTrack(row, track) ? libraryArtistName(track.artistIndex)
                                    : String();
  }
  default:
    return String();
  }
}

// A track list inside one album is ordered by track number, so it has no
// letter key to search. It is also never long enough to need one.
bool screenSupportsJump(const NavFrame &frame)
{
  return frame.kind == SCREEN_ARTISTS || frame.kind == SCREEN_ALBUMS ||
         frame.kind == SCREEN_ALL_TRACKS;
}

uint16_t jumpTarget(const NavFrame &frame, char letter)
{
  uint16_t count = rowCount(frame);
  if (count == 0 || letter == '#')
    return 0;
  uint16_t low = 0, high = count;
  while (low < high)
  {
    uint16_t mid = low + (high - low) / 2;
    String key = rowSortKey(frame, mid);
    char first = key.length() ? (char)toupper((unsigned char)key[0]) : 0;
    if (first < letter)
      low = mid + 1;
    else
      high = mid;
  }
  return low < count ? low : count - 1;
}

void moveSelection(NavFrame &frame, int32_t delta, bool wrap)
{
  uint16_t count = rowCount(frame);
  if (count == 0)
    return;
  int32_t next = (int32_t)frame.selected + delta;
  if (wrap)
  {
    if (next < 0)
      next = count - 1;
    else if (next >= (int32_t)count)
      next = 0;
  }
  else
  {
    if (next < 0)
      next = 0;
    else if (next >= (int32_t)count)
      next = count - 1;
  }
  frame.selected = (uint16_t)next;

  if (frame.selected < frame.top)
    frame.top = frame.selected;
  else if (frame.selected >= frame.top + VISIBLE_ROWS)
    frame.top = frame.selected - VISIBLE_ROWS + 1;
}

// ------------------------------------------------------------- activation

void rememberContextTrack(uint16_t libraryIndex)
{
  contextTrackPath = libraryTrackPath(libraryIndex);
  contextTrackTitle = libraryTrackTitle(libraryIndex);
}

void goNowPlaying()
{
  depth = 1;
  stack[0] = {SCREEN_NOW_PLAYING, 0, 0, 0};
  wantFull = true;
}

void adjustSetting(uint16_t row)
{
  PersistentState &state = settings();
  switch (row)
  {
  case 0:
    playerSetVolume(state.volume >= AUDIO_VOLUME_MAX
                        ? 0
                        : (uint8_t)(state.volume + 2));
    break;
  case 1:
    playerSetShuffle(!state.shuffle);
    break;
  case 2:
    playerCycleRepeat();
    break;
  case 3:
  {
    uint8_t choice = 0;
    for (uint8_t i = 0; i < SLEEP_CHOICE_COUNT; i++)
      if (SLEEP_CHOICES[i] == state.sleepMinutes)
        choice = i;
    state.sleepMinutes =
        SLEEP_CHOICES[(choice + 1) % SLEEP_CHOICE_COUNT];
    settingsSave();
    break;
  }
  case 4:
    rescanRequested = true;
    break;
  default:
    uiShowMessage("ABOUT", String("MusicStation V3 / ") + audioBackendName());
    break;
  }
}

void activate(NavFrame &frame)
{
  switch (frame.kind)
  {
  case SCREEN_NOW_PLAYING:
    playerToggle();
    break;

  case SCREEN_LIBRARY:
    switch (frame.selected)
    {
    case 0:
      playlistRefresh();
      push(SCREEN_PLAYLISTS, 0);
      break;
    case 1:
      push(SCREEN_ARTISTS, 0);
      break;
    case 2:
      push(SCREEN_ALBUMS, 0xFFFF);
      break;
    case 3:
      push(SCREEN_ALL_TRACKS, 0);
      break;
    default:
      push(SCREEN_SETTINGS, 0);
      break;
    }
    break;

  case SCREEN_PLAYLISTS:
    if (playerPlayPlaylist(playlistPathAt(frame.selected), 0))
      goNowPlaying();
    else
      uiShowMessage("EMPTY", "That playlist has no tracks");
    break;

  case SCREEN_ARTISTS:
    push(SCREEN_ALBUMS, frame.selected);
    break;

  case SCREEN_ALBUMS:
    push(SCREEN_TRACKS, recordIndex(frame, frame.selected));
    break;

  case SCREEN_TRACKS:
    if (playerPlayAlbum(frame.context, frame.selected))
      goNowPlaying();
    break;

  case SCREEN_ALL_TRACKS:
    if (playerPlayAll(frame.selected))
      goNowPlaying();
    break;

  case SCREEN_CONTEXT:
  {
    uint16_t item = frame.selected;
    if (item == 0 || item == 1)
    {
      TrackRecord record;
      uint16_t index = frame.context;
      if (libraryTrack(index, record))
      {
        AlbumRecord album;
        bool ok = false;
        if (item == 1 && libraryAlbum(record.albumIndex, album))
          ok = playerPlayAlbum(record.albumIndex,
                               (uint16_t)(index - album.firstTrack));
        else
          ok = playerPlayAll(index);
        if (ok)
          goNowPlaying();
      }
    }
    else if (item == 2)
    {
      playlistRefresh();
      if (playlistCount() == 0)
        uiShowMessage("NO PLAYLISTS", "Add an .m3u to /Playlists");
      else
        push(SCREEN_PLAYLIST_PICK, 0);
    }
    else if (item == 3)
      playerSetShuffle(!settings().shuffle);
    else
      playerCycleRepeat();
    break;
  }

  case SCREEN_PLAYLIST_PICK:
  {
    bool ok = contextTrackPath.length() &&
              playlistAppend(playlistPathAt(frame.selected), contextTrackPath);
    pop();
    uiShowMessage(ok ? "ADDED" : "FAILED",
                  ok ? contextTrackTitle : String("Could not write playlist"));
    break;
  }

  case SCREEN_SETTINGS:
    adjustSetting(frame.selected);
    break;

  case SCREEN_JUMP:
  {
    char letter = JUMP_LETTERS[frame.selected];
    pop();
    NavFrame &target = top();
    target.selected = jumpTarget(target, letter);
    target.top = target.selected;
    break;
  }

  default:
    goNowPlaying();
    break;
  }
}

void openContextMenu(NavFrame &frame)
{
  switch (frame.kind)
  {
  case SCREEN_NOW_PLAYING:
  {
    const NowTrack &track = playerTrack();
    if (!track.valid)
      return;
    contextTrackPath = track.path;
    contextTrackTitle = track.title;
    push(SCREEN_CONTEXT,
         track.libraryIndex >= 0 ? (uint16_t)track.libraryIndex : 0xFFFF);
    break;
  }
  case SCREEN_TRACKS:
  case SCREEN_ALL_TRACKS:
  {
    uint16_t index = recordIndex(frame, frame.selected);
    rememberContextTrack(index);
    push(SCREEN_CONTEXT, index);
    break;
  }
  default:
    if (screenSupportsJump(frame) && rowCount(frame) > VISIBLE_ROWS)
      push(SCREEN_JUMP, 0);
    break;
  }
}

// -------------------------------------------------------------- rendering

void drawHeader(const NavFrame &frame)
{
  String title = screenTitle(frame);
  uint16_t count = rowCount(frame);
  String position;
  if (frame.kind != SCREEN_MESSAGE && frame.kind != SCREEN_JUMP &&
      frame.kind != SCREEN_NOW_PLAYING && count > 0)
    position = String(frame.selected + 1) + "/" + count;

  int16_t reserved =
      position.length() ? panelTextWidth(position, SMALL_FONT) + 8 : 0;
  panelText(4, HEADER_BASELINE,
            panelFitText(title, LIST_FONT, PANEL_W - 8 - reserved), LIST_FONT);
  if (position.length())
    panelTextRight(PANEL_W - 4, HEADER_BASELINE, position, SMALL_FONT);
  display.drawFastHLine(0, HEADER_RULE_Y, PANEL_W, GxEPD_BLACK);
}

void drawList(const NavFrame &frame)
{
  uint16_t count = rowCount(frame);
  if (count == 0)
  {
    panelTextCentered(70, "Nothing here", LIST_FONT);
    return;
  }

  for (uint8_t i = 0; i < VISIBLE_ROWS; i++)
  {
    uint16_t index = frame.top + i;
    if (index >= count)
      break;
    int16_t y = ROW_TOP + i * ROW_HEIGHT;
    bool selected = index == frame.selected;
    if (selected)
    {
      // A full-width inverted bar rather than a caret: it stays legible as
      // contrast decays between cleanup waveforms, and this panel has no
      // backlight to fall back on.
      display.fillRect(0, y, PANEL_W, ROW_HEIGHT, GxEPD_BLACK);
      unicodeText.setForegroundColor(GxEPD_WHITE);
    }

    Row row = rowAt(frame, index);
    int16_t rightWidth =
        row.right.length() ? panelTextWidth(row.right, SMALL_FONT) + 8 : 0;
    String left = (row.warn ? String("* ") : String("  ")) + row.left;
    panelText(2, y + 11,
              panelFitText(left, LIST_FONT, PANEL_W - 6 - rightWidth),
              LIST_FONT);
    if (row.right.length())
      panelTextRight(PANEL_W - 4, y + 11, row.right, SMALL_FONT);

    if (selected)
      unicodeText.setForegroundColor(GxEPD_BLACK);
  }
}

void drawJump(const NavFrame &frame)
{
  // The grid is static; only the selected letter at the bottom changes, so a
  // jump costs two refreshes to reach anywhere in the library.
  constexpr uint8_t PER_ROW = 14;
  for (uint16_t i = 0; i < JUMP_COUNT; i++)
  {
    int16_t x = 12 + (i % PER_ROW) * 20;
    int16_t y = 40 + (i / PER_ROW) * 22;
    if (i == frame.selected)
    {
      display.fillRect(x - 5, y - 12, 18, 17, GxEPD_BLACK);
      unicodeText.setForegroundColor(GxEPD_WHITE);
    }
    panelText(x, y, String(JUMP_LETTERS[i]), LIST_FONT);
    if (i == frame.selected)
      unicodeText.setForegroundColor(GxEPD_BLACK);
  }
  panelTextCentered(100, String("-> ") + JUMP_LETTERS[frame.selected] + " <-",
                    LIST_FONT);
  panelTextCentered(118, "ENTER jumps, BACK cancels", SMALL_FONT);
}

// Word wrap on UTF-8 codepoint boundaries, breaking at spaces.
uint8_t wrapLyric(const String &text, const uint8_t *font, int16_t maxWidth,
                  String *rows, uint8_t maxRows)
{
  uint8_t used = 0;
  int start = 0;
  while (start < (int)text.length() && used < maxRows)
  {
    int lastSpace = -1;
    int end = start;
    while (end < (int)text.length())
    {
      int next = text.indexOf(' ', end + 1);
      if (next < 0)
        next = text.length();
      if (panelTextWidth(text.substring(start, next), font) > maxWidth)
        break;
      lastSpace = next;
      end = next;
    }
    if (lastSpace < 0)
    {
      // A single word wider than the column: hand it to the ellipsis fitter,
      // which respects codepoint boundaries.
      int next = text.indexOf(' ', start + 1);
      if (next < 0)
        next = text.length();
      rows[used++] = panelFitText(text.substring(start, next), font, maxWidth);
      start = next + 1;
    }
    else
    {
      rows[used++] = text.substring(start, lastSpace);
      start = lastSpace + 1;
    }
  }
  return used;
}

void drawLyricArea()
{
  String body;
  if (!playerHasTrack())
    body = "";
  else if (lyricsAvailable())
  {
    int16_t index = playerLyricIndex();
    body = index >= 0 ? lyricsText((uint16_t)index) : String();
  }
  else
    body = lyricsStatus();

  if (body.length() == 0)
    return;

  String rows[LYRIC_ROWS];
  uint8_t count = wrapLyric(body, LYRIC_FONT, RIGHT_W - 8, rows, LYRIC_ROWS);
  for (uint8_t i = 0; i < count; i++)
    panelText(RIGHT_X + 4, LYRIC_TOP + 13 + i * 18, rows[i], LYRIC_FONT);
}

void drawVolumeOverlay()
{
  uint8_t level = settings().volume;
  constexpr int16_t transportRight = RIGHT_X + RIGHT_W - 4;
  panelText(RIGHT_X + 4, TRANSPORT_Y + 12, "VOLUME", SMALL_FONT);
  panelTextRight(transportRight, TRANSPORT_Y + 12,
                 level == 0 ? String("MUTE")
                            : String(level) + "/" + AUDIO_VOLUME_MAX,
                 SMALL_FONT);

  // Same place and shape as the progress bar, so the strip does not jump.
  int16_t barY = TRANSPORT_Y + 20;
  display.drawRect(RIGHT_X + 4, barY, RIGHT_W - 8, 4, GxEPD_BLACK);
  int16_t filled = (int16_t)((int32_t)(RIGHT_W - 10) * level / AUDIO_VOLUME_MAX);
  if (filled > 0)
    display.fillRect(RIGHT_X + 5, barY + 1, filled, 2, GxEPD_BLACK);
}

void drawSyncOverlay()
{
  int16_t offset = settings().lyricOffsetMs;
  constexpr int16_t transportRight = RIGHT_X + RIGHT_W - 4;
  panelText(RIGHT_X + 4, TRANSPORT_Y + 12, "LYRIC SYNC", SMALL_FONT);
  panelTextRight(transportRight, TRANSPORT_Y + 12,
                 String(offset > 0 ? "+" : "") + offset + " ms", SMALL_FONT);
  panelText(RIGHT_X + 4, TRANSPORT_Y + 24, "UP earlier  DOWN later  ENTER ok",
            SMALL_FONT);
}

void drawTransportArea()
{
  volumeOverlayDrawn = volumeOverlayVisible();
  if (volumeOverlayDrawn)
  {
    if (overlayKind == OVERLAY_SYNC)
      drawSyncOverlay();
    else
      drawVolumeOverlay();
    return;
  }

  const NowTrack &track = playerTrack();
  uint32_t elapsed = playerPositionMs() / 1000;
  uint32_t total = track.durationSeconds;

  constexpr int16_t transportRight = RIGHT_X + RIGHT_W - 4;
  panelText(RIGHT_X + 4, TRANSPORT_Y + 12, formatDuration(elapsed), SMALL_FONT);
  if (total > 0)
    panelTextRight(transportRight, TRANSPORT_Y + 12,
                   String("-") + formatDuration(total > elapsed ? total - elapsed
                                                               : 0),
                   SMALL_FONT);
  else
    panelTextRight(transportRight, TRANSPORT_Y + 12, "--:--", SMALL_FONT);

  // Play/pause action icon, centred between the two times.
  int16_t cx = RIGHT_X + RIGHT_W / 2;
  int16_t cy = TRANSPORT_Y + 8;
  if (playerIsPlaying())
  {
    display.fillRect(cx - 5, cy - 5, 3, 10, GxEPD_BLACK);
    display.fillRect(cx + 2, cy - 5, 3, 10, GxEPD_BLACK);
  }
  else
  {
    display.fillTriangle(cx - 4, cy - 5, cx - 4, cy + 5, cx + 6, cy,
                         GxEPD_BLACK);
  }

  int16_t barY = TRANSPORT_Y + 20;
  display.drawRect(RIGHT_X + 4, barY, RIGHT_W - 8, 4, GxEPD_BLACK);
  if (total > 0 && elapsed > 0)
  {
    uint32_t filled = (uint32_t)(RIGHT_W - 10) * elapsed / total;
    if (filled > (uint32_t)(RIGHT_W - 10))
      filled = RIGHT_W - 10;
    display.fillRect(RIGHT_X + 5, barY + 1, (int16_t)filled, 2, GxEPD_BLACK);
  }
}

void drawNowPlaying()
{
  const NowTrack &track = playerTrack();
  if (!track.valid)
  {
    panelTextCentered(50, "MUSICSTATION", LIST_FONT);
    panelTextCentered(72, "BACK opens the library", SMALL_FONT);
    return;
  }

  int16_t artistWidth = panelTextWidth(track.artist, SMALL_FONT);
  panelText(4, 12, panelFitText(track.title, LIST_FONT, PANEL_W - 12 - artistWidth),
            LIST_FONT);
  panelTextRight(PANEL_W - 4, 12, track.artist, SMALL_FONT);
  panelText(4, 26, panelFitText(track.album, SMALL_FONT, PANEL_W - 8),
            SMALL_FONT);

  if (coverValid())
    display.drawBitmap(COVER_X, COVER_Y, coverBitmap(), coverWidth(),
                       coverHeight(), GxEPD_BLACK, GxEPD_WHITE);
  else
    display.drawRect(COVER_X, COVER_Y, COVER_SIZE, COVER_SIZE, GxEPD_BLACK);

  drawLyricArea();
  drawTransportArea();
}

} // namespace

void uiBegin()
{
  goNowPlaying();
}

void uiGoNowPlaying() { goNowPlaying(); }

void uiStartInLibrary()
{
  goNowPlaying();
  push(SCREEN_LIBRARY, 0);
}

bool uiOnNowPlaying()
{
  return depth > 0 && top().kind == SCREEN_NOW_PLAYING;
}

PanelWindow uiLyricWindow()
{
  return panelRect(RIGHT_X, LYRIC_TOP, RIGHT_W, LYRIC_HEIGHT);
}

PanelWindow uiTransportWindow()
{
  return panelRect(RIGHT_X, TRANSPORT_Y, RIGHT_W, TRANSPORT_H);
}

void uiDrawLyricColumn() { drawLyricArea(); }

PanelWindow uiRightColumnWindow()
{
  return panelRect(RIGHT_X, LYRIC_TOP, RIGHT_W,
                   TRANSPORT_Y + TRANSPORT_H - LYRIC_TOP);
}

void uiDrawRightColumn()
{
  drawLyricArea();
  drawTransportArea();
}
void uiDrawTransport() { drawTransportArea(); }

namespace
{

void stepVolume(int8_t delta)
{
  int16_t level = (int16_t)settings().volume + delta;
  if (level < 0)
    level = 0;
  if (level > AUDIO_VOLUME_MAX)
    level = AUDIO_VOLUME_MAX;
  // Applied to the amp immediately, persisted on release: NVS on every step
  // of a hold would be twenty flash writes for one gesture.
  settings().volume = (uint8_t)level;
  audioSetVolume((uint8_t)level);
  showOverlay(OVERLAY_VOLUME, VOLUME_OVERLAY_MS);
}

// UP moves lyrics earlier: a larger offset starts each line sooner.
void stepLyricOffset(int16_t delta)
{
  int16_t offset = settings().lyricOffsetMs + delta;
  if (offset < LYRIC_OFFSET_MIN_MS)
    offset = LYRIC_OFFSET_MIN_MS;
  if (offset > LYRIC_OFFSET_MAX_MS)
    offset = LYRIC_OFFSET_MAX_MS;
  settings().lyricOffsetMs = offset;
  showOverlay(OVERLAY_SYNC, SYNC_OVERLAY_MS);
  Serial.printf("[lyrics] offset %+d ms, lead %u ms\n", offset,
                (unsigned)playerLyricLeadMs());
}

void leaveSyncMode()
{
  volumeOverlayUntil = millis(); // expired: the clock comes back
  if (volumeOverlayUntil == 0)
    volumeOverlayUntil = 1;
  transportDirty = true;
  settingsSaveThrottled(true);
}

bool handleSyncKey(const InputEvent &event)
{
  int16_t direction = event.action == ACT_UP ? SYNC_STEP_MS : -SYNC_STEP_MS;
  if (event.kind == EV_PRESS)
  {
    syncRepeats = 0;
    stepLyricOffset(direction);
  }
  else if (event.kind == EV_REPEAT && ++syncRepeats % 2 == 0)
    stepLyricOffset(direction);
  return false;
}

bool handleNowPlayingMenu(const InputEvent &event, NavFrame &frame)
{
  switch (event.kind)
  {
  case EV_PRESS:
    // Decided on release or hold, like UP/DOWN.
    pendingMenu = true;
    return false;
  case EV_HOLD:
    pendingMenu = false;
    if (playerHasTrack())
      showOverlay(OVERLAY_SYNC, SYNC_OVERLAY_MS);
    return false;
  case EV_RELEASE:
    if (!pendingMenu)
      return false;
    pendingMenu = false;
    openContextMenu(frame);
    return true;
  default:
    return false;
  }
}

// Returns true when the whole screen needs redrawing.
bool handleTransportKey(const InputEvent &event)
{
  switch (event.kind)
  {
  case EV_PRESS:
    pendingSkip = event.action;
    volumeGestureActive = false;
    volumeRepeats = 0;
    return false;

  case EV_REPEAT:
    // Held past the repeat delay: this is a volume gesture, not a skip.
    pendingSkip = ACT_COUNT;
    volumeGestureActive = true;
    if (volumeRepeats++ % VOLUME_REPEATS_PER_STEP == 0)
      stepVolume(event.action == ACT_UP ? 1 : -1);
    return false;

  case EV_RELEASE:
    if (pendingSkip == event.action)
    {
      pendingSkip = ACT_COUNT;
      if (event.action == ACT_UP)
        playerPrevious();
      else
        playerNext();
      return true;
    }
    if (volumeGestureActive)
    {
      volumeGestureActive = false;
      settingsSaveThrottled(true);
    }
    return false;

  default:
    return false;
  }
}

} // namespace

bool uiHandleEvent(const InputEvent &event)
{
  if (depth == 0)
    uiBegin();

  bool onNowPlaying = top().kind == SCREEN_NOW_PLAYING;

  if (onNowPlaying && syncModeActive())
  {
    if (event.action == ACT_UP || event.action == ACT_DOWN)
      return handleSyncKey(event);
    if ((event.action == ACT_ENTER || event.action == ACT_BACK ||
         event.action == ACT_MENU) &&
        event.kind == EV_PRESS)
    {
      leaveSyncMode();
      return false;
    }
    return false; // swallow releases and holds while tuning
  }

  if (onNowPlaying && event.action == ACT_MENU)
    return handleNowPlayingMenu(event, top());
  if (event.action == ACT_MENU)
    pendingMenu = false;

  if ((event.action == ACT_UP || event.action == ACT_DOWN) && onNowPlaying)
    return handleTransportKey(event);
  // A key pressed on Now Playing and released elsewhere is not a skip.
  if (event.action == pendingSkip)
    pendingSkip = ACT_COUNT;

  if (event.kind == EV_RELEASE)
    return false;

  NavFrame &frame = top();

  switch (event.action)
  {
  case ACT_UP:
  case ACT_DOWN:
  {
    if (event.kind != EV_PRESS && event.kind != EV_REPEAT)
      return false;
    int32_t step = event.action == ACT_UP ? -1 : 1;
    // Fast scroll moves by a page and stops at the ends. Wrapping at speed
    // makes it impossible to tell where you are on a panel this slow.
    if (event.fast)
      moveSelection(frame, step * VISIBLE_ROWS, false);
    else
      moveSelection(frame, step, true);
    return true;
  }

  case ACT_ENTER:
    if (event.kind == EV_PRESS)
    {
      activate(frame);
      return true;
    }
    return false;

  case ACT_BACK:
    if (event.kind == EV_HOLD)
    {
      goNowPlaying();
      return true;
    }
    if (event.kind == EV_PRESS)
    {
      // Now Playing is home, so BACK there opens the library rather than
      // unwinding to nothing.
      if (frame.kind == SCREEN_NOW_PLAYING)
      {
        push(SCREEN_LIBRARY, 0);
        return true;
      }
      if (!pop())
        goNowPlaying();
      return true;
    }
    return false;

  case ACT_MENU:
    if (event.kind == EV_PRESS)
    {
      openContextMenu(frame);
      return true;
    }
    return false;

  default:
    return false;
  }
}

void uiDraw()
{
  if (depth == 0)
    uiBegin();
  const NavFrame &frame = top();

  if (frame.kind == SCREEN_NOW_PLAYING)
  {
    drawNowPlaying();
    return;
  }

  drawHeader(frame);
  switch (frame.kind)
  {
  case SCREEN_JUMP:
    drawJump(frame);
    break;
  case SCREEN_MESSAGE:
    panelTextCentered(60, messageDetail, LIST_FONT);
    panelTextCentered(90, "Any key continues", SMALL_FONT);
    break;
  default:
    drawList(frame);
    break;
  }
}

bool uiTakeRescanRequest()
{
  bool requested = rescanRequested;
  rescanRequested = false;
  return requested;
}

bool uiWantsFullRefresh() { return wantFull; }

bool uiTakeTransportChanged()
{
  bool changed = transportDirty;
  transportDirty = false;
  // The overlay just timed out: put the clock back.
  if (volumeOverlayDrawn && !volumeOverlayVisible())
  {
    // Sync mode timing out is as good as ENTER: keep what was tuned.
    if (overlayKind == OVERLAY_SYNC)
      settingsSaveThrottled(true);
    overlayKind = OVERLAY_VOLUME;
    volumeOverlayUntil = 0;
    changed = true;
  }
  return changed;
}
void uiClearFullRefresh() { wantFull = false; }

namespace
{
uint16_t progressTracks = 0;
String progressArtist, progressAlbum;

void drawBuildProgress()
{
  panelTextCentered(34, "BUILDING INDEX", LIST_FONT);
  panelTextCentered(58, String(progressTracks) + " tracks found", LIST_FONT);
  panelTextCentered(86, panelFitText(progressArtist, SMALL_FONT, PANEL_W - 16),
                    SMALL_FONT);
  panelTextCentered(100, panelFitText(progressAlbum, SMALL_FONT, PANEL_W - 16),
                    SMALL_FONT);
}
} // namespace

void uiBuildProgress(uint16_t tracksFound, const char *artist,
                     const char *album)
{
  static uint32_t lastDrawnAt = 0;
  // At ~780 ms a refresh, drawing per file would make indexing several times
  // slower than the scan. Two seconds keeps it visibly alive for a few percent.
  if (millis() - lastDrawnAt < 2000)
    return;
  lastDrawnAt = millis();
  progressTracks = tracksFound;
  progressArtist = artist ? artist : "";
  progressAlbum = album ? album : "";
  panelDraw(panelRect(0, 0, PANEL_W, PANEL_H), drawBuildProgress);
}

void uiShowMessage(const String &title, const String &detail)
{
  messageTitle = title;
  messageDetail = detail;
  if (depth == 0)
    depth = 1;
  stack[depth - 1] = {SCREEN_MESSAGE, 0, 0, 0};
  wantFull = true;
}
