#pragma once
#include <Arduino.h>

#include "storage.h"

// Golf GameBook client. GameBook has no public API; these are the requests its
// app makes (recorded with mitmproxy), so a GameBook app update can break them.
// Needs internet (hotspot). Logs in with the saved email/password and keeps the
// token in memory, logging in again when GameBook rejects it.
namespace gamebook {

bool configured();  // email and password saved

// For the phone page's "Link GameBook course". Both return compact JSON, or ""
// with err set (e.g. "Not online", "Login failed").
// [{"courseId","club","course","holes"}] - your GameBook favourites.
String favoritesJson(String& err);
// {"courseId","club","course","holes","defaultTee","tees":[{"id","name"}],"par":[...]}
String courseJson(uint32_t courseId, String& err);

// Live round tracking. Each updates r.gb on success.
// Creates the game for r (course/tee from r.gb, 18 / front 9 / back 9 from r).
bool createGame(Round& r, String& err);
// Sends every hole whose strokes differ from what GameBook last got.
bool sendHoles(Round& r, String& err);
// Sends the remaining holes and closes the game, like "finish" in the app.
bool finishGame(Round& r, String& err);
// Deletes a game (discarded round). A game that's already gone counts as done.
bool deleteGame(const String& gameId, const String& code, String& err);

// Forget the cached token (after the login details change).
void logout();

}  // namespace gamebook
