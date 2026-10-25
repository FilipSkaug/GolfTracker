#include "upload.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include "github_ca.h"
#include "storage.h"

namespace upload {

bool configured() { return !storage::settings().githubToken.isEmpty() && !storage::settings().githubRepo.isEmpty(); }

// Certificate checks need a real clock; the display has none until NTP.
static bool syncClock() {
  if (time(nullptr) > 1700000000) return true;
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  for (int i = 0; i < 50 && time(nullptr) < 1700000000; i++) delay(100);
  return time(nullptr) > 1700000000;
}

static int dispatch(const String& roundJson, const String& token) {
  const Settings& s = storage::settings();
  WiFiClientSecure client;
  client.setCACert(kGithubRootCAs);
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client, "https://api.github.com/repos/" + s.githubRepo + "/dispatches")) return -1;
  http.addHeader("Authorization", "Bearer " + token);
  http.addHeader("Accept", "application/vnd.github+json");
  http.addHeader("X-GitHub-Api-Version", "2022-11-28");
  http.addHeader("User-Agent", "GolfTracker");
  http.addHeader("Content-Type", "application/json");
  int code = http.POST("{\"event_type\":\"golfbox-round\",\"client_payload\":{\"round\":" + roundJson + "}}");
  if (code != 204) Serial.printf("Upload: HTTP %d %s\n", code, http.getString().substring(0, 200).c_str());
  http.end();
  return code;
}

String sendPending() {
  if (!configured()) return "";
  std::vector<Round> rounds = storage::history();
  std::vector<const Round*> pending;
  for (auto it = rounds.rbegin(); it != rounds.rend(); ++it) {  // oldest first
    if (!it->sent && !it->skip && totals(*it).played > 0) pending.push_back(&*it);
  }
  if (pending.empty()) return "GolfBox: all sent";
  if (!syncClock()) return "GolfBox: no internet";

  int sent = 0;
  for (const Round* r : pending) {
    int code = dispatch(storage::roundPayload(*r), storage::settings().githubToken);
    Serial.printf("Upload: round #%d -> %d (free heap %u)\n", r->id, code, ESP.getFreeHeap());
    if (code == 204) {
      storage::markSent(r->id);
      sent++;
    } else if (code == 401 || code == 403 || code == 404) {
      return "GolfBox: check token";  // retrying the rest won't help
    } else {
      break;
    }
  }
  if (sent == (int)pending.size()) return "GolfBox: " + String(sent) + " sent";
  return "GolfBox: " + String(sent) + "/" + String(pending.size()) + " sent";
}

int selfTest() {
  if (!syncClock()) return -2;
  return dispatch("{}", "invalid-token");
}

}  // namespace upload
