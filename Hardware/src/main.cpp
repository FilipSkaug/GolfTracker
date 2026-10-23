// GolfTracker — score keeping on the CrowPanel 1.28" rotary display.
//
//   Home:    Resume round (if any) / Select course / My rounds / Phone sync.
//   Rounds:  newest first; press for the scorecard, turn there to flip rounds.
//   Courses: turn through all courses, press to pick, hold to go back.
//   Holes:   18 holes, front 9 or back 9 (a 9-hole course: 9 holes, or 18 = twice round).
//   Hole:    turn to set strokes, press to save and move to the next hole.
//            Hold for the menu (previous hole, finish early, discard).
//   Summary: shown after the last hole; press to sync rounds to the phone.

#include <Adafruit_NeoPixel.h>
#include <Arduino.h>

#include <vector>

#include "config.h"
#include "input.h"
#include "storage.h"
#include "ui.h"
#include "upload.h"
#include "web.h"

enum class Screen { Home, CourseList, HoleSelect, Hole, HoleMenu, Confirm, Summary, Sync, RoundList, RoundView };
enum class ConfirmKind { DiscardRound, ReplaceRound };
enum class HomeItem : uint8_t { Resume, Courses, Rounds, Sync };
enum class Variant : uint8_t { Full, Front9, Back9, Twice };

struct HoleOption {
  Variant variant;
  const char* label;
};

static Adafruit_NeoPixel leds(LED_COUNT, PIN_LED, NEO_GRB + NEO_KHZ800);

static Screen screen = Screen::Home;
static Round active;    // round in progress
static Round finished;  // last completed round, for the summary
static uint8_t wheel = 0;  // strokes shown for the current hole
static int homeSel = 0, courseSel = 0, holeSel = 0, menuSel = 0, confirmSel = 0, roundSel = 0;
static std::vector<Round> rounds;  // loaded from history while browsing "My rounds"
static int pendingCourse = -1;
static Variant pendingVariant = Variant::Full;
static ConfirmKind confirmKind = ConfirmKind::DiscardRound;
static bool dirty = true;
static bool dimmed = false;
static uint32_t lastInputAt = 0;
static bool lastPhoneConnected = false;
static String lastHomeIp;
static String uploadStatus;  // GolfBox upload result for this sync session
static bool uploadDone = false;

static const char* const kHoleMenu[] = {"Continue", "Previous hole", "Finish round", "Discard round"};
static const char* const kConfirm[] = {"No", "Yes"};

static int wrap(int v, int n) { return n <= 0 ? 0 : ((v % n) + n) % n; }
static int clampIndex(int v, int n) { return n <= 0 ? 0 : constrain(v, 0, n - 1); }

static std::vector<HomeItem> homeItems() {
  std::vector<HomeItem> items;
  if (active.active) items.push_back(HomeItem::Resume);
  items.push_back(HomeItem::Courses);
  items.push_back(HomeItem::Rounds);
  items.push_back(HomeItem::Sync);
  return items;
}

static std::vector<HoleOption> holeOptions(const Course& c) {
  if (c.holes == 18) return {{Variant::Full, "18 holes"}, {Variant::Front9, "Front 9"}, {Variant::Back9, "Back 9"}};
  if (c.holes == 9) return {{Variant::Full, "9 holes"}, {Variant::Twice, "18 holes (2 x 9)"}};
  return {{Variant::Full, "Play all holes"}};
}

static void setLeds(uint32_t rgb) {
  for (int i = 0; i < LED_COUNT; i++) leds.setPixelColor(i, rgb);
  leds.show();
}

static void go(Screen s) {
  screen = s;
  dirty = true;
  if (s != Screen::Hole) setLeds(s == Screen::Sync ? 0x0040A0 : 0);
}

static void goHome() {
  homeSel = 0;
  rounds.clear();
  rounds.shrink_to_fit();
  go(Screen::Home);
}

static void openHole() {
  wheel = active.strokes[active.hole] ? active.strokes[active.hole] : active.par[active.hole];
  go(Screen::Hole);
}

