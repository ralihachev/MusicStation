// Standalone microSD probe for MusicStation V2.
//
// No display, no index, no UI: just the card. It re-prints everything every
// few seconds, so connecting a monitor at any moment shows the whole picture
// rather than a fragment of a boot that already happened.

#include <FS.h>
#include <SD.h>
#include <SPI.h>

#define SD_CLK 38
#define SD_CMD 39   // MOSI
#define SD_DAT0 40  // MISO
#define SD_DAT3 41  // CS

SPIClass sdSPI(HSPI);

const char *ALBUM = "/Music/Radiohead/Pablo Honey";
const char *TARGETS[] = {
    "/Music/Radiohead/Pablo Honey/02 Creep.lrc",
    "/Music/Radiohead/Pablo Honey/03 How do you.lrc",
    "/Music/Radiohead/Pablo Honey/cover.bin",
    "/Music/Radiohead/Pablo Honey/02 Creep.mp3",
};

uint32_t clockHz = 4000000;
bool mounted = false;

bool mountCard(uint32_t hz) {
  SD.end();
  sdSPI.end();
  sdSPI.begin(SD_CLK, SD_DAT0, SD_CMD, SD_DAT3);
  return SD.begin(SD_DAT3, sdSPI, hz, "/sd", 16);
}

void probeFile(const char *path) {
  Serial.printf("  %-46s ", strrchr(path, '/') + 1);
  if (!SD.exists(path)) {
    Serial.println("MISSING");
    return;
  }
  File f = SD.open(path, FILE_READ);
  if (!f) {
    Serial.println("OPEN FAILED");
    return;
  }
  uint32_t size = f.size();
  uint8_t buf[16] = {0};
  size_t n = f.read(buf, sizeof(buf));
  bool seekOk = f.seek(0);
  f.close();

  Serial.printf("size=%-8u read=%-3u seek=%d  ", (unsigned)size, (unsigned)n,
                (int)seekOk);
  for (size_t i = 0; i < 8 && i < n; i++) Serial.printf("%02X ", buf[i]);
  Serial.print(" |");
  for (size_t i = 0; i < 8 && i < n; i++)
    Serial.print((buf[i] >= 32 && buf[i] < 127) ? (char)buf[i] : '.');
  Serial.println(n == sizeof(buf) ? "|  OK" : "|  SHORT READ");
}

// Counts how many handles are free right now, without keeping any.
int freeHandles() {
  static File pool[20];
  int n = 0;
  for (int i = 0; i < 20; i++) {
    pool[i] = SD.open(TARGETS[1], FILE_READ);
    if (!pool[i]) break;
    n++;
  }
  for (int i = 0; i < n; i++) pool[i].close();
  return n;
}

