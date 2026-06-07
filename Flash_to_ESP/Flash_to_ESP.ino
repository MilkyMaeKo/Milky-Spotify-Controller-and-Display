#include <WiFiManager.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Base64.h>
#include <U8g2lib.h>
#include <esp_system.h>
#include <ESP32Encoder.h>

// !! PASTE CREDENTIALS HERE
const char* CLIENT_ID     = "PASTE_CLIENT_ID_HERE";
const char* CLIENT_SECRET = "PASTE_CLIENT_SECRET_HERE";
const char* REFRESH_TOKEN = "PASTE_REFRESH_TOKEN_HERE";

// ── Display ───────────────────────────────────────────────────────────────
U8G2_SSD1322_NHD_256X64_F_4W_HW_SPI u8g2(U8G2_R0, 5, 16, 17);

// ── Encoder ───────────────────────────────────────────────────────────────
ESP32Encoder encoder;
long lastEncoderPos = 0;

// ── Pins ──────────────────────────────────────────────────────────────────
#define PIN_ENC_SW  33
#define PIN_PLAY    25
#define PIN_NEXT    26
#define PIN_PREV    27
#define PIN_REPEAT  14
#define PIN_SHUFFLE 13

// ── Control queue ─────────────────────────────────────────────────────────
// Commands are drained and deduplicated before any API call is made,
// so spamming buttons only results in one net API call per drain cycle.
enum ControlCmd {
  CMD_PLAY, CMD_PAUSE, CMD_NEXT_TRACK, CMD_PREV_TRACK,
  CMD_REPEAT, CMD_SHUFFLE, CMD_VOLUME, CMD_MUTE, CMD_UNMUTE
};
struct ControlMsg { ControlCmd cmd; int value; };
QueueHandle_t controlQueue;

// ── Token ─────────────────────────────────────────────────────────────────
String accessToken = "";
unsigned long tokenExpiry = 0;

// ── Track state ───────────────────────────────────────────────────────────
SemaphoreHandle_t trackMutex;
String trackName      = "Connecting...";
String artistName     = "";
String nextTrackName  = "";
String nextArtistName = "";
int    progress       = 0;
int    duration       = 1;
bool   isPlaying      = false;
int    volume         = 50;
int    volumePreMute  = 50;
bool   isMuted        = false;
String repeatState    = "off";
bool   shuffleState   = false;

// ── Volume debounce ───────────────────────────────────────────────────────
// Accumulate encoder ticks and only send to API after 1s of no movement.
// Also prevents API poll from snapping volume back while user is turning knob.
int  pendingVolume   = -1;   // -1 = nothing pending
unsigned long volumeSendAt = 0;
const int VOLUME_DEBOUNCE_MS = 1000;

// ── Timers ────────────────────────────────────────────────────────────────
unsigned long lastQueuePoll   = 0;
unsigned long rateLimitExpiry = 0;
unsigned long lastDraw        = 0;
unsigned long lastSecond      = 0;

// ── Scroll ────────────────────────────────────────────────────────────────
int    scrollOffset    = 0;
int    scrollCycleLen  = 0;
bool   scrollNeeded    = false;
unsigned long lastScroll       = 0;
unsigned long scrollPauseUntil = 0;
String lastRenderedTrack       = "";
String marqueeStr              = "";
const int SCROLL_AREA  = 236;
const int SCROLL_SPEED = 40;
const int SCROLL_PAUSE = 2000;

// ── Sanitise ──────────────────────────────────────────────────────────────
String sanitise(String s) {
  String out = "";
  for (int i = 0; i < s.length(); i++) {
    unsigned char c = s[i];
    if (c >= 32 && c < 128) out += (char)c;
    else if (c > 128) out += '?';
  }
  return out;
}

