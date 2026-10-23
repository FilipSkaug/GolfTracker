#include "storage.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

#include <algorithm>

#include "default_courses.h"

static const char* kCoursesPath = "/courses.json";
static const char* kRoundPath = "/round.json";
static const char* kHistoryPath = "/history.json";
static const char* kSettingsPath = "/settings.json";
// Bump when kDefaultCourses gains courses that existing devices should receive.
static const char* kDefaultsMarker = "/defaults_v2";

static std::vector<Course> s_courses;
static Settings s_settings;

Totals totals(const Round& r, int from, int to) {
  Totals t;
  if (to > r.holes) to = r.holes;
  for (int i = from; i < to; i++) {
    if (!r.strokes[i]) continue;
    t.strokes += r.strokes[i];
    t.par += r.par[i];
    t.played++;
  }
  return t;
}

namespace storage {

static bool readJson(const char* path, JsonDocument& doc) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  return !err;
}

// Write to a temp file first so a power cut never leaves a half-written file.
static bool writeJson(const char* path, const JsonDocument& doc) {
  String tmp = String(path) + ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;
  size_t n = serializeJson(doc, f);
  f.close();
  if (n == 0) return false;
  LittleFS.remove(path);
  return LittleFS.rename(tmp, path);
}

static String parseCourses(JsonArrayConst arr, std::vector<Course>& out) {
  if (arr.size() > MAX_COURSES) return "Too many courses";
  for (JsonObjectConst o : arr) {
    Course c;
    c.name = o["name"] | "";
    c.name.trim();
    if (c.name.isEmpty() || c.name.length() > 40) return "Course name must be 1-40 characters";
    JsonArrayConst par = o["par"];
    if (par.size() < 1 || par.size() > MAX_HOLES) return "A course needs 1-18 holes";
    c.holes = par.size();
    for (size_t i = 0; i < par.size(); i++) {
      int p = par[i] | 0;
      if (p < 3 || p > 6) return "Par must be 3-6";
      c.par[i] = p;
    }
    out.push_back(c);
  }
  return "";
}

static void writeCourses() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (const Course& c : s_courses) {
    JsonObject o = arr.add<JsonObject>();
    o["name"] = c.name;
    JsonArray par = o["par"].to<JsonArray>();
    for (int i = 0; i < c.holes; i++) par.add(c.par[i]);
  }
  writeJson(kCoursesPath, doc);
}

// Adds the built-in courses once (by name, so a user's own copy is kept) and
// drops the placeholder course from earlier firmware.
static void installDefaults() {
  if (LittleFS.exists(kDefaultsMarker)) return;
  for (size_t i = 0; i < s_courses.size(); i++) {
    if (s_courses[i].name == "Example GC") {
      s_courses.erase(s_courses.begin() + i);
      break;
    }
  }
  for (const DefaultCourse& d : kDefaultCourses) {
    bool exists = false;
    for (const Course& c : s_courses) exists |= c.name.equalsIgnoreCase(d.name);
    if (exists || s_courses.size() >= MAX_COURSES) continue;
    Course c;
    c.name = d.name;
    c.holes = d.holes;
    memcpy(c.par, d.par, d.holes);
    s_courses.push_back(c);
  }
  writeCourses();
  File f = LittleFS.open(kDefaultsMarker, "w");
  f.close();
}

bool begin() {
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
    return false;
  }
  JsonDocument doc;
  if (readJson(kCoursesPath, doc) && doc.is<JsonArray>()) {
    std::vector<Course> loaded;
    if (parseCourses(doc.as<JsonArrayConst>(), loaded).isEmpty()) s_courses = loaded;
  }
  installDefaults();
  doc.clear();
  if (readJson(kSettingsPath, doc)) {
    s_settings.apiKey = doc["apiKey"] | "";
    s_settings.wifiSsid = doc["wifiSsid"] | "";
    s_settings.wifiPass = doc["wifiPass"] | "";
    s_settings.githubRepo = doc["githubRepo"] | s_settings.githubRepo.c_str();
    s_settings.githubToken = doc["githubToken"] | "";
  }
  return true;
}

const std::vector<Course>& courses() { return s_courses; }

String coursesJson() {
  File f = LittleFS.open(kCoursesPath, "r");
  if (!f) return "[]";
  String s = f.readString();
  f.close();
  return s;
}

String setCoursesJson(const String& json) {
  JsonDocument doc;
  if (deserializeJson(doc, json) || !doc.is<JsonArray>()) return "Invalid JSON";
  std::vector<Course> parsed;
  String err = parseCourses(doc.as<JsonArrayConst>(), parsed);
  if (!err.isEmpty()) return err;
  s_courses = parsed;
  writeCourses();
  return "";
}

static void roundToJson(const Round& r, JsonObject o) {
  o["course"] = r.course;
  o["hole"] = r.hole;
  o["first"] = r.firstHole;
  JsonArray par = o["par"].to<JsonArray>();
  JsonArray strokes = o["strokes"].to<JsonArray>();
  for (int i = 0; i < r.holes; i++) {
    par.add(r.par[i]);
    strokes.add(r.strokes[i]);
  }
}

