#include "ui.h"

#include <M5Cardputer.h>

#include <atomic>
#include <vector>

#include "catalog.h"
#include "fm1.h"
#include "hal.h"
#include "jobs.h"
#include "midi.h"
#include "play.h"
#include "settings.h"
#include "themes.h"

// The screen is drawn a band of BAND rows at a time, into one band's canvas (4 bits a pixel, the theme's palette: 1.8
// KB), each pushed as it's done: the whole screen's would take 16 KB of the internal RAM, which TLS needs (no PSRAM).
// Everything drawn is moved up by bandY, so what's outside the band falls off it
#define BAND 15
static M5Canvas canvas(&M5Cardputer.Display);
static int bandY;
static std::vector<ui::Page*> stack;
static bool dirty = true;
static std::string toastText;
static bool toastBad;
static uint32_t toastUntil, lastTick, seenFinished;
static char jobStatus[96];

static const Theme& theme() {
  return THEMES[settings.theme < THEME_COUNT ? settings.theme : 0];
}

void ui::applyTheme() {
  const Theme& t = theme();
  uint32_t c[] = {t.bg, t.panel, t.off, t.beat, t.text, t.dim, t.accent, t.knob[0], t.knob[1], t.knob[2], t.knob[3]};
  for (int i = 0; i < (int)(sizeof c / sizeof c[0]); i++) {
    canvas.setPaletteColor(i, c[i]);
  }
  hal::backlight(settings.brightness ? settings.brightness : 80);
  dirty = true;
}

// the flash map's colors for the FM-1's own screen while its loader runs (jobs.h): the theme's
void jobs::screenColors(uint32_t rgb[SCREEN_COLORS]) {
  const Theme& t = theme();
  uint32_t c[SCREEN_COLORS] = {t.bg, t.off, t.knob[1], t.knob[2], t.accent, t.beat, t.knob[3], t.knob[0], t.text, t.dim};
  memcpy(rgb, c, sizeof c);
}

// -------------------------------------------------------------------------------------------- drawing ---
void ui::fill(int x, int y, int w, int h, uint8_t color) {
  canvas.fillRect(x, y - bandY, w, h, color);
}

// (no heap: it's drawn once a band, many times a second, while a download needs every byte)
void ui::text(int x, int y, const char* s, uint8_t color, int maxW) {
  canvas.setTextColor(color);
  if (maxW > 0 && canvas.textWidth(s) > maxW) {
    char t[128];
    int n = std::min<int>(strlen(s), sizeof t - 2);
    memcpy(t, s, n);
    t[n] = '~', t[n + 1] = 0;
    while (n > 0 && canvas.textWidth(t) > maxW) {
      t[--n] = '~', t[n + 1] = 0;
    }
    canvas.drawString(t, x, y - bandY);
    return;
  }
  canvas.drawString(s, x, y - bandY);
}

void ui::textRight(int x, int y, const char* s, uint8_t color) {
  canvas.setTextColor(color);
  canvas.drawString(s, x - canvas.textWidth(s), y - bandY);
}

void ui::bar(int x, int y, int w, int h, uint32_t done, uint32_t total) {
  fill(x, y, w, h, OFF);
  if (total) {
    fill(x, y, (int)((uint64_t)w * std::min(done, total) / total), h, ACCENT);
  }
}

int ui::wrap(int x, int y, int w, const char* s, uint8_t color, int maxLines) {
  int lines = 0;
  char line[128];
  while (*s) {
    // as many words as fit (or one too long, cut), up to a line break
    int n = 0, fit = 0;
    for (;;) {
      while (s[n] && s[n] != ' ' && s[n] != '\n' && n < (int)sizeof line - 1) {
        n++;
      }
      memcpy(line, s, n);
      line[n] = 0;
      if (fit && canvas.textWidth(line) > w) {
        break;
      }
      fit = n;
      if (s[n] != ' ' || n >= (int)sizeof line - 1) {
        break;
      }
      n++;
    }
    line[fit] = 0;
    if (y + lines * 10 + 10 > bandY && y + lines * 10 < bandY + BAND && lines < maxLines) {
      text(x, y + lines * 10, line, color, w);  // (only in the band being drawn)
    }
    lines++;
    s += fit;
    if (*s == ' ' || *s == '\n') {
      s++;
    }
  }
  return std::min(lines, maxLines);
}