// ── Token ─────────────────────────────────────────────────────────────────
bool refreshAccessToken() {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  http.begin(client, "https://accounts.spotify.com/api/token");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.addHeader("Authorization", "Basic " + base64::encode(String(CLIENT_ID) + ":" + String(CLIENT_SECRET)));
  int code = http.POST("grant_type=refresh_token&refresh_token=" + String(REFRESH_TOKEN));
  Serial.printf("Token: %d\n", code);
  if (code != 200) { http.end(); return false; }
  DynamicJsonDocument doc(512);
  deserializeJson(doc, http.getString());
  http.end();
  accessToken = doc["access_token"].as<String>();
  tokenExpiry = millis() + ((doc["expires_in"].as<int>() - 60) * 1000UL);
  return true;
}

void ensureValidToken() {
  if (accessToken == "" || millis() > tokenExpiry) refreshAccessToken();
}

// ── Spotify API helpers ───────────────────────────────────────────────────
void spotifyPut(String ep) {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  http.begin(client, "https://api.spotify.com" + ep);
  http.addHeader("Authorization", "Bearer " + accessToken);
  http.addHeader("Content-Length", "0");
  http.PUT(""); http.end();
}
void spotifyPost(String ep) {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  http.begin(client, "https://api.spotify.com" + ep);
  http.addHeader("Authorization", "Bearer " + accessToken);
  http.addHeader("Content-Length", "0");
  http.POST(""); http.end();
}

// ── Fetch now playing ─────────────────────────────────────────────────────
void fetchNowPlaying() {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  http.begin(client, "https://api.spotify.com/v1/me/player");
  http.addHeader("Authorization", "Bearer " + accessToken);
  const char* hdrs[] = {"Retry-After"};
  http.collectHeaders(hdrs, 1);
  int code = http.GET();
  Serial.printf("Player: %d\n", code);

  if (code == 429) {
    String ra = http.header("Retry-After");
    int wait = ra.length() > 0 ? ra.toInt() : 60;
    http.end();
    rateLimitExpiry = millis() + (wait * 1000UL);
    Serial.printf("Rate limited %ds\n", wait);
    return;
  }
  if (code == 204) {
    http.end();
    xSemaphoreTake(trackMutex, portMAX_DELAY);
    isPlaying = false;
    xSemaphoreGive(trackMutex);
    return;
  }
  if (code == 401) { http.end(); accessToken = ""; return; }
  if (code != 200) { http.end(); return; }

  DynamicJsonDocument filter(512);
  filter["is_playing"]                 = true;
  filter["progress_ms"]                = true;
  filter["shuffle_state"]              = true;
  filter["repeat_state"]               = true;
  filter["device"]["volume_percent"]   = true;
  filter["item"]["name"]               = true;
  filter["item"]["duration_ms"]        = true;
  filter["item"]["artists"][0]["name"] = true;

  DynamicJsonDocument doc(2048);
  deserializeJson(doc, http.getString(), DeserializationOption::Filter(filter));
  http.end();

  xSemaphoreTake(trackMutex, portMAX_DELAY);

  // Detect track change — reset queue poll so next track shows immediately
  String newTrack = sanitise(doc["item"]["name"].as<String>());
  if (newTrack != trackName && trackName != "Connecting..." && trackName.length() > 0) {
    lastQueuePoll = 0;
  }

  isPlaying    = doc["is_playing"];
  progress     = doc["progress_ms"].as<int>() / 1000;
  duration     = doc["item"]["duration_ms"].as<int>() / 1000;
  trackName    = newTrack;
  artistName   = sanitise(doc["item"]["artists"][0]["name"].as<String>());
  shuffleState = doc["shuffle_state"].as<bool>();
  repeatState  = doc["repeat_state"].as<String>();

  // Only update volume from API if user isn't actively adjusting the knob
  if (pendingVolume < 0 && !isMuted) {
    volume = doc["device"]["volume_percent"].as<int>();
  }

  xSemaphoreGive(trackMutex);
}

