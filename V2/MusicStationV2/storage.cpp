#include "storage.h"

#include "pins.h"

namespace
{
CardInfo card = {false, CARD_NONE, 0, 0};
SemaphoreHandle_t cardMutex = nullptr;
} // namespace

bool storageLock(uint32_t timeoutMs)
{
  if (cardMutex == nullptr)
    return true; // before storageBegin(), nothing else can be running
  return xSemaphoreTakeRecursive(cardMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void storageUnlock()
{
  if (cardMutex != nullptr)
    xSemaphoreGiveRecursive(cardMutex);
}

bool storageBegin()
{
  if (cardMutex == nullptr)
    // Recursive, because the index build holds the card across a full scan
    // while the helpers beneath it take the same lock.
    cardMutex = xSemaphoreCreateRecursiveMutex();
  card = {false, CARD_NONE, 0, 0};

#if SD_CD_ENABLED
  pinMode(SD_CD, INPUT_PULLUP);
  if (!storageCardPresent())
  {
    Serial.println("[sd] no card detected");
    return false;
  }
#endif

  if (!CARD.setPins(SD_CLK, SD_CMD, SD_DAT0))
  {
    Serial.println("[sd] setPins failed: those GPIOs cannot reach the SDMMC peripheral");
    return false;
  }
  if (!CARD.begin(SD_MOUNTPOINT, SD_ONE_BIT_MODE, false, SD_FREQ_KHZ,
                  SD_MAX_OPEN_FILES))
  {
    // A card left in SPI mode by earlier firmware will refuse SDIO until it is
    // physically power-cycled; reseating it is the fix.
    Serial.println("[sd] mount failed (check CLK/CMD/DAT0, FAT32, and reseat the card)");
    return false;
  }

  card.type = CARD.cardType();
  if (card.type == CARD_NONE)
  {
    Serial.println("[sd] mounted but reports no card");
    CARD.end();
    return false;
  }

  card.mounted = true;
  card.sizeBytes = CARD.cardSize();
  card.usedBytes = CARD.usedBytes();
  Serial.printf("[sd] mounted %s %s\n", storageCardTypeName(),
                storageFormatBytes(card.sizeBytes).c_str());
  return true;
}

void storageEnd()
{
  if (card.mounted)
    CARD.end();
  card = {false, CARD_NONE, 0, 0};
}

bool storageCardPresent()
{
#if SD_CD_ENABLED
  int level = digitalRead(SD_CD);
  return SD_CD_ACTIVE_LOW ? level == LOW : level == HIGH;
#else
  return true;
#endif
}

const CardInfo &storageCard() { return card; }
bool storageMounted() { return card.mounted; }

const char *storageCardTypeName()
{
  switch (card.type)
  {
  case CARD_MMC:
    return "MMC";
  case CARD_SD:
    return "SDSC";
  case CARD_SDHC:
    return "SDHC";
  case CARD_NONE:
    return "none";
  }
  return "unknown";
}

String storageFormatBytes(uint64_t bytes)
{
  if (bytes >= 1024ULL * 1024 * 1024)
    return String((float)bytes / (1024.0f * 1024 * 1024), 1) + " GB";
  if (bytes >= 1024ULL * 1024)
    return String((float)bytes / (1024.0f * 1024), 1) + " MB";
  return String((uint32_t)bytes) + " B";
}

void storageDiagnose(const char *path, const char *when)
{
  Serial.printf("[sdtest] === %s ===\n", when);
  Serial.printf("[sdtest] exists=%d\n", CARD.exists(path));

  File f = CARD.open(path, FILE_READ);
  Serial.printf("[sdtest] open=%d size=%u pos=%u\n", (int)(bool)f,
                f ? (unsigned)f.size() : 0u, f ? (unsigned)f.position() : 0u);
  if (f)
  {
    uint8_t buf[16] = {0};
    size_t n = f.read(buf, sizeof(buf));
    Serial.printf("[sdtest] read16=%u bytes: %02X %02X %02X %02X  seek0=%d\n",
                  (unsigned)n, buf[0], buf[1], buf[2], buf[3], (int)f.seek(0));
    f.close();
  }

  // How many handles are actually available right now.
  static File pool[20];
  int opened = 0;
  for (int i = 0; i < 20; i++)
  {
    pool[i] = CARD.open(path, FILE_READ);
    if (!pool[i])
      break;
    opened++;
  }
  Serial.printf("[sdtest] simultaneous opens: %d (max_files=%u)\n", opened,
                (unsigned)SD_MAX_OPEN_FILES);
  for (int i = 0; i < opened; i++)
    pool[i].close();
}

int storageCountEntries(const char *path, uint16_t limit)
{
  StorageGuard guard;
  if (!guard || !card.mounted)
    return -1;
  File dir = CARD.open(path);
  if (!dir || !dir.isDirectory())
  {
    if (dir)
      dir.close();
    return -1;
  }
  int count = 0;
  File entry = dir.openNextFile();
  while (entry && count < limit)
  {
    count++;
    entry.close();
    entry = dir.openNextFile();
  }
  if (entry)
    entry.close();
  dir.close();
  return count;
}
