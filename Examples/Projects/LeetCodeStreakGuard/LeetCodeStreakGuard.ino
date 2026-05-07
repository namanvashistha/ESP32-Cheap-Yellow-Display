#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <SPI.h>
#include <time.h>
#include <sys/time.h>
#include <vector>
#include <algorithm>
// FreeSansBold24pt7b / 18pt7b are bundled with TFT_eSPI and auto-included via TFT_eSPI.h.

#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33

// ===== Config =====
const char* LEETCODE_USERNAME = "namanvashistha15";
const unsigned long FETCH_INTERVAL_MS = 5UL * 60UL * 1000UL; // 5 min
const long NTP_GMT_OFFSET = 0;       // keep system clock in UTC
const int  NTP_DAYLIGHT_OFFSET = 0;
const char* NTP_SERVER = "pool.ntp.org";

// Day-boundary timezone for "today" / streak logic.
// IST (UTC+5:30) = 19800. UTC = 0. PST = -28800. etc.
// submissionCalendar keys are UTC midnight; we map local date X to UTC key X
// (heuristic: works for submissions made during waking hours of your TZ).
const long LOCAL_TZ_OFFSET_SEC = 0;

// ===== WiFi =====
struct WiFiCredentials {
  const char* ssid;
  const char* password;
};

WiFiCredentials wifiList[] = {
    {"DIR-825-5E23", "38078446"},
    {"MobileHotspot", "hotspotpw"},
    {"OfficeNet", "securepass"}
};
const int wifiCount = sizeof(wifiList) / sizeof(wifiList[0]);