// ── Fetch queue ───────────────────────────────────────────────────────────
void fetchQueue() {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  http.begin(client, "https://api.spotify.com/v1/me/player/queue");
  http.addHeader("Authorization", "Bearer " + accessToken);
  int code = http.GET();
  if (code != 200) { http.end(); return; }
  DynamicJsonDocument filter(200);
  filter["queue"][0]["name"]               = true;
  filter["queue"][0]["artists"][0]["name"] = true;
  DynamicJsonDocument doc(1024);
  deserializeJson(doc, http.getString(), DeserializationOption::Filter(filter));
  http.end();
  xSemaphoreTake(trackMutex, portMAX_DELAY);
  nextTrackName  = sanitise(doc["queue"][0]["name"].as<String>());
  nextArtistName = sanitise(doc["queue"][0]["artists"][0]["name"].as<String>());
  xSemaphoreGive(trackMutex);
}

// ── Process control queue (core 0) ────────────────────────────────────────
// Drains the ENTIRE queue first, deduplicates, then makes the minimum
// number of API calls. Prevents button spam from stacking up seconds of work.
void processControlQueue() {
  if (uxQueueMessagesWaiting(controlQueue) == 0) return;

  // Accumulate state across all queued messages
  int  playPauseNet = 0;   // net play/pause presses; >0=play, <0=pause, 0=no change
  int  nextCount    = 0;   // how many times next was pressed
  int  prevCount    = 0;   // how many times prev was pressed
  bool hasRepeat    = false; int repeatVal  = 0;
  bool hasShuffle   = false; int shuffleVal = 0;
  int  lastVolume   = -1;  // -1 = no volume cmd pending
  bool hasMute      = false;
  bool hasUnmute    = false; int unmuteVal  = 0;

  ControlMsg msg;
  while (xQueueReceive(controlQueue, &msg, 0) == pdTRUE) {
    switch (msg.cmd) {
      case CMD_PLAY:        playPauseNet++;  break;
      case CMD_PAUSE:       playPauseNet--;  break;
      case CMD_NEXT_TRACK:  nextCount++;     break;
      case CMD_PREV_TRACK:  prevCount++;     break;
      case CMD_REPEAT:      hasRepeat  = true; repeatVal  = msg.value; break;
      case CMD_SHUFFLE:     hasShuffle = true; shuffleVal = msg.value; break;
      case CMD_VOLUME:      lastVolume = msg.value; hasMute = false; hasUnmute = false; break;
      case CMD_MUTE:        hasMute    = true; hasUnmute = false; lastVolume = -1; break;
      case CMD_UNMUTE:      hasUnmute  = true; hasMute   = false; lastVolume = -1; unmuteVal = msg.value; break;
    }
  }

  bool needsFetch = false;

  // Prev/Next — respect count so intentional multi-skips still work
  for (int i = 0; i < prevCount; i++) {
    spotifyPost("/v1/me/player/previous");
    if (i < prevCount - 1) vTaskDelay(pdMS_TO_TICKS(300));
    needsFetch = true;
  }
  for (int i = 0; i < nextCount; i++) {
    spotifyPost("/v1/me/player/next");
    if (i < nextCount - 1) vTaskDelay(pdMS_TO_TICKS(300));
    needsFetch = true;
  }

  // Net play/pause — if even number of presses, net is 0 = no API call needed
  if (playPauseNet > 0)      { spotifyPut("/v1/me/player/play");  needsFetch = true; }
  else if (playPauseNet < 0) { spotifyPut("/v1/me/player/pause"); needsFetch = true; }

  if (hasRepeat) {
    String states[] = {"off", "context", "track"};
    spotifyPut("/v1/me/player/repeat?state=" + states[constrain(repeatVal, 0, 2)]);
    needsFetch = true;
  }
  if (hasShuffle) {
    spotifyPut("/v1/me/player/shuffle?state=" + String(shuffleVal ? "true" : "false"));
    needsFetch = true;
  }
  if (hasMute)               { spotifyPut("/v1/me/player/volume?volume_percent=0"); needsFetch = true; }
  if (hasUnmute)             { spotifyPut("/v1/me/player/volume?volume_percent=" + String(unmuteVal)); needsFetch = true; }
  if (lastVolume >= 0)       { spotifyPut("/v1/me/player/volume?volume_percent=" + String(lastVolume)); needsFetch = true; }

  // Single re-fetch at the end of ALL commands, not after each one
  if (needsFetch) {
    vTaskDelay(pdMS_TO_TICKS(200));
    fetchNowPlaying();
  }
}

