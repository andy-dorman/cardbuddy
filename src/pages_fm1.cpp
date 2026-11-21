// The pages for the FM-1 on the USB-C port: backup and recovery (and recovery mode itself), USB disk mode, sounds,
// SLOOP, MIDI, and the keyboard as a MIDI controller
#include <M5Cardputer.h>
#include <dirent.h>
#include <strings.h>

#include <algorithm>
#include <functional>
#include <vector>

#include "catalog.h"
#include "fm1.h"
#include "hal.h"
#include "jobs.h"
#include "midi.h"
#include "savename.h"
#include "sloop.h"
#include "sounds.h"
#include "uboot.h"
#include "ui.h"

using namespace ui;

// the device to speak to: the FM-1 if there's one, else the first USB MIDI device (-1: none)
static int device() {
  int d = fm1::current();
  midi::Dev v;
  for (int i = 0; d < 0 && i < MIDI_USB; i++) {
    if (midi::dev(i, v)) {
      d = i;
    }
  }
  return d;
}

static std::string devName(int d) {
  midi::Dev v;
  if (d >= 0 && midi::dev(d, v)) {
    return v.name;
  }
  return "";
}

static void busy() {
  toast("Busy with something else.", true);
}

// the flash map (jobs.h), 16 x 16 sectors from x, y, each c pixels
static void drawMap(int x, int y, int c) {
  for (int i = 0; i < MAP_SECTORS; i++) {
    uint8_t color = OFF;
    if (jobs::mapCheck[i] == jobs::CHECK_BAD) {
      color = BAD;
    } else if (jobs::mapMark[i] == jobs::MARK_DIFFERS) {
      color = WARN;
    } else if (jobs::mapMark[i] == jobs::MARK_WRITTEN) {
      color = GOOD;
    } else {
      switch (jobs::mapKind[i]) {
        case jobs::MAP_HEAD:
        case jobs::MAP_KEEP:
          color = INFO;
          break;
        case jobs::MAP_DATA:
          color = ACCENT;
          break;
        case jobs::MAP_FREE:
          color = BEAT;
          break;
      }
    }
    fill(x + (i % 16) * c, y + (i / 16) * c, c - 1, c - 1, color);
  }
  if (jobs::mapAt >= 0) {
    int i = jobs::mapAt;
    fill(x + (i % 16) * c, y + (i / 16) * c, c - 1, c - 1, TEXT);
  }
}

// --------------------------------------------------------------------------------------- a list page ---
// rows of text (and a dim note on the right), and what enter (or another key) does with the one chosen
// file: the name of the saved file that the key r renames on this row ("" if the row is no saved file)
struct Row {
  std::string name, note;
  std::function<void()> enter;
  std::string file;
};

struct ListPage : Page {
  std::string t;
  const char* h;
  std::vector<Row> rows;
  int sel = 0, scroll = 0;
  std::string empty = "Nothing here.";
  std::function<void(ListPage&)> fill;         // makes rows (again: a job finished, a device came)
  std::function<bool(char, ListPage&)> other;  // another key, on row sel: true if it took it
  std::function<void(ListPage&)> done;         // a job finished
  uint32_t seenSeq = 0;
  ListPage(const std::string& t, const char* h) : t(t), h(h) {}
  const char* title() override { return t.c_str(); }
  const char* hints() override { return h; }
  void refill() {
    rows.clear();
    if (fill) {
      fill(*this);
    }
    sel = std::min(sel, std::max(0, (int)rows.size() - 1));
    seenSeq = midi::seq;
    redraw();
  }
  void key(const Key& k) override {
    int n = rows.size();
    if (k.up && n) {
      sel = (sel + n - 1) % n;
    } else if (k.down && n) {
      sel = (sel + 1) % n;
    } else if (k.enter && n && rows[sel].enter) {
      rows[sel].enter();
    } else if (k.ch && other) {
      other(k.ch, *this);
    }
  }
  void tick() override {
    if (midi::seq != seenSeq) {
      refill();
    }
  }
  void jobDone() override {
    if (done) {
      done(*this);
    }
    refill();
  }
  void draw() override {
    if (rows.empty()) {
      wrap(6, TOP + 6, SCREEN_W - 12, empty.c_str(), DIM, 6);
      return;
    }
    listRows(rows.size(), sel, scroll, TOP + 2, 9, [](int i, int y, bool s, void* arg) {
      ListPage* p = (ListPage*)arg;
      const Row& r = p->rows[i];
      int w = r.note.empty() ? SCREEN_W - 16 : 150;
      text(8, y + 2, r.name.c_str(), s ? TEXT : DIM, w);
      if (!r.note.empty()) {
        textRight(SCREEN_W - 6, y + 2, r.note.c_str(), s ? DIM : OFF);
      } }, this);
  }
};

