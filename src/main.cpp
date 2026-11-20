// Commute traffic display for M5StickC Plus2
//
// Polls the Google Routes API every 10 minutes during the configured window
// and shows the current drive time on a colored background:
//   green < GREEN_MAX_MIN, yellow GREEN_MAX_MIN..YELLOW_MAX_MIN, red > YELLOW_MAX_MIN.
//
// Buttons:
//   A (big front button)  : fetch now (works any time of day)
//   B (side button)       : cycle demo screens (green/yellow/red) to check colors

#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <sys/time.h>

#include "config.h"
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#define USING_EXAMPLE_SECRETS 1
#endif

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
struct RouteResult {
  bool valid = false;
  int durationSec = 0;     // with live traffic
  int staticSec = 0;       // without traffic
  int distanceM = 0;
  String description;      // e.g. "I-880 N and US-101 S"
  time_t fetchedAt = 0;
};

static RouteResult g_last;
static String g_error;               // last error message (empty = none)
static long g_lastPolledSlot = -1;   // key of the last scheduled check that succeeded
static uint32_t g_retryAtMs = 0;     // millis() before which we won't retry after a failure
static time_t g_manualUntil = 0;     // show the colored result off-hours until this time
static int g_demoIndex = -1;         // -1 = not in demo mode
static uint32_t g_lastDrawMs = 0;

static uint8_t targetBrightness() {
  if (M5.getBoard() == m5::board_t::board_M5StickCPlus ||
      M5.getBoard() == m5::board_t::board_M5StickC) {
    return BRIGHTNESS_ACTIVE_PLUS1;
  }
  return BRIGHTNESS_ACTIVE;
}

static M5Canvas canvas(&M5.Display);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static bool timeValid() {
  return time(nullptr) > 1700000000;  // after Nov 2023
}

static int minutesOfDay(const struct tm& t) { return t.tm_hour * 60 + t.tm_min; }

struct PollWindow { int sh, sm, eh, em, interval; };
static const PollWindow kWindows[] = POLL_WINDOWS;
static const int kNumWindows = sizeof(kWindows) / sizeof(kWindows[0]);

// Returns a unique key (yearday*1440 + minute) for the most recent scheduled
// check at or before `now`, or -1 if `now` is outside every polling window.
static long dueCheckKey(time_t now) {
  struct tm t;
  localtime_r(&now, &t);
  if (!(ACTIVE_DAYS_MASK & (1 << t.tm_wday))) return -1;
  int m = minutesOfDay(t);
  int best = -1;
  for (int i = 0; i < kNumWindows; i++) {
    int s = kWindows[i].sh * 60 + kWindows[i].sm;
    int e = kWindows[i].eh * 60 + kWindows[i].em;
    if (m < s || m > e) continue;
    int due = s + ((m - s) / kWindows[i].interval) * kWindows[i].interval;
    if (due > best) best = due;
  }
  if (best < 0) return -1;
  return (long)(t.tm_year * 366 + t.tm_yday) * 1440L + best;
}

static bool inWindow(time_t now) {
  return dueCheckKey(now) >= 0;
}

static String fmtClock(time_t ts) {
  struct tm t;
  localtime_r(&ts, &t);
  int h = t.tm_hour % 12;
  if (h == 0) h = 12;
  char buf[16];
  snprintf(buf, sizeof(buf), "%d:%02d %s", h, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
  return String(buf);
}

static String fmtHourMin(int hour, int minute) {
  int h = hour % 12;
  if (h == 0) h = 12;
  char buf[16];
  snprintf(buf, sizeof(buf), "%d:%02d %s", h, minute, hour < 12 ? "AM" : "PM");
  return String(buf);
}

static int roundedMinutes(int sec) { return (sec + 30) / 60; }

static uint16_t bgColorFor(int minutes) {
  if (minutes < GREEN_MAX_MIN) return M5.Display.color565(0, 170, 60);    // green
  if (minutes <= YELLOW_MAX_MIN) return M5.Display.color565(255, 210, 0); // yellow
  return M5.Display.color565(210, 20, 20);                                // red
}

static uint16_t fgColorFor(int minutes) {
  if (minutes > YELLOW_MAX_MIN) return TFT_WHITE;
  return TFT_BLACK;
}

// ---------------------------------------------------------------------------
// Networking
// ---------------------------------------------------------------------------
static volatile int g_lastDisconnectReason = 0;

static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    g_lastDisconnectReason = info.wifi_sta_disconnected.reason;
  }
}

