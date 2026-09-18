#include "cover.h"

#include "storage.h"

namespace
{
uint8_t bitmap[COVER_MAX_BYTES];
uint16_t width = 0;
uint16_t height = 0;
bool valid = false;
} // namespace

void coverClear()
{
  valid = false;
  width = 0;
  height = 0;
}

bool coverLoad(const String &path)
{
  coverClear();

  StorageGuard guard;
  if (!guard || !storageMounted())
    return false;
  if (!CARD.exists(path.c_str()))
  {
    Serial.printf("[cover] no file at %s\n", path.c_str());
    return false;
  }

  File file = CARD.open(path.c_str(), FILE_READ);
  if (!file)
  {
    Serial.printf("[cover] open failed: %s\n", path.c_str());
    return false;
  }

  uint8_t header[10];
  size_t headerRead = file.read(header, sizeof(header));
  if (headerRead != sizeof(header) || memcmp(header, "MSBM", 4) != 0 ||
      header[4] != 1)
  {
    Serial.printf("[cover] bad header: read=%u bytes %02X %02X %02X %02X v%u\n",
                  (unsigned)headerRead, header[0], header[1], header[2],
                  header[3], header[4]);
    file.close();
    return false;
  }

  uint16_t fileWidth = (uint16_t)header[6] | ((uint16_t)header[7] << 8);
  uint16_t fileHeight = (uint16_t)header[8] | ((uint16_t)header[9] << 8);
  bool inverted = (header[5] & 0x01) != 0;

  // Rows are byte-packed, so a width that is not a multiple of 8 would make
  // every row after the first land at the wrong offset.
  size_t needed = (size_t)(fileWidth / 8) * fileHeight;
  if (fileWidth == 0 || fileWidth % 8 != 0 || fileHeight == 0 ||
      needed > COVER_MAX_BYTES)
  {
    Serial.printf("[cover] rejected %ux%u, needs %u bytes, buffer is %u\n",
                  (unsigned)fileWidth, (unsigned)fileHeight, (unsigned)needed,
                  (unsigned)COVER_MAX_BYTES);
    file.close();
    return false;
  }

  size_t got = file.read(bitmap, needed);
  file.close();
  if (got != needed)
  {
    Serial.printf("[cover] short read: wanted %u, got %u\n", (unsigned)needed,
                  (unsigned)got);
    return false;
  }

  if (inverted)
    for (size_t i = 0; i < needed; i++)
      bitmap[i] = (uint8_t)~bitmap[i];

  width = fileWidth;
  height = fileHeight;
  valid = true;
  Serial.printf("[cover] loaded %ux%u\n", (unsigned)width, (unsigned)height);
  return true;
}

bool coverValid() { return valid; }
uint16_t coverWidth() { return width; }
uint16_t coverHeight() { return height; }
const uint8_t *coverBitmap() { return bitmap; }