// a question: y goes ahead
struct AskPage : Page {
  std::string t, q;
  std::function<void()> yes;
  AskPage(const std::string& t, const std::string& q, std::function<void()> yes) : t(t), q(q), yes(yes) {}
  const char* title() override { return t.c_str(); }
  const char* hints() override { return "y yes  esc no"; }
  void key(const Key& k) override {
    if (k.ch == 'y' || k.ch == 'Y') {
      auto f = yes;
      pop();
      f();
    }
  }
  void draw() override { wrap(6, TOP + 6, SCREEN_W - 12, q.c_str(), TEXT, 9); }
};

static void ask(const std::string& t, const std::string& q, std::function<void()> yes) {
  push(new AskPage(t, q, yes));
}

// install f on the FM-1 (the firmware page's i)
void ui::install(const Firmware& f) {
  if (!fm1::known(fm1::current())) {
    toast("No FM-1 on the USB-C port.", true);
  } else if (!jobs::install(f)) {
    busy();
  }
}

void ui::installAnyway(const Firmware& f) {
  Firmware again = f;
  again.onSd = true;  // (the install got its file on the SD card before the data save)
  again.file = jobs::saved;
  ask("Install", "The data was not saved. Install anyway?", [again]() {
    if (!jobs::install(again, false)) {
      busy();
    }
  });
}

// ---------------------------------------------------------------------------------- the data restore ---
static void startDataBack(const std::string& file, bool dataSaveFirst, bool transfer);

// the data of a data save going back on the FM-1: its steps and the flash map, then what happened
struct DataBackPage : Page {
  std::string file;
  explicit DataBackPage(const std::string& file) : file(file) {}
  const char* title() override { return "Your data"; }
  const char* hints() override { return jobs::running ? "leave it plugged in" : "enter done"; }
  bool closing() override { return !jobs::running; }
  void key(const Key& k) override {
    if (k.enter && !jobs::running) {
      pop();
    }
  }
  void tick() override { redraw(); }
  // a restore that cannot finish offers recovery mode with the data save. A data save of another FM-1 asks the
  // transfer question, and its yes runs the job again as a transfer
  void jobDone() override {
    if (jobs::kind != jobs::DATA_RESTORE || jobs::ok) {
      return;
    }
    if (jobs::anotherFm1) {
      std::string name = file;
      ask("Your data",
          "This data save is from another FM-1. Put it on this FM-1? The two FM-1 devices can get the same Bluetooth "
          "identity.",
          [name]() {
            pop();  // (this page: the new job gets its own)
            startDataBack(name, true, true);
          });
      return;
    }
    std::string path = DATA_SAVES_DIR "/" + file;
    std::string question;
    if (jobs::noPackage) {
      question = "No firmware in /fm1 matches " + file + ". Start recovery mode with all of that data save?";
    } else if (jobs::sectorFailed) {
      question = "A sector failed 3 times. Start recovery mode with " + file + "?";
    } else {
      return;
    }
    ask("Recovery", question, [path]() {
      if (!jobs::prepare(path)) {
        busy();
      }
    });
  }
  void draw() override {
    bool mine = jobs::kind == jobs::DATA_RESTORE;
    int w = SCREEN_W - 12;
    if (mine && (jobs::running || jobs::mapKind[0] != jobs::MAP_UNREAD)) {
      drawMap(SCREEN_W - 16 * 6 - 4, TOP + 6, 6);
      w = SCREEN_W - 16 * 6 - 16;
    }
    int y = TOP + 4;
    text(6, y, file.c_str(), ACCENT, w);
    y += 12;
    if (jobs::running) {
      wrap(6, y, w, jobs::status().c_str(), TEXT, 7);
    } else if (mine && jobs::ok) {
      wrap(6, y, w, jobs::error.empty() ? "Done: your data is on the FM-1 again." : jobs::error.c_str(), GOOD, 7);
    } else if (mine) {
      wrap(6, y, w, jobs::error.c_str(), BAD, 7);  // (a refusal says why, and has no way round it)
    } else {
      wrap(6, y, w, "Starting...", TEXT, 7);
    }
  }
};

// start the restore of a data save, and show its page. dataSaveFirst: a data save of the current data comes first.
// transfer: the data save may be from another FM-1
static void startDataBack(const std::string& file, bool dataSaveFirst, bool transfer) {
  int d = fm1::current();
  if (d < 0) {
    toast("Plug the FM-1 into the USB-C port (on a powered hub).", true);
  } else if (!jobs::restoreData(d, file, dataSaveFirst, transfer)) {
    busy();
  } else {
    push(new DataBackPage(file));
  }
}

void ui::askDataBack(const Firmware& f) {
  std::string file = jobs::dataSaveFor(f.product, jobs::dataSaveUid);
  if (file.empty()) {
    return;
  }
  ask("Your data", "Put your saved data back?", [file]() { startDataBack(file, false, false); });
}

// ------------------------------------------------------------------------------------------- name page ---
// the name page: one at a time, so its callback is kept here (the text page takes a plain function)
static std::function<void(const std::string&)> nameDone;

static void askName(const char* title, const std::string& prompt, const std::string& start,
                    std::function<void(const std::string&)> go) {
  nameDone = go;
  push(textPage(title, prompt.c_str(), false, [](const std::string& typed, void*) {
    auto go = nameDone;
    go(typed); }, 0, start, true));
}

