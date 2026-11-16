// fm1core's hal.h on the M5 Cardputer (ADV): the SD card (SPI), settings (NVS), Wi-Fi and HTTPS, the battery. The
// USB host (the FM-1, controllers, its boot loader), Bluetooth MIDI and USB audio aren't here yet: they say there's
// nothing there
#include <Arduino.h>
#include <M5Cardputer.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <ff.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <string>
#include <vector>

#include "board.h"
#include "hal.h"

// the SD card: SPI, shared with the LoRa cap (the ADV's GPIO5 lets the card have the bus)
#define SD_SCK 40
#define SD_MISO 39
#define SD_MOSI 14
#define SD_CS 12
#define SD_GATE 5
#define SD_ROOT "/sdcard"
#define LOG_FILE SD_ROOT "/fm1buddy/cardbuddy-log.txt"  // (fm1buddy's own is log.txt: a card shared by both)

static bool sdOk, wifiUp, radio;
static SemaphoreHandle_t logLock;

// ---------------------------------------------------------------------------------------------- board ---
void board::begin() {
  logLock = xSemaphoreCreateMutex();
  pinMode(SD_GATE, OUTPUT);
  digitalWrite(SD_GATE, HIGH);
  delay(2);
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdOk = SD.begin(SD_CS, SPI, 25000000, SD_ROOT) || (SD.end(), SD.begin(SD_CS, SPI, 16000000, SD_ROOT));
  if (sdOk) {
    mkdir(SD_ROOT "/fm1buddy", 0777);
    remove(LOG_FILE);
  }
  hal::log("cardbuddy %s: sd %s", FM1BUDDY_VERSION, sdOk ? "ok" : "missing");
}

// ------------------------------------------------------------------------------------------- the rest ---
uint32_t hal::millis() {
  return ::millis();
}

void hal::log(const char* fmt, ...) {
  char line[200];
  va_list a;
  va_start(a, fmt);
  vsnprintf(line, sizeof line, fmt, a);
  va_end(a);
  Serial.printf("%6u %s\n", (unsigned)::millis(), line);
  if (sdOk && logLock && xSemaphoreTake(logLock, pdMS_TO_TICKS(50))) {
    if (FILE* f = fopen(LOG_FILE, "a")) {
      fprintf(f, "%6u %s\n", (unsigned)::millis(), line);
      fclose(f);
    }
    xSemaphoreGive(logLock);
  }
}

void hal::logRam(const char* what) {
  hal::log("ram: %s: %u KB free (biggest %u KB)", what, (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
}

void hal::psramFirst(bool) {
  // (no PSRAM on the Cardputer: everything is in the internal RAM)
}

void hal::idle() {
  vTaskDelay(1);
}

const char* hal::sd() {
  return sdOk ? SD_ROOT : 0;
}

bool hal::sdSpace(uint64_t& total, uint64_t& used) {
  if (!sdOk) {
    return false;
  }
  total = SD.totalBytes();
  used = SD.usedBytes();
  return true;
}

bool hal::load(const char* key, void* p, size_t n) {
  Preferences prefs;
  prefs.begin("cardbuddy", true);
  bool ok = prefs.getBytesLength(key) == n && prefs.getBytes(key, p, n) == n;
  prefs.end();
  return ok;
}

void hal::save(const char* key, const void* p, size_t n) {
  Preferences prefs;
  prefs.begin("cardbuddy", false);
  prefs.putBytes(key, p, n);
  prefs.end();
}

void hal::backlight(uint8_t percent) {
  M5Cardputer.Display.setBrightness(percent * 255 / 100);
}

uint32_t hal::batteryMv() {
  return M5Cardputer.Power.getBatteryVoltage();
}

void hal::sleep() {
  // (later: deep sleep, woken by a key)
}

// --------------------------------------------------------------------------------------------- Wi-Fi ---
bool hal::wifiScan(std::vector<Ap>& out) {
  radio = true;
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks();
  out.clear();
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) {
      continue;  // hidden
    }
    auto a = std::find_if(out.begin(), out.end(), [&](const Ap& x) { return x.ssid == s.c_str(); });
    if (a == out.end()) {
      out.push_back({s.c_str(), (int)WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN});
    } else if (WiFi.RSSI(i) > a->rssi) {
      a->rssi = WiFi.RSSI(i);
    }
  }
  WiFi.scanDelete();
  std::sort(out.begin(), out.end(), [](const Ap& a, const Ap& b) { return a.rssi > b.rssi; });
  return n >= 0;
}

