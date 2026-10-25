#include "gamebook.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include "gamebook_ca.h"
#include "storage.h"

namespace gamebook {

static const char* kHost = "https://core.golfgamebook.com";
// Sent by the GameBook app (v13.10.5); the server may expect them.
static const char* kAppVersion = "13.10.5";
static const char* kUserAgent = "com.Tivadar.Best.HTTP v3.0.11.0/Unity 6000.3.22f1";

static String s_token;
static String s_userId;

// The phone page (main loop) and live tracking (its own task) both use GameBook;
// one at a time. Recursive: finishGame calls sendHoles.
static SemaphoreHandle_t lockHandle() {
  static SemaphoreHandle_t m = xSemaphoreCreateRecursiveMutex();
  return m;
}
struct Lock {
  Lock() { xSemaphoreTakeRecursive(lockHandle(), portMAX_DELAY); }
  ~Lock() { xSemaphoreGiveRecursive(lockHandle()); }
};

bool configured() {
  const Settings& s = storage::settings();
  return !s.gamebookEmail.isEmpty() && !s.gamebookPass.isEmpty();
}

void logout() {
  Lock lock;
  s_token = "";
  s_userId = "";
}

// Certificate checks need a real clock; the display has none until NTP.
static bool syncClock() {
  if (time(nullptr) > 1700000000) return true;
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  for (int i = 0; i < 50 && time(nullptr) < 1700000000; i++) delay(100);
  return time(nullptr) > 1700000000;
}

// A stable id for this display, so its login doesn't replace the phone's.
static String deviceUuid() {
  uint8_t m[6];
  WiFi.macAddress(m);
  char b[40];
  snprintf(b, sizeof(b), "60C0F7A0-6B00-4000-8000-%02X%02X%02X%02X%02X%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return b;
}

static String urlEncode(const String& s) {
  String out;
  char hex[4];
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      snprintf(hex, sizeof(hex), "%%%02X", c);
      out += hex;
    }
  }
  return out;
}

// One HTTPS request. Plain HTTP/1.0 so replies aren't chunked and can be parsed
// straight from the stream (course details are ~16 KB).
struct Request {
  WiFiClientSecure client;
  HTTPClient http;
  bool begin(const String& path) {
    client.setCACert(kGamebookRootCAs);
    http.setTimeout(15000);
    http.useHTTP10(true);
    if (!http.begin(client, String(kHost) + path)) return false;
    http.setUserAgent(kUserAgent);
    http.addHeader("App-Version", kAppVersion);
    return true;
  }
  ~Request() { http.end(); }
};

static bool login(String& err) {
  const Settings& s = storage::settings();
  Request req;
  if (!req.begin("/api/2/login")) {
    err = "Can't reach GameBook";
    return false;
  }
  req.http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int code = req.http.POST("account=" + urlEncode(s.gamebookEmail) + "&password=" + urlEncode(s.gamebookPass) +
                           "&uuid=" + deviceUuid());
  if (code != 200) {
    err = code == 400 || code == 401 || code == 403 ? "GameBook login failed - check email/password"
                                                    : "GameBook login error " + String(code);
    return false;
  }
  JsonDocument filter;
  filter["success"] = true;
  filter["data"]["accessToken"] = true;
  filter["data"]["userId"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, req.http.getStream(), DeserializationOption::Filter(filter),
                     DeserializationOption::NestingLimit(40)) ||
      !(doc["success"] | false)) {
    err = "GameBook login failed - check email/password";
    return false;
  }
  JsonVariantConst uid = doc["data"]["userId"];
  s_token = doc["data"]["accessToken"] | "";
  s_userId = uid.is<const char*>() ? uid.as<String>() : String((long long)(uid | 0LL));
  if (s_token.isEmpty() || s_userId == "0") {
    err = "GameBook login failed";
    return false;
  }
  Serial.printf("GameBook: logged in as user %s\n", s_userId.c_str());
  return true;
}

