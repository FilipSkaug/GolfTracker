#include "web.h"

#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>

#include "config.h"
#include "storage.h"
#include "web_page.h"

namespace web {

static WebServer server(80);
static bool s_running = false;
static bool s_routesAdded = false;
// While the station keeps searching for a network that isn't there, the radio
// hops channels and the GolfTracker network becomes invisible. So only try for
// a short while, then fall back to the access point alone.
static const uint32_t kHomeTimeoutMs = 15000;
static bool s_homeTrying = false;
static uint32_t s_homeStartedAt = 0;

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

// Joining home Wi-Fi as well lets the phone stay on its normal network (with
// internet for course search) and still reach the display at golf.local.
static void connectHome() {
  const Settings& s = storage::settings();
  WiFi.disconnect(false);
  s_homeTrying = false;
  if (s.wifiSsid.isEmpty()) {
    WiFi.mode(WIFI_AP);
    return;
  }
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(false);
  WiFi.begin(s.wifiSsid.c_str(), s.wifiPass.c_str());
  s_homeTrying = true;
  s_homeStartedAt = millis();
  Serial.printf("Sync: joining home Wi-Fi '%s'\n", s.wifiSsid.c_str());
}

static void checkHome() {
  if (!s_homeTrying) return;
  if (WiFi.status() == WL_CONNECTED) {
    s_homeTrying = false;
    Serial.printf("Sync: on home Wi-Fi, ip=%s\n", WiFi.localIP().toString().c_str());
  } else if (millis() - s_homeStartedAt > kHomeTimeoutMs) {
    s_homeTrying = false;
    WiFi.disconnect(false);
    WiFi.mode(WIFI_AP);
    Serial.println("Sync: home Wi-Fi not found, using the GolfTracker network only");
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
  server.on("/api/rounds/delete", HTTP_POST, [] {
    if (storage::deleteHistory(server.arg("id").toInt())) sendJson(200, "{}");
    else sendError(404, "Round not found");
  });

  server.on("/api/settings", HTTP_GET, [] { sendJson(200, storage::settingsJson()); });
  server.on("/api/settings", HTTP_POST, [] {
    String oldSsid = storage::settings().wifiSsid;
    String oldPass = storage::settings().wifiPass;
    String err = storage::updateSettingsJson(server.arg("plain"));
    if (!err.isEmpty()) return sendError(400, err);
    sendJson(200, storage::settingsJson());
    if (storage::settings().wifiSsid != oldSsid || storage::settings().wifiPass != oldPass) connectHome();
  });

  server.onNotFound([] {
    // Anything else (e.g. iOS probing) gets the app.
    server.sendHeader("Location", "/");
    server.send(302);
  });
}

void start() {
  if (s_running) return;
  bool modeOk = WiFi.mode(WIFI_AP);
  bool apOk = WiFi.softAP(WIFI_SSID, WIFI_PASS);
  Serial.printf("Sync: mode=%d softAP=%d channel=%d ip=%s\n", modeOk, apOk, WiFi.channel(),
                WiFi.softAPIP().toString().c_str());
  connectHome();
  MDNS.begin(MDNS_NAME);
  MDNS.addService("http", "tcp", 80);
  if (!s_routesAdded) {
    addRoutes();
    s_routesAdded = true;
  }
  server.begin();
  s_running = true;
  Serial.printf("Sync: Wi-Fi '%s' up, http://%s.local or http://%s\n", WIFI_SSID, MDNS_NAME,
                WiFi.softAPIP().toString().c_str());
}

void stop() {
  if (!s_running) return;
  s_homeTrying = false;
  server.stop();
  MDNS.end();
  WiFi.disconnect(true);
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  s_running = false;
}

void handle() {
  if (!s_running) return;
  checkHome();
  server.handleClient();
}

bool phoneConnected() { return s_running && WiFi.softAPgetStationNum() > 0; }

String homeIp() {
  if (!s_running || WiFi.status() != WL_CONNECTED) return "";
  return WiFi.localIP().toString();
}

}  // namespace web