void ui::listRows(int count, int sel, int& scroll, int y0, int rows, void (*row)(int, int, bool, void*), void* arg) {
  if (sel < scroll) {
    scroll = sel;
  }
  if (sel >= scroll + rows) {
    scroll = sel - rows + 1;
  }
  scroll = std::max(0, std::min(scroll, std::max(0, count - rows)));
  for (int i = scroll; i < count && i < scroll + rows; i++) {
    int y = y0 + (i - scroll) * ROW;
    if (y + ROW <= bandY || y >= bandY + BAND) {
      continue;  // (not in the band being drawn)
    }
    if (i == sel) {
      fill(0, y, SCREEN_W - 3, ROW, OFF);
      fill(0, y, 2, ROW, ACCENT);
    }
    row(i, y, i == sel, arg);
  }
  if (count > rows) {  // the scroll bar
    int h = std::max(6, (BOTTOM - y0) * rows / count);
    int at = y0 + (BOTTOM - y0 - h) * scroll / std::max(1, count - rows);
    fill(SCREEN_W - 2, y0, 2, BOTTOM - y0, PANEL);
    fill(SCREEN_W - 2, at, 2, h, DIM);
  }
}

// one band of the screen (from bandY) into the canvas
static void drawBand() {
  using namespace ui;
  canvas.fillScreen(BG);
  Page* p = stack.back();
  // the title bar: the page, then what's going on (a job, the battery)
  fill(0, 0, SCREEN_W, TOP - 1, PANEL);
  text(3, 3, p->title(), ACCENT, 130);
  char right[40];
  int level = M5Cardputer.Power.getBatteryLevel();
  const Fm1Id* id = fm1::known(fm1::current());
  snprintf(right, sizeof right, "%s%s%s%d%%", id ? fm1::machineName() : "", id ? "  " : "", hal::wifiOn() ? "wifi  " : "",
           level);
  textRight(SCREEN_W - 3, 3, right, DIM);
  p->draw();
  // the bottom: a job, a toast, or the page's keys
  fill(0, BOTTOM, SCREEN_W, SCREEN_H - BOTTOM, PANEL);
  if (jobs::running) {
    if (jobs::total) {
      bar(0, BOTTOM, SCREEN_W, 2, jobs::done, jobs::total);
    }
    text(3, BOTTOM + 3, jobStatus, TEXT, SCREEN_W - 6);
  } else if (hal::millis() < toastUntil) {
    text(3, BOTTOM + 3, toastText.c_str(), toastBad ? BAD : GOOD, SCREEN_W - 6);
  } else {
    text(3, BOTTOM + 3, p->hints(), DIM, SCREEN_W - 6);
  }
}

static void drawFrame() {
  if (jobs::running) {  // (once a frame, not once a band)
    snprintf(jobStatus, sizeof jobStatus, "%s", jobs::status().c_str());
  }
  for (bandY = 0; bandY < SCREEN_H; bandY += BAND) {
    drawBand();
    // (the display named here: a global M5Canvas is made before M5Cardputer is)
    canvas.pushSprite(&M5Cardputer.Display, 0, bandY);
  }
  bandY = 0;
}

// ------------------------------------------------------------------------------------------- the pages ---
void ui::push(Page* p) {
  stack.push_back(p);
  dirty = true;
}

void ui::pop() {
  if (stack.size() > 1) {
    delete stack.back();
    stack.pop_back();
  }
  dirty = true;
}

void ui::replace(Page* p) {
  delete stack.back();
  stack.back() = p;
  dirty = true;
}

void ui::redraw() {
  dirty = true;
}

void ui::toast(const std::string& s, bool bad) {
  toastText = s;
  toastBad = bad;
  toastUntil = hal::millis() + 3000;
  hal::log("toast: %s", s.c_str());
  dirty = true;
}

// --------------------------------------------------------------------------------------- the firmwares ---
// The SD card's .fwsc files are read on a thread of their own (catalog::readSd), one scan at a time, as fm1buddy does
// at power-on (its ui.cpp bootBegin): the UI's thread takes the result once it is there (catalog::takeSd). The first
// scan reads the Hub's list on the SD card first. A scan asked for while one runs is made again when that one ends
static FwList bootHub;
static std::string bootUpdated;
static catalog::SdScan scanResult;
static std::atomic<bool> scanDone{false};
static bool scanRunning, scanAgain, scanIsBoot, hubNew, updateDonePending;
static uint32_t scanMs;

