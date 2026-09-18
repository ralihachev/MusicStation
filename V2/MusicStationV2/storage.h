// microSD access. The card is the source of truth for V2: audio, lyrics,
// artwork, playlists, and the device index all live on it.
//
// It runs on its own SPI host (SPI3/HSPI) rather than sharing the panel's, so
// the M4 audio decoder can read continuously on one core while the UI drives
// the panel on the other without arbitrating for a bus.
#pragma once

#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>

// The card is driven over SDIO, not SPI. Same three wires (CLK, CMD, DAT0),
// but the ESP32-S3's dedicated SDMMC peripheral instead of a general-purpose
// SPI master.
//
// SPI mode was tried first and could not read any file larger than 1024 bytes:
// FATFS satisfies a 1024-byte stdio refill with a multi-block read, and those
// failed while single-block reads succeeded. Lyrics over 1 KB, every cover and
// every MP3 were unreadable; directory listings were perfect, which made it
// look like a filesystem problem for far too long.
//
// One gotcha worth knowing: a card that has entered SPI mode stays there until
// it loses power. Switching to SDIO needs a physical power cycle of the card,
// not a reset.
#define CARD SD_MMC

// SDIO in 1-bit mode. DAT1, DAT2 and DAT3 are not needed, so the existing
// three-wire hookup is unchanged.
constexpr bool SD_ONE_BIT_MODE = true;
constexpr int SD_FREQ_KHZ = SDMMC_FREQ_DEFAULT;

// SD.begin() defaults to five simultaneously open files, and the library index
// holds four of them open for its whole life (strings, tracks, albums,
// artists). That leaves one, which the index build blows straight through: its
// four record writers, a directory handle, a directory entry and the audio
// file being probed are all open at once. Opens past the limit fail silently,
// so durations came back as zero and lyric sidecars would not open.
//
// Descriptors are cheap. Sixteen leaves headroom for the M4 decoder's file too.
constexpr uint8_t SD_MAX_OPEN_FILES = 16;
constexpr const char *SD_MOUNTPOINT = "/sdcard";

struct CardInfo
{
  bool mounted;
  uint8_t type;        // CARD_NONE / CARD_MMC / CARD_SD / CARD_SDHC
  uint64_t sizeBytes;
  uint64_t usedBytes;
};

bool storageBegin();
void storageEnd();

// True only when card detect is wired AND enabled in pins.h. With SD_CD
// disabled this always returns true, because an unwired pin cannot be trusted.
bool storageCardPresent();

const CardInfo &storageCard();
bool storageMounted();
const char *storageCardTypeName();
String storageFormatBytes(uint64_t bytes);

// Counts entries directly under `path`. Used by the bring-up screen to prove
// the card is not merely mounted but actually readable.
int storageCountEntries(const char *path, uint16_t limit = 1000);

// Opens one known file and reports what actually works on it, then measures
// how many files can be open at once. Called at boot, before and after the
// index takes its four long-lived handles.
void storageDiagnose(const char *path, const char *when);

// The UI core reads index records while the M4 decoder core streams audio from
// the same card, and the Arduino SD library is not thread-safe. Every access
// takes this lock. Hold it only around the transfer itself: the decoder's
// buffer has to ride out whatever the UI does while holding it.
bool storageLock(uint32_t timeoutMs = 2000);
void storageUnlock();

// RAII wrapper, so an early return cannot leave the card locked.
class StorageGuard
{
public:
  StorageGuard() : locked(storageLock()) {}
  ~StorageGuard()
  {
    if (locked)
      storageUnlock();
  }
  explicit operator bool() const { return locked; }

private:
  bool locked;
};
