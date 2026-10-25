// GolfTracker — score keeping on the CrowPanel 1.28" rotary display.
//
//   Home:    Resume round (if any) / Select course / My rounds / Phone sync.
//   Sync:    choose Hotspot (internet, GolfBox upload) or GolfTracker Wi-Fi.
//   Rounds:  newest first; press for the scorecard, turn there to flip rounds.
//   Courses: turn through all courses, press to pick, hold to go back.
//   Holes:   18 holes, front 9 or back 9 (a 9-hole course: 9 holes, or 18 = twice round).
//   Hole:    turn to set strokes, press to save and move to the next hole.
//            Hold for the menu (previous hole, finish early, discard).
//   Summary: shown after the last hole; press -> "Send to GolfBox?" (send now /
//            later / don't send) when the GolfBox upload is set up.

#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <WiFi.h>

#include <vector>

#include "config.h"
#include "gamebook.h"
#include "input.h"
#include "live.h"
#include "storage.h"
#include "ui.h"
#include "upload.h"
#include "web.h"

enum class Screen { Home, CourseList, HoleSelect, GameBookAsk, Hole, HoleMenu, Confirm, Summary, GolfBoxAsk, SyncSelect, Sync, RoundList, RoundView };
enum class ConfirmKind { DiscardRound, ReplaceRound };
enum class HomeItem : uint8_t { Resume, Courses, Rounds, Sync, Sleep };
enum class Variant : uint8_t { Full, Front9, Back9, Twice };

struct HoleOption {
  Variant variant;
  const char* label;
  const char* badge;  // big text in the dial
};

// The smooth score font is drawn via a per-glyph stack buffer (LovyanGFX allocas
// width x height bytes, ~8 KB for a big digit), more than the default 8 KB stack.
SET_LOOP_TASK_STACK_SIZE(24 * 1024);

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
static web::HomeState lastHomeState = web::HomeState::Off;
static int syncSel = 0;
static int golfboxSel = 0;
static int gameBookSel = 0;
static bool pendingTrack = false;  // "Track on GameBook" chosen for the round being started
static live::Status lastLiveStatus = live::Status::Off;
static const ui::DialItem kGameBookMenu[] = {{"Track on GameBook", ui::Icon::Upload, nullptr},
                                             {"Don't track", ui::Icon::Cross, nullptr}};
static const ui::DialItem kGolfBoxMenu[] = {
    {"Send now", ui::Icon::Upload, nullptr}, {"Send later", ui::Icon::Clock, nullptr}, {"Don't send", ui::Icon::Cross, nullptr}};
static const ui::DialItem kSyncMenu[] = {{"Hotspot", ui::Icon::Hotspot, nullptr},
                                         {"GolfTracker Wi-Fi", ui::Icon::Wifi, nullptr}};

static uint32_t syncLedColor(web::HomeState s) {
  switch (s) {
    case web::HomeState::Joining: return 0x604000;  // yellow
    case web::HomeState::Online: return 0x006010;   // green
    case web::HomeState::Failed: return 0x602000;   // orange
    default: return 0x0040A0;                       // blue: GolfTracker Wi-Fi mode
  }
}

static const ui::DialItem kHoleMenu[] = {{"Continue", ui::Icon::Play, nullptr},
                                         {"Previous hole", ui::Icon::Back, nullptr},
                                         {"Main menu", ui::Icon::Home, nullptr},
                                         {"Finish round", ui::Icon::Flag, nullptr},
                                         {"Discard round", ui::Icon::Trash, nullptr}};
static const int kHoleMenuCount = sizeof(kHoleMenu) / sizeof(kHoleMenu[0]);
static const ui::DialItem kConfirm[] = {{"No", ui::Icon::Cross, nullptr}, {"Yes", ui::Icon::Check, nullptr}};

static int wrap(int v, int n) { return n <= 0 ? 0 : ((v % n) + n) % n; }
static int clampIndex(int v, int n) { return n <= 0 ? 0 : constrain(v, 0, n - 1); }