// One GameBook request ("{user}" in path is replaced by the user id). Logs in
// first if needed, and once more if the token expired. The reply is parsed into
// doc keeping only the fields in filter (pass nullptr to skip parsing).
static bool request(const char* method, String path, const String& body, JsonDocument* filter, JsonDocument* doc,
                    String& err) {
  if (WiFi.status() != WL_CONNECTED) {
    err = "Not online";
    return false;
  }
  if (!configured()) {
    err = "Add your GameBook login in Settings";
    return false;
  }
  if (!syncClock()) {
    err = "No internet";
    return false;
  }
  for (int attempt = 0; attempt < 2; attempt++) {
    if (s_token.isEmpty() && !login(err)) return false;
    Request req;
    String p = path;
    p.replace("{user}", s_userId);
    if (!req.begin(p)) {
      err = "Can't reach GameBook";
      return false;
    }
    req.http.addHeader("Authorization", "Bearer " + s_token);
    if (!body.isEmpty()) req.http.addHeader("Content-Type", "application/json");
    int code = strcmp(method, "GET") == 0 ? req.http.GET() : req.http.sendRequest(method, body);
    if (code == 401 || code == 403) {
      s_token = "";  // expired: log in again and retry
      continue;
    }
    if (code != 200) {
      err = "GameBook error " + String(code);
      Serial.printf("GameBook: %s %s -> %d %s\n", method, path.c_str(), code, req.http.getString().substring(0, 300).c_str());
      return false;
    }
    if (!doc) return true;
    // Course replies carry deeply nested GPS data; even skipped by the filter it
    // counts towards the nesting limit (default 10).
    auto depth = DeserializationOption::NestingLimit(40);
    DeserializationError e = filter ? deserializeJson(*doc, req.http.getStream(), DeserializationOption::Filter(*filter), depth)
                                    : deserializeJson(*doc, req.http.getStream(), depth);
    if (e) {
      err = String("GameBook reply unreadable: ") + e.c_str();
      return false;
    }
    return true;
  }
  err = "GameBook login expired";
  return false;
}

static bool getJson(const String& path, JsonDocument& filter, JsonDocument& doc, String& err) {
  return request("GET", path, "", &filter, &doc, err);
}

String favoritesJson(String& err) {
  Lock lock;
  JsonDocument filter;
  JsonObject f = filter["data"].add<JsonObject>();
  for (const char* k : {"courseId", "clubName", "courseName", "holes"}) f[k] = true;
  JsonDocument doc;
  if (!getJson("/api/2/user/{user}/courses?favorites=true", filter, doc, err)) return "";

  JsonDocument out;
  JsonArray arr = out.to<JsonArray>();
  for (JsonObjectConst c : doc["data"].as<JsonArrayConst>()) {
    JsonObject o = arr.add<JsonObject>();
    o["courseId"] = c["courseId"];
    o["club"] = c["clubName"];
    o["course"] = c["courseName"];
    o["holes"] = c["holes"];
  }
  String s;
  serializeJson(out, s);
  return s;
}

String courseJson(uint32_t courseId, String& err) {
  Lock lock;
  JsonDocument filter;
  JsonObject d = filter["data"].to<JsonObject>();
  for (const char* k : {"courseId", "clubName", "courseName", "holes"}) d[k] = true;
  d["scorecard"]["defaultTeeboxId"]["men"] = true;
  JsonObject t = d["scorecard"]["teeboxes"].add<JsonObject>();
  t["id"] = true;
  t["teeboxName"] = true;
  JsonObject h = d["scorecard"]["holes"].add<JsonObject>();
  h["holeNumber"] = true;
  h["par"] = true;
  JsonDocument doc;
  if (!getJson("/api/2/course/" + String(courseId) + "?scorecard=True&requireCourseVersion=True", filter, doc, err)) {
    return "";
  }
  JsonObjectConst c = doc["data"];
  JsonDocument out;
  out["courseId"] = c["courseId"];
  out["club"] = c["clubName"];
  out["course"] = c["courseName"];
  out["holes"] = c["holes"];
  out["defaultTee"] = c["scorecard"]["defaultTeeboxId"]["men"];
  // Par per hole, in hole order (for importing the course onto the display).
  uint8_t par[MAX_HOLES] = {};
  int n = 0;
  for (JsonObjectConst hole : c["scorecard"]["holes"].as<JsonArrayConst>()) {
    int num = hole["holeNumber"] | 0;
    if (num >= 1 && num <= MAX_HOLES) {
      par[num - 1] = hole["par"] | 0;
      n = max(n, num);
    }
  }
  JsonArray pars = out["par"].to<JsonArray>();
  for (int i = 0; i < n; i++) pars.add(par[i]);
  JsonArray tees = out["tees"].to<JsonArray>();
  for (JsonObjectConst tb : c["scorecard"]["teeboxes"].as<JsonArrayConst>()) {
    JsonObject o = tees.add<JsonObject>();
    o["id"] = tb["id"];
    o["name"] = tb["teeboxName"];
  }
  String s;
  serializeJson(out, s);
  return s;
}

// ---- Live round tracking --------------------------------------------------------
// The game requests mirror the GameBook app's own (create = PATCH with temporary
// negative ids; finish = the same game sent back with the group "closed").