static const char* reasonText(int r) {
  switch (r) {
    case 2: case 15: case 204: case 205: case 207: return "wrong password?";
    case 201: return "network not found (5GHz only or out of range?)";
    case 202: return "auth failed";
    case 203: return "assoc failed";
    case 200: return "beacon timeout (weak signal?)";
    default: return "";
  }
}

static bool g_targetIsOpen = false;
static uint8_t g_bestBssid[6] = {0};
static int32_t g_bestChannel = 0;
static bool g_hasBestBssid = false;

static void scanNetworks() {
  Serial.println("[wifi] scanning...");
  int n = WiFi.scanNetworks();
  bool found = false;
  int bestRssi = -1000;
  g_hasBestBssid = false;
  for (int i = 0; i < n; i++) {
    bool match = WiFi.SSID(i) == WIFI_SSID;
    found |= match;
    if (match) {
      if (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) g_targetIsOpen = true;
      if (WiFi.RSSI(i) > bestRssi && WiFi.BSSID(i) != nullptr) {
        bestRssi = WiFi.RSSI(i);
        memcpy(g_bestBssid, WiFi.BSSID(i), 6);
        g_bestChannel = WiFi.channel(i);
        g_hasBestBssid = true;
      }
    }
    Serial.printf("[wifi]  %s '%s' ch%d rssi=%d enc=%d\n", match ? "*" : " ",
                  WiFi.SSID(i).c_str(), (int)WiFi.channel(i), (int)WiFi.RSSI(i), (int)WiFi.encryptionType(i));
  }
  Serial.printf("[wifi] scan: %d networks, target %s\n", n, found ? "FOUND" : "NOT FOUND");
  WiFi.scanDelete();
}

static bool ensureWiFi(uint32_t timeoutMs = 15000) {
  if (WiFi.status() == WL_CONNECTED) return true;
  static bool scanned = false;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // stay awake on USB power for reliable TLS/HTTP
  if (!scanned) { scanNetworks(); scanned = true; }
  Serial.printf("[wifi] connecting to '%s' (ch %d)...\n", WIFI_SSID, (int)g_bestChannel);
  WiFi.disconnect(false, false);
  delay(100);
  g_lastDisconnectReason = 0;
  const char* pass = g_targetIsOpen ? nullptr : WIFI_PASSWORD;
  if (g_hasBestBssid) {
    WiFi.begin(WIFI_SSID, pass, g_bestChannel, g_bestBssid);
  } else {
    WiFi.begin(WIFI_SSID, pass);
  }
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(200);
    M5.update();
  }
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setSleep(false);
    Serial.printf("[wifi] connected, IP %s, ch %d, RSSI %d\n",
                  WiFi.localIP().toString().c_str(), (int)WiFi.channel(), (int)WiFi.RSSI());
    return true;
  }
  // If BSSID-pinned connect failed, rescan next time
  scanned = false;
  Serial.printf("[wifi] failed, status=%d reason=%d %s\n", (int)WiFi.status(),
                (int)g_lastDisconnectReason, reasonText(g_lastDisconnectReason));
  return false;
}

// Parse HTTP "Date: Fri, 20 Nov 2026 16:38:00 GMT" fallback if NTP UDP is slow/blocked
static void syncTimeFromHttpDate(const String& dateHdr) {
  if (timeValid() || dateHdr.length() < 25) return;
  struct tm tmUtc = {};
  if (strptime(dateHdr.c_str(), "%a, %d %b %Y %H:%M:%S", &tmUtc) == nullptr) return;
  // Convert UTC struct tm to time_t
  setenv("TZ", "UTC0", 1);
  tzset();
  time_t t = mktime(&tmUtc);
  setenv("TZ", TZ_INFO, 1);
  tzset();
  if (t > 1700000000) {
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    Serial.println("[time] synced from Google HTTP Date header: " + fmtClock(time(nullptr)));
  }
}

static int postComputeRoutes(const String& body, String& resp) {
  WiFiClientSecure client;
  client.setInsecure();          // skip cert validation (keeps firmware simple)
  client.setHandshakeTimeout(8); // 8 s max for TLS handshake (default is 120 s)
  HTTPClient http;
  http.setConnectTimeout(8000);  // 8 s max for TCP connect
  http.setTimeout(10000);        // 10 s max for HTTP response
  if (!http.begin(client, "https://routes.googleapis.com/directions/v2:computeRoutes")) {
    return -100;
  }
  const char* headerKeys[] = {"Date"};
  http.collectHeaders(headerKeys, 1);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Goog-Api-Key", GOOGLE_MAPS_API_KEY);
  http.addHeader("X-Goog-FieldMask",
                 "routes.duration,routes.staticDuration,routes.distanceMeters,routes.description");

  uint32_t t0 = millis();
  Serial.printf("[api] POST computeRoutes (RSSI %d)\n", (int)WiFi.RSSI());
  int code = http.POST(body);
  if (code > 0 && !timeValid() && http.hasHeader("Date")) {
    syncTimeFromHttpDate(http.header("Date"));
  }
  resp = http.getString();
  http.end();
  Serial.printf("[api] HTTP %d, %u bytes in %lu ms\n", code, resp.length(), (unsigned long)(millis() - t0));
  return code;
}