static void startRound(int courseIndex, Variant v) {
  const Course& c = storage::courses()[courseIndex];
  active = Round();
  active.active = true;
  active.course = c.name;
  switch (v) {
    case Variant::Full:
      active.holes = c.holes;
      memcpy(active.par, c.par, c.holes);
      break;
    case Variant::Front9:
      active.holes = 9;
      memcpy(active.par, c.par, 9);
      active.course += " (front 9)";
      break;
    case Variant::Back9:
      active.holes = 9;
      memcpy(active.par, c.par + 9, 9);
      active.firstHole = 10;
      active.course += " (back 9)";
      break;
    case Variant::Twice:
      active.holes = c.holes * 2;
      memcpy(active.par, c.par, c.holes);
      memcpy(active.par + c.holes, c.par, c.holes);
      active.course += " (18)";
      break;
  }
  storage::saveRound(active);
  openHole();
}

static void discardRound() {
  active = Round();
  storage::clearRound();
}

static void finishRound() {
  if (totals(active).played == 0) {
    discardRound();
    goHome();
    return;
  }
  storage::appendHistory(active);
  finished = active;
  discardRound();
  go(Screen::Summary);
}

static void startSync() {
  web::start();
  lastPhoneConnected = false;
  lastHomeIp = "";
  uploadStatus = "";
  uploadDone = false;
  go(Screen::Sync);
}

static void askConfirm(ConfirmKind kind) {
  confirmKind = kind;
  confirmSel = 0;
  go(Screen::Confirm);
}

// ---- Input handling per screen --------------------------------------------------

static void onHome(int steps, input::Button btn) {
  auto items = homeItems();
  if (steps) {
    homeSel = wrap(homeSel + steps, items.size());
    dirty = true;
  }
  if (btn != input::Button::Short) return;
  switch (items[wrap(homeSel, items.size())]) {
    case HomeItem::Resume:
      openHole();
      break;
    case HomeItem::Courses:
      courseSel = clampIndex(courseSel, storage::courses().size());
      go(Screen::CourseList);
      break;
    case HomeItem::Rounds:
      rounds = storage::history();
      roundSel = 0;
      go(Screen::RoundList);
      break;
    case HomeItem::Sync:
      startSync();
      break;
  }
}

static void onCourseList(int steps, input::Button btn) {
  int n = storage::courses().size();
  if (steps) {
    // Stops at the ends instead of wrapping, like scrolling a list.
    courseSel = clampIndex(courseSel + steps, n);
    dirty = true;
  }
  if (btn == input::Button::Long || (btn == input::Button::Short && n == 0)) {
    goHome();
  } else if (btn == input::Button::Short) {
    holeSel = 0;
    go(Screen::HoleSelect);
  }
}

static void onRoundList(int steps, input::Button btn) {
  int n = rounds.size();
  if (steps) {
    roundSel = clampIndex(roundSel + steps, n);
    dirty = true;
  }
  if (btn == input::Button::Long || (btn == input::Button::Short && n == 0)) goHome();
  else if (btn == input::Button::Short) go(Screen::RoundView);
}

// Turning flips through rounds; press goes back to the list, hold goes home.
static void onRoundView(int steps, input::Button btn) {
  if (steps) {
    int next = clampIndex(roundSel + steps, rounds.size());
    if (next != roundSel) {
      roundSel = next;
      dirty = true;
    }
  }
  if (btn == input::Button::Short) go(Screen::RoundList);
  else if (btn == input::Button::Long) goHome();
}

static void onHoleSelect(int steps, input::Button btn) {
  auto options = holeOptions(storage::courses()[courseSel]);
  if (steps) {
    holeSel = wrap(holeSel + steps, options.size());
    dirty = true;
  }
  if (btn == input::Button::Long) {
    go(Screen::CourseList);
  } else if (btn == input::Button::Short) {
    Variant v = options[holeSel].variant;
    if (active.active) {
      pendingCourse = courseSel;
      pendingVariant = v;
      askConfirm(ConfirmKind::ReplaceRound);
    } else {
      startRound(courseSel, v);
    }
  }
}

