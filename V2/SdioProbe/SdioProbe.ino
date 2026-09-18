// Read the same files over SDIO instead of SPI.
//
// The Adafruit 4682 supports both. SDIO is the card's native protocol and uses
// the ESP32-S3's dedicated SDMMC peripheral rather than a general-purpose SPI
// master, so it does not share the failure mode we have been chasing.
//
// 1-bit mode needs only the three lines already wired: CLK, CMD, DAT0.

#include <FS.h>
#include <SD_MMC.h>

#define SDIO_CLK 38
#define SDIO_CMD 39   // same wire as SPI MOSI
#define SDIO_DAT0 40  // same wire as SPI MISO

const char *TARGETS[] = {
    "/Music/Radiohead/Pablo Honey/08 Vegetable.lrc",  //  982 B, read OK over SPI
    "/Music/Radiohead/Pablo Honey/06 Anyone can play guitar.lrc",  // 1032 B, failed
    "/Music/Radiohead/Pablo Honey/02 Creep.lrc",      // 1224 B, failed
    "/Music/Radiohead/Pablo Honey/cover.bin",         // 1162 B, failed
    "/Music/Radiohead/Pablo Honey/02 Creep.mp3",      // 3.8 MB, failed
};

bool mounted = false;

void probe(const char *path) {
  const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
  Serial.printf("  %-34s ", base);
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) { Serial.println("OPEN FAILED"); return; }
  uint32_t size = f.size();
  uint8_t buf[16] = {0};
  size_t n = f.read(buf, sizeof(buf));
  bool seekOk = f.seek(0);

  // The 4 KB read is the one that matters: it is what artwork and playback do,
  // and it is what failed over SPI.
  uint8_t *big = (uint8_t *)malloc(4096);
  int bigN = big ? (int)f.read(big, 4096) : -1;
  free(big);
  f.close();

  Serial.printf("size=%-9u read16=%-3u seek=%d read4k=%-5d  ", (unsigned)size,
                (unsigned)n, (int)seekOk, bigN);
  for (size_t i = 0; i < 8 && i < n; i++) Serial.printf("%02X ", buf[i]);
  Serial.print("|");
  for (size_t i = 0; i < 8 && i < n; i++)
    Serial.print((buf[i] >= 32 && buf[i] < 127) ? (char)buf[i] : '.');
  Serial.println(n == 16 ? "|  OK" : "|  FAIL");
}

void setup() {
  Serial.begin(115200);
  uint32_t t = millis();
  while (!Serial && millis() - t < 3000) delay(10);
  delay(300);
  Serial.println("\n\nSDIO probe (1-bit mode)");
  Serial.printf("CLK=%d CMD=%d DAT0=%d\n", SDIO_CLK, SDIO_CMD, SDIO_DAT0);

  if (!SD_MMC.setPins(SDIO_CLK, SDIO_CMD, SDIO_DAT0)) {
    Serial.println("setPins FAILED - these pins cannot reach the SDMMC peripheral");
    return;
  }
  // true = 1-bit mode, which needs no DAT1/DAT2/DAT3 wiring.
  mounted = SD_MMC.begin("/sdcard", true);
  Serial.printf("begin: %s\n", mounted ? "MOUNTED" : "FAILED");
}

void loop() {
  Serial.println("\n========== SDIO PROBE ==========");
  if (!mounted) {
    Serial.println("not mounted; retrying");
    mounted = SD_MMC.begin("/sdcard", true);
    delay(3000);
    return;
  }
  Serial.printf("type=%d size=%llu MB heap=%u\n", (int)SD_MMC.cardType(),
                SD_MMC.cardSize() / (1024ULL * 1024), (unsigned)ESP.getFreeHeap());
  for (const char *p : TARGETS) probe(p);
  delay(4000);
}
