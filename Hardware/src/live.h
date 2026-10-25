#pragma once
#include <Arduino.h>

#include "storage.h"

// Live Golf GameBook tracking during a round: keeps the display on the phone's
// hotspot in the background, creates the game, sends each hole as it's saved and
// closes the game at the end (also later, for rounds finished without signal).
//
// The network work runs in its own task on the other CPU core, so the screen and
// knob never wait for GameBook. The main loop hands it copies of the round and
// merges the results back.
namespace live {

enum class Status { Off, Waiting, Synced, Error };

void begin();

// Call every loop with the round in progress (may be inactive). Applies results
// from the background task to `active` and returns true when it changed it (the
// caller saves the round). syncScreen: Phone sync owns Wi-Fi then, so the task
// only uses the connection, never changes it.
bool loop(Round& active, bool syncScreen);

// A hole was saved / the round started or finished: work on it right away.
void kick();
// The round is being discarded: delete its GameBook game (now or once online; if
// the game is still being created, as soon as it exists).
void discard(const Round& r);
// Turn the background Wi-Fi off (before sleep) and pause until the next kick().
void suspend();

Status status();
// Last GameBook error from the background task ("" if none), for debugging.
String lastError();

}  // namespace live