static bool fetchRoute(RouteResult& out, String& err) {
#ifdef USING_EXAMPLE_SECRETS
  err = "No secrets.h - copy secrets.example.h";
  return false;
#endif
  if (!ensureWiFi()) {
    err = String("WiFi connect failed (") + WIFI_SSID + ")";
    return false;
  }

  JsonDocument req;
  req["origin"]["address"] = ORIGIN_ADDRESS;
  req["destination"]["address"] = DESTINATION_ADDRESS;
  req["travelMode"] = "DRIVE";
  req["routingPreference"] = "TRAFFIC_AWARE";
  req["units"] = "IMPERIAL";
  String body;
  serializeJson(req, body);

  String resp;
  int code = postComputeRoutes(body, resp);
  if (code <= 0) {
    // Stale Wi-Fi session or dropped TLS handshake: reconnect Wi-Fi and retry once immediately
    Serial.println("[api] transport failed, reconnecting WiFi and retrying once...");
    WiFi.disconnect(false, false);
    delay(200);
    if (ensureWiFi(12000)) {
      code = postComputeRoutes(body, resp);
    }
  }

  JsonDocument doc;
  DeserializationError jerr = deserializeJson(doc, resp);

  if (code != 200) {
    String msg = jerr ? String("") : String((const char*)(doc["error"]["message"] | ""));
    if (code < 0) msg = HTTPClient::errorToString(code);
    err = "HTTP " + String(code) + (msg.length() ? ": " + msg : "");
    Serial.println("[api] " + resp);
    return false;
  }
  if (jerr) {
    err = String("Bad JSON: ") + jerr.c_str();
    return false;
  }
  JsonObject r = doc["routes"][0];
  if (r.isNull()) {
    err = "No route returned";
    Serial.println("[api] " + resp);
    return false;
  }

  // Durations come back as strings like "2345s"; atoi stops at the 's'.
  out.durationSec = atoi(r["duration"] | "0");
  out.staticSec = atoi(r["staticDuration"] | "0");
  out.distanceM = r["distanceMeters"] | 0;
  out.description = String((const char*)(r["description"] | ""));
  out.fetchedAt = time(nullptr);
  out.valid = out.durationSec > 0;
  if (!out.valid) {
    err = "Route had no duration";
    return false;
  }
  Serial.printf("[api] %d min (no-traffic %d min), %.1f mi via %s\n",
                roundedMinutes(out.durationSec), roundedMinutes(out.staticSec),
                out.distanceM / 1609.344, out.description.c_str());
  return true;
}