static void startScan(bool boot) {
  scanRunning = true;
  scanIsBoot = boot;
  scanAgain = false;
  scanDone = false;
  hal::spawn(
      [](void* arg) {
        const bool boot = arg != 0;
        if (boot) {
          std::string err;
          // (the Hub's list stays on the SD card: catalog::keepHub is off. One left half-read is finished)
          if (!catalog::hubWhen(bootUpdated)) {
            catalog::writeHub(bootUpdated, err);
          }
        }
        uint32_t t0 = hal::millis();
        scanResult = catalog::readSd(boot ? &bootHub : 0);
        scanMs = hal::millis() - t0;
        // (the Hub's count is in the Hub job's log: its list stays on the SD card, so bootHub is empty here)
        hal::log("%s: sd %u (found %u, %u with no sha256), scan %u ms", boot ? "boot" : "scan",
                 (unsigned)catalog::sdShown(&scanResult), (unsigned)catalog::sdFound(&scanResult),
                 (unsigned)catalog::sdUnknown(&scanResult), (unsigned)scanMs);
        hal::logRam(boot ? "boot scan" : "scan");
        scanDone = true;
      },
      boot ? (void*)1 : (void*)0);
}

// a scan of /fm1 is wanted: now, or when the one that runs ends
static void wantScan() {
  if (!hal::sd()) {
    return;
  }
  if (scanRunning) {
    scanAgain = true;
  } else {
    startScan(false);
  }
}

static void bootBegin() {
  if (hal::sd()) {
    startScan(true);
  }
}

bool ui::scanning() {
  return scanRunning;
}

// the UI's thread takes a scan that ended
static void scanWatch() {
  if (!scanRunning || !scanDone) {
    return;
  }
  scanRunning = false;
  catalog::takeSd(scanResult);  // (it starts the task that reads the sha256 of the new files)
  if (scanIsBoot) {
    if (!hubNew) {
      catalog::hub.swap(bootHub);
      catalog::hubUpdated = bootUpdated;
    }
    FwList().swap(bootHub);
  }
  if (scanAgain) {
    startScan(false);
  } else if (updateDonePending) {
    updateDonePending = false;
    stack.back()->jobDone();  // (the page finds the new file in the SD list now)
  }
  dirty = true;
}

static void watch() {
  if (catalog::sdHashed()) {
    wantScan();
    dirty = true;
  }
  if (seenFinished != jobs::finished) {
    seenFinished = jobs::finished;
    if (jobs::ok && jobs::kind == jobs::HUB) {
      catalog::hub.swap(jobs::hub);
      FwList().swap(jobs::hub);
      hubNew = true;
      catalog::hubUpdated = jobs::hubUpdated;
      wantScan();
    } else if (jobs::kind == jobs::DOWNLOAD || jobs::kind == jobs::INSTALL || jobs::kind == jobs::FW_UPDATE || jobs::kind == jobs::SYNC) {
      wantScan();
    }
    if (jobs::kind == jobs::SYNC && !jobs::hubUpdated.empty()) {
      catalog::hubUpdated = jobs::hubUpdated;  // (the list it got first)
    }
    if (!jobs::ok && !jobs::error.empty()) {
      ui::toast(jobs::error, true);
    }
    if (jobs::kind == jobs::FW_UPDATE && jobs::ok && scanRunning) {
      updateDonePending = true;  // (its page looks for the new file: it waits for the new SD list)
    } else {
      stack.back()->jobDone();
    }
    dirty = true;
  }
  fm1::watch();
  uint32_t fm1s = 0;  // the keys' notes leave the FM-1s' own alone (play.h)
  for (int d = 0; d < MIDI_DEVS; d++) {
    fm1s |= fm1::known(d) ? 1u << d : 0;
  }
  play::skip(fm1s);
  scanWatch();
}

// ------------------------------------------------------------------------------------------ the keyboard ---
static std::vector<ui::Key> injected;

void ui::inject(const Key& k) {
  injected.push_back(k);
}

void ui::shot(void (*row)(const uint16_t* px, int n)) {
  std::vector<uint16_t> line(SCREEN_W);
  for (bandY = 0; bandY < SCREEN_H; bandY += BAND) {
    drawBand();
    for (int y = 0; y < BAND && bandY + y < SCREEN_H; y++) {
      for (int x = 0; x < SCREEN_W; x++) {
        line[x] = canvas.readPixel(x, y);
      }
      row(line.data(), SCREEN_W);
    }
  }
  bandY = 0;
}