static void onHole(int steps, input::Button btn) {
  if (steps) {
    wheel = constrain((int)wheel + steps, 1, MAX_STROKES);
    dirty = true;
  }
  if (btn == input::Button::Long) {
    menuSel = 0;
    go(Screen::HoleMenu);
  } else if (btn == input::Button::Short) {
    active.strokes[active.hole] = wheel;
    if (active.hole + 1 >= active.holes) {
      finishRound();
    } else {
      active.hole++;
      storage::saveRound(active);
      openHole();
    }
  }
}

static void onHoleMenu(int steps, input::Button btn) {
  if (steps) {
    menuSel = wrap(menuSel + steps, 4);
    dirty = true;
  }
  if (btn == input::Button::Long) go(Screen::Hole);
  if (btn != input::Button::Short) return;
  switch (menuSel) {
    case 0:
      go(Screen::Hole);
      break;
    case 1:
      if (active.hole > 0) {
        active.hole--;
        storage::saveRound(active);
        openHole();
      } else {
        go(Screen::Hole);
      }
      break;
    case 2:
      finishRound();
      break;
    case 3:
      askConfirm(ConfirmKind::DiscardRound);
      break;
  }
}

static void onConfirm(int steps, input::Button btn) {
  if (steps) {
    confirmSel = wrap(confirmSel + steps, 2);
    dirty = true;
  }
  if (btn != input::Button::Short) return;
  bool yes = confirmSel == 1;
  if (confirmKind == ConfirmKind::DiscardRound) {
    if (yes) {
      discardRound();
      goHome();
    } else {
      go(Screen::Hole);
    }
  } else {
    if (yes) {
      discardRound();
      startRound(pendingCourse, pendingVariant);
    } else {
      go(Screen::HoleSelect);
    }
  }
}

static void onSummary(int, input::Button btn) {
  if (btn == input::Button::Short) startSync();
  else if (btn == input::Button::Long) goHome();
}

static void onSync(int, input::Button btn) {
  if (btn == input::Button::None) return;
  web::stop();
  goHome();
}

// ---- Rendering ------------------------------------------------------------------

static String toParText(int diff) {
  if (diff == 0) return "E";
  return (diff > 0 ? "+" : "") + String(diff);
}

static void render() {
  switch (screen) {
    case Screen::Home: {
      auto items = homeItems();
      homeSel = wrap(homeSel, items.size());
      switch (items[homeSel]) {
        case HomeItem::Resume: {
          Totals t = totals(active);
          String sub = "Hole " + String(active.firstHole + active.hole);
          if (t.played) sub += "  " + toParText(t.strokes - t.par);
          ui::drawHome("RESUME ROUND", active.course, sub, homeSel, items.size());
          break;
        }
        case HomeItem::Courses:
          ui::drawHome("PLAY", "Select course", String(storage::courses().size()) + " courses", homeSel,
                       items.size());
          break;
        case HomeItem::Rounds:
          ui::drawHome("HISTORY", "My rounds", "Scorecards", homeSel, items.size());
          break;
        case HomeItem::Sync:
          ui::drawHome("PHONE SYNC", "Sync", "Courses & scores", homeSel, items.size());
          break;
      }
      break;
    }
    case Screen::RoundList: {
      if (rounds.empty()) {
        const char* const none[] = {"Back"};
        ui::drawMenu("NO ROUNDS YET", none, 1, 0);
        break;
      }
      std::vector<String> labels;
      for (const Round& r : rounds) {
        Totals t = totals(r);
        labels.push_back(String(t.strokes) + " (" + toParText(t.strokes - t.par) + ")  " + r.course);
      }
      ui::drawList("MY ROUNDS", labels, roundSel);
      break;
    }
    case Screen::RoundView:
      ui::drawRound(rounds[roundSel], roundSel, rounds.size());
      break;
    case Screen::CourseList: {
      std::vector<String> names;
      for (const Course& c : storage::courses()) names.push_back(c.name);
      if (names.empty()) {
        const char* const none[] = {"Add via Phone sync"};
        ui::drawMenu("NO COURSES", none, 1, 0);
      } else {
        courseSel = clampIndex(courseSel, names.size());
        ui::drawList("SELECT COURSE", names, courseSel);
      }
      break;
    }
    case Screen::HoleSelect: {
      auto options = holeOptions(storage::courses()[courseSel]);
      std::vector<const char*> labels;
      for (const HoleOption& o : options) labels.push_back(o.label);
      ui::drawMenu(storage::courses()[courseSel].name, labels.data(), labels.size(), holeSel);
      break;
    }
    case Screen::Hole:
      ui::drawHole(active, wheel);
      setLeds(ui::scoreColor(wheel - active.par[active.hole]));
      break;
    case Screen::HoleMenu:
      ui::drawMenu("HOLE " + String(active.firstHole + active.hole), kHoleMenu, 4, menuSel);
      break;
    case Screen::Confirm:
      ui::drawMenu(confirmKind == ConfirmKind::DiscardRound ? "DISCARD ROUND?" : "REPLACE ROUND?", kConfirm, 2,
                   confirmSel);
      break;
    case Screen::Summary:
      ui::drawSummary(finished);
      break;
    case Screen::Sync:
      ui::drawSync(lastPhoneConnected, lastHomeIp, uploadStatus);
      break;
  }
}