// ---------------------------------------------------------------------------
// Drawing (240 x 135 landscape)
// ---------------------------------------------------------------------------
static void drawResultScreen(const RouteResult& r, bool stale) {
  const int W = canvas.width(), H = canvas.height();
  int mins = roundedMinutes(r.durationSec);
  uint16_t bg = bgColorFor(mins), fg = fgColorFor(mins);

  canvas.fillScreen(bg);
  canvas.setTextColor(fg, bg);

  // Header: label (left) and current clock (right)
  canvas.setTextSize(1.0f);
  canvas.setFont(&fonts::FreeSansBold9pt7b);
  canvas.setTextDatum(top_left);
  canvas.drawString("Arrive home", 6, 4);
  if (timeValid()) {
    canvas.setTextDatum(top_right);
    canvas.drawString(fmtClock(time(nullptr)), W - 6, 4);
  }

  // Main display: arrival time if leaving now (7-segment font) + AM/PM
  time_t arrive = (timeValid() ? time(nullptr) : r.fetchedAt) + r.durationSec;
  struct tm at;
  localtime_r(&arrive, &at);
  int h12 = at.tm_hour % 12;
  if (h12 == 0) h12 = 12;
  char hm[8];
  snprintf(hm, sizeof(hm), "%d:%02d", h12, at.tm_min);
  const char* ampm = at.tm_hour < 12 ? "AM" : "PM";

  canvas.setFont(&fonts::FreeSansBold12pt7b);
  int ampmW = canvas.textWidth(ampm);
  canvas.setFont(&fonts::Font7);
  float size = 1.5f;
  int numW;
  while (true) {  // shrink until it fits the width
    canvas.setTextSize(size);
    numW = canvas.textWidth(hm);
    if (numW + 6 + ampmW <= W - 8 || size <= 1.0f) break;
    size -= 0.05f;
  }
  int x0 = (W - (numW + 6 + ampmW)) / 2;
  int baseY = 98;
  canvas.setTextDatum(bottom_left);
  canvas.drawString(hm, x0, baseY);
  canvas.setTextSize(1.0f);
  canvas.setFont(&fonts::FreeSansBold12pt7b);
  canvas.drawString(ampm, x0 + numW + 6, baseY);

  // Footer: travel time (smaller), traffic delay, and when it was fetched
  canvas.setTextDatum(bottom_left);
  String drive = String(mins) + " min";
  canvas.drawString(drive, 6, H - 3);
  int x = 6 + canvas.textWidth(drive) + 8;
  canvas.setFont(&fonts::FreeSans9pt7b);
  int delay = roundedMinutes(r.durationSec) - roundedMinutes(r.staticSec);
  if (r.staticSec > 0 && delay > 0) canvas.drawString("(+" + String(delay) + " traffic)", x, H - 5);
  canvas.setTextDatum(bottom_right);
  String upd = fmtClock(r.fetchedAt);
  upd = upd.substring(0, upd.indexOf(' '));  // "1:45" (drop AM/PM to save room)
  canvas.drawString((stale ? "@" : "") + upd, W - 6, H - 5);

  canvas.pushSprite(0, 0);
}

static void drawMessageScreen(uint16_t bg, uint16_t fg, const String& title,
                              const String& line1, const String& line2 = "") {
  const int W = canvas.width();
  canvas.fillScreen(bg);
  canvas.setTextColor(fg, bg);
  canvas.setTextSize(1.0f);

  canvas.setFont(&fonts::FreeSansBold9pt7b);
  canvas.setTextDatum(top_left);
  canvas.drawString(ROUTE_LABEL, 6, 4);
  if (timeValid()) {
    canvas.setTextDatum(top_right);
    canvas.drawString(fmtClock(time(nullptr)), W - 6, 4);
  }

  canvas.setFont(&fonts::FreeSansBold12pt7b);
  canvas.setTextDatum(middle_center);
  canvas.drawString(title, W / 2, 50);

  // Wrap line1 across up to 3 rows
  canvas.setFont(&fonts::FreeSans9pt7b);
  canvas.setTextDatum(top_left);
  String rest = line1;
  int y = 72;
  for (int row = 0; row < 3 && rest.length(); row++) {
    int n = rest.length();
    while (n > 0 && canvas.textWidth(rest.substring(0, n)) > W - 12) {
      int sp = rest.lastIndexOf(' ', n - 1);
      n = (sp > 0) ? sp : n - 1;
    }
    canvas.drawString(rest.substring(0, n), 6, y);
    rest = rest.substring(n);
    rest.trim();
    y += 20;
  }
  if (line2.length()) {
    canvas.setTextDatum(bottom_center);
    canvas.drawString(line2, W / 2, canvas.height() - 4);
  }
  canvas.pushSprite(0, 0);
}

static void render() {
  time_t now = time(nullptr);

  static uint8_t lastBrt = 0;
  uint8_t brt = targetBrightness();
  if (brt != lastBrt) {
    M5.Display.setBrightness(brt);
    lastBrt = brt;
  }

  if (g_demoIndex >= 0) {
    static const int demoMins[] = {GREEN_MAX_MIN - 5, (GREEN_MAX_MIN + YELLOW_MAX_MIN) / 2, YELLOW_MAX_MIN + 5};
    RouteResult d;
    d.valid = true;
    d.durationSec = demoMins[g_demoIndex] * 60;
    d.staticSec = 30 * 60;
    d.distanceM = 45000;
    d.description = "DEMO";
    d.fetchedAt = now;
    drawResultScreen(d, false);
    return;
  }

  if (g_last.valid) {
    drawResultScreen(g_last, false);
  } else if (g_error.length()) {
    drawMessageScreen(TFT_NAVY, TFT_WHITE, "Error", g_error, "Retrying...");
  } else if (!timeValid()) {
    drawMessageScreen(TFT_BLACK, TFT_WHITE, "Starting up",
                      WiFi.status() == WL_CONNECTED ? "Checking traffic..." : "Connecting to WiFi...");
  } else {
    drawMessageScreen(TFT_DARKGREY, TFT_WHITE, "Checking traffic", "Contacting Google Maps...");
  }
}