static std::vector<HomeItem> homeItems() {
  std::vector<HomeItem> items;
  if (active.active) items.push_back(HomeItem::Resume);
  items.push_back(HomeItem::Courses);
  items.push_back(HomeItem::Rounds);
  items.push_back(HomeItem::Sync);
  items.push_back(HomeItem::Sleep);
  return items;
}

static std::vector<HoleOption> holeOptions(const Course& c) {
  if (c.holes == 18) {
    return {{Variant::Full, "18 holes", "18"}, {Variant::Front9, "Front 9", "1-9"}, {Variant::Back9, "Back 9", "10-18"}};
  }
  if (c.holes == 9) return {{Variant::Full, "9 holes", "9"}, {Variant::Twice, "18 holes (2 x 9)", "18"}};
  return {{Variant::Full, "Play all holes", "ALL"}};
}

static void setLeds(uint32_t rgb) {
  for (int i = 0; i < LED_COUNT; i++) leds.setPixelColor(i, rgb);
  leds.show();
}

static void go(Screen s) {
  screen = s;
  dirty = true;
  if (s != Screen::Hole) setLeds(s == Screen::Sync ? syncLedColor(lastHomeState) : 0);
}

static void goHome() {
  homeSel = 0;
  rounds.clear();
  rounds.shrink_to_fit();
  go(Screen::Home);
}

// Light sleep until the knob is pressed: screen, LEDs and Wi-Fi off, CPU paused,
// everything kept in RAM. (The knob's pin can't wake the chip from deep sleep.)
// testWakeMs > 0 also wakes on a timer, for the "sleeptest" serial command.
static void sleepNow(uint32_t testWakeMs = 0) {
  Serial.println("Sleeping - press the knob to wake");
  Serial.flush();
  if (screen == Screen::Sync) {
    web::stop();
    goHome();
  }
  setLeds(0);
  live::suspend();
  ui::sleep();
  while (digitalRead(PIN_BTN) == LOW) delay(10);  // don't wake straight away
  delay(50);
  gpio_wakeup_enable((gpio_num_t)PIN_BTN, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  if (testWakeMs) esp_sleep_enable_timer_wakeup(testWakeMs * 1000ULL);
  esp_light_sleep_start();
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  gpio_wakeup_disable((gpio_num_t)PIN_BTN);

  ui::wake();
  while (digitalRead(PIN_BTN) == LOW) delay(10);  // the waking press does nothing else
  input::reset();
  live::kick();  // resume live tracking
  dimmed = false;
  lastInputAt = millis();
  dirty = true;
  Serial.println("Awake");
}

static void openHole() {
  wheel = active.strokes[active.hole] ? active.strokes[active.hole] : active.par[active.hole];
  go(Screen::Hole);
}

static void startRound(int courseIndex, Variant v, bool track) {
  const Course& c = storage::courses()[courseIndex];
  active = Round();
  active.active = true;
  active.course = c.name;
  if (track) {
    active.gb.track = true;
    active.gb.courseId = c.gbCourseId;
    active.gb.teeMen = c.gbTeeboxId;
  }
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
  live::kick();  // create the GameBook game as soon as there's a connection
  openHole();
}

// Clears the round in progress (it's been stored in history, or is being discarded).
static void clearActive() {
  active = Round();
  storage::clearRound();
}

// The user threw the round away: also delete its GameBook game.
static void discardRound() {
  live::discard(active);
  active = Round();
  storage::clearRound();
}

static void finishRound() {
  if (totals(active).played == 0) {
    discardRound();
    goHome();
    return;
  }
  finished = active;
  finished.id = storage::appendHistory(active);
  live::kick();  // send the last holes and close the GameBook game
  clearActive();
  go(Screen::Summary);
}

// Hotspot falls back to GolfTracker Wi-Fi when no hotspot is saved.
static void startSync(web::Mode mode) {
  web::start(mode);
  lastPhoneConnected = false;
  lastHomeIp = "";
  uploadStatus = "";
  uploadDone = false;
  lastHomeState = web::homeState();
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
    case HomeItem::Sleep:
      sleepNow();
      break;
    case HomeItem::Sync:
      if (web::hotspotConfigured()) {
        syncSel = 0;
        go(Screen::SyncSelect);
      } else {
        startSync(web::Mode::Wifi);
      }
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

// Starts the chosen course/holes, asking first if it would replace a round in progress.
static void beginRound(bool track) {
  pendingTrack = track;
  if (active.active) askConfirm(ConfirmKind::ReplaceRound);
  else startRound(pendingCourse, pendingVariant, track);
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
    pendingCourse = courseSel;
    pendingVariant = options[holeSel].variant;
    // Offer GameBook only when this course is linked and the login is saved.
    if (storage::courses()[courseSel].gbCourseId && gamebook::configured()) {
      gameBookSel = 0;
      go(Screen::GameBookAsk);
    } else {
      beginRound(false);
    }
  }
}

// "Track on GameBook?" after choosing 9 / 18 holes.
static void onGameBookAsk(int steps, input::Button btn) {
  if (steps) {
    gameBookSel = wrap(gameBookSel + steps, 2);
    dirty = true;
  }
  if (btn == input::Button::Long) go(Screen::HoleSelect);
  else if (btn == input::Button::Short) beginRound(gameBookSel == 0);
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
      live::kick();  // send this hole to GameBook
      openHole();
    }
  }
}