// the UTC date that a default name holds, or why it holds none
static std::string dateNote() {
  std::string date = savename::dateText(time(0));
  return date.empty() ? "No date yet: join Wi-Fi first." : "UTC date " + date + ".";
}

// the name page of a save that the user starts. It runs no job: the job starts when enter takes the text
static void askSaveName(savename::Kind kind, int d, std::function<bool(const std::string&)> start) {
  const Fm1Id* id = fm1::known(d);
  askName("Name the file", dateNote() + " enter saves with this name.", jobs::defaultName(kind, id ? id->text : ""),
          [start](const std::string& typed) {
            if (!start(typed)) {
              busy();
            }
          });
}

// the key r: the name page with the name of the selected file, and the rename. folder: where the file is
static bool renameKey(char c, ListPage& p, const std::string& folder) {
  if (c != 'r') {
    return false;
  }
  if (p.rows.empty() || p.rows[p.sel].file.empty()) {
    return true;
  }
  std::string file = p.rows[p.sel].file;
  size_t dot = folder == SETUPS_DIR ? std::string::npos : file.rfind('.');
  std::string stem = dot == std::string::npos || !dot ? file : file.substr(0, dot);
  ListPage* list = &p;
  askName("Rename", "enter renames it. fn+esc goes back.", stem, [list, folder, file](const std::string& typed) {
    if (jobs::renameSaved(folder, file, typed)) {
      toast(jobs::saved == file ? "The name stays: " + file : "Renamed to " + jobs::saved);
    } else {
      toast(jobs::error.empty() ? "Can't rename it." : jobs::error, true);
    }
    list->refill();
  });
  return true;
}

// --------------------------------------------------------------------------------- backup & recover ---
struct Choice {
  std::string path, name, what;
  long number = 0;  // its save number (a firmware: 0)
};

// the backups that can go back on it, newest first. The SD card's firmwares are too many for a list here: the page of
// them (SdPickPage) reads them as they are on screen
static std::vector<Choice> choices() {
  std::vector<Choice> out;
  std::string dir = catalog::path(BACKUPS_DIR);
  std::vector<Choice> found;
  if (DIR* d = dir.empty() ? 0 : opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      size_t n = strlen(e->d_name);
      if (n <= 4 || strcasecmp(e->d_name + n - 4, ".bin")) {
        continue;
      }
      Choice c{BACKUPS_DIR "/" + std::string(e->d_name), e->d_name, "backup"};
      c.number = savename::numberOfSaved(dir, e->d_name);
      // its .txt beside it says what it was taken from
      if (FILE* f = fopen((dir + "/" + std::string(e->d_name, n - 4) + ".txt").c_str(), "r")) {
        char line[96];
        while (fgets(line, sizeof line, f)) {
          if (!strncmp(line, "firmware ", 9)) {
            line[strcspn(line, "\r\n")] = 0;
            c.what = line + 9;
          }
        }
        fclose(f);
      }
      found.push_back(c);
    }
    closedir(d);
  }
  std::sort(found.begin(), found.end(), [](const Choice& a, const Choice& b) {
    return savename::newer(savename::Numbered{a.name, a.number}, savename::Numbered{b.name, b.number});
  });
  out.insert(out.end(), found.begin(), found.end());
  return out;
}

// the data saves in /backups/switch, newest first. The list reads no unique ID: the restore job checks it
static Page* dataSaves() {
  ListPage* p = new ListPage("Data saves", "enter put back  r name  esc back");
  p->empty = "No data saves in " DATA_SAVES_DIR ".";
  p->fill = [](ListPage& p) {
    for (const datasave::Save& s : jobs::dataSaveList()) {
      std::string file = s.file;
      p.rows.push_back({file, s.firmware, [file]() {
                          ask("Your data", "Put this data save back?", [file]() { startDataBack(file, true, false); });
                        },
                        file});
    }
  };
  p->other = [](char c, ListPage& p) { return renameKey(c, p, DATA_SAVES_DIR); };
  p->refill();
  return p;
}

