#pragma once
#include <Arduino.h>

// Sends finished rounds to GitHub (repository_dispatch), which runs the
// "GolfBox upload" workflow that registers them in GolfBox.
namespace upload {

bool configured();  // a GitHub token is saved
// Sends every round not yet sent. Needs internet (home Wi-Fi or hotspot).
// Returns a short status for the sync screen, e.g. "GolfBox: 1 sent".
String sendPending();
// Diagnostic: HTTPS call with an invalid token; 401 means TLS + internet work.
int selfTest();

}  // namespace upload
