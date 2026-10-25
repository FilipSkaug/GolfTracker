#pragma once
#include <Arduino.h>

#include <vector>

#include "config.h"

struct Course {
  String name;
  uint8_t holes = 0;
  uint8_t par[MAX_HOLES] = {};
  // Linked Golf GameBook course (0 = not linked), picked on the phone page.
  uint32_t gbCourseId = 0;
  uint32_t gbTeeboxId = 0;
  String gbName;  // e.g. "Byneset Golfklubb / Byneset Syd - Yellow"
};

// Golf GameBook state for a round tracked live (see live.cpp).
struct GameBookRound {
  bool track = false;     // "Track on GameBook" chosen
  uint32_t courseId = 0;  // GameBook course and tees, copied from the course link
  uint32_t teeMen = 0;
  String gameId;          // filled once the game exists in GameBook
  String roundId;
  String groupId;
  String teamId;
  String code;            // join code for playing partners
  String startTime;       // ISO time the game was created with
  int configVersion = 0;
  uint8_t sent[MAX_HOLES] = {};  // strokes last sent per hole (0 = not sent)
  bool finished = false;         // closed in GameBook
};

struct Round {
  bool active = false;
  int id = 0;         // history id (finished rounds only)
  bool sent = false;  // handed to the GolfBox upload workflow
  bool skip = false;  // "Don't send" chosen: never sent automatically
  String course;
  uint8_t holes = 0;
  uint8_t hole = 0;       // index of the hole being played
  uint8_t firstHole = 1;  // number shown for index 0 (10 when playing the back 9)
  uint8_t par[MAX_HOLES] = {};
  uint8_t strokes[MAX_HOLES] = {};  // 0 = not played yet
  GameBookRound gb;
};

struct Settings {
  String apiKey;    // GolfCourseAPI key, used by the phone page for course search
  String wifiSsid;  // optional home Wi-Fi the display joins during sync
  String wifiPass;
  // GolfBox upload: rounds are sent to this GitHub repo's "GolfBox upload" workflow.
  String githubRepo = "FilipSkaug/GolfTracker";
  String githubToken;  // fine-grained token with Contents: read & write on that repo
  // Golf GameBook login (the display logs in itself to get a token).
  String gamebookEmail;
  String gamebookPass;
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

int appendHistory(const Round& r);  // returns the new round's id
std::vector<Round> history();  // finished rounds, newest first
String historyJson();
bool deleteHistory(int id);
// Rewrites a finished round's GameBook state (by id) after live.cpp changed it.
void updateHistoryGb(const Round& r);
// Most recent finished round still to be closed in GameBook; false if none.
bool pendingGbRound(Round& r);
// GameBook games to delete (discarded rounds), kept until GameBook confirms.
void queueGbDelete(const String& gameId, const String& code);
bool nextGbDelete(String& gameId, String& code);
void doneGbDelete(const String& gameId);
void markSent(int id, bool sent = true);  // false = (re)send on next sync, also clears skip
void markSkip(int id);                    // never send this round automatically
// {"id","course","par","strokes","first"} as sent to the GolfBox workflow.
String roundPayload(const Round& r);

const Settings& settings();
// Updates the fields present in a JSON object {apiKey, wifiSsid, wifiPass, githubRepo,
// githubToken, gamebookEmail, gamebookPass}. Returns "" on success.
String updateSettingsJson(const String& json);
String settingsJson();  // never includes passwords or the GitHub token

}  // namespace storage