// the SD card's firmwares for the FM-1 or the SMK-37, to put on one that won't start. A row is a place in the SD list:
// the rows near the screen are read, as on the Firmware page
struct SdPickPage : Page {
  struct Line {
    uint32_t at;
    std::string path, name, what;
  };
  std::vector<uint32_t> rows;
  std::vector<Line> shown;
  uint32_t seen = 0xFFFFFFFF;
  int sel = 0, scroll = 0;
  SdPickPage() { filter(); }
  const char* title() override { return "Recover from the SD card"; }
  const char* hints() override { return "enter recover  esc back"; }
  static bool wanted(const FwList& one) {
    const char* product = one.str(one[0].product);
    return !strncmp(product, "FM-1_", 5) || isSmk(product);  // (it says what it's for: recovery checks the flash it finds)
  }
  void filter() {
    rows.clear();
    shown.clear();
    FILE* f = 0;
    FwList one;
    if (catalog::openSd(f)) {
      for (uint32_t i = 0, n = catalog::sdShown(); i < n; i++) {
        if (catalog::sdAt(f, i, one) && wanted(one)) {
          rows.push_back(i);
        }
      }
      if (f) {
        fclose(f);
      }
    }
    sel = std::min(sel, std::max(0, (int)rows.size() - 1));
    seen = FwList::changes;
  }
  const Line& line(int i) {
    for (const Line& l : shown) {
      if (l.at == rows[i]) {
        return l;
      }
    }
    if (shown.size() > 60) {
      shown.clear();
    }
    FILE* f = 0;
    if (!catalog::openSd(f)) {
      f = 0;
    }
    FwList one;
    for (int k = i; k < (int)rows.size() && k < i + 12; k++) {
      Line l{rows[k], "", "?", ""};
      if (catalog::sdAt(f, rows[k], one)) {
        const FwItem& it = one[0];
        const char *v = one.str(it.version), *file = one.str(it.file), *name = one.str(it.name);
        bool smk = isSmk(one.str(it.product));
        std::string what = it.flags & FwItem::STOCK ? "stock" : *v ? v
                                                                   : "firmware";
        l = {rows[k], std::string(FW_DIR "/") + file, *name ? name : file, smk ? what + ", SMK-37" : what};
      }
      shown.push_back(l);
    }
    if (f) {
      fclose(f);
    }
    return shown[shown.size() - std::min<int>(12, rows.size() - i)];  // (the row i is the first of those read)
  }
  void tick() override {
    if (seen != FwList::changes) {
      filter();
      redraw();
    }
  }
  void key(const Key& k) override {
    int n = rows.size();
    if (k.up && n) {
      sel = (sel + n - 1) % n;
    } else if (k.down && n) {
      sel = (sel + 1) % n;
    } else if (k.enter && n) {
      std::string path = line(sel).path, name = line(sel).name;
      ask("Recover", "Put " + name +
                         " on an FM-1 that won't start? cardbuddy restarts into recovery mode, "
                         "then says what to do.",
          [path]() {
            if (!jobs::prepare(path)) {
              busy();
            }
          });
    }
  }
  void draw() override {
    int y0 = TOP + 2;
    if (ui::scanning()) {
      text(4, y0, "Reading the SD card...", ACCENT, SCREEN_W - 8);
      y0 += 11;
    }
    if (rows.empty()) {
      if (!ui::scanning()) {
        wrap(6, y0 + 4, SCREEN_W - 12, hal::sd() ? "Download a firmware (stock is on the Hub) first." : "No SD card.", DIM, 4);
      }
      return;
    }
    listRows(rows.size(), sel, scroll, y0, (BOTTOM - y0) / ROW, [](int i, int y, bool s, void* arg) {
      SdPickPage* p = (SdPickPage*)arg;
      const Line& l = p->line(i);
      text(10, y + 2, l.name.c_str(), s ? TEXT : DIM, 120);
      textRight(SCREEN_W - 6, y + 2, l.what.c_str(), s ? DIM : OFF); }, this);
  }
};

struct BackupPage : ListPage {
  BackupPage() : ListPage("Backup & recover", "r name  esc back") {
    fill = [](ListPage& p) {
      int d = fm1::current();
      const Fm1Id* id = fm1::known(d);
      p.rows.push_back({"Back up the FM-1 now", id ? id->text : "none here", [d]() {
                          if (d < 0) {
                            toast("Plug the FM-1 into the USB-C port (on a powered hub).", true);
                          } else {
                            askSaveName(savename::BACKUP, d, [d](const std::string& name) { return jobs::backup(d, name); });
                          }
                        }});
      p.rows.push_back({"Data saves", "put one back", []() { push(dataSaves()); }});
      if (hal::sd()) {
        p.rows.push_back({"Recover from the SD card", "firmwares", []() { push(new SdPickPage()); }});
      }
      std::vector<Choice> cs = choices();
      if (cs.empty() && !hal::sd()) {
        p.rows.push_back({"Recover: nothing to put on it", "no SD card", []() { toast("No SD card.", true); }});
      }
      for (const Choice& c : cs) {
        std::string path = c.path, name = c.name;
        bool backup = !strncmp(path.c_str(), BACKUPS_DIR "/", strlen(BACKUPS_DIR "/"));
        p.rows.push_back({"Recover: " + c.name, c.what, [path, name]() {
                            ask("Recover", "Put " + name +
                                               " on an FM-1 that won't start? cardbuddy restarts into recovery mode, "
                                               "then says what to do.",
                                [path]() {
                                  if (!jobs::prepare(path)) {
                                    busy();
                                  }
                                });
                          },
                          backup ? name : ""});
      }
    };
    other = [](char c, ListPage& p) { return renameKey(c, p, BACKUPS_DIR); };
    done = [](ListPage&) {
      if (jobs::kind == jobs::BACKUP && jobs::ok) {
        toast("Saved as " BACKUPS_DIR "/" + jobs::saved);
      }
    };
    refill();
  }
  // the map of a backup shows while it runs, and after it until a key: a new visit to this page shows the list
  bool watched = false;
  bool showsMap() {
    return jobs::kind == jobs::BACKUP && (jobs::running || (watched && jobs::mapKind[0] != jobs::MAP_UNREAD));
  }
  void key(const Key& k) override {
    if (showsMap() && !jobs::running) {
      watched = false;  // (the key only closes the map)
      redraw();
      return;
    }
    ListPage::key(k);
  }
  void tick() override {
    ListPage::tick();
    if (jobs::running && jobs::kind == jobs::BACKUP) {
      watched = true;
      redraw();
    }
  }
  void draw() override {
    if (showsMap()) {
      drawMap(SCREEN_W - 16 * 6 - 4, TOP + 6, 6);
      wrap(6, TOP + 6, SCREEN_W - 16 * 6 - 16, jobs::running ? jobs::status().c_str() : "Backed up.", TEXT, 8);
      return;
    }
    ListPage::draw();
  }
};