// ===== Display =====
TFT_eSPI tft = TFT_eSPI();
SPIClass mySpi = SPIClass(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

const int SCREEN_WIDTH = 240;
const int SCREEN_HEIGHT = 320;

// ===== Colors =====
const uint16_t COL_BG          = TFT_BLACK;
const uint16_t COL_DIVIDER     = 0x2104;        // very dark grey
const uint16_t COL_TEXT_DIM    = 0x8410;        // mid grey
const uint16_t COL_TEXT        = TFT_WHITE;
const uint16_t COL_OK          = 0x07E6;        // green
const uint16_t COL_GLOW_3      = 0x05C4;        // mid green
const uint16_t COL_GLOW_2      = 0x02A2;        // dim green
const uint16_t COL_GLOW_1      = 0x0140;        // dimmest green
const uint16_t COL_CAUTION     = TFT_YELLOW;
const uint16_t COL_WARN        = TFT_ORANGE;
const uint16_t COL_DANGER      = TFT_RED;
const uint16_t COL_NEUTRAL     = TFT_SKYBLUE;

// ===== State =====
struct StreakState {
  int currentStreak = 0;
  int bestStreak = 0;
  bool solvedToday = false;
  time_t lastSolveTs = 0;
  bool hasData = false;
  String errorMsg = "";
};
StreakState state;

unsigned long lastFetchMs = 0;
unsigned long lastTickMs = 0;
bool blinkPhase = false;
unsigned long lastBlinkMs = 0;

// Countdown cache (invalidate on full re-render)
String  cdLastBuf = "";
uint16_t cdLastColor = 0;

// Forward decls
time_t parseHttpDate(const String& s);
void drawGiantTick(int cx, int cy, int span, uint16_t color);
void thickLine(int x1, int y1, int x2, int y2, int thick, uint16_t color);
void drawBorder(uint16_t color, bool useGlow);
void drawBigNumber(const String& n, uint16_t color);
void drawCountdown(uint16_t color);
void centerText(const char* s, int y);

// ===== Setup =====
void setup() {
  Serial.begin(115200);
  delay(300);

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(COL_BG);

  mySpi.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(mySpi);

  drawBoot("Connecting WiFi...");
  connectToWiFi();

  drawBoot("Syncing time...");
  configTime(NTP_GMT_OFFSET, NTP_DAYLIGHT_OFFSET, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  waitForTime();
  time_t boot = time(nullptr);
  Serial.printf("[boot] time after sync: %ld\n", (long)boot);

  if (boot > 1700000000) {
    long ist = (long)boot + 19800;
    int hh = (ist / 3600) % 24;
    int mm = (ist / 60) % 60;
    char tbuf[32];
    snprintf(tbuf, sizeof(tbuf), "Time: %02d:%02d IST", hh, mm);
    drawBoot(tbuf);
    delay(1000);
  }

  drawBoot("Fetching streak...");
  fetchStreak();
  renderAll();
}

// ===== Loop =====
void loop() {
  // Touch wakes/forces refresh
  if (ts.tirqTouched() && ts.touched()) {
    ts.getPoint();
    fetchStreak();
    renderAll();
    delay(300);
  }

  // Periodic data refresh
  if (millis() - lastFetchMs > FETCH_INTERVAL_MS) {
    fetchStreak();
    renderAll();
  }

  // Countdown ticker — minute resolution; check every 10s, redraw only on change
  if (state.hasData && !state.solvedToday && millis() - lastTickMs > 10000) {
    lastTickMs = millis();
    int level = urgencyLevel();
    drawCountdown(levelColor(level));
  }

  // Border blink — asymmetric on/off durations per urgency level
  // blinkPhase = false → currently lit, true → currently dark
  if (state.hasData && !state.solvedToday) {
    int level = urgencyLevel();
    if (level >= 2) {
      unsigned long onMs  = (level == 3) ? 500 : 5000;  // lit duration
      unsigned long offMs = (level == 3) ? 500 : 200;                        // dark duration
      unsigned long need  = blinkPhase ? offMs : onMs;
      if (millis() - lastBlinkMs > need) {
        lastBlinkMs = millis();
        blinkPhase = !blinkPhase;
        drawBorder(blinkPhase ? COL_BG : levelColor(level), false);
      }
    }
  }

  // Re-render whole screen when urgency level changes (color transitions)
  static int lastLevel = -1;
  int curLevel = state.solvedToday ? -2 : urgencyLevel();
  if (state.hasData && curLevel != lastLevel) {
    lastLevel = curLevel;
    renderAll();
  }
}

// ===== WiFi =====
void connectToWiFi() {
  for (int i = 0; i < wifiCount; i++) {
    String trying = String("Trying: ") + wifiList[i].ssid;
    drawBoot(trying.c_str());

    WiFi.begin(wifiList[i].ssid, wifiList[i].password);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
      delay(300);
    }
    if (WiFi.status() == WL_CONNECTED) {
      String ok = String("Connected: ") + wifiList[i].ssid;
      drawBoot(ok.c_str());
      delay(800);
      return;
    }
  }
}

void waitForTime() {
  unsigned long start = millis();
  while (millis() - start < 15000) {
    time_t t = time(nullptr);
    if (t > 1700000000) return; // synced
    delay(300);
  }
  Serial.println("[time] NTP timed out — will fall back to HTTP Date header");
}

// Parse RFC 1123 date "Wed, 08 May 2026 03:31:42 GMT" into epoch seconds (UTC).
time_t parseHttpDate(const String& s) {
  if (s.length() < 29) return 0;
  int day = s.substring(5, 7).toInt();
  String monStr = s.substring(8, 11);
  int year = s.substring(12, 16).toInt();
  int hour = s.substring(17, 19).toInt();
  int minute = s.substring(20, 22).toInt();
  int second = s.substring(23, 25).toInt();

  const char* MON = "JanFebMarAprMayJunJulAugSepOctNovDec";
  int month = -1;
  for (int i = 0; i < 12; i++) {
    if (monStr[0] == MON[i*3] && monStr[1] == MON[i*3+1] && monStr[2] == MON[i*3+2]) {
      month = i;
      break;
    }
  }
  if (month < 0 || year < 2024) return 0;

  struct tm t;
  memset(&t, 0, sizeof(t));
  t.tm_year = year - 1900;
  t.tm_mon = month;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min = minute;
  t.tm_sec = second;

  // Interpret as UTC: temporarily set TZ=UTC, then mktime.
  setenv("TZ", "UTC0", 1);
  tzset();
  return mktime(&t);
}

// ===== Fetch =====
void fetchStreak() {
  lastFetchMs = millis();

  if (WiFi.status() != WL_CONNECTED) {
    state.errorMsg = "no wifi";
    return;
  }

  HTTPClient http;
  http.begin("https://leetcode.com/graphql");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("User-Agent", "Mozilla/5.0");
  const char* hdrs[] = { "Date" };
  http.collectHeaders(hdrs, 1);

  String body = String("{\"query\":\"query userProfileCalendar($username: String!) { matchedUser(username: $username) { userCalendar { submissionCalendar } } }\",\"variables\":{\"username\":\"") + LEETCODE_USERNAME + "\"}}";

  int code = http.POST(body);
  if (code != 200) {
    state.errorMsg = "HTTP " + String(code);
    http.end();
    return;
  }

  // If NTP didn't sync, bootstrap clock from HTTP Date header (UTC).
  if (time(nullptr) < 1700000000) {
    String dateHdr = http.header("Date");
    time_t fromHdr = parseHttpDate(dateHdr);
    if (fromHdr > 1700000000) {
      struct timeval tv = { .tv_sec = fromHdr, .tv_usec = 0 };
      settimeofday(&tv, nullptr);
      Serial.printf("[time] bootstrapped from HTTP Date: %ld\n", (long)fromHdr);
    } else {
      Serial.printf("[time] HTTP Date parse failed: '%s'\n", dateHdr.c_str());
    }
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(16384);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    state.errorMsg = "json err";
    return;
  }

  JsonObject cal = doc["data"]["matchedUser"]["userCalendar"];
  if (cal.isNull()) {
    state.errorMsg = "user?";
    return;
  }

  String calStr = cal["submissionCalendar"].as<String>();

  // Parse the encoded JSON-string of timestamps -> counts
  DynamicJsonDocument calDoc(8192);
  if (deserializeJson(calDoc, calStr)) {
    state.errorMsg = "cal parse";
    return;
  }

  std::vector<time_t> days;
  days.reserve(64);
  for (JsonPair kv : calDoc.as<JsonObject>()) {
    time_t ts = (time_t) strtoll(kv.key().c_str(), nullptr, 10);
    if (ts > 0) days.push_back(ts);
  }
  std::sort(days.begin(), days.end());

  // Today's UTC midnight
  time_t now = time(nullptr);
  if (now < 1700000000) { // sanity: NTP didn't sync
    state.errorMsg = "no time";
    Serial.printf("[time] now=%ld looks unsynced\n", (long)now);
    return;
  }
  // "today" = UTC key matching the local date (TZ-shifted)
  time_t today = ((now + LOCAL_TZ_OFFSET_SEC) / 86400) * 86400;
  Serial.printf("[time] now=%ld today=%ld (tz_offset=%lds)\n", (long)now, (long)today, LOCAL_TZ_OFFSET_SEC);
  Serial.printf("[cal] %d days, last=%ld\n", (int)days.size(), days.empty() ? 0L : (long)days.back());

  state.solvedToday = false;
  for (auto t : days) if (t == today) { state.solvedToday = true; break; }

  state.lastSolveTs = days.empty() ? 0 : days.back();

  // Best streak: longest run of consecutive 86400-spaced days
  int best = 0, run = 0;
  time_t prev = 0;
  for (auto t : days) {
    if (run == 0 || t - prev == 86400) run++;
    else run = 1;
    if (run > best) best = run;
    prev = t;
  }
  state.bestStreak = best;

  // Current streak: walk back from latest day if it's today or yesterday
  int cur = 0;
  if (!days.empty()) {
    time_t yesterday = today - 86400;
    time_t last = days.back();
    if (last == today || last == yesterday) {
      cur = 1;
      time_t expected = last - 86400;
      for (int i = (int)days.size() - 2; i >= 0; i--) {
        if (days[i] == expected) {
          cur++;
          expected -= 86400;
        } else if (days[i] < expected) {
          break;
        }
      }
    }
  }
  state.currentStreak = cur;
  state.hasData = true;
  state.errorMsg = "";
  Serial.printf("[streak] current=%d best=%d solvedToday=%d\n", cur, state.bestStreak, state.solvedToday);
}

// ===== Urgency =====
// 0 = solved (or no data), 1 = >12h yellow, 2 = 6-12h orange slow blink, 3 = <6h red fast blink
int urgencyLevel() {
  if (!state.hasData) return 0;
  if (state.solvedToday) return 0;
  long secs = secondsToMidnight();
  // Thresholds tuned for IST user with UTC offset (LOCAL_TZ_OFFSET_SEC = 0).
  // UTC midnight = IST 5:30 AM next day.
  // IST 10pm → 27000s left (red). IST 6pm → 41400s left (orange).
  if (secs < 27000) return 3;
  if (secs < 41400) return 2;
  return 1;
}

uint16_t levelColor(int level) {
  switch (level) {
    case 3: return COL_DANGER;
    case 2: return COL_WARN;
    case 1: return COL_CAUTION;
    default: return COL_OK;
  }
}

// Blink interval in ms. 0 = no blink.
unsigned long blinkIntervalMs(int level) {
  switch (level) {
    case 3: return 500;    // fast (2 Hz)
    case 2: return 5000;   // very slow (every 2s)
    default: return 0;     // no blink
  }
}

long secondsToMidnight() {
  time_t now = time(nullptr);
  long localNow = (long)now + LOCAL_TZ_OFFSET_SEC;
  return 86400 - (localNow % 86400);
}

// ===== Render =====
void drawBoot(const char* msg) {
  tft.fillScreen(COL_BG);
  tft.setTextColor(COL_TEXT_DIM, COL_BG);
  // Auto-shrink size if message would overflow screen width
  int len = strlen(msg);
  int size = 2;
  if (len * 12 > SCREEN_WIDTH - 20) size = 1;
  tft.setTextSize(size);
  int w = len * 6 * size;
  int h = 8 * size;
  tft.setCursor((SCREEN_WIDTH - w) / 2, SCREEN_HEIGHT / 2 - h / 2);
  tft.print(msg);
}

void renderAll() {
  tft.fillScreen(COL_BG);
  cdLastBuf = "";       // invalidate countdown cache so it redraws
  cdLastColor = 0;
  drawScreen();
}

void drawScreen() {
  if (!state.hasData) {
    tft.setTextColor(COL_DANGER, COL_BG);
    tft.setTextSize(2);
    centerText(state.errorMsg.c_str(), SCREEN_HEIGHT / 2);
    return;
  }

  uint16_t numColor, borderColor;
  bool useGlow = false;

  if (state.solvedToday) {
    numColor = COL_OK;
    borderColor = COL_OK;
    useGlow = true;
  } else {
    int level = urgencyLevel();
    numColor = levelColor(level);
    borderColor = numColor;
  }

  drawBorder(borderColor, useGlow);
  drawBigNumber(String(state.currentStreak), numColor);

  if (state.solvedToday) {
    drawGiantTick(SCREEN_WIDTH / 2, SCREEN_HEIGHT * 80 / 100, 55, COL_OK);
  } else {
    drawCountdown(numColor);
  }
}

void drawBorder(uint16_t color, bool useGlow) {
  int bx = 8, by = 8, bw = SCREEN_WIDTH - 16, bh = SCREEN_HEIGHT - 16;
  uint16_t shades[5];
  if (useGlow) {
    shades[0] = COL_GLOW_1;
    shades[1] = COL_GLOW_2;
    shades[2] = COL_GLOW_3;
    shades[3] = color;
    shades[4] = color;
  } else {
    for (int i = 0; i < 5; i++) shades[i] = color;
  }
  for (int i = 0; i < 5; i++) {
    tft.drawRoundRect(bx + i, by + i, bw - 2*i, bh - 2*i, 16 - i, shades[i]);
  }
}

void drawBigNumber(const String& n, uint16_t color) {
  int len = n.length();
  int ts;
  if (len <= 1)      ts = 4;
  else if (len == 2) ts = 3;
  else if (len == 3) ts = 2;
  else               ts = 1;

  tft.setFreeFont(&FreeSansBold24pt7b);
  tft.setTextSize(ts);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(color, COL_BG);
  tft.drawString(n, SCREEN_WIDTH / 2, SCREEN_HEIGHT * 42 / 100);

  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
  tft.setTextFont(1);
}

void drawCountdown(uint16_t color) {
  int cy = SCREEN_HEIGHT * 80 / 100;

  long secs = secondsToMidnight();
  int hh = secs / 3600;
  int mm = (secs % 3600) / 60;
  char buf[24];
  snprintf(buf, sizeof(buf), "%02dh %02dm", hh, mm); // fixed-width

  // Skip redraw if nothing changed (eliminates flicker between minute boundaries)
  String cur = String(buf);
  if (cur == cdLastBuf && color == cdLastColor) return;
  cdLastBuf = cur;
  cdLastColor = color;

  tft.setFreeFont(&FreeSansBold18pt7b);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(color, COL_BG);
  tft.setTextPadding(SCREEN_WIDTH - 60); // atomic clear+draw, no flicker
  tft.drawString(buf, SCREEN_WIDTH / 2, cy);
  tft.setTextPadding(0);

  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(1);
}

// Solid thick stroke: render as a filled quad (two triangles) + round caps.
void thickLine(int x1, int y1, int x2, int y2, int thick, uint16_t color) {
  float dx = x2 - x1;
  float dy = y2 - y1;
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 1) return;
  float pdx = -dy / len * (thick / 2.0f);
  float pdy = dx / len * (thick / 2.0f);
  int ax = (int)(x1 + pdx), ay = (int)(y1 + pdy);
  int bx = (int)(x1 - pdx), by = (int)(y1 - pdy);
  int cx = (int)(x2 - pdx), cy = (int)(y2 - pdy);
  int dx2 = (int)(x2 + pdx), dy2 = (int)(y2 + pdy);
  tft.fillTriangle(ax, ay, bx, by, cx, cy, color);
  tft.fillTriangle(ax, ay, cx, cy, dx2, dy2, color);
  tft.fillCircle(x1, y1, thick / 2, color);
  tft.fillCircle(x2, y2, thick / 2, color);
}

// Asymmetric V tick centered at (cx, cy).
void drawGiantTick(int cx, int cy, int span, uint16_t color) {
  int leftX  = cx - span / 2;
  int leftY  = cy;
  int dipX   = cx - span / 6;
  int dipY   = cy + span / 4;
  int rightX = cx + span / 2;
  int rightY = cy - span / 3;
  int thick  = span / 8;
  thickLine(leftX, leftY, dipX, dipY, thick, color);
  thickLine(dipX, dipY, rightX, rightY, thick, color);
}

void centerText(const char* s, int y) {
  int size = tft.textsize;
  int w = strlen(s) * 6 * size;
  tft.setCursor((SCREEN_WIDTH - w) / 2, y);
  tft.print(s);
}
