#include "web.h"

#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>

#include "config.h"
#include "gamebook.h"
#include "storage.h"
#include "web_page.h"

namespace web {

static WebServer server(80);
static bool s_running = false;
static bool s_routesAdded = false;
static Mode s_mode = Mode::Wifi;

// Hotspot mode: the display is only a client of the phone's hotspot (no own
// network), so its single radio stays on the hotspot's channel. If the hotspot
// isn't there yet, keep trying until it appears.
static const uint32_t kJoinTimeoutMs = 15000;
static const uint32_t kRetryMs = 10000;
static bool s_joining = false;
static uint32_t s_joinStartedAt = 0;
static uint32_t s_failedAt = 0;
static String s_joinError;  // why joining failed, for the sync screen
static bool s_resendRequested = false;  // "Send to GolfBox again" pressed on the phone

// Scans once so a failure can say *why*: not visible (off, or 5 GHz only)
// versus visible but refusing us (usually the password).
static String diagnose(const String& ssid, wl_status_t status) {
  int n = WiFi.scanNetworks();
  bool seen = false;
  Serial.printf("Sync: join status=%d, %d networks visible:\n", status, n);
  for (int i = 0; i < n; i++) {
    Serial.printf("  '%s' ch%d %ddBm\n", WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i));
    seen |= WiFi.SSID(i) == ssid;
  }
  WiFi.scanDelete();
  if (!seen) return "Hotspot not found";
  if (status == WL_CONNECT_FAILED) return "Wi-Fi password wrong?";
  return "Can't join hotspot";
}

static void sendJson(int code, const String& body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body);
}

static void sendError(int code, const String& msg) {
  String body = "{\"error\":\"";
  for (char c : msg) {
    if (c == '"' || c == '\\') body += '\\';
    body += c;
  }
  sendJson(code, body + "\"}");
}

static void join() {
  const Settings& s = storage::settings();
  WiFi.disconnect(false);
  WiFi.setAutoReconnect(false);
  WiFi.begin(s.wifiSsid.c_str(), s.wifiPass.c_str());
  s_joining = true;
  s_joinStartedAt = millis();
  Serial.printf("Sync: joining '%s'\n", s.wifiSsid.c_str());
}

static void checkJoin() {
  if (s_mode != Mode::Hotspot) return;
  if (WiFi.status() == WL_CONNECTED) {
    if (s_joining) {
      s_joining = false;
      s_joinError = "";
      Serial.printf("Sync: online, open http://%s\n", WiFi.localIP().toString().c_str());
    }
    return;
  }
  if (s_joining) {
    if (millis() - s_joinStartedAt < kJoinTimeoutMs) return;
    s_joining = false;
    wl_status_t status = WiFi.status();
    WiFi.disconnect(false);
    s_joinError = diagnose(storage::settings().wifiSsid, status);
    s_failedAt = millis();
    Serial.printf("Sync: %s, retrying in %lus\n", s_joinError.c_str(), kRetryMs / 1000);
  } else if (s_joinError.isEmpty() || millis() - s_failedAt > kRetryMs) {
    join();  // first try, a retry, or the hotspot went away
  }
}

static void addRoutes() {
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html", WEB_PAGE); });

  server.on("/api/courses", HTTP_GET, [] { sendJson(200, storage::coursesJson()); });
  server.on("/api/courses", HTTP_POST, [] {
    String err = storage::setCoursesJson(server.arg("plain"));
    if (err.isEmpty()) sendJson(200, storage::coursesJson());
    else sendError(400, err);
  });

  server.on("/api/rounds", HTTP_GET, [] { sendJson(200, storage::historyJson()); });
  server.on("/api/rounds/resend", HTTP_POST, [] {
    storage::markSent(server.arg("id").toInt(), false);
    s_resendRequested = true;
    sendJson(200, "{}");
  });
  server.on("/api/rounds/delete", HTTP_POST, [] {
    if (storage::deleteHistory(server.arg("id").toInt())) sendJson(200, "{}");
    else sendError(404, "Round not found");
  });

  // Saving new hotspot details takes effect the next time Hotspot sync starts.
  server.on("/api/settings", HTTP_GET, [] { sendJson(200, storage::settingsJson()); });
  server.on("/api/settings", HTTP_POST, [] {
    String oldEmail = storage::settings().gamebookEmail, oldPass = storage::settings().gamebookPass;
    String err = storage::updateSettingsJson(server.arg("plain"));
    if (!err.isEmpty()) return sendError(400, err);
    if (storage::settings().gamebookEmail != oldEmail || storage::settings().gamebookPass != oldPass) {
      gamebook::logout();
    }
    sendJson(200, storage::settingsJson());
  });

  // GameBook lookups for linking courses; the display fetches them over the hotspot.
  server.on("/api/gamebook/favorites", HTTP_GET, [] {
    String err, json = gamebook::favoritesJson(err);
    if (json.isEmpty()) sendError(503, err);
    else sendJson(200, json);
  });
  server.on("/api/gamebook/course", HTTP_GET, [] {
    String err, json = gamebook::courseJson(server.arg("id").toInt(), err);
    if (json.isEmpty()) sendError(503, err);
    else sendJson(200, json);
  });

  server.onNotFound([] {
    // Anything else (e.g. iOS probing) gets the app.
    server.sendHeader("Location", "/");
    server.send(302);
  });
}

bool hotspotConfigured() { return !storage::settings().wifiSsid.isEmpty(); }

void start(Mode mode) {
  if (s_running) return;
  s_mode = mode == Mode::Hotspot && hotspotConfigured() ? Mode::Hotspot : Mode::Wifi;
  s_joining = false;
  s_joinError = "";
  if (s_mode == Mode::Hotspot) {
    WiFi.mode(WIFI_STA);
    join();
  } else {
    bool modeOk = WiFi.mode(WIFI_AP);
    bool apOk = WiFi.softAP(WIFI_SSID, WIFI_PASS);
    Serial.printf("Sync: GolfTracker Wi-Fi mode=%d softAP=%d ip=%s\n", modeOk, apOk,
                  WiFi.softAPIP().toString().c_str());
  }
  MDNS.begin(MDNS_NAME);
  MDNS.addService("http", "tcp", 80);
  if (!s_routesAdded) {
    addRoutes();
    s_routesAdded = true;
  }
  server.begin();
  s_running = true;
}

void stop() {
  if (!s_running) return;
  s_joining = false;
  server.stop();
  MDNS.end();
  WiFi.disconnect(true);
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  s_running = false;
}

void handle() {
  if (!s_running) return;
  checkJoin();
  server.handleClient();
}

Mode mode() { return s_mode; }

bool phoneConnected() { return s_running && s_mode == Mode::Wifi && WiFi.softAPgetStationNum() > 0; }

bool takeResendRequest() {
  bool r = s_resendRequested;
  s_resendRequested = false;
  return r;
}

String homeError() { return s_running ? s_joinError : ""; }

HomeState homeState() {
  if (!s_running || s_mode != Mode::Hotspot) return HomeState::Off;
  if (WiFi.status() == WL_CONNECTED) return HomeState::Online;
  if (s_joining) return HomeState::Joining;
  return s_joinError.isEmpty() ? HomeState::Joining : HomeState::Failed;
}

String homeIp() {
  if (!s_running || s_mode != Mode::Hotspot || WiFi.status() != WL_CONNECTED) return "";
  return WiFi.localIP().toString();
}

}  // namespace web