Page* ui::backupPage() {
  return new BackupPage();
}

// recovery mode: the board started for it (jobs::prepare), the FM-1 straight on the USB-C port
struct RecoverPage : Page {
  bool started = false;
  const char* title() override { return "Recovery"; }
  // nothing's gone on it yet: it's still waiting for the FM-1 (it would wait for it forever)
  bool waiting() { return jobs::running && (jobs::step == jobs::GETTING_READY || jobs::step == jobs::KEYING); }
  const char* hints() override {
    if (waiting()) {
      return "esc cancel";
    }
    if (jobs::running) {
      return "leave it plugged in";
    }
    return jobs::kind == jobs::RECOVER && !jobs::ok && jobs::calLost ? "a again  s it's an SMK-37  enter done"
                                                                     : "a again  enter done";
  }
  bool closing() override { return false; }
  void key(const Key& k) override {
    if (waiting() && k.back) {
      hal::recoveryModeSet(false);  // (it restarts as it was)
    }
    if (jobs::running) {
      return;
    }
    if (k.enter) {
      hal::recoveryModeSet(false);  // (it restarts as it was)
    } else if (k.ch == 'a') {
      hal::recoveryModeSet(true);  // (again, from the start)
    } else if (k.ch == 's' && jobs::calLost) {
      jobs::giveCalibration();
      hal::recoveryModeSet(true);
    }
  }
  void tick() override {
    if (!started) {
      started = jobs::recover();
    }
    redraw();
  }
  void draw() override {
    std::string from = jobs::recovering();
    std::string name = from.substr(from.rfind('/') + 1);
    // the name the SD list gives it: looked for once for each list and file, not at each draw
    static std::string seenFrom, seenName;
    static uint32_t seenChanges = 0xFFFFFFFF;
    if (seenChanges != FwList::changes || seenFrom != from) {
      seenChanges = FwList::changes;
      seenFrom = from;
      seenName = name;
      const std::string dir = FW_DIR "/";
      FwList one;
      int at = from.compare(0, dir.size(), dir) ? -1 : catalog::sdFind(from.c_str() + dir.size());
      if (at >= 0 && catalog::sdAt(at, one) && *one.str(one[0].name)) {
        seenName = one.str(one[0].name);
      }
    }
    name = seenName;
    int st = jobs::step;
    bool onFlash = st == jobs::READING_FLASH || st == jobs::SAVING || st == jobs::WRITING_FLASH ||
                   st == jobs::CHECKING_FLASH || st == jobs::LEAVING;
    bool mine = jobs::kind == jobs::RECOVER;
    int w = SCREEN_W - 12;
    if (mine && (jobs::running ? onFlash : jobs::mapKind[0] != jobs::MAP_UNREAD)) {
      drawMap(SCREEN_W - 16 * 6 - 4, TOP + 6, 6);
      w = SCREEN_W - 16 * 6 - 16;
    }
    int y = TOP + 4;
    text(6, y, name.c_str(), ACCENT, w);
    y += 12;
    std::string s;
    uint8_t c = TEXT;
    if (jobs::running) {
      s = jobs::status();
      if (waiting() && hal::keyState() < hal::KEY_ANSWERED) {
        s += "\nSwitch the FM-1 off, plug it straight into the USB-C port (no hub), then switch it on.";
      }
    } else if (mine && jobs::ok) {
      s = jobs::error.empty() ? "Done: it's on the FM-1." : jobs::error;
      s += "\nSwitch it off and on: it starts what was written. Then enter.";
      c = GOOD;
    } else if (mine) {
      s = jobs::error;
      c = BAD;
    } else {
      s = "Starting...";
    }
    y += 10 * wrap(6, y, w, s.c_str(), c, 7);
    if (mine && !jobs::saved.empty()) {
      wrap(6, y + 2, w, ("Its flash as it was: " BACKUPS_DIR "/" + jobs::saved).c_str(), DIM, 2);
    }
  }
};

Page* ui::recoverPage() {
  return new RecoverPage();
}