struct Profile {
  String firstName, lastName, gender;
  float hcp = 0;
  long homeClubId = -1;
};

static bool fetchProfile(Profile& p, String& err) {
  JsonDocument filter;
  JsonObject f = filter["data"].add<JsonObject>();
  for (const char* k : {"firstName", "lastName", "gender", "hcp", "homeClubId"}) f[k] = true;
  JsonDocument doc;
  if (!getJson("/api/2/users?ids={user}", filter, doc, err)) return false;
  JsonObjectConst u = doc["data"][0];
  p.firstName = u["firstName"] | "";
  p.lastName = u["lastName"] | "";
  p.gender = u["gender"] | "male";
  p.hcp = u["hcp"] | 0.0f;
  p.homeClubId = u["homeClubId"] | -1L;
  return true;
}

// Women's default tee for the course (the game needs one even if you play men's).
static uint32_t womenTee(uint32_t courseId, String& err) {
  JsonDocument filter;
  filter["data"]["scorecard"]["defaultTeeboxId"]["women"] = true;
  JsonDocument doc;
  if (!getJson("/api/2/course/" + String(courseId) + "?scorecard=True&requireCourseVersion=True", filter, doc, err)) {
    return 0;
  }
  return doc["data"]["scorecard"]["defaultTeeboxId"]["women"] | 0u;
}

static const char* holesToPlay(const Round& r) {
  if (r.holes > 9) return "18";
  return r.firstHole == 10 ? "b9" : "f9";
}

static long long idOr(const String& id, long long fallback) { return id.isEmpty() ? fallback : atoll(id.c_str()); }

static void addConfig(JsonObject c, int id) {
  c["Id"] = id;
  c["UseHandicaps"] = true;
  c["UsePlayingHandicaps"] = true;
  c["HandicapPercentage"] = 100;
  c["TieBreakRule"] = "handicap";
  for (const char* k : {"EradoLivesFor9", "EradoLivesFor18", "SkinsWagerValue", "SkinsUseCarryOver", "ShowTiedPositions",
                        "UseLowestSlope", "EradoLives", "UseCustomHcpPercentages", "CustomHcpPercentages",
                        "UseMatchplayHandicaps", "NumOfResultsToSelect", "StablefordType"}) {
    c[k] = nullptr;
  }
}

static void addTeamRef(JsonObject t, long long id) {
  t["Id"] = id;
  t["Order"] = id ? 1 : 0;
  t["PlayerHasResultsEntered"] = false;
  t["PlayerIsGrouped"] = false;
}

static void addHole(JsonArray holes, const Round& r, int i, long long roundId, long long userId) {
  JsonObject h = holes.add<JsonObject>();
  h["RoundId"] = roundId;
  h["HoleNumber"] = r.firstHole + i;
  h["UserId"] = userId;
  h["Strokes"] = r.strokes[i];
  h["Erado"] = false;
  for (const char* k : {"Putts", "Chips", "Sands", "Penalties"}) h[k] = -1;
  h["UpAndDown"] = nullptr;
  h["BunkerSave"] = nullptr;
  h["FirstPuttLength"] = -1;
  h["GreenHit"] = "";
  h["InitialStroke"] = "";
}

