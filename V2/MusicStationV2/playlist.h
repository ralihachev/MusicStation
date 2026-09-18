// .m3u playlists in /Playlists.
//
// Entries are absolute card paths and nothing else. Because lyrics and artwork
// are sidecars sharing the track's basename, a path is sufficient on its own:
// there is no index lookup to do and no "playlist points at a track that
// moved" failure to handle.
#pragma once

#include <Arduino.h>
#include <vector>

constexpr uint16_t PLAYLIST_MAX_ENTRIES = 500;

// Directory listing, cached so the browser does not rescan per draw.
bool playlistRefresh();
uint16_t playlistCount();
String playlistName(uint16_t index);
String playlistPathAt(uint16_t index);

// Reads entries into a caller-owned arena, so the player owns the memory and
// it is released with the queue.
bool playlistRead(const String &path, String &arena,
                  std::vector<uint32_t> &offsets);

bool playlistAppend(const String &playlistPath, const String &trackPath);
bool playlistCreate(const String &name);
