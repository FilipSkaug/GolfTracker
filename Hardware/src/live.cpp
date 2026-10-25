#include "live.h"

#include <WiFi.h>

#include "gamebook.h"

namespace live {

static const uint32_t kJoinRetryMs = 20000;     // try the hotspot again this often
static const uint32_t kErrorRetryMs = 30000;    // after a GameBook error
static const uint32_t kPendingCheckMs = 60000;  // look for unfinished games in history

// ---- Shared between the main loop and the task (guarded by s_lock) --------------
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static Round s_snapshot;          // latest copy of the round in progress
static uint32_t s_snapshotSerial = 0;
static bool s_syncScreen = false;
static bool s_paused = false;
static bool s_recheck = false;    // kick(): look for unfinished games right away
static uint32_t s_discardSerial = 0;  // round discarded before its game existed
static bool s_hasResult = false;  // the task finished work on s_result
static Round s_result;
static uint32_t s_resultSerial = 0;
static volatile Status s_status = Status::Off;
static String s_lastError;  // guarded by s_lock

// ---- Main-loop side -------------------------------------------------------------
static uint32_t s_serial = 0;  // changes when a different round becomes active
static String s_activeKey;     // identifies the active round (course + start state)
static Round s_lastSent;       // what the task last got, to notice changes

static bool sameRoundData(const Round& a, const Round& b) {
  return a.active == b.active && a.gb.track == b.gb.track && a.holes == b.holes && a.gb.gameId == b.gb.gameId &&
         memcmp(a.strokes, b.strokes, sizeof(a.strokes)) == 0 && memcmp(a.gb.sent, b.gb.sent, sizeof(a.gb.sent)) == 0;
}

static bool needsWork(const Round& r) {
  if (!r.active || !r.gb.track) return false;
  if (r.gb.gameId.isEmpty()) return true;
  for (int i = 0; i < r.holes; i++) {
    if (r.strokes[i] && r.strokes[i] != r.gb.sent[i]) return true;
  }
  return false;
}

// ---- Task side --------------------------------------------------------------------
static bool s_ownsWifi = false;
// The last game this task created. If its round finishes before the main loop has
// stored the ids, closing it reuses them instead of creating a second game.
static Round s_lastCreated;
// What the task itself knows about the current round's game. The main loop merges
// results into its round a moment later, so a snapshot can still lack the game id
// it just created - without this the task would create the game a second time.
static GameBookRound s_known;
static uint32_t s_knownSerial = 0;

static void wifiOff() {
  if (s_ownsWifi) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    s_ownsWifi = false;
  }
}

static void keepConnected(uint32_t& joinAt) {
  const Settings& s = storage::settings();
  if (WiFi.getMode() != WIFI_STA) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    s_ownsWifi = true;
    joinAt = 0;
  }
  if (WiFi.status() != WL_CONNECTED && (joinAt == 0 || millis() - joinAt > kJoinRetryMs)) {
    joinAt = millis();
    WiFi.begin(s.wifiSsid.c_str(), s.wifiPass.c_str());
  }
}