static void onHoleMenu(int steps, input::Button btn) {
  if (steps) {
    menuSel = wrap(menuSel + steps, kHoleMenuCount);
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
    case 2:  // the round stays saved: "Resume" on the start screen returns to it
      goHome();
      break;
    case 3:
      finishRound();
      break;
    case 4:
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
      startRound(pendingCourse, pendingVariant, pendingTrack);
    } else {
      go(Screen::HoleSelect);
    }
  }
}

static void onSummary(int, input::Button btn) {
  if (btn == input::Button::Long) {
    goHome();
  } else if (btn == input::Button::Short) {
    if (upload::configured()) {
      golfboxSel = 0;
      go(Screen::GolfBoxAsk);
    } else {
      startSync(web::Mode::Hotspot);
    }
  }
}

// After a round: send it to GolfBox now, with the next hotspot sync, or never.
static void onGolfBoxAsk(int steps, input::Button btn) {
  if (steps) {
    golfboxSel = wrap(golfboxSel + steps, 3);
    dirty = true;
  }
  if (btn == input::Button::Long) {
    go(Screen::Summary);
  } else if (btn == input::Button::Short) {
    switch (golfboxSel) {
      case 0: startSync(web::Mode::Hotspot); break;
      case 1: goHome(); break;
      case 2:
        storage::markSkip(finished.id);
        goHome();
        break;
    }
  }
}