bool hal::wifiJoin(const char* ssid, const char* pass, std::string& err) {
  hal::logRam("wifi");
  radio = true;
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  for (uint32_t t = ::millis(); ::millis() - t < 15000; delay(50)) {
    wl_status_t s = WiFi.status();
    if (s == WL_CONNECTED) {
      wifiUp = true;
      return true;
    }
    if (s == WL_CONNECT_FAILED || s == WL_NO_SSID_AVAIL) {
      break;
    }
  }
  wl_status_t s = WiFi.status();
  err = std::string(s == WL_NO_SSID_AVAIL ? "Can't find " : "Can't join ") + ssid +
        (s == WL_NO_SSID_AVAIL ? "." : ": check the password.");
  wifiOff();
  return false;
}

void hal::wifiOff() {
  wifiUp = false;
  if (!radio) {
    return;
  }
  radio = false;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

bool hal::wifiOn() {
  return wifiUp;
}

// ---------------------------------------------------------------------------------------------- HTTPS ---
static esp_err_t onEvent(esp_http_client_event_t* e) {
  if (e->event_id == HTTP_EVENT_ON_HEADER && !strcasecmp(e->header_key, "ETag")) {
    *(std::string*)e->user_data = e->header_value;
  }
  return ESP_OK;
}

int hal::get(const char* url, const char* path, std::string* etag, void (*progress)(uint32_t, uint32_t),
             std::string& err) {
  std::string tag;
  esp_http_client_config_t c = {};
  c.url = url;
  c.event_handler = onEvent;
  c.user_data = &tag;
  c.crt_bundle_attach = esp_crt_bundle_attach;
  c.timeout_ms = 15000;
  c.buffer_size = 4096;
  c.buffer_size_tx = 8192;  // (GitHub's release downloads redirect to a ~4 KB signed URL)
  c.user_agent = "cardbuddy";
  esp_http_client_handle_t h = esp_http_client_init(&c);
  if (etag && !etag->empty()) {
    esp_http_client_set_header(h, "If-None-Match", etag->c_str());
  }
  int status = -1;
  for (int hops = 0; hops < 5; hops++) {
    esp_err_t r = esp_http_client_open(h, 0);
    if (r != ESP_OK) {
      err = std::string("Can't reach it (") + esp_err_to_name(r) + ").";
      break;
    }
    int64_t len = esp_http_client_fetch_headers(h);
    status = esp_http_client_get_status_code(h);
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
      if (esp_http_client_set_redirection(h) != ESP_OK) {
        err = "The server's redirect didn't fit: too much of it to read.";
        status = -1;
        break;
      }
      esp_http_client_close(h);
      status = -1;
      continue;
    }
    if (status != 200) {
      break;
    }
    std::string part = std::string(path) + ".part";
    FILE* f = fopen(part.c_str(), "wb");
    if (!f) {
      err = "Can't write to the SD card.";
      status = -1;
      break;
    }
    static char buf[4096];
    uint32_t got = 0;
    bool ok = true;
    for (;;) {
      int n = esp_http_client_read(h, buf, sizeof buf);
      if (n < 0) {
        err = "The download stopped part way.";
        ok = false;
        break;
      }
      if (n == 0) {
        if (!esp_http_client_is_complete_data_received(h)) {
          err = "The download stopped part way.";
          ok = false;
        }
        break;
      }
      if (fwrite(buf, 1, n, f) != (size_t)n) {
        err = "Can't write to the SD card (is it full?).";
        ok = false;
        break;
      }
      got += n;
      if (progress) {
        progress(got, len > 0 ? (uint32_t)len : 0);
      }
    }
    fclose(f);
    remove(path);
    if (!ok || rename(part.c_str(), path)) {
      if (ok) {
        err = "Can't rename it on the SD card.";
      }
      remove(part.c_str());
      status = -1;
    }
    break;
  }
  esp_http_client_close(h);
  esp_http_client_cleanup(h);
  if (status == 200 && etag) {
    *etag = tag;
  }
  return status;
}

static void toHex(const uint8_t sum[32], char hex[65]) {
  for (int i = 0; i < 32; i++) {
    sprintf(hex + i * 2, "%02x", sum[i]);
  }
}

bool hal::sha256(const uint8_t* p, size_t n, char hex[65]) {
  uint8_t sum[32];
  if (mbedtls_sha256(p, n, sum, 0)) {
    return false;
  }
  toHex(sum, hex);
  return true;
}