// The whole game, as the app sends it. finish = closing it (real ids, holes, closed group).
static void buildGame(JsonDocument& g, const Round& r, const Profile& p, uint32_t womenTeeId, bool finish) {
  long long uid = atoll(s_userId.c_str());
  long long gameId = finish ? idOr(r.gb.gameId, -237) : -237;
  long long roundId = finish ? idOr(r.gb.roundId, -238) : -238;
  long long groupId = finish ? idOr(r.gb.groupId, -239) : -239;
  long long teamId = finish ? idOr(r.gb.teamId, -240) : -240;
  const char* start = r.gb.startTime.c_str();
  int startHole = r.firstHole == 10 && r.holes <= 9 ? 10 : 1;

  g["Id"] = gameId;
  g["Name"] = "Game " + r.gb.startTime.substring(8, 10) + "." + r.gb.startTime.substring(5, 7) + "." +
              r.gb.startTime.substring(0, 4);
  g["IsPrivate"] = false;
  g["IsConfigured"] = true;
  g["StartTime"] = start;
  g["PlayerLimit"] = 0;
  g["Description"] = nullptr;
  g["NameDisplayMode"] = nullptr;
  g["ForceRoundScoreVerification"] = false;
  JsonObject comp = g["Competition"].to<JsonObject>();
  comp["CompetitionType"] = nullptr;
  comp["Id"] = 0;
  comp["ForceHandicapRound"] = false;
  comp["Players"] = nullptr;
  g["RoundTemplate"] = nullptr;

  JsonObject rd = g["Rounds"].add<JsonObject>();
  rd["Id"] = roundId;
  rd["CourseId"] = r.gb.courseId;
  rd["CourseVersion"] = -1;
  rd["StartTime"] = start;
  rd["DefaultTeeboxId"]["Men"] = r.gb.teeMen;
  rd["DefaultTeeboxId"]["Women"] = womenTeeId;
  rd["IsDeleted"] = false;
  rd["PrimaryGame"] = 2;
  rd["SecondaryGame"] = 1;
  addConfig(rd["PrimaryConfig"].to<JsonObject>(), 2);
  addConfig(rd["SecondaryConfig"].to<JsonObject>(), 1);
  JsonObject gen = rd["GenericConfig"].to<JsonObject>();
  gen["IsScoringDisabled"] = false;
  gen["HideFinalHoles"] = 0;
  gen["IsSimulator"] = false;
  gen["GroupSize"] = 4;
  gen["DefaultStartHole"] = startHole;
  gen["HolesToPlay"] = holesToPlay(r);
  gen["DisableGPS"] = false;
  gen["DisableAds"] = false;
  gen["ShowFlags"] = false;
  gen["UseStartTimes"] = true;
  gen["StartTimeInterval"] = 600;
  JsonObject contests = rd["Contests"].to<JsonObject>();
  for (const char* k : {"ClosestToPin", "LongestDrive", "StraightestDrive"}) contests[k].to<JsonArray>();
  contests["MeasureResultsClosestToPin"] = true;
  contests["MeasureResultsLongestDrive"] = false;
  contests["MeasureResultsStraightestDrive"] = false;
  if (finish) rd["State"] = "scoring";
  else rd["State"] = nullptr;
  rd["IsConfigured"] = false;
  rd["IsCompleted"] = nullptr;

  JsonObject grp = g["Groups"].add<JsonObject>();
  grp["Id"] = groupId;
  grp["RoundId"] = roundId;
  grp["Name"] = "Group 1";
  grp["StartHole"] = startHole;
  grp["IsDeleted"] = false;
  grp["StartTime"] = start;
  if (finish) grp["Status"] = "closed";
  else grp["Status"] = nullptr;
  JsonObject gp = grp["Players"].add<JsonObject>();
  gp["UserId"] = uid;
  gp["IsDeleted"] = false;
  gp["GgbHcpStatus"] = nullptr;
  gp["Hcp"] = p.hcp;
  gp["TeeboxId"] = r.gb.teeMen;
  gp["HcpStatus"] = "NONE";
  gp["Order"] = 1;
  gp["Status"] = nullptr;

  JsonObject team = g["Teams"].add<JsonObject>();
  team["Id"] = teamId;
  team["Name"] = "TEAM 1";
  team["IsDeleted"] = false;
  team["TeamName"] = "TEAM";
  team["MatchName"] = "MATCH";
  team["PairName"] = "PAIR";
  team["Status"] = nullptr;

  JsonArray holes = g["PlayedHoles"].to<JsonArray>();
  if (finish) {
    for (int i = 0; i < r.holes; i++) {
      if (r.strokes[i]) addHole(holes, r, i, roundId, uid);
    }
  }
  g["ContestResults"].to<JsonArray>();
  g["SlopeRule"] = "WHS";
  g["ConfigVersion"] = finish ? r.gb.configVersion : 0;
  g["CreatorId"] = uid;
  g["TournamentType"] = nullptr;

  JsonObject pl = g["Players"].add<JsonObject>();
  pl["UserId"] = uid;
  pl["Status"] = nullptr;
  pl["IsDeleted"] = false;
  pl["IsGold"] = false;
  pl["IsVerified"] = false;
  pl["FirstName"] = p.firstName;
  pl["LastName"] = p.lastName;
  pl["Gender"] = p.gender;
  pl["IsTemporary"] = false;
  pl["Email"] = nullptr;
  pl["Hcp"] = p.hcp;
  pl["HomeClubId"] = p.homeClubId;
  pl["HasAvatar"] = false;
  pl["HasCoverImg"] = false;
  for (const char* k : {"CollectStats", "CollectPuttLengthStats", "CollectGreenMissStats", "CollectPenaltyStats"}) {
    pl[k] = false;
  }
  addTeamRef(pl["Team"].to<JsonObject>(), teamId);
  addTeamRef(pl["SuperTeam"].to<JsonObject>(), 0);
  pl["RoundCount"] = nullptr;
}

