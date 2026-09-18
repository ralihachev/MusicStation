// Does SdFat read the files that the stock SD library cannot?
//
// The stock path is Arduino SD -> ESP-IDF VFS -> newlib stdio -> FATFS, and a
// 1024-byte stdio refill makes FATFS issue a multi-block read that this card
// or driver fails. SdFat replaces all of that with its own FAT implementation
// and block driver, so if the failure is in that stack it disappears here.

#include <SPI.h>
#include <SdFat.h>

#define SD_CLK 38
#define SD_CMD 39   // MOSI
#define SD_DAT0 40  // MISO
#define SD_DAT3 41  // CS

SPIClass sdSPI(HSPI);
SdFs sd;

const char *TARGETS[] = {
    "/Music/Radiohead/Pablo Honey/08 Vegetable.lrc",   //  982 B, worked before
    "/Music/Radiohead/Pablo Honey/06 Anyone can play guitar.lrc",  // 1032 B, failed
    "/Music/Radiohead/Pablo Honey/02 Creep.lrc",       // 1224 B, failed
    "/Music/Radiohead/Pablo Honey/cover.bin",          // 1162 B, failed
    "/Music/Radiohead/Pablo Honey/02 Creep.mp3",       // 3.8 MB, failed
};

void probe(const char *path) {
  const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
  Serial.printf("  %-34s ", base);
  FsFile f = sd.open(path, O_RDONLY);
  if (!f) { Serial.println("OPEN FAILED"); return; }
  uint64_t size = f.fileSize();
  uint8_t buf[16] = {0};
  int n = f.read(buf, sizeof(buf));
  bool seekOk = f.seek(0);

  // Also pull a big chunk, which is what playback and artwork actually need.
  uint8_t *big = (uint8_t *)malloc(4096);
  int bigN = big ? f.read(big, 4096) : -1;
  free(big);
  f.close();

  Serial.printf("size=%-9llu read16=%-3d seek=%d read4k=%-5d  ",
                (unsigned long long)size, n, (int)seekOk, bigN);
  for (int i = 0; i < 8 && i < n; i++) Serial.printf("%02X ", buf[i]);
  Serial.print("|");
  for (int i = 0; i < 8 && i < n; i++)
    Serial.print((buf[i] >= 32 && buf[i] < 127) ? (char)buf[i] : '.');
  Serial.println(n == 16 ? "|  OK" : "|  FAIL");
}

bool mounted = false;
uint32_t mhz = 4;

void setup() {
  Serial.begin(115200);
  uint32_t t = millis();
  while (!Serial && millis() - t < 3000) delay(10);
  delay(300);
  Serial.println("\n\nSdFat probe");
  // Bus is started once. Re-running begin() on every cycle was leaving the
  // card mid-transaction and is why init kept failing.
  sdSPI.begin(SD_CLK, SD_DAT0, SD_CMD, SD_DAT3);
  pinMode(SD_DAT3, OUTPUT);
  digitalWrite(SD_DAT3, HIGH);
  delay(50);
}

void loop() {
  Serial.printf("\n========== SDFAT PROBE @ %u MHz ==========\n",
                (unsigned)mhz);
  if (!mounted) {
    // SHARED_SPI is the more forgiving mode; try it before blaming the card.
    mounted = sd.begin(SdSpiConfig(SD_DAT3, SHARED_SPI, SD_SCK_MHZ(mhz), &sdSPI));
    if (!mounted) {
      Serial.print("sd.begin FAILED  ");
      sd.initErrorPrint(&Serial);
      // Walk the clock down: 4 -> 2 -> 1 -> 4 ...
      mhz = (mhz > 1) ? mhz / 2 : 4;
      delay(3000);
      return;
    }
    Serial.printf("mounted OK, FAT type=%d\n", (int)sd.fatType());
  }
  for (const char *p : TARGETS) probe(p);
  delay(4000);
}
