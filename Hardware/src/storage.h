#pragma once
#include <Arduino.h>

#include <vector>

#include "config.h"

struct Course {
  String name;
  uint8_t holes = 0;
  uint8_t par[MAX_HOLES] = {};
};

struct Round {
  bool active = false;
  int id = 0;         // history id (finished rounds only)
  bool sent = false;  // handed to the GolfBox upload workflow
  String course;
  uint8_t holes = 0;
  uint8_t hole = 0;       // index of the hole being played
  uint8_t firstHole = 1;  // number shown for index 0 (10 when playing the back 9)
  uint8_t par[MAX_HOLES] = {};
  uint8_t strokes[MAX_HOLES] = {};  // 0 = not played yet
};

struct Settings {
  String apiKey;    // GolfCourseAPI key, used by the phone page for course search
  String wifiSsid;  // optional home Wi-Fi the display joins during sync
  String wifiPass;
  // GolfBox upload: rounds are sent to this GitHub repo's "GolfBox upload" workflow.
  String githubRepo = "FilipSkaug/GolfTracker";
  String githubToken;  // fine-grained token with Contents: read & write on that repo
};

struct Totals {
  int strokes = 0;
  int par = 0;
  int played = 0;
};

// Sum of played holes in [from, to).
Totals totals(const Round& r, int from = 0, int to = MAX_HOLES);

namespace storage {

bool begin();

const std::vector<Course>& courses();
String coursesJson();
// Replaces all courses with a JSON array of {name, par:[...]}. Returns "" on success.
String setCoursesJson(const String& json);

bool loadRound(Round& r);
void saveRound(const Round& r);
void clearRound();

void appendHistory(const Round& r);
std::vector<Round> history();  // finished rounds, newest first
String historyJson();
bool deleteHistory(int id);
void markSent(int id);
// {"id","course","par","strokes","first"} as sent to the GolfBox workflow.
String roundPayload(const Round& r);

const Settings& settings();
// Updates the fields present in a JSON object {apiKey, wifiSsid, wifiPass, githubRepo,
// githubToken}. Returns "" on success.
String updateSettingsJson(const String& json);
String settingsJson();  // never includes the Wi-Fi password or GitHub token

}  // namespace storage
