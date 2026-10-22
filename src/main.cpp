// Copilot credit meter: ESP32-WROOM-32 + SSD1306 128x64 I2C (SDA=21, SCL=22)
// Shows % used, used/limit and time until reset (00:00 UTC on the 1st of each month).
//
// WiFi password and Copilot token are stored in plain NVS.

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <U8g2lib.h>
#include <ArduinoJson.h>
#include <esp_err.h>
#include <esp_http_client.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <time.h>

extern "C" esp_err_t esp_crt_bundle_attach(void* conf);

// ---------- config ----------
static const int PIN_RESET_BTN = 0;               // BOOT button: hold 3 s at power-up to wipe settings
static const uint32_t REFRESH_MS = 15UL * 60 * 1000; // API poll interval
static const uint32_t REDRAW_MS = 30UL * 1000;      // countdown redraw
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// ---------- settings storage (plain NVS, initialised by the Arduino core) ----------
static String nvsGet(const char* key) {
  nvs_handle_t h;
  if (nvs_open("cfg", NVS_READONLY, &h) != ESP_OK) return "";
  size_t len = 0;
  String out;
  if (nvs_get_str(h, key, NULL, &len) == ESP_OK && len > 0) {
    char* buf = (char*)malloc(len);
    if (buf && nvs_get_str(h, key, buf, &len) == ESP_OK) out = buf;
    free(buf);
  }
  nvs_close(h);
  return out;
}

static void nvsSet(const char* key, const String& val) {
  nvs_handle_t h;
  if (nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_str(h, key, val.c_str());
  nvs_commit(h);
  nvs_close(h);
}

static void nvsWipe() {
  nvs_handle_t h;
  if (nvs_open("cfg", NVS_READWRITE, &h) == ESP_OK) { nvs_erase_all(h); nvs_commit(h); nvs_close(h); }
}

// ---------- display ----------
static void showMessage(const char* l1, const char* l2 = "", const char* l3 = "", const char* l4 = "") {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, l1);
  u8g2.drawStr(0, 24, l2);
  u8g2.drawStr(0, 38, l3);
  u8g2.drawStr(0, 52, l4);
  u8g2.sendBuffer();
}

static void secondsToReset(time_t now, long& days, long& hours, char* dateLabel, size_t n) {
  struct tm t;
  gmtime_r(&now, &t);
  t.tm_mon += 1; t.tm_mday = 1; t.tm_hour = t.tm_min = t.tm_sec = 0; t.tm_isdst = 0;
  time_t reset = mktime(&t);  // TZ is UTC on Arduino-ESP32 by default
  long diff = (long)(reset - now);
  if (diff < 0) diff = 0;
  days = diff / 86400;
  hours = (diff % 86400) / 3600;
  strftime(dateLabel, n, "%b %d", &t);
}

static void drawUsage(float used, float limit) {
  float pct = limit > 0 ? used * 100.0f / limit : 0;
  char big[12], a[20], b[20], r[32], d[12];
  snprintf(big, sizeof big, "%.1f%%", pct);
  snprintf(a, sizeof a, "%.1f", used);
  snprintf(b, sizeof b, "of %ld", (long)(limit + 0.5f));
  long days, hours;
  secondsToReset(time(nullptr), days, hours, d, sizeof d);
  snprintf(r, sizeof r, "Reset %ldd %ldh (%s)", days, hours, d);

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 9, "Copilot AI credits");
  u8g2.setFont(u8g2_font_helvB18_tf);
  u8g2.drawStr(0, 34, big);
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(78, 24, a);
  u8g2.drawStr(78, 35, b);
  u8g2.drawFrame(0, 40, 128, 8);
  int w = (int)(126.0f * constrain(pct, 0.0f, 100.0f) / 100.0f);
  u8g2.drawBox(1, 41, w, 6);
  u8g2.drawStr(0, 62, r);
  u8g2.sendBuffer();
}

// ---------- GitHub API ----------
static String httpBody;

static esp_err_t httpEvent(esp_http_client_event_t* e) {
  if (e->event_id == HTTP_EVENT_ON_DATA && httpBody.length() + e->data_len < 32768)
    httpBody.concat((const char*)e->data, e->data_len);
  return ESP_OK;
}

// returns 200 on success, HTTP status on API error, -1 on transport/parse error
static int fetchUsage(const String& token, bool businessAccount, float& usedOut) {
  httpBody = "";
  httpBody.reserve(4096);

  esp_http_client_config_t cfg = {};
  cfg.url = "https://api.github.com/copilot_internal/user";
  cfg.event_handler = httpEvent;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;  // verifies the server cert against the Mozilla CA bundle
  cfg.timeout_ms = 15000;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) {
    Serial.println("GitHub API: failed to initialize HTTP client");
    return -1;
  }
  String auth = "Bearer " + token;
  esp_http_client_set_header(c, "Authorization", auth.c_str());
  esp_http_client_set_header(c, "Accept", "application/json");
  esp_http_client_set_header(c, "User-Agent", "copilot-meter-esp32");
  esp_err_t err = esp_http_client_perform(c);
  int status = esp_http_client_get_status_code(c);
  int socketErr = esp_http_client_get_errno(c);
  esp_http_client_cleanup(c);
  if (err != ESP_OK) {
    Serial.printf("GitHub API transport error: %s (0x%x), socket errno=%d\n",
                  esp_err_to_name(err), (unsigned int)err, socketErr);
    return -1;
  }
  if (status != 200) {
    Serial.printf("Copilot internal API returned HTTP %d\n", status);
    return status;
  }

  JsonDocument doc;
  DeserializationError jsonError = deserializeJson(doc, httpBody);
  if (jsonError) {
    Serial.printf("GitHub API JSON parse error: %s\n", jsonError.c_str());
    return -1;
  }
  Serial.print("Copilot quota snapshots: ");
  serializeJson(doc["quota_snapshots"], Serial);
  Serial.println();
  const char* quotaCategory = businessAccount ? "premium_interactions" : "chat";
  JsonVariantConst creditsUsed = doc["quota_snapshots"][quotaCategory]["credits_used"];
  if (!creditsUsed.is<float>()) {
    Serial.printf("Copilot internal API response has no %s.credits_used field\n", quotaCategory);
    return -1;
  }
  usedOut = creditsUsed.as<float>();
  Serial.printf("Copilot %s quota usage: %.3f credits\n", quotaCategory, usedOut);
  return 200;
}