// Debug helpers over USB serial: "sync" starts phone sync, "home" stops it,
// "ghtest" checks the HTTPS connection to GitHub (expects 401).
static void handleSerial() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') {
      if (line.length() < 32) line += c;
      continue;
    }
    line.trim();
    if (line == "sync" && screen != Screen::Sync) startSync();
    else if (line == "home") onSync(0, input::Button::Short);
    else if (line == "ghtest") Serial.printf("ghtest: HTTP %d, free heap %u\n", upload::selfTest(), ESP.getFreeHeap());
    line = "";
  }
}

void setup() {
  Serial.begin(115200);
  ui::begin();
  input::begin();
  leds.begin();
  leds.setBrightness(LED_BRIGHTNESS);
  setLeds(0);
  storage::begin();
  if (storage::loadRound(active)) Serial.println("Resuming saved round");
  lastInputAt = millis();
  goHome();
}

void loop() {
  handleSerial();
  int steps = input::takeSteps();
  input::Button btn = input::takeButton();
  uint32_t now = millis();

  if (steps || btn != input::Button::None) {
    lastInputAt = now;
    if (dimmed) {
      // The first touch only wakes the screen.
      dimmed = false;
      ui::setBrightness(BRIGHTNESS_ON);
      steps = 0;
      btn = input::Button::None;
    }
  } else if (!dimmed && now - lastInputAt > DIM_AFTER_MS) {
    dimmed = true;
    ui::setBrightness(BRIGHTNESS_DIM);
  }

  switch (screen) {
    case Screen::Home: onHome(steps, btn); break;
    case Screen::CourseList: onCourseList(steps, btn); break;
    case Screen::HoleSelect: onHoleSelect(steps, btn); break;
    case Screen::Hole: onHole(steps, btn); break;
    case Screen::HoleMenu: onHoleMenu(steps, btn); break;
    case Screen::Confirm: onConfirm(steps, btn); break;
    case Screen::Summary: onSummary(steps, btn); break;
    case Screen::Sync: onSync(steps, btn); break;
    case Screen::RoundList: onRoundList(steps, btn); break;
    case Screen::RoundView: onRoundView(steps, btn); break;
  }

  if (screen == Screen::Sync) {
    web::handle();
    bool connected = web::phoneConnected();
    String ip = web::homeIp();
    if (connected != lastPhoneConnected || ip != lastHomeIp) {
      lastPhoneConnected = connected;
      lastHomeIp = ip;
      dirty = true;
    }
    // On the internet: hand unsent rounds to the GolfBox workflow (once per sync).
    if (!uploadDone && !ip.isEmpty() && upload::configured()) {
      uploadDone = true;
      uploadStatus = "GolfBox: sending...";
      render();
      uploadStatus = upload::sendPending();
      Serial.println("Upload: " + uploadStatus);
      dirty = true;
    }
  }

  if (dirty) {
    dirty = false;
    render();
  }
  delay(2);
}
