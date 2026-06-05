#include <WiFiManager.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Base64.h>
#include <U8g2lib.h>

// !! PASTE CREDENTIALS HERE
const char* CLIENT_ID     = "PASTE_CLIENT_ID_HERE";
const char* CLIENT_SECRET = "PASTE_CLIENT_SECRET_HERE";
const char* REFRESH_TOKEN = "PASTE_REFRESH_TOKEN_HERE";

// Display
U8G2_SSD1322_NHD_256X64_F_8080 u8g2(
  U8G2_R0,
  /*d0=*/ 12, /*d1=*/ 13, /*d2=*/ 14, /*d3=*/ 27,
  /*d4=*/ 26, /*d5=*/ 25, /*d6=*/ 33, /*d7=*/ 32,
  /*enable=*/ 4, /*cs=*/ 5, /*dc=*/ 16, /*reset=*/ 17
);

// Token
String accessToken = "";
unsigned long tokenExpiry = 0;

// Track state
SemaphoreHandle_t trackMutex;
String trackName      = "Connecting...";
String artistName     = "";
String nextTrackName  = "";
String nextArtistName = "";
int    progress       = 0;
int    duration       = 1;
bool   isPlaying      = false;
int    volume         = 50;

// Queue poll timer
unsigned long lastQueuePoll = 0;

// Scroll state
int   scrollOffset     = 0;
int   scrollWidth      = 0;
bool  scrollNeeded     = false;
unsigned long lastScroll       = 0;
unsigned long scrollPauseUntil = 0;
String lastRenderedTrack       = "";
const int SCROLL_AREA  = 236;
const int SCROLL_SPEED = 80;
const int SCROLL_PAUSE = 2000;

// Timers
unsigned long lastDraw   = 0;
unsigned long lastSecond = 0;

// ── Sanitise ───────────────────────────────────────────────────────────────
String sanitise(String input) {
  String out = "";
  for (int i = 0; i < input.length(); i++) {
    unsigned char c = input[i];
    if (c >= 32 && c < 128) out += (char)c;
    else if (c > 128) out += '?';
  }
  return out;
}

// ── Token ──────────────────────────────────────────────────────────────────
bool refreshAccessToken() {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  http.begin(client, "https://accounts.spotify.com/api/token");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.addHeader("Authorization", "Basic " + base64::encode(String(CLIENT_ID) + ":" + String(CLIENT_SECRET)));
  int code = http.POST("grant_type=refresh_token&refresh_token=" + String(REFRESH_TOKEN));
  if (code != 200) { http.end(); return false; }
  StaticJsonDocument<512> doc;
  deserializeJson(doc, http.getString());
  http.end();
  accessToken = doc["access_token"].as<String>();
  tokenExpiry = millis() + ((doc["expires_in"].as<int>() - 60) * 1000UL);
  return true;
}

void ensureValidToken() {
  if (accessToken == "" || millis() > tokenExpiry) refreshAccessToken();
}

// ── Spotify polling ────────────────────────────────────────────────────────
void fetchNowPlaying() {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  http.begin(client, "https://api.spotify.com/v1/me/player");
  http.addHeader("Authorization", "Bearer " + accessToken);
  int code = http.GET();

  if (code == 204) {
    http.end();
    xSemaphoreTake(trackMutex, portMAX_DELAY);
    isPlaying = false;
    xSemaphoreGive(trackMutex);
    return;
  }
  if (code == 401) { http.end(); accessToken = ""; return; }
  if (code != 200) { http.end(); return; }

  StaticJsonDocument<400> filter;
  filter["is_playing"]                 = true;
  filter["progress_ms"]                = true;
  filter["device"]["volume_percent"]   = true;
  filter["item"]["name"]               = true;
  filter["item"]["duration_ms"]        = true;
  filter["item"]["artists"][0]["name"] = true;

  StaticJsonDocument<2048> doc;
  deserializeJson(doc, http.getString(), DeserializationOption::Filter(filter));
  http.end();

  xSemaphoreTake(trackMutex, portMAX_DELAY);
  isPlaying  = doc["is_playing"];
  progress   = doc["progress_ms"].as<int>() / 1000;
  duration   = doc["item"]["duration_ms"].as<int>() / 1000;
  volume     = doc["device"]["volume_percent"].as<int>();
  trackName  = sanitise(doc["item"]["name"].as<String>());
  artistName = sanitise(doc["item"]["artists"][0]["name"].as<String>());
  xSemaphoreGive(trackMutex);
}

void fetchQueue() {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  http.begin(client, "https://api.spotify.com/v1/me/player/queue");
  http.addHeader("Authorization", "Bearer " + accessToken);
  int code = http.GET();
  if (code != 200) { http.end(); return; }

  StaticJsonDocument<200> filter;
  filter["queue"][0]["name"]               = true;
  filter["queue"][0]["artists"][0]["name"] = true;

  StaticJsonDocument<1024> doc;
  deserializeJson(doc, http.getString(), DeserializationOption::Filter(filter));
  http.end();

  xSemaphoreTake(trackMutex, portMAX_DELAY);
  nextTrackName  = sanitise(doc["queue"][0]["name"].as<String>());
  nextArtistName = sanitise(doc["queue"][0]["artists"][0]["name"].as<String>());
  xSemaphoreGive(trackMutex);
}