static const char* kGameQuery =
    "?playedHoles=True&roundResults=False&contestResults=True&conversation=True&createIfNotExists=True"
    "&legacyGameReset=True";

// Reads the ids GameBook assigned from a game reply into r.gb.
static void readIds(JsonDocument& doc, Round& r) {
  JsonObjectConst d = doc["data"];
  r.gb.gameId = d["id"].as<String>();
  r.gb.code = d["gameCode"] | "";
  r.gb.configVersion = d["configVersion"] | r.gb.configVersion;
  r.gb.roundId = d["rounds"][0]["id"].as<String>();
  r.gb.groupId = d["groups"][0]["id"].as<String>();
  r.gb.teamId = d["teams"][0]["id"].as<String>();
}

static String isoNow() {
  time_t now = time(nullptr);
  now -= now % 600;  // like the app: start time on a 10-minute mark
  struct tm t;
  gmtime_r(&now, &t);
  char b[32];
  strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:00+00:00", &t);
  return b;
}

static JsonDocument gameFilter() {
  JsonDocument f;
  JsonObject d = f["data"].to<JsonObject>();
  d["id"] = d["gameCode"] = true;
  d["configVersion"] = true;
  d["rounds"].add<JsonObject>()["id"] = true;
  d["groups"].add<JsonObject>()["id"] = true;
  d["teams"].add<JsonObject>()["id"] = true;
  return f;
}

bool createGame(Round& r, String& err) {
  Lock lock;
  Profile p;
  if (!fetchProfile(p, err)) return false;
  uint32_t women = womenTee(r.gb.courseId, err);
  if (!women) return false;
  r.gb.startTime = isoNow();
  JsonDocument body;
  buildGame(body, r, p, women, false);
  String json;
  serializeJson(body, json);
  JsonDocument filter = gameFilter(), doc;
  if (!request("PATCH", String("/api/3/game/-237") + kGameQuery, json, &filter, &doc, err)) return false;
  readIds(doc, r);
  if (r.gb.gameId.isEmpty() || r.gb.roundId.isEmpty()) {
    err = "GameBook didn't return the game";
    return false;
  }
  Serial.printf("GameBook: created game %s (code %s)\n", r.gb.gameId.c_str(), r.gb.code.c_str());
  return true;
}

bool sendHoles(Round& r, String& err) {
  Lock lock;
  JsonDocument body;
  JsonArray holes = body.to<JsonArray>();
  long long uid = atoll(s_userId.c_str()), roundId = idOr(r.gb.roundId, 0);
  for (int i = 0; i < r.holes; i++) {
    if (r.strokes[i] && r.strokes[i] != r.gb.sent[i]) addHole(holes, r, i, roundId, uid);
  }
  if (holes.size() == 0) return true;
  if (s_userId.isEmpty()) {  // make sure we know who we are before sending
    String e2;
    if (!login(e2)) {
      err = e2;
      return false;
    }
    return sendHoles(r, err);
  }
  String json;
  serializeJson(body, json);
  String path = "/api/3/game/" + r.gb.gameId + "/playedholes?configVersion=" + String(r.gb.configVersion);
  if (!r.gb.code.isEmpty()) path += "&joinCode=" + r.gb.code;
  if (!request("PATCH", path, json, nullptr, nullptr, err)) return false;
  for (int i = 0; i < r.holes; i++) r.gb.sent[i] = r.strokes[i];
  return true;
}

bool finishGame(Round& r, String& err) {
  Lock lock;
  if (!sendHoles(r, err)) return false;
  Profile p;
  if (!fetchProfile(p, err)) return false;
  uint32_t women = womenTee(r.gb.courseId, err);
  if (!women) return false;
  JsonDocument body;
  buildGame(body, r, p, women, true);
  String json;
  serializeJson(body, json);
  String path = "/api/3/game/" + r.gb.gameId + kGameQuery;
  if (!r.gb.code.isEmpty()) path += "&joinCode=" + r.gb.code;
  JsonDocument filter = gameFilter(), doc;
  if (!request("PATCH", path, json, &filter, &doc, err)) return false;
  r.gb.finished = true;
  Serial.printf("GameBook: closed game %s\n", r.gb.gameId.c_str());
  return true;
}

bool deleteGame(const String& gameId, const String& code, String& err) {
  Lock lock;
  String path = "/api/3/game/" + gameId;
  if (!code.isEmpty()) path += "?joinCode=" + code;
  if (request("DELETE", path, "", nullptr, nullptr, err)) return true;
  if (err == "GameBook error 404") return true;  // already deleted (e.g. in the app)
  return false;
}

}  // namespace gamebook