// USB disk mode: the SD card is a computer's drive
struct DiskPage : Page {
  const char* title() override { return "USB disk"; }
  const char* hints() override { return "eject it on the computer to go back"; }
  bool closing() override { return false; }
  void key(const Key&) override {}
  void tick() override {
    if (hal::diskState() == hal::DISK_EJECTED) {
      hal::diskModeSet(false);  // (ejected: back as it was, by itself)
    }
    redraw();
  }
  void draw() override {
    int s = hal::diskState();
    const char* what = s == hal::DISK_MOUNTED   ? "A computer has the SD card. Eject it there before you're done."
                       : s == hal::DISK_EJECTED ? "Ejected: back in a moment."
                                                : "Plug the Cardputer into a computer: the SD card shows up as a drive.";
    wrap(6, TOP + 6, SCREEN_W - 12, what, s == hal::DISK_EJECTED ? GOOD : TEXT, 6);
  }
};

Page* ui::diskPage() {
  return new DiskPage();
}

// ---------------------------------------------------------------------------------------------- sounds ---
static sounds::Speaks speaks;
static int speaksDev = -1;

static Page* syxPage(const std::string& folder);
static Page* setupsPage();
static Page* sloopPage();

static Page* syxPage(const std::string& folder) {
  ListPage* p = new ListPage(folder.empty() ? "Sounds: /syx" : "/syx/" + folder, "enter send  r name  esc back");
  p->empty = "No .syx files here (" SYX_DIR " on the SD card).";
  p->fill = [folder](ListPage& p) {
    for (const sounds::File& f : sounds::list(folder)) {
      std::string path = folder.empty() ? f.name : folder + "/" + f.name;
      if (f.folder) {
        p.rows.push_back({f.name + "/", "", [path]() { push(syxPage(path)); }});
        continue;
      }
      char kb[16];
      snprintf(kb, sizeof kb, "%u KB", (unsigned)((f.size + 1023) / 1024));
      p.rows.push_back({f.name, kb, [path]() {
                          int d = device();
                          if (d < 0) {
                            toast("Nothing on the USB-C port to send it to.", true);
                          } else if (!jobs::send(d, path)) {
                            busy();
                          }
                        },
                        f.name});
    }
  };
  p->other = [folder](char c, ListPage& p) { return renameKey(c, p, folder.empty() ? SYX_DIR : SYX_DIR "/" + folder); };
  p->done = [](ListPage&) {
    if (jobs::kind == jobs::SEND && jobs::ok) {
      toast("Sent.");
    }
  };
  p->refill();
  return p;
}

static Page* setupsPage() {
  ListPage* p = new ListPage("Setups", "enter load  e export  r name  esc back");
  p->empty = "No setups yet: Sounds > Keep its song and kits.";
  p->fill = [](ListPage& p) {
    for (const std::string& s : sounds::setups()) {
      p.rows.push_back({s, "", [s]() {
                          int d = device();
                          ask("Load", "Load setup " + s + " onto " + devName(d) + "? It saves it.", [d, s]() {
                            if (d < 0 || !jobs::restore(d, s)) {
                              busy();
                            }
                          });
                        },
                        s});
    }
  };
  p->other = [](char c, ListPage& p) {
    if (c == 'r') {
      return renameKey(c, p, SETUPS_DIR);
    }
    if (c == 'e' && !p.rows.empty() && !jobs::exportSetup(p.rows[p.sel].name, speaks.app[0] ? speaks.app : "FOMAJAM")) {
      busy();
    }
    return c == 'e';
  };
  p->done = [](ListPage&) {
    if (jobs::ok && (jobs::kind == jobs::RESTORE || jobs::kind == jobs::EXPORT)) {
      toast(jobs::kind == jobs::RESTORE ? "Loaded: it's saved on it." : jobs::saved);
    }
  };
  p->refill();
  return p;
}

struct SoundsPage : ListPage {
  SoundsPage() : ListPage("Sounds", "p ask again  esc back") {
    fill = [](ListPage& p) {
      int d = device();
      if (d < 0) {
        p.empty = "Plug the FM-1 (or any MIDI device) into the USB-C port, on a powered hub.";
        return;
      }
      if (d != speaksDev && !jobs::running) {
        speaksDev = d;
        memset(&speaks, 0, sizeof speaks);
        jobs::probe(d);
      }
      p.rows.push_back({"Send a .syx", devName(d), []() { push(syxPage("")); }});
      if (speaks.stockVoice) {
        p.rows.push_back({"Save its voice", ".syx", []() {
                            int d = device();
                            askSaveName(savename::VOICE, d, [d](const std::string& name) { return jobs::voice(d, name); });
                          }});
      }
      if (speaks.share) {
        p.rows.push_back({"Keep its song and kits", speaks.app, []() {
                            int d = device();
                            askSaveName(savename::SETUP, d, [d](const std::string& name) { return jobs::keep(d, name); });
                          }});
      }
      p.rows.push_back({"Setups", "load, export", []() { push(setupsPage()); }});
      if (speaks.sloop) {
        p.rows.push_back({"SLOOP", speaks.version, []() { push(sloopPage()); }});
      }
    };
    other = [](char c, ListPage& p) {
      if (c == 'p' && device() >= 0 && !jobs::probe(device())) {
        busy();
      }
      return c == 'p';
    };
    done = [](ListPage&) {
      if (jobs::kind == jobs::PROBE) {
        speaks = jobs::speaks;
      } else if (jobs::ok && jobs::kind == jobs::VOICE) {
        toast("Saved as " SYX_DIR "/" + jobs::saved);
      } else if (jobs::ok && jobs::kind == jobs::KEEP) {
        toast("Saved as " SETUPS_DIR "/" + jobs::saved);
      }
    };
    refill();
  }
};