void spotifyTask(void* param) {
  while (true) {
    ensureValidToken();
    fetchNowPlaying();
    if (millis() - lastQueuePoll > 1000) {
      fetchQueue();
      lastQueuePoll = millis();
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// ── Scroll ─────────────────────────────────────────────────────────────────
void updateScroll(String currentTrack) {
  unsigned long now = millis();
  if (currentTrack != lastRenderedTrack) {
    u8g2.setFont(u8g2_font_7x13B_tr);
    scrollWidth       = u8g2.getStrWidth(currentTrack.c_str());
    scrollNeeded      = scrollWidth > SCROLL_AREA;
    scrollOffset      = 0;
    scrollPauseUntil  = now + SCROLL_PAUSE;
    lastRenderedTrack = currentTrack;
  }
  if (!scrollNeeded || now < scrollPauseUntil) return;
  if (now - lastScroll > SCROLL_SPEED) {
    scrollOffset++;
    if (scrollOffset > scrollWidth - SCROLL_AREA + 20) {
      scrollOffset     = 0;
      scrollPauseUntil = now + SCROLL_PAUSE;
    }
    lastScroll = now;
  }
}

// ── Display ────────────────────────────────────────────────────────────────
void drawScreen() {
  xSemaphoreTake(trackMutex, portMAX_DELAY);
  String tName      = trackName;
  String tArtist    = artistName;
  String tNext      = nextTrackName + " - " + nextArtistName;
  int    tProg      = progress;
  int    tDur       = duration;
  bool   tPlay      = isPlaying;
  int    tVol       = volume;
  xSemaphoreGive(trackMutex);

  updateScroll(tName);

  u8g2.clearBuffer();

  // Corner brackets
  int bl = 6;
  u8g2.drawHLine(2,        2,       bl); u8g2.drawVLine(2,     2,       bl);
  u8g2.drawHLine(256-2-bl, 2,       bl); u8g2.drawVLine(256-3, 2,       bl);
  u8g2.drawHLine(2,        63-2,    bl); u8g2.drawVLine(2,     63-2-bl, bl);
  u8g2.drawHLine(256-2-bl, 63-2,    bl); u8g2.drawVLine(256-3, 63-2-bl, bl);

  // Status + volume
  u8g2.setFont(u8g2_font_4x6_tr);
  u8g2.drawStr(10, 9, tPlay ? "NOW PLAYING" : "PAUSED");
  char volStr[10];
  sprintf(volStr, "VOL %d%%", tVol);
  u8g2.drawStr(215, 9, volStr);
  u8g2.drawHLine(10, 11, 236);

  // Track name with scroll + clip
  u8g2.setFont(u8g2_font_7x13B_tr);
  u8g2.setClipWindow(10, 12, 246, 27);
  u8g2.drawStr(10 - scrollOffset, 24, tName.c_str());
  u8g2.setMaxClipWindow();

  // Artist
  u8g2.setFont(u8g2_font_5x7_tr);
  char truncArtist[36];
  strncpy(truncArtist, tArtist.c_str(), 35); truncArtist[35] = '\0';
  u8g2.drawStr(10, 33, truncArtist);

  // Progress bar
  char progStr[6], durStr[6];
  sprintf(progStr, "%d:%02d", tProg / 60, tProg % 60);
  sprintf(durStr,  "%d:%02d", tDur  / 60, tDur  % 60);
  u8g2.setFont(u8g2_font_4x6_tr);
  u8g2.drawStr(10, 42, progStr);
  u8g2.drawStr(226, 42, durStr);
  u8g2.drawFrame(30, 37, 190, 5);
  int filled = (tDur > 0) ? (188 * tProg / tDur) : 0;
  u8g2.drawBox(31, 38, filled, 3);

  // Next in queue
  u8g2.drawHLine(10, 45, 236);
  u8g2.setFont(u8g2_font_4x6_tr);
  u8g2.drawStr(10, 54, "NEXT:");
  char nextLine[42];
  strncpy(nextLine, tNext.c_str(), 41); nextLine[41] = '\0';
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(36, 54, nextLine);

  u8g2.sendBuffer();
}

void drawBootScreen(const char* msg) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(10, 30, msg);
  u8g2.drawStr(10, 42, "MILKY SPOTIFY INTERFACE");
  u8g2.sendBuffer();
}

// ── Setup ──────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  u8g2.begin();
  u8g2.setContrast(200);

  trackMutex = xSemaphoreCreateMutex();

  drawBootScreen("CONNECTING TO WIFI...");
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  if (!wm.autoConnect("SpotifyDesk", "spotifydesk")) {
    drawBootScreen("WIFI FAILED. RESTARTING...");
    delay(3000);
    ESP.restart();
  }

  drawBootScreen("AUTHORISING SPOTIFY...");
  ensureValidToken();
  drawBootScreen("READY.");
  delay(500);

  xTaskCreatePinnedToCore(spotifyTask, "spotify", 8192, NULL, 1, NULL, 0);
}

// ── Loop ───────────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  if (now - lastSecond > 1000) {
    xSemaphoreTake(trackMutex, portMAX_DELAY);
    if (isPlaying && progress < duration) progress++;
    xSemaphoreGive(trackMutex);
    lastSecond = now;
  }

  if (now - lastDraw > 33) {
    drawScreen();
    lastDraw = now;
  }
}