void report() {
  Serial.println("\n========== SD PROBE ==========");
  Serial.printf("clock=%u Hz  mounted=%d  heap=%u\n", (unsigned)clockHz,
                (int)mounted, (unsigned)ESP.getFreeHeap());
  if (!mounted) {
    Serial.println("mount failed: check CMD=MOSI(39), DAT0=MISO(40), CLK(38), CS(41), 3V3");
    return;
  }
  Serial.printf("card type=%d size=%llu MB\n", (int)SD.cardType(),
                SD.cardSize() / (1024ULL * 1024));

  // FIRST operation after mount, before anything else opens a file. If a big
  // file reads here but not later, the fault is a descriptor leak in the test
  // above it, not the file size.
  Serial.printf("-- cold read, nothing else opened (handles free: %d) --\n",
                freeHandles());
  probeFile(TARGETS[3]);  // Creep.mp3, 3.8 MB
  probeFile(TARGETS[2]);  // cover.bin, 1162 B
  probeFile(TARGETS[0]);  // Creep.lrc, 1224 B
  Serial.printf("handles free after three reads: %d\n", freeHandles());

  Serial.println("-- directory listing --");
  File dir = SD.open(ALBUM);
  if (!dir || !dir.isDirectory()) {
    Serial.println("  album folder not readable");
  } else {
    File e = dir.openNextFile();
    int n = 0;
    while (e && n < 30) {
      Serial.printf("  %-46s %u\n", strrchr(e.name(), '/') ? strrchr(e.name(), '/') + 1 : e.name(),
                    (unsigned)e.size());
      e.close();
      e = dir.openNextFile();
      n++;
    }
    dir.close();
  }

  // The .lrc files range from 564 to 1262 bytes, which brackets 1024 neatly.
  // If everything at or below 1024 reads and everything above fails, the
  // threshold is a buffer size, not the card.
  Serial.printf("handles free after directory listing: %d\n", freeHandles());
  Serial.println("-- file reads, every .lrc by size --");
  {
    File d = SD.open(ALBUM);
    char names[32][64];
    uint32_t sizes[32];
    int count = 0;
    File e = d.openNextFile();
    while (e && count < 32) {
      const char *base = strrchr(e.name(), '/') ? strrchr(e.name(), '/') + 1 : e.name();
      if (strstr(base, ".lrc") || strstr(base, ".bin")) {
        snprintf(names[count], sizeof(names[0]), "%s/%s", ALBUM, base);
        sizes[count] = e.size();
        count++;
      }
      e.close();
      e = d.openNextFile();
    }
    d.close();
    for (int i = 0; i < count; i++)
      for (int j = i + 1; j < count; j++)
        if (sizes[j] < sizes[i]) {
          uint32_t ts = sizes[i]; sizes[i] = sizes[j]; sizes[j] = ts;
          char tn[64]; strcpy(tn, names[i]); strcpy(names[i], names[j]); strcpy(names[j], tn);
        }
    for (int i = 0; i < count; i++) probeFile(names[i]);
  }
  probeFile(TARGETS[3]);

  // Reads larger than newlib's 1024-byte stdio buffer bypass it and go
  // straight to the filesystem. If small reads fail while large ones succeed,
  // the fault is the buffered path, not the card.
  Serial.println("-- read-size sweep on a large file --");
  {
    const size_t sizes[] = {16, 256, 512, 1023, 1024, 1025, 2048, 4096};
    for (size_t want : sizes) {
      File f = SD.open(TARGETS[3], FILE_READ);
      if (!f) { Serial.printf("  want=%-5u OPEN FAILED\n", (unsigned)want); continue; }
      uint8_t *buf = (uint8_t *)malloc(want);
      if (!buf) { Serial.printf("  want=%-5u malloc failed\n", (unsigned)want); f.close(); continue; }
      size_t got = f.read(buf, want);
      Serial.printf("  want=%-5u got=%-5u %s  first: %02X %02X %02X %02X\n",
                    (unsigned)want, (unsigned)got, got == want ? "OK  " : "FAIL",
                    buf[0], buf[1], buf[2], buf[3]);
      free(buf);
      f.close();
    }
  }

  // How many handles are really available.
  static File pool[20];
  int opened = 0;
  for (int i = 0; i < 20; i++) {
    pool[i] = SD.open(TARGETS[0], FILE_READ);
    if (!pool[i]) break;
    opened++;
  }
  Serial.printf("simultaneous opens: %d\n", opened);
  for (int i = 0; i < opened; i++) pool[i].close();
}

void setup() {
  Serial.begin(115200);
  uint32_t t = millis();
  while (!Serial && millis() - t < 3000) delay(10);
  delay(300);
  Serial.println("\n\nMusicStation V2 - standalone SD probe");
  mounted = mountCard(clockHz);
}

void loop() {
  report();
  // Alternate 4 MHz and 1 MHz: if the slower clock reads files the faster one
  // cannot, the problem is signal integrity rather than software.
  clockHz = (clockHz == 4000000) ? 1000000 : 4000000;
  mounted = mountCard(clockHz);
  delay(4000);
}