Page* ui::soundsPage() {
  return new SoundsPage();
}

// ----------------------------------------------------------------------------------------------- SLOOP ---
static Page* sloopFiles(bool songs) {
  ListPage* p = new ListPage(songs ? "SLOOP: songs" : "SLOOP: backups",
                             songs ? "enter load  r name  esc back" : "enter restore  0 a-d  r name  esc back");
  p->empty = songs ? "No songs in " SLOOP_SONGS_DIR "." : "No backups in " SLOOP_DIR ".";
  p->fill = [songs](ListPage& p) {
    for (const std::string& f : songs ? sloop::songs() : sloop::backups()) {
      p.rows.push_back({f, "", [f, songs]() {
                          ask(songs ? "Load song" : "Restore",
                              songs ? "Load " + f + " onto SLOOP? Its song is saved first."
                                    : "Put all of " + f + " back on SLOOP? It's backed up first.",
                              [f, songs]() {
                                int d = device();
                                if (d < 0 || !(songs ? jobs::sloopSongIn(d, f) : jobs::sloopRestore(d, f))) {
                                  busy();
                                }
                              });
                        },
                        f});
    }
  };
  if (songs) {
    p->other = [](char c, ListPage& p) { return renameKey(c, p, SLOOP_SONGS_DIR); };
  } else {  // one project: 0 the working one, a-d sections A-D
    p->other = [](char c, ListPage& p) {
      if (c == 'r') {
        return renameKey(c, p, SLOOP_DIR);
      }
      int id = c == '0' ? 0 : c >= 'a' && c <= 'd' ? 2 + c - 'a'
                                                   : -1;
      if (id >= 0 && !p.rows.empty() && !jobs::sloopProject(device(), p.rows[p.sel].name, id, id)) {
        busy();
      }
      return id >= 0;
    };
  }
  p->done = [](ListPage&) {
    if (jobs::ok) {
      toast("Done.");
    }
  };
  p->refill();
  return p;
}

static Page* sloopSamples() {
  ListPage* p = new ListPage("SLOOP: samples", "1-4 into that slot  esc back");
  p->empty = "No .wav files or folders of them in " SLOOP_DIR "/samples.";
  p->fill = [](ListPage& p) {
    for (const std::string& s : sloop::sampleSources()) {
      p.rows.push_back({s, "", 0});
    }
  };
  p->other = [](char c, ListPage& p) {
    if (c >= '1' && c <= '4' && !p.rows.empty() && !jobs::sloopSamples(device(), p.rows[p.sel].name, c - '1')) {
      busy();
    }
    return c >= '1' && c <= '4';
  };
  p->done = [](ListPage&) {
    if (jobs::ok) {
      toast("In the slot.");
    }
  };
  p->refill();
  return p;
}

static Page* sloopPage() {
  ListPage* p = new ListPage("SLOOP", "esc back");
  p->fill = [](ListPage& p) {
    p.rows.push_back({"Back it up", SLOOP_DIR, []() {
                        int d = device();
                        askSaveName(savename::SLOOP_BACKUP, d,
                                    [d](const std::string& name) { return jobs::sloopBackup(d, name); });
                      }});
    p.rows.push_back({"Save its song", SLOOP_SONGS_DIR, []() {
                        int d = device();
                        askSaveName(savename::SLOOP_SONG, d,
                                    [d](const std::string& name) { return jobs::sloopSong(d, name); });
                      }});
    p.rows.push_back({"Backups", "restore, projects", []() { push(sloopFiles(false)); }});
    p.rows.push_back({"Songs", "load", []() { push(sloopFiles(true)); }});
    p.rows.push_back({"Samples", "into a slot", []() { push(sloopSamples()); }});
  };
  p->done = [](ListPage&) {
    if (jobs::ok && (jobs::kind == jobs::SLOOP_BACKUP || jobs::kind == jobs::SLOOP_SONG)) {
      toast("Saved as " + jobs::saved);
    }
  };
  p->refill();
  return p;
}

// ------------------------------------------------------------------------------------------------ MIDI ---
static void saveFlag(const char* key, bool on) {
  hal::save(key, &on, 1);
}