static bool roundFromJson(JsonObjectConst o, Round& r) {
  JsonArrayConst par = o["par"];
  JsonArrayConst strokes = o["strokes"];
  if (par.size() < 1 || par.size() > MAX_HOLES || strokes.size() != par.size()) return false;
  r = Round();
  r.id = o["id"] | 0;
  r.sent = o["sent"] | false;
  r.course = o["course"] | "";
  r.holes = par.size();
  r.hole = min<int>(o["hole"] | 0, r.holes - 1);
  r.firstHole = o["first"] | 1;
  for (int i = 0; i < r.holes; i++) {
    r.par[i] = par[i] | 4;
    r.strokes[i] = strokes[i] | 0;
  }
  return true;
}

bool loadRound(Round& r) {
  JsonDocument doc;
  if (!readJson(kRoundPath, doc) || !roundFromJson(doc.as<JsonObjectConst>(), r)) return false;
  r.active = true;
  return true;
}

void saveRound(const Round& r) {
  JsonDocument doc;
  roundToJson(r, doc.to<JsonObject>());
  writeJson(kRoundPath, doc);
}

void clearRound() { LittleFS.remove(kRoundPath); }

static void loadHistory(JsonDocument& doc) {
  if (!readJson(kHistoryPath, doc) || !doc["rounds"].is<JsonArray>()) {
    doc.clear();
    doc["nextId"] = 1;
    doc["rounds"].to<JsonArray>();
  }
}

void appendHistory(const Round& r) {
  JsonDocument doc;
  loadHistory(doc);
  int id = doc["nextId"] | 1;
  JsonObject o = doc["rounds"].as<JsonArray>().add<JsonObject>();
  o["id"] = id;
  roundToJson(r, o);
  o.remove("hole");
  doc["nextId"] = id + 1;
  writeJson(kHistoryPath, doc);
}

std::vector<Round> history() {
  JsonDocument doc;
  loadHistory(doc);
  std::vector<Round> out;
  for (JsonObjectConst o : doc["rounds"].as<JsonArrayConst>()) {
    Round r;
    if (roundFromJson(o, r)) out.push_back(r);
  }
  std::reverse(out.begin(), out.end());
  return out;
}

String historyJson() {
  JsonDocument doc;
  loadHistory(doc);
  String s;
  serializeJson(doc, s);
  return s;
}

bool deleteHistory(int id) {
  JsonDocument doc;
  loadHistory(doc);
  JsonArray rounds = doc["rounds"];
  for (size_t i = 0; i < rounds.size(); i++) {
    if ((rounds[i]["id"] | -1) == id) {
      rounds.remove(i);
      return writeJson(kHistoryPath, doc);
    }
  }
  return false;
}

void markSent(int id) {
  JsonDocument doc;
  loadHistory(doc);
  for (JsonObject o : doc["rounds"].as<JsonArray>()) {
    if ((o["id"] | -1) == id) {
      o["sent"] = true;
      writeJson(kHistoryPath, doc);
      return;
    }
  }
}

String roundPayload(const Round& r) {
  JsonDocument doc;
  JsonObject o = doc.to<JsonObject>();
  roundToJson(r, o);
  o.remove("hole");
  o["id"] = r.id;
  String s;
  serializeJson(doc, s);
  return s;
}

const Settings& settings() { return s_settings; }

String updateSettingsJson(const String& json) {
  JsonDocument in;
  if (deserializeJson(in, json) || !in.is<JsonObject>()) return "Invalid JSON";
  Settings next = s_settings;
  if (in["apiKey"].is<const char*>()) next.apiKey = in["apiKey"].as<String>();
  if (in["wifiSsid"].is<const char*>()) next.wifiSsid = in["wifiSsid"].as<String>();
  if (in["wifiPass"].is<const char*>()) next.wifiPass = in["wifiPass"].as<String>();
  if (in["githubRepo"].is<const char*>()) next.githubRepo = in["githubRepo"].as<String>();
  if (in["githubToken"].is<const char*>()) next.githubToken = in["githubToken"].as<String>();
  next.apiKey.trim();
  next.wifiSsid.trim();
  next.githubRepo.trim();
  next.githubToken.trim();
  if (next.apiKey.length() > 128 || next.wifiSsid.length() > 32 || next.wifiPass.length() > 64 ||
      next.githubRepo.length() > 100 || next.githubToken.length() > 255) {
    return "Value too long";
  }
  if (!next.githubRepo.isEmpty() && next.githubRepo.indexOf('/') < 1) return "Repository must look like owner/name";
  if (!next.wifiPass.isEmpty() && next.wifiPass.length() < 8) return "Wi-Fi password must be 8+ characters";

  JsonDocument doc;
  doc["apiKey"] = next.apiKey;
  doc["wifiSsid"] = next.wifiSsid;
  doc["wifiPass"] = next.wifiPass;
  doc["githubRepo"] = next.githubRepo;
  doc["githubToken"] = next.githubToken;
  if (!writeJson(kSettingsPath, doc)) return "Could not save";
  s_settings = next;
  return "";
}

String settingsJson() {
  JsonDocument doc;
  doc["apiKey"] = s_settings.apiKey;
  doc["wifiSsid"] = s_settings.wifiSsid;
  doc["hasWifiPass"] = !s_settings.wifiPass.isEmpty();
  doc["githubRepo"] = s_settings.githubRepo;
  doc["hasGithubToken"] = !s_settings.githubToken.isEmpty();
  String s;
  serializeJson(doc, s);
  return s;
}

}  // namespace storage