// ---------- provisioning portal ----------
static void runPortal() {
  char pw[9];
  snprintf(pw, sizeof pw, "%08lu", (unsigned long)(esp_random() % 100000000UL));
  WiFi.mode(WIFI_AP);
  WiFi.softAP("CopilotMeter-Setup", pw);
  showMessage("Setup mode", "WiFi: CopilotMeter-", "Setup  pass:", pw);
  Serial.printf("Setup AP pass: %s  -> http://192.168.4.1\n", pw);

  static WebServer server(80);
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html",
      "<meta name=viewport content='width=device-width'><h3>Copilot meter setup</h3>"
      "<form method=POST action=/save>"
      "WiFi SSID<br><input name=ssid><br>WiFi password<br><input name=wpass><br>"
      "Copilot session token (not a GitHub PAT)<br><input name=token><br>"
      "Copilot account type<br><select name=account>"
      "<option value=personal>Personal</option>"
      "<option value=business>Business / Enterprise</option>"
      "</select><br>"
      "Monthly credit limit<br><input name=limit value=1000><br><br>"
      "<button>Save</button></form>");
  });
  server.on("/save", HTTP_POST, []() {
    nvsSet("ssid", server.arg("ssid"));
    nvsSet("wpass", server.arg("wpass"));
    nvsSet("token", server.arg("token"));
    nvsSet("account", server.arg("account") == "business" ? "business" : "personal");
    nvsSet("limit", server.arg("limit"));
    server.send(200, "text/html", "Saved. Rebooting...");
    delay(800);
    ESP.restart();
  });
  server.begin();
  for (;;) { server.handleClient(); delay(5); }
}

// ---------- main ----------
static String gToken;
static bool gBusinessAccount = false;
static float gLimit = 1000;
static float gUsed = -1;
static uint32_t lastFetch = 0, lastDraw = 0;
static uint32_t resetPressedAt = 0;
static bool resetPromptShown = false;

static bool pollSettingsReset() {
  if (digitalRead(PIN_RESET_BTN) != LOW) {
    if (resetPressedAt != 0) {
      resetPressedAt = 0;
      resetPromptShown = false;
      lastDraw = 0;
    }
    return false;
  }

  if (resetPressedAt == 0) {
    resetPressedAt = millis();
    resetPromptShown = false;
  }
  if (!resetPromptShown) {
    showMessage("Hold BOOT 3 sec", "Release to cancel");
    resetPromptShown = true;
  }
  if (millis() - resetPressedAt >= 3000) {
    nvsWipe();
    showMessage("Settings cleared", "Restarting...");
    delay(1000);
    ESP.restart();
  }
  return true;
}

static bool connectWifi(const String& ssid, const String& pass) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    pollSettingsReset();
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

static bool syncTime() {
  configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
  uint32_t t0 = millis();
  while (time(nullptr) < 1700000000 && millis() - t0 < 15000) {
    pollSettingsReset();
    delay(250);
  }
  return time(nullptr) >= 1700000000;
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_RESET_BTN, INPUT_PULLUP);
  u8g2.begin();
  showMessage("Copilot meter", "starting...");

  String ssid = nvsGet("ssid"), wpass = nvsGet("wpass");
  gToken = nvsGet("token");
  gBusinessAccount = nvsGet("account") == "business";
  String lim = nvsGet("limit");
  if (lim.toInt() > 0) gLimit = lim.toInt();

  if (ssid.isEmpty() || gToken.isEmpty()) runPortal();

  showMessage("Connecting WiFi", ssid.c_str());
  if (!connectWifi(ssid, wpass)) {
    showMessage("WiFi failed", "hold BOOT 3s at", "power-up to reset");
    delay(10000);
    ESP.restart();
  }
  showMessage("Syncing clock...");
  if (!syncTime()) { showMessage("NTP failed"); delay(5000); ESP.restart(); }
}

void loop() {
  if (pollSettingsReset()) return;

  uint32_t now = millis();
  if (gUsed < 0 || now - lastFetch >= REFRESH_MS) {
    lastFetch = now;
    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
    float used;
    int rc = fetchUsage(gToken, gBusinessAccount, used);
    if (rc == 200) {
      gUsed = used;
      Serial.printf("Quota display: %.3f of %.3f credits (%.3f%%)\n",
                    gUsed, gLimit, gLimit > 0 ? gUsed * 100.0f / gLimit : 0.0f);
    } else if (gUsed < 0) {
      char m[24];
      snprintf(m, sizeof m, rc > 0 ? "HTTP %d" : "Bad API data", rc);
      showMessage("API error", m, rc == 401 || rc == 403 ? "check Copilot token" : "retrying...");
      lastFetch = now - REFRESH_MS + 30000;  // retry in 30 s
      delay(1000);
      return;
    }
    lastDraw = 0;
  }
  if (gUsed >= 0 && now - lastDraw >= REDRAW_MS) {
    lastDraw = now;
    drawUsage(gUsed, gLimit);
  }
  delay(200);
}