bool hal::sha256(const char* path, char hex[65]) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    return false;
  }
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  static uint8_t buf[4096];
  for (size_t n; (n = fread(buf, 1, sizeof buf, f));) {
    mbedtls_sha256_update(&ctx, buf, n);
  }
  fclose(f);
  uint8_t sum[32];
  mbedtls_sha256_finish(&ctx, sum);
  mbedtls_sha256_free(&ctx);
  toHex(sum, hex);
  return true;
}

// through FatFs: its directory entries have the size and time (the card is its only volume: drive 0)
bool hal::listDir(const char* path, void (*fn)(const char* name, uint32_t size, uint32_t mtime, bool folder, void* arg), void* arg) {
  if (strncmp(path, SD_ROOT "/", 8)) {
    return false;
  }
  std::string p = std::string("0:") + (path + 7);
  FF_DIR d;
  FRESULT r = f_opendir(&d, p.c_str());
  if (r != FR_OK) {
    hal::log("sd: can't open %s (FatFs %d)", p.c_str(), (int)r);
    return false;
  }
  FILINFO fi;
  while ((r = f_readdir(&d, &fi)) == FR_OK && fi.fname[0]) {
    if (fi.fattrib & AM_DIR) {
      fn(fi.fname, 0, 0, true, arg);
      continue;
    }
    struct tm tm = {};
    tm.tm_mday = fi.fdate & 0x1F;
    tm.tm_mon = ((fi.fdate >> 5) & 0xF) - 1;
    tm.tm_year = (fi.fdate >> 9) + 80;
    tm.tm_sec = (fi.ftime & 0x1F) * 2;
    tm.tm_min = (fi.ftime >> 5) & 0x3F;
    tm.tm_hour = fi.ftime >> 11;
    tm.tm_isdst = -1;
    fn(fi.fname, fi.fsize, (uint32_t)mktime(&tm), false, arg);
  }
  f_closedir(&d);
  return r == FR_OK;
}

const char* hal::selfUpdate(const char*, void (*)(uint32_t, uint32_t)) {
  return "Update cardbuddy from M5 Launcher or M5Burner.";
}

void hal::reboot() {
  hal::log("restarting");
  esp_restart();
}

struct Spawn {
  void (*fn)(void*);
  void* arg;
};

static void trampoline(void* p) {
  Spawn s = *(Spawn*)p;
  delete (Spawn*)p;
  s.fn(s.arg);
  vTaskDelete(0);
}

void hal::spawn(void (*fn)(void*), void* arg, uint32_t stack, int priority) {
  xTaskCreatePinnedToCore(trampoline, "job", stack, new Spawn{fn, arg}, priority, 0, 0);
}

// ------------------------------------------------------------------------------- not here yet: USB ---
bool hal::usbHost() {
  return false;
}
uint32_t hal::midiOut(int, const uint8_t*, uint32_t n) {
  return n;  // (nothing to send it to: dropped)
}
uint32_t hal::midiSent(int) {
  return 0;
}
bool hal::storageThere(uint16_t, uint16_t) {
  return false;
}
bool hal::storageSeen(uint16_t, uint16_t) {
  return false;
}
bool hal::storageCommand(const uint8_t*, uint8_t, bool, uint8_t*, uint32_t, uint32_t* got, uint32_t) {
  if (got) {
    *got = 0;
  }
  return false;
}
void hal::usbRestart() {}

bool hal::computerMidi() {
  return false;
}
void hal::computerMidiSet(bool) {}
bool hal::onComputer() {
  return false;
}

bool hal::diskMode() {
  return false;
}
void hal::diskModeSet(bool) {}
int hal::diskState() {
  return DISK_WAITING;
}

bool hal::recoveryMode() {
  return false;
}
void hal::recoveryModeSet(bool) {}
void hal::keyStart() {}
int hal::keyState() {
  return KEY_IDLE;
}

bool hal::audioIn(AudioIn&) {
  return false;
}
void hal::audioOn(bool) {}
uint32_t hal::audioRead(uint8_t*, uint32_t) {
  return 0;
}
uint32_t hal::audioLost() {
  return 0;
}

// ------------------------------------------------------------------------- not here yet: Bluetooth ---
void hal::bleOn(bool) {}
bool hal::bleIsOn() {
  return false;
}
void hal::bleSearch(bool) {}
int hal::blePeers(BlePeer*, int) {
  return 0;
}
void hal::bleToggle(int) {}
bool hal::bleChanged() {
  return false;
}
void hal::bleHold(bool) {}
void hal::bleAllow(bool) {}

// --------------------------------------------------------------------------------- no touchscreen ---
int hal::fingers(Finger*, int) {
  return 0;
}
void hal::touchCal(const TouchCal*) {}