static void onSyncSelect(int steps, input::Button btn) {
  if (steps) {
    syncSel = wrap(syncSel + steps, 2);
    dirty = true;
  }
  if (btn == input::Button::Long) goHome();
  else if (btn == input::Button::Short) startSync(syncSel == 0 ? web::Mode::Hotspot : web::Mode::Wifi);
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

// Dot on the hole screen: GameBook up to date (green), sending/connecting (yellow),
// error (red); none when the round isn't tracked.
static uint32_t liveDotColor() {
  if (!active.gb.track) return 0;
  switch (live::status()) {
    case live::Status::Synced: return 0x4ADE80;
    case live::Status::Error: return 0xF43F5E;
    default: return 0xFACC15;
  }
}

static void render() {
  switch (screen) {
    case Screen::Home: {
      auto items = homeItems();
      homeSel = wrap(homeSel, items.size());
      std::vector<ui::DialItem> dial;
      for (HomeItem it : items) {
        switch (it) {
          case HomeItem::Resume: dial.push_back({"Resume", ui::Icon::Play, nullptr}); break;
          case HomeItem::Courses: dial.push_back({"Play", ui::Icon::Flag, nullptr}); break;
          case HomeItem::Rounds: dial.push_back({"My rounds", ui::Icon::Clock, nullptr}); break;
          case HomeItem::Sync: dial.push_back({"Phone sync", ui::Icon::Phone, nullptr}); break;
          case HomeItem::Sleep: dial.push_back({"Sleep", ui::Icon::Power, nullptr}); break;
        }
      }
      String label, sub;
      switch (items[homeSel]) {
        case HomeItem::Resume: {
          Totals t = totals(active);
          label = active.course;
          sub = "Resume - hole " + String(active.firstHole + active.hole);
          if (t.played) sub += "  " + toParText(t.strokes - t.par);
          break;
        }
        case HomeItem::Courses: sub = String(storage::courses().size()) + " courses"; break;
        case HomeItem::Rounds: sub = "Scorecards"; break;
        case HomeItem::Sync: sub = web::hotspotConfigured() ? "Hotspot or Wi-Fi" : "Courses & scores"; break;
        case HomeItem::Sleep: sub = "Press the knob to wake"; break;
      }
      ui::drawDial("GOLFTRACKER", dial.data(), dial.size(), homeSel, label, sub);
      break;
    }
    case Screen::RoundList: {
      if (rounds.empty()) {
        const ui::DialItem none[] = {{"No rounds yet", ui::Icon::Clock, nullptr}};
        ui::drawDial("MY ROUNDS", none, 1, 0, "", "Press to go back");
        break;
      }
      std::vector<String> labels;
      for (const Round& r : rounds) {
        Totals t = totals(r);
        labels.push_back(String(t.strokes) + "  " + r.course);
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
        const ui::DialItem none[] = {{"No courses", ui::Icon::Flag, nullptr}};
        ui::drawDial("SELECT COURSE", none, 1, 0, "", "Add them via Phone sync");
      } else {
        courseSel = clampIndex(courseSel, names.size());
        ui::drawList("SELECT COURSE", names, courseSel);
      }
      break;
    }
    case Screen::HoleSelect: {
      auto options = holeOptions(storage::courses()[courseSel]);
      std::vector<ui::DialItem> dial;
      for (const HoleOption& o : options) dial.push_back({o.label, ui::Icon::None, o.badge});
      ui::drawDial(storage::courses()[courseSel].name, dial.data(), dial.size(), holeSel);
      break;
    }
    case Screen::GameBookAsk:
      ui::drawDial("GAMEBOOK", kGameBookMenu, 2, gameBookSel, "",
                   gameBookSel == 0 ? "Send each hole live" : "Only on this display");
      break;
    case Screen::Hole:
      ui::drawHole(active, wheel, liveDotColor());
      if (!ui::isParty(wheel, active.par[active.hole])) setLeds(ui::holeColor(wheel, active.par[active.hole]));
      break;
    case Screen::HoleMenu:
      ui::drawDial("HOLE " + String(active.firstHole + active.hole), kHoleMenu, kHoleMenuCount, menuSel, "",
                   active.gb.code.isEmpty() ? String("") : "GameBook code " + active.gb.code);
      break;
    case Screen::Confirm:
      ui::drawDial(confirmKind == ConfirmKind::DiscardRound ? "DISCARD ROUND?" : "REPLACE ROUND?", kConfirm, 2,
                   confirmSel, "", active.gb.track ? "Also deleted in GameBook" : "");
      break;
    case Screen::Summary:
      ui::drawSummary(finished);
      break;
    case Screen::GolfBoxAsk:
      ui::drawDial("GOLFBOX", kGolfBoxMenu, 3, golfboxSel);
      break;
    case Screen::SyncSelect:
      ui::drawDial("PHONE SYNC", kSyncMenu, 2, syncSel);
      break;
    case Screen::Sync:
      if (web::mode() == web::Mode::Wifi) {
        ui::drawSyncWifi(lastPhoneConnected);
      } else {
        ui::NetState net = ui::NetState::Joining;
        String netText = "Joining...";
        if (lastHomeState == web::HomeState::Online) {
          net = ui::NetState::Online;
          netText = "Online";
        } else if (lastHomeState == web::HomeState::Failed) {
          net = ui::NetState::Failed;
          netText = web::homeError() + " - retrying";
        }
        ui::drawSyncHotspot(storage::settings().wifiSsid, net, netText, lastHomeIp, uploadStatus);
      }
      break;
  }
}

// Debug helpers over USB serial: "sync" starts hotspot sync, "syncwifi" GolfTracker
// Wi-Fi sync, "home" stops it, "shot" dumps the screen, "turn N"/"press"/"hold" act
// as the knob, "demo <hole|summary|round|wifi|hotspot|joining|holemenu|golfbox|confirm>",
// "ghtest" checks the HTTPS connection to GitHub (expects 401).
// Debug input from serial ("turn N", "press", "hold"), fed into the next loop.
static int injectedSteps = 0;
static input::Button injectedButton = input::Button::None;

// Debug: draws a screen with sample data (nothing is stored), until the next input.
static void drawDemo(const String& which) {
  const Course& c = storage::courses().empty() ? Course() : storage::courses()[0];
  Round r;
  r.id = 42;
  r.course = c.holes ? c.name : String("Demo GC");
  r.holes = c.holes ? c.holes : 18;
  for (int i = 0; i < r.holes; i++) r.par[i] = c.holes ? c.par[i] : 4;
  const int8_t diffs[18] = {0, 1, -1, 0, 2, 0, 1, 0, -2, 1, 0, 0, 3, -1, 0, 1, 0, 1};
  for (int i = 0; i < r.holes; i++) r.strokes[i] = r.par[i] + diffs[i];
  if (which == "dot") {
    r.hole = 6;
    for (int i = 6; i < r.holes; i++) r.strokes[i] = 0;
    ui::drawHole(r, r.par[6], 0x4ADE80);
  } else if (which == "hole" || which.startsWith("hole ")) {  // "hole" or "hole <strokes>"
    r.hole = 6;
    for (int i = 6; i < r.holes; i++) r.strokes[i] = 0;
    int strokes = which.length() > 5 ? which.substring(5).toInt() : r.par[6] - 1;
    ui::drawHole(r, strokes);
  } else if (which == "summary") {
    ui::drawSummary(r);
  } else if (which == "round") {
    ui::drawRound(r, 0, 12);
  } else if (which == "wifi") {
    ui::drawSyncWifi(true);
  } else if (which == "hotspot") {
    ui::drawSyncHotspot("Filipos mobilos", ui::NetState::Online, "Online", "172.20.10.2", "GolfBox: 1 sent");
  } else if (which == "joining") {
    ui::drawSyncHotspot("Filipos mobilos", ui::NetState::Joining, "Joining...", "", "");
  } else if (which == "holemenu") {
    ui::drawDial("HOLE 7", kHoleMenu, kHoleMenuCount, 2);
  } else if (which == "golfbox") {
    ui::drawDial("GOLFBOX", kGolfBoxMenu, 3, 0);
  } else if (which == "gbask") {
    ui::drawDial("GAMEBOOK", kGameBookMenu, 2, 0, "", "Send each hole live");
  } else if (which == "confirm") {
    ui::drawDial("DISCARD ROUND?", kConfirm, 2, 1);
  }
}

static void handleSerial() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') {
      if (line.length() < 32) line += c;
      continue;
    }
    line.trim();
    if (line == "sync" && screen != Screen::Sync) startSync(web::Mode::Hotspot);
    else if (line == "syncwifi" && screen != Screen::Sync) startSync(web::Mode::Wifi);
    else if (line == "home") onSync(0, input::Button::Short);
    else if (line == "ghtest") Serial.printf("ghtest: HTTP %d, free heap %u\n", upload::selfTest(), ESP.getFreeHeap());
    else if (line == "shot") ui::dumpFrame(Serial);
    else if (line == "sleeptest") sleepNow(4000);
    else if (line == "live") {
      const char* st[] = {"off", "waiting", "synced", "error"};
      Serial.printf("live: round active=%d tracked=%d gameId=%s code=%s holes=%d | gamebook login saved=%d "
                    "hotspot saved=%d wifi mode=%d status=%d ip=%s | live=%s err='%s' | free heap %u\n",
                    active.active, active.gb.track, active.gb.gameId.isEmpty() ? "-" : "set",
                    active.gb.code.isEmpty() ? "-" : active.gb.code.c_str(), active.holes, gamebook::configured(),
                    !storage::settings().wifiSsid.isEmpty(), WiFi.getMode(), WiFi.status(),
                    WiFi.localIP().toString().c_str(), st[(int)live::status()], live::lastError().c_str(),
                    ESP.getFreeHeap());
    }
    else if (line.startsWith("turn ")) injectedSteps += line.substring(5).toInt();
    else if (line == "press") injectedButton = input::Button::Short;
    else if (line == "hold") injectedButton = input::Button::Long;
    else if (line.startsWith("demo ")) drawDemo(line.substring(5));
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
  live::begin();  // GameBook network work, on the other CPU core
  if (storage::loadRound(active)) Serial.println("Resuming saved round");
  lastInputAt = millis();
  goHome();
}