// ── Spotify task (core 0) ─────────────────────────────────────────────────
void spotifyTask(void* param) {
  while (true) {
    processControlQueue();

    if (millis() < rateLimitExpiry) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    ensureValidToken();
    fetchNowPlaying();

    if (millis() >= rateLimitExpiry && millis() - lastQueuePoll > 10000) {
      fetchQueue();
      lastQueuePoll = millis();
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// ── Button state ──────────────────────────────────────────────────────────
struct Btn { int pin; unsigned long lastPress; bool lastState; };

// Explicit forward declaration so the compiler knows it exists with this struct type
bool justPressed(Btn &b); 

Btn btns[] = {
  {PIN_ENC_SW,   0, true},
  {PIN_PLAY,     0, true},
  {PIN_NEXT,     0, true},
  {PIN_PREV,     0, true},
  {PIN_REPEAT,   0, true},
  {PIN_SHUFFLE, 0, true},
};

const int DEBOUNCE_MS = 250;

bool justPressed(Btn &b) {
  bool state = digitalRead(b.pin);
  unsigned long now = millis();
  bool pressed = (state == LOW && b.lastState == HIGH && now - b.lastPress > DEBOUNCE_MS);
  if (pressed) b.lastPress = now;
  b.lastState = state;
  return pressed;
}

// ── Handle controls (core 1 — non-blocking, just queues commands) ─────────
void handleControls() {
  ControlMsg msg;

  // Encoder push = mute/unmute
  if (justPressed(btns[0])) {
    xSemaphoreTake(trackMutex, portMAX_DELAY);
    bool muted = isMuted;
    int  vol   = volume;
    int  pre   = volumePreMute;
    if (muted) { volume = pre;  isMuted = false; }
    else       { volumePreMute = vol; volume = 0; isMuted = true; }
    xSemaphoreGive(trackMutex);
    msg = muted ? ControlMsg{CMD_UNMUTE, pre} : ControlMsg{CMD_MUTE, 0};
    xQueueSend(controlQueue, &msg, 0);
    pendingVolume = -1; // cancel any pending encoder volume
  }

  // Play/Pause — optimistic UI flip
  if (justPressed(btns[1])) {
    xSemaphoreTake(trackMutex, portMAX_DELAY);
    bool playing = isPlaying;
    isPlaying = !playing;
    xSemaphoreGive(trackMutex);
    msg = {playing ? CMD_PAUSE : CMD_PLAY, 0};
    xQueueSend(controlQueue, &msg, 0);
  }

  if (justPressed(btns[2])) { msg = {CMD_NEXT_TRACK, 0}; xQueueSend(controlQueue, &msg, 0); }
  if (justPressed(btns[3])) { msg = {CMD_PREV_TRACK, 0}; xQueueSend(controlQueue, &msg, 0); }

  // Repeat — optimistic UI cycle, encode target state in value
  if (justPressed(btns[4])) {
    xSemaphoreTake(trackMutex, portMAX_DELAY);
    String next = (repeatState == "off") ? "context" : (repeatState == "context") ? "track" : "off";
    repeatState = next;
    xSemaphoreGive(trackMutex);
    int val = (next == "off") ? 0 : (next == "context") ? 1 : 2;
    msg = {CMD_REPEAT, val};
    xQueueSend(controlQueue, &msg, 0);
  }

  // Shuffle — optimistic UI flip
  if (justPressed(btns[5])) {
    xSemaphoreTake(trackMutex, portMAX_DELAY);
    shuffleState = !shuffleState;
    int val = shuffleState ? 1 : 0;
    xSemaphoreGive(trackMutex);
    msg = {CMD_SHUFFLE, val};
    xQueueSend(controlQueue, &msg, 0);
  }

  // Volume encoder — accumulate changes, send after 1s idle
  // Does NOT queue immediately; pendingVolume is flushed in loop()
  long pos  = encoder.getCount();
  long diff = pos - lastEncoderPos;
  if (diff != 0) {
    xSemaphoreTake(trackMutex, portMAX_DELAY);
    int newVol = constrain(volume + (int)(diff * 3), 0, 100);
    isMuted    = (newVol == 0);
    volume     = newVol;
    xSemaphoreGive(trackMutex);
    pendingVolume = newVol;
    volumeSendAt  = millis() + VOLUME_DEBOUNCE_MS;
    lastEncoderPos = pos;
  }
}

// ── Scroll ────────────────────────────────────────────────────────────────
void updateScroll(String track) {
  unsigned long now = millis();
  if (track != lastRenderedTrack) {
    u8g2.setFont(u8g2_font_7x13B_tr);
    int textW = u8g2.getStrWidth(track.c_str());
    int sepW  = u8g2.getStrWidth("   -   ");
    scrollNeeded      = textW > SCROLL_AREA;
    scrollOffset      = 0;
    scrollPauseUntil  = now + SCROLL_PAUSE;
    lastRenderedTrack = track;
    if (scrollNeeded) {
      marqueeStr    = track + "   -   " + track;
      scrollCycleLen = textW + sepW;
    }
  }
  if (!scrollNeeded || now < scrollPauseUntil) return;
  if (now - lastScroll > SCROLL_SPEED) {
    scrollOffset++;
    if (scrollOffset >= scrollCycleLen) {
      scrollOffset     = 0;
      scrollPauseUntil = now + SCROLL_PAUSE;
    }
    lastScroll = now;
  }
}

// ── Icons ─────────────────────────────────────────────────────────────────
void drawRepeatIcon(int x, int y, String state) {
  if (state == "off") return;
  u8g2.drawHLine(x+2, y-5, 6);
  u8g2.drawHLine(x+2, y,   6);
  u8g2.drawVLine(x+1, y-4, 3);
  u8g2.drawVLine(x+8, y-3, 3);
  u8g2.drawPixel(x+9, y-4);
  u8g2.drawPixel(x+9, y-6);
  u8g2.drawPixel(x,   y-1);
  u8g2.drawPixel(x,   y+1);
  if (state == "track") {
    u8g2.setFont(u8g2_font_4x6_tr);
    u8g2.drawStr(x+11, y, "1");
  }
}

void drawShuffleIcon(int x, int y, bool on) {
  if (!on) return;
  u8g2.drawLine(x, y-4, x+5, y);
  u8g2.drawPixel(x+4, y-1);
  u8g2.drawPixel(x+5, y-2);
  u8g2.drawLine(x, y, x+5, y-4);
  u8g2.drawPixel(x+1, y-4);
  u8g2.drawPixel(x,   y-3);
}

// ── Draw screen ───────────────────────────────────────────────────────────
void drawScreen() {
  xSemaphoreTake(trackMutex, portMAX_DELAY);
  String tName    = trackName;
  String tArtist  = artistName;
  String tNext    = nextTrackName.length() > 0
                    ? nextTrackName + " - " + nextArtistName : "---";
  int    tProg    = progress;
  int    tDur     = duration;
  bool   tPlay    = isPlaying;
  int    tVol     = volume;
  bool   tMuted   = isMuted;
  String tRepeat  = repeatState;
  bool   tShuffle = shuffleState;
  xSemaphoreGive(trackMutex);

  updateScroll(tName);
  u8g2.clearBuffer();

  int bl = 6;
  u8g2.drawHLine(2,        2,  bl); u8g2.drawVLine(2,     2,  bl);
  u8g2.drawHLine(256-2-bl, 2,  bl); u8g2.drawVLine(256-3, 2,  bl);
  u8g2.drawHLine(2,        61, bl); u8g2.drawVLine(2,     55, bl);
  u8g2.drawHLine(256-2-bl, 61, bl); u8g2.drawVLine(256-3, 55, bl);

  u8g2.setFont(u8g2_font_4x6_tr);
  u8g2.drawStr(10, 9, tPlay ? "NOW PLAYING" : "PAUSED");
  drawRepeatIcon(140, 9, tRepeat);
  drawShuffleIcon(160, 9, tShuffle);
  char volStr[12];
  sprintf(volStr, tMuted ? "MUTE" : "VOL %d%%", tVol);
  u8g2.setFont(u8g2_font_4x6_tr);
  u8g2.drawStr(200, 9, volStr);
  u8g2.drawHLine(10, 11, 236);

  u8g2.setFont(u8g2_font_7x13B_tr);
  u8g2.setClipWindow(10, 12, 246, 27);
  u8g2.drawStr(10 - scrollOffset, 24,
               scrollNeeded ? marqueeStr.c_str() : tName.c_str());
  u8g2.setMaxClipWindow();

  u8g2.setFont(u8g2_font_5x7_tr);
  char truncArtist[36];
  strncpy(truncArtist, tArtist.c_str(), 35); truncArtist[35] = '\0';
  u8g2.drawStr(10, 33, truncArtist);

  char progStr[6], durStr[6];
  sprintf(progStr, "%d:%02d", tProg / 60, tProg % 60);
  sprintf(durStr,  "%d:%02d", tDur  / 60, tDur  % 60);
  u8g2.setFont(u8g2_font_4x6_tr);
  u8g2.drawStr(10, 42, progStr);
  u8g2.drawStr(226, 42, durStr);
  u8g2.drawFrame(30, 37, 190, 5);
  int filled = (tDur > 0) ? (188 * tProg / tDur) : 0;
  u8g2.drawBox(31, 38, filled, 3);

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
  u8g2.drawStr(10, 42, "SPOTIFY HARDWARE INTERFACE :3");
  u8g2.sendBuffer();
}

// ── Setup ─────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.printf("Reset reason: %d\n", esp_reset_reason());

  u8g2.setBusClock(10000000);
  u8g2.begin();
  u8g2.setContrast(175);

  trackMutex   = xSemaphoreCreateMutex();
  controlQueue = xQueueCreate(20, sizeof(ControlMsg));

  pinMode(PIN_ENC_SW,  INPUT_PULLUP);
  pinMode(PIN_PLAY,    INPUT_PULLUP);
  pinMode(PIN_NEXT,    INPUT_PULLUP);
  pinMode(PIN_PREV,    INPUT_PULLUP);
  pinMode(PIN_REPEAT,  INPUT_PULLUP);
  pinMode(PIN_SHUFFLE, INPUT_PULLUP);

  ESP32Encoder::useInternalWeakPullResistors = puType::up;
  encoder.attachHalfQuad(19, 32);
  encoder.setCount(0);

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
  delay(1000);

  xTaskCreatePinnedToCore(spotifyTask, "spotify", 16384, NULL, 1, NULL, 0);
}

// ── Loop (core 1) ─────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  handleControls();

  // Flush pending volume after 1s of encoder idle
  if (pendingVolume >= 0 && now >= volumeSendAt) {
    ControlMsg msg = {CMD_VOLUME, pendingVolume};
    xQueueSend(controlQueue, &msg, 0);
    pendingVolume = -1;
  }

  if (now - lastSecond > 1000) {
    xSemaphoreTake(trackMutex, portMAX_DELAY);
    if (isPlaying && progress < duration) progress++;
    xSemaphoreGive(trackMutex);
    lastSecond = now;
  }

  if (now - lastDraw > 50) {
    drawScreen();
    lastDraw = now;
  }
}