// ---------------------------------------------------------------------------
// Polling logic
// ---------------------------------------------------------------------------
static void doFetch(const char* reason) {
  Serial.printf("[poll] fetching (%s)\n", reason);
  if (!g_last.valid) {
    drawMessageScreen(TFT_DARKGREY, TFT_WHITE, "Checking traffic", "Contacting Google Maps...");
  } else {
    // small "updating" indicator in corner
    M5.Display.fillCircle(M5.Display.width() - 8, M5.Display.height() / 2, 4, TFT_BLUE);
  }
  RouteResult r;
  String err;
  if (fetchRoute(r, err)) {
    g_last = r;
    g_error = "";
    g_retryAtMs = 0;
  } else {
    g_error = err;
    g_retryAtMs = millis() + RETRY_SECONDS * 1000UL;
    Serial.println("[poll] error: " + err);
  }
  render();
  g_lastDrawMs = millis();
}

static void maybePoll() {
  if (g_retryAtMs && (int32_t)(millis() - g_retryAtMs) < 0) return;
  if (!g_last.valid) {
    // Always do the initial fetch right after boot without waiting for NTP
    doFetch("initial");
    if (g_error.length() == 0 && timeValid()) {
      g_lastPolledSlot = dueCheckKey(time(nullptr));
    }
    return;
  }
  if (!timeValid()) return;
  time_t now = time(nullptr);
  long slot = dueCheckKey(now);
  if (slot < 0 || slot == g_lastPolledSlot) return;
  doFetch("scheduled");
  if (g_error.length() == 0) g_lastPolledSlot = slot;
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  Serial.println("\n[boot] Commute traffic display");
  Serial.printf("[boot] board=%d  %s -> %s\n", (int)M5.getBoard(), ORIGIN_ADDRESS, DESTINATION_ADDRESS);

  M5.Display.setRotation(DISPLAY_ROTATION);  // landscape, 240x135
  M5.Display.setBrightness(targetBrightness());
  canvas.setColorDepth(16);
  canvas.createSprite(M5.Display.width(), M5.Display.height());

  drawMessageScreen(TFT_BLACK, TFT_WHITE, "Starting up", "Connecting to WiFi...", WIFI_SSID);

#ifdef USING_EXAMPLE_SECRETS
  g_error = "No secrets.h - copy secrets.example.h";
  Serial.println("[boot] WARNING: secrets.h not found, using placeholders");
#endif

  WiFi.onEvent(onWiFiEvent);
  WiFi.setAutoReconnect(true);
  if (!ensureWiFi()) {
    g_error = String("WiFi connect failed (") + WIFI_SSID + ")";
  }
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com", "time.nist.gov");

  uint32_t start = millis();
  while (!timeValid() && millis() - start < 3000) {
    delay(100);
    M5.update();
  }
  if (timeValid()) {
    Serial.println("[boot] time synced via NTP: " + fmtClock(time(nullptr)));
  } else {
    Serial.println("[boot] NTP still pending, will sync from Google HTTP Date header");
  }
  maybePoll();
  render();
  g_lastDrawMs = millis();
}

void loop() {
  M5.update();

  // Serial 'f' = same as button A (handy for testing over USB)
  bool serialFetch = false;
  while (Serial.available()) {
    if (Serial.read() == 'f') serialFetch = true;
  }

  if (M5.BtnA.wasPressed() || serialFetch) {
    g_demoIndex = -1;
    if (timeValid()) g_manualUntil = time(nullptr) + MANUAL_SHOW_SECONDS;
    doFetch("button A");
  }
  if (M5.BtnB.wasPressed()) {
    g_demoIndex = (g_demoIndex + 1 >= 3) ? -1 : g_demoIndex + 1;
    Serial.printf("[demo] index %d\n", g_demoIndex);
    render();
    g_lastDrawMs = millis();
  }

  if (g_demoIndex < 0) maybePoll();

  // If WiFi dropped and time never synced, keep trying
  if (!timeValid() && WiFi.status() != WL_CONNECTED && millis() % 30000 < 20) ensureWiFi(10000);

  // Redraw every 5 s so the clock stays current and window transitions show up
  if (millis() - g_lastDrawMs > 5000) {
    render();
    g_lastDrawMs = millis();
  }
  delay(20);
}