void loop() {
  handleSerial();
  int steps = input::takeSteps() + injectedSteps;
  input::Button btn = input::takeButton();
  if (btn == input::Button::None) btn = injectedButton;
  injectedSteps = 0;
  injectedButton = input::Button::None;
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
    case Screen::GameBookAsk: onGameBookAsk(steps, btn); break;
    case Screen::HoleMenu: onHoleMenu(steps, btn); break;
    case Screen::Confirm: onConfirm(steps, btn); break;
    case Screen::Summary: onSummary(steps, btn); break;
    case Screen::GolfBoxAsk: onGolfBoxAsk(steps, btn); break;
    case Screen::SyncSelect: onSyncSelect(steps, btn); break;
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
    web::HomeState homeState = web::homeState();
    if (homeState != lastHomeState) {
      lastHomeState = homeState;
      setLeds(syncLedColor(homeState));
      dirty = true;
    }
    // A round was marked "send again" on the phone: upload again in this session.
    if (web::takeResendRequest()) uploadDone = false;
    // On the internet: hand unsent rounds to the GolfBox workflow (once per sync).
    if (!uploadDone && !ip.isEmpty()) {
      uploadDone = true;
      if (!upload::configured()) {
        uploadStatus = "GolfBox: no token set";
      } else {
        uploadStatus = "GolfBox: sending...";
        render();
        uploadStatus = upload::sendPending();
      }
      Serial.println("Upload: " + uploadStatus);
      dirty = true;
    }
  }

  // Live GameBook tracking (creates the game, sends holes, closes finished games).
  if (live::loop(active, screen == Screen::Sync) && active.active) storage::saveRound(active);
  if (live::status() != lastLiveStatus) {
    lastLiveStatus = live::status();
    if (screen == Screen::Hole) dirty = true;
  }

  // Party (hole in one / albatross): rainbow LEDs and animated confetti.
  static uint32_t lastParty = 0;
  if (screen == Screen::Hole && ui::isParty(wheel, active.par[active.hole]) && now - lastParty > 60) {
    lastParty = now;
    for (int i = 0; i < LED_COUNT; i++) leds.setPixelColor(i, ui::rainbow(now, i * 72));
    leds.show();
    dirty = true;
  }

  // Animate the hotspot spinner while joining.
  static uint32_t lastSpin = 0;
  if (screen == Screen::Sync && web::mode() == web::Mode::Hotspot && lastHomeState != web::HomeState::Online &&
      lastHomeState != web::HomeState::Failed && now - lastSpin > 80) {
    lastSpin = now;
    dirty = true;
  }

  if (dirty) {
    dirty = false;
    render();
  }
  delay(2);
}