struct MidiPage : ListPage {
  MidiPage() : ListPage("MIDI", "r routing  b bluetooth  k keys  esc back") {
    fill = [](ListPage& p) {
      midi::Dev v;
      for (int d = 0; d < MIDI_DEVS; d++) {
        if (!midi::dev(d, v)) {
          continue;
        }
        char last[32] = "";
        if (v.count) {
          midi::describe(v.last, last, sizeof last);
        }
        p.rows.push_back({v.name, last, 0});
      }
      p.empty = "Nothing on the USB-C port: a powered hub, with the FM-1 and controllers on it.";
    };
    other = [](char c, ListPage& p) {
      if (c == 'r') {
        midi::route(!midi::routing);
        saveFlag("routing", midi::routing);
      } else if (c == 'b') {
        hal::bleOn(!hal::bleIsOn());
        saveFlag("bluetooth", hal::bleIsOn());
      } else if (c == 'k') {
        push(keysPage());
      }
      return c == 'r' || c == 'b' || c == 'k';
    };
    refill();
  }
  void tick() override {
    ListPage::tick();
    static uint32_t t;
    if (hal::millis() - t > 300) {  // (what each sent last)
      t = hal::millis();
      refill();
    }
  }
  void draw() override {
    char s[48];
    snprintf(s, sizeof s, "routing %s   bluetooth %s", midi::routing ? "on" : "off", hal::bleIsOn() ? "on" : "off");
    text(6, TOP + 2, s, midi::routing ? GOOD : WARN);
    // the devices under it
    if (rows.empty()) {
      wrap(6, TOP + 16, SCREEN_W - 12, empty.c_str(), DIM, 6);
      return;
    }
    listRows(rows.size(), sel, scroll, TOP + 14, 8, [](int i, int y, bool s, void* arg) {
      ListPage* p = (ListPage*)arg;
      text(8, y + 2, p->rows[i].name.c_str(), s ? TEXT : DIM, 110);
      textRight(SCREEN_W - 6, y + 2, p->rows[i].note.c_str(), s ? DIM : OFF); }, this);
  }
};

Page* ui::midiPage() {
  return new MidiPage();
}

// ------------------------------------------------------------------------------------- the keyboard ---
// The keyboard as a MIDI controller (device MIDI_TOUCH, "Keys"): two rows, as trackers have it. z s x d c v g b h n j m
// , l . ; / play from C, q 2 w 3 e r 5 t 6 y 7 u i 9 o 0 p an octave up. - = change the octave, [ ] the channel.
// Notes go wherever MIDI routing sends them (it's turned on here)
static const char LOWER[] = "zsxdcvgbhnjm,l.;/";
static const char UPPER[] = "q2w3er5t6y7ui9o0p";

struct KeysPage : Page {
  int octave = 4, channel = 0;
  bool down[128] = {};
  const char* title() override { return "Keys"; }
  const char* hints() override { return "- = octave  [ ] channel  esc back"; }
  bool typing() override { return false; }  // (esc goes back; the notes come from held(), every key as it is)
  bool raw() override { return true; }
  KeysPage() {
    midi::route(true);
  }
  ~KeysPage() {
    allOff();
  }
  int noteOf(char c) {
    const char* l = strchr(LOWER, c);
    const char* u = strchr(UPPER, c);
    int n = l ? l - LOWER : u ? 12 + (u - UPPER)
                              : -1;
    return n < 0 || !c ? -1 : std::min(127, (octave + 1) * 12 + n);
  }
  void send(int note, bool on) {
    uint8_t m[3] = {(uint8_t)((on ? 0x90 : 0x80) | channel), (uint8_t)note, (uint8_t)(on ? 100 : 0)};
    midi::bytesIn(MIDI_TOUCH, m, 3);
    down[note] = on;
  }
  void allOff() {
    for (int n = 0; n < 128; n++) {
      if (down[n]) {
        send(n, false);
      }
    }
  }
  // the keys held now: notes start and stop as they change
  void held(const std::vector<char>& keys) override {
    bool now[128] = {};
    for (char c : keys) {
      int n = noteOf(c);
      if (n >= 0) {
        now[n] = true;
      }
    }
    for (int n = 0; n < 128; n++) {
      if (now[n] != down[n]) {
        send(n, now[n]);
      }
    }
    redraw();
  }
  void key(const Key& k) override {
    if (k.ch == '-' || k.ch == '=') {
      allOff();
      octave = std::max(0, std::min(8, octave + (k.ch == '=' ? 1 : -1)));
    } else if (k.ch == '[' || k.ch == ']') {
      allOff();
      channel = (channel + (k.ch == ']' ? 1 : 15)) % 16;
    }
  }
  void draw() override {
    char s[48];
    snprintf(s, sizeof s, "octave %d   channel %d   %s", octave, channel + 1, midi::routing ? "" : "(routing off)");
    text(6, TOP + 2, s, ACCENT);
    // two octaves of keys, lit while held
    static const bool black[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
    int base = (octave + 1) * 12, x = 6, w = 16, y = TOP + 20;
    for (int i = 0, wi = 0; i < 24; i++) {
      if (!black[i % 12]) {
        fill(x + wi * w, y, w - 1, 70, base + i < 128 && down[base + i] ? ACCENT : TEXT);
        wi++;
      }
    }
    for (int i = 0, wi = 0; i < 24; i++) {
      if (black[i % 12]) {
        fill(x + wi * w - 5, y, 10, 42, base + i < 128 && down[base + i] ? ACCENT : BG);
      } else {
        wi++;
      }
    }
  }
};

Page* ui::keysPage() {
  return new KeysPage();
}