static void task(void*) {
  uint32_t joinAt = 0, retryAt = 0, pendingCheckAt = 0;
  bool hasPending = false, hasDelete = false;
  for (;;) {
    // Wake on kick() or every half second.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
    Round work;
    uint32_t serial;
    bool syncScreen, paused;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    work = s_snapshot;
    serial = s_snapshotSerial;
    syncScreen = s_syncScreen;
    paused = s_paused;
    if (s_recheck) pendingCheckAt = 0;
    s_recheck = false;
    xSemaphoreGive(s_lock);
    if (serial == s_knownSerial && !s_known.gameId.isEmpty()) {
      bool track = work.gb.track;
      work.gb = s_known;  // newer than the snapshot: game ids and holes already sent
      work.gb.track = track;
    }

    uint32_t now = millis();
    if (pendingCheckAt == 0 || now - pendingCheckAt > kPendingCheckMs) {
      Round r;
      String id, code;
      hasPending = storage::pendingGbRound(r);
      hasDelete = storage::nextGbDelete(id, code);
      pendingCheckAt = now ? now : 1;
    }
    bool tracking = work.active && work.gb.track;
    bool wanted = !paused && (tracking || hasPending || hasDelete) && gamebook::configured() &&
                  !storage::settings().wifiSsid.isEmpty();
    if (!wanted) {
      if (!syncScreen) wifiOff();
      s_status = Status::Off;
      continue;
    }
    if (!syncScreen) keepConnected(joinAt);
    if (WiFi.status() != WL_CONNECTED) {
      if (s_status != Status::Error) s_status = Status::Waiting;
      continue;
    }
    if (retryAt && millis() < retryAt) continue;
    retryAt = 0;

    String err;
    // Games of discarded rounds first.
    String delId, delCode;
    if (hasDelete && storage::nextGbDelete(delId, delCode)) {
      s_status = Status::Waiting;
      if (gamebook::deleteGame(delId, delCode, err)) {
        storage::doneGbDelete(delId);
        Serial.printf("GameBook: deleted game %s\n", delId.c_str());
        if (s_lastCreated.gb.gameId == delId) s_lastCreated = Round();
        pendingCheckAt = 0;  // more?
        xTaskNotifyGive(s_task);
      } else {
        Serial.println("GameBook: " + err);
        s_status = Status::Error;
        retryAt = millis() + kErrorRetryMs;
      }
      continue;
    }
    if (needsWork(work)) {
      // The round in progress: create the game, then send new/changed holes.
      s_status = Status::Waiting;
      bool ok = true;
      if (work.gb.gameId.isEmpty()) {
        ok = gamebook::createGame(work, err);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool discarded = ok && s_discardSerial == serial;
        xSemaphoreGive(s_lock);
        if (discarded) {  // the round was discarded while its game was being created
          storage::queueGbDelete(work.gb.gameId, work.gb.code);
          hasDelete = true;
          xTaskNotifyGive(s_task);
          continue;
        }
        if (ok) s_lastCreated = work;
      }
      if (!work.gb.gameId.isEmpty()) {
        s_known = work.gb;  // remember the game even if sending holes fails
        s_knownSerial = serial;
      }
      ok = ok && gamebook::sendHoles(work, err);
      if (!work.gb.gameId.isEmpty()) s_known = work.gb;
      if (!work.gb.gameId.isEmpty() && work.course == s_lastCreated.course) s_lastCreated.gb = work.gb;
      xSemaphoreTake(s_lock, portMAX_DELAY);
      s_result = work;  // ids from a successful create are kept even if sending failed
      s_resultSerial = serial;
      s_hasResult = true;
      xSemaphoreGive(s_lock);
      if (ok) {
        s_status = Status::Synced;
        xTaskNotifyGive(s_task);  // check again straight away
      } else {
        Serial.println("GameBook: " + err);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_lastError = err;
        xSemaphoreGive(s_lock);
        s_status = Status::Error;
        retryAt = millis() + kErrorRetryMs;
      }
      continue;
    }
    if (hasPending) {
      // A finished round whose game isn't closed yet (e.g. no signal at the end).
      Round done;
      if (storage::pendingGbRound(done)) {
        if (done.gb.gameId.isEmpty() && !s_lastCreated.gb.gameId.isEmpty() && done.course == s_lastCreated.course &&
            done.holes == s_lastCreated.holes) {
          bool finishedFlag = done.gb.finished;
          done.gb = s_lastCreated.gb;
          done.gb.finished = finishedFlag;
          done.gb.track = true;
        }
        s_status = Status::Waiting;
        bool ok = done.gb.gameId.isEmpty() ? gamebook::createGame(done, err) : true;
        ok = ok && gamebook::finishGame(done, err);
        if (!done.gb.gameId.isEmpty()) storage::updateHistoryGb(done);
        if (!ok) {
          Serial.println("GameBook: " + err);
          xSemaphoreTake(s_lock, portMAX_DELAY);
          s_lastError = err;
          xSemaphoreGive(s_lock);
          s_status = Status::Error;
          retryAt = millis() + kErrorRetryMs;
          continue;
        }
      }
      pendingCheckAt = 0;  // re-check for more
    }
    s_status = tracking ? Status::Synced : Status::Off;
  }
}

// ---- Public -----------------------------------------------------------------------

void begin() {
  s_lock = xSemaphoreCreateMutex();
  // Core 0 (the main loop runs on core 1); TLS needs a roomy stack.
  xTaskCreatePinnedToCore(task, "gamebook", 12 * 1024, nullptr, 1, &s_task, 0);
}

void kick() {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_paused = false;
  s_recheck = true;
  xSemaphoreGive(s_lock);
  if (s_task) xTaskNotifyGive(s_task);
}

void discard(const Round& r) {
  if (!r.gb.track) return;
  if (!r.gb.gameId.isEmpty()) {
    storage::queueGbDelete(r.gb.gameId, r.gb.code);
  } else {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_discardSerial = s_serial;  // in case its game is being created right now
    xSemaphoreGive(s_lock);
  }
  kick();
}

void suspend() {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_paused = true;
  xSemaphoreGive(s_lock);
  // Let the task notice, then make sure the radio is off before sleeping.
  for (int i = 0; i < 40 && WiFi.getMode() != WIFI_OFF && s_status != Status::Off; i++) delay(50);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

Status status() { return s_status; }

String lastError() {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  String e = s_lastError;
  xSemaphoreGive(s_lock);
  return e;
}

bool loop(Round& active, bool syncScreen) {
  // A different round than before (started, finished, discarded)?
  String key = active.active ? active.course + "|" + String(active.holes) + "|" + String(active.gb.track) : "";
  if (key != s_activeKey || (active.active && !s_lastSent.active)) {
    s_activeKey = key;
    s_serial++;
  }

  bool changed = false;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (s_hasResult) {
    s_hasResult = false;
    if (s_resultSerial == s_serial && active.active) {
      // Merge what GameBook now knows into the live round.
      GameBookRound& g = active.gb;
      g.gameId = s_result.gb.gameId;
      g.roundId = s_result.gb.roundId;
      g.groupId = s_result.gb.groupId;
      g.teamId = s_result.gb.teamId;
      g.code = s_result.gb.code;
      g.startTime = s_result.gb.startTime;
      g.configVersion = s_result.gb.configVersion;
      memcpy(g.sent, s_result.gb.sent, sizeof(g.sent));  // holes changed since are resent
      changed = true;
    }
  }
  bool update = !sameRoundData(active, s_lastSent) || syncScreen != s_syncScreen;
  if (update) {
    s_snapshot = active;
    s_snapshotSerial = s_serial;
    s_syncScreen = syncScreen;
    s_lastSent = active;
  }
  xSemaphoreGive(s_lock);
  if (update && s_task) xTaskNotifyGive(s_task);
  return changed;
}

}  // namespace live