static bool readKey(ui::Key& k) {
  if (!injected.empty()) {
    k = injected.front();
    injected.erase(injected.begin());
    return true;
  }
  if (!M5Cardputer.Keyboard.isChange()) {
    return false;
  }
  auto s = M5Cardputer.Keyboard.keysState();
  if (stack.back()->raw()) {  // (every change: what's held now)
    std::vector<char> held(s.word.begin(), s.word.end());
    stack.back()->held(s.fn ? std::vector<char>() : held);
  }
  if (!M5Cardputer.Keyboard.isPressed()) {
    return false;
  }
  k = {};
  k.enter = s.enter;
  k.del = s.backspace || s.del;  // (the del key is backspace; fn+del, forward delete)
  k.tab = s.tab;
  // fn with ` ; . , /: esc and the arrows (the library says so itself)
  k.back = s.esc;
  k.up = s.up, k.down = s.down, k.left = s.left, k.right = s.right;
  bool typing = stack.back()->typing();
  for (char c : s.word) {
    bool nav = !typing;  // (nothing being typed: ` ; . , / alone are esc and the arrows too)
    if (nav && c == ';') {
      k.up = true;
    } else if (nav && c == '.') {
      k.down = true;
    } else if (nav && c == ',') {
      k.left = true;
    } else if (nav && c == '/') {
      k.right = true;
    } else if (nav && c == '`') {
      k.back = true;
    } else if (!k.ch) {
      k.ch = c;
    }
  }
  return k.ch || k.up || k.down || k.left || k.right || k.enter || k.back || k.del || k.tab;
}

void ui::begin() {
  catalog::keepHub = false;  // (the Hub's list, ~100 KB, stays on the SD card: read as it's needed)
  catalog::keepSd = false;   // (the SD list stays on the SD card too: a package long is ~100 bytes of memory)
  settingsLoad();
  canvas.setColorDepth(4);
  canvas.createSprite(SCREEN_W, BAND);
  canvas.createPalette();
  canvas.setFont(&fonts::Font0);
  canvas.setTextSize(1);
  canvas.setTextDatum(top_left);
  applyTheme();
  push(homePage());
  // as fm1buddy starts: Bluetooth MIDI and MIDI routing as they were left, the mapping's rules, and the keyboard as a
  // controller that's always there
  bool on = false;
  if (!hal::recoveryMode() && hal::load("bluetooth", &on, 1) && on) {
    hal::bleOn(true);
  }
  play::begin();
  on = false;
  hal::load("routing", &on, 1);
  if (!hal::diskMode() && !hal::recoveryMode()) {
    midi::loadMap();
    midi::mount(MIDI_TOUCH, 0, 0, "Keys");
  }
  midi::route(on && !hal::diskMode() && !hal::recoveryMode());
  if (hal::recoveryMode()) {
    push(recoverPage());
  } else if (hal::diskMode()) {
    push(diskPage());
  }
  drawFrame();
  if (!hal::diskMode()) {  // (the computer has the card then)
    bootBegin();
  }
}

// an arrow held: it goes again after a moment, and faster the longer it's held (8 a second, up to 40)
static ui::Key held;
static uint32_t heldAt, nextAt;

static bool arrow(const ui::Key& k) {
  return (k.up || k.down || k.left || k.right) && !k.ch && !k.enter && !k.back && !k.del && !k.tab;
}

void ui::loop() {
  Key k;
  bool got = readKey(k);
  uint32_t now = hal::millis();
  if (got) {
    held = arrow(k) && !stack.back()->raw() ? k : Key{};
    heldAt = now, nextAt = now + 350;
  } else if (arrow(held) && M5Cardputer.Keyboard.isPressed() && (int32_t)(now - nextAt) >= 0) {
    k = held, got = true;
    nextAt = now + std::max<uint32_t>(25, 125 - (now - heldAt) / 20);
  } else if (!M5Cardputer.Keyboard.isPressed()) {
    held = {};
  }
  if (got) {
    Page* p = stack.back();
    if (k.back && p->closing()) {
      pop();
    } else if (!k.back || stack.back() == p) {
      p->key(k);
    }
    dirty = true;
  }
  if (hal::millis() - lastTick >= 100) {
    lastTick = hal::millis();
    watch();
    stack.back()->tick();
    static bool wasRunning, hadToast;
    bool toasting = hal::millis() < toastUntil;
    if (jobs::running || wasRunning || toasting != hadToast) {
      dirty = true;  // (its status, as it goes)
    }
    wasRunning = jobs::running;
    hadToast = toasting;
  }
  if (dirty) {
    dirty = false;
    drawFrame();
  }
}
