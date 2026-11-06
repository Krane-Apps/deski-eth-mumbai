// desky talk: hold the talk button, speak, let go. desky turns your words into text,
// thinks of a reply, says it out loud and types it on the screen, with its pixel face.
// libraries: "Adafruit ST7735 and ST7789" and "ArduinoJson" (v7).
// tools: ESP32S3 Dev Module, PSRAM: OPI PSRAM, Flash Size: 16MB,
// Partition Scheme: 16M Flash (3MB APP/9.9MB FATFS), USB CDC On Boot: Disabled, port: USB-UART.

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <ESP_I2S.h>
#include "mbedtls/base64.h"
#include <time.h>
#include "secrets.h"   // WIFI_NAME, WIFI_PASSWORD, OPENROUTER_KEY, ELEVENLABS_KEY

// ================= settings =================
#define LAYOUT 2   // 1: the old wiring. 2: the compact build from the guide

const char* STT_MODEL  = "openai/whisper-1";
const char* CHAT_MODEL = "openai/gpt-4o-mini";
const char* VOICE_ID   = "JBFqnCBsd6RMkjVDRZzb";
const char* TTS_MODEL  = "eleven_flash_v2_5";
const char* PERSONA =
  "You are Desky, a small cheerful robot that lives on the user's desk and helps them stay focused. "
  "Answer in one or two short spoken sentences. No lists, no emoji, no markdown. "
  "Begin every answer with one mood tag: [happy], [sad], [thinking] or [neutral].";

const float VOLUME    = 0.7;   // 0.1 quiet ... 1.0 loud
const int   MIC_SHIFT = 16;    // 15 louder, 17 quieter (good peaks: 8000 to 30000)
const int   RATE      = 16000;
const int   MAX_TALK_S  = 8;
const int   MAX_REPLY_S = 30;
// ============================================

#if LAYOUT == 2
#define TFT_CS 45
#define TFT_RST 48
#define TFT_DC 47
#define TFT_MOSI 21
#define TFT_SCLK 20
#define TFT_BL 19
#define AMP_LRC 38
#define AMP_BCLK 39
#define AMP_DIN 40
#define AMP_SD 42      // driven high to switch the amp on
#define MIC_WS 3
#define MIC_SCK 14
#define MIC_SD 46
#else
#define TFT_CS 45
#define TFT_RST 21
#define TFT_DC 47
#define TFT_MOSI 20
#define TFT_SCLK 19
#define TFT_BL 38
#define AMP_LRC 41
#define AMP_BCLK 40
#define AMP_DIN 39
#define AMP_SD -1      // old wiring: amp SD tied to 5V
#define MIC_WS 1
#define MIC_SCK 2
#define MIC_SD 42
#endif
#define BTN 0          // talk button and the board's BOOT button

#define LOG(tag, fmt, ...) Serial.printf("[%6lu ms] [%s] " fmt "\n", millis(), tag, ##__VA_ARGS__)

Adafruit_ST7735 tft = Adafruit_ST7735(&SPI, TFT_CS, TFT_DC, TFT_RST);
GFXcanvas16* canvas;
I2SClass mic, spk;

uint8_t* wavBuf;      // 44-byte wav header, then the recording
int16_t* voice;       // the recording, right after the header
size_t   voiceMax;
uint8_t* replyBuf;    // spoken reply from ElevenLabs (16 kHz, 16-bit)
size_t   replyMax;

// ============ face: desky's pixel character ============
const char* DESKY[16] = {
  ".......##.......", ".......##.......", "........#.......", "..############..",
  "..#..........#..", "..#..........#..", "..#..........#..", "..#..........#..",
  "..#..........#..", "..#..........#..", "..#..........#..", "..############..",
  "......#..#......", ".....##..##.....", "................", "................",
};
enum Face { NEUTRAL, BLINK, HAPPY, SAD, FOCUS, DIZZY };
const char* FACES[][5] = {
  { "..##..##..", "..##..##..", "..........", "...####...", ".........." },
  { "..........", "..##..##..", "..........", "...####...", ".........." },
  { "..#....#..", ".#.#..#.#.", "..........", "..#....#..", "...####..." },
  { "..........", "..##..##..", "..........", "...####...", "..#....#.." },
  { ".###..###.", "..##..##..", "..........", "...####...", ".........." },
  { ".#.#..#.#.", "..#....#..", ".#.#..#.#.", "..........", "...####..." },
};

enum State { S_IDLE, S_LISTEN, S_THINK, S_TALK, S_HAPPY, S_SAD, S_OFFLINE };
volatile int state = S_IDLE;
uint16_t BG, INK, UNLIT;

// reply text, typed out while desky speaks
char sayText[400];
char lines[12][27];
volatile int lineCount = 0, textSize = 2, totalChars = 0;
volatile uint32_t typeStart = 0, msPerChar = 45;

void drawDesky(int face, int dy) {
  const int X = 40, Y = 4, CELL = 5;
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 16; x++) {
      bool on = DESKY[y][x] == '#';
      int fy = y - 5, fx = x - 3;
      if (fy >= 0 && fy < 5 && fx >= 0 && fx < 10 && FACES[face][fy][fx] == '#') on = true;
      canvas->fillRect(X + x * CELL, Y + (y + dy) * CELL, CELL - 1, CELL - 1, on ? INK : UNLIT);
    }
}

void label(const char* big, const char* small) {
  canvas->setTextColor(INK);
  canvas->setTextSize(2);
  canvas->setCursor((160 - (int)strlen(big) * 12) / 2, 92);
  canvas->print(big);
  canvas->setTextSize(1);
  canvas->setCursor((160 - (int)strlen(small) * 6) / 2, 114);
  canvas->print(small);
}

void drawTyped() {
  int cw = 6 * textSize, lh = 8 * textSize + 3;
  int due = (millis() - typeStart) / msPerChar + 1, k = 0;
  canvas->setTextColor(INK);
  canvas->setTextSize(textSize);
  for (int i = 0; i < lineCount && k < due; i++)
    for (int j = 0; lines[i][j] && k < due; j++, k++) {
      canvas->setCursor(4 + j * cw, 6 + i * lh);
      canvas->print(lines[i][j]);
    }
}

// owns the screen; runs on its own core so the face keeps moving during web calls
void faceTask(void*) {
  for (int f = 0;; f++) {
    canvas->fillScreen(BG);
    bool wifi = WiFi.status() == WL_CONNECTED;
    int s = state;
    if (s == S_IDLE && !wifi) s = S_OFFLINE;
    switch (s) {
      case S_IDLE:    drawDesky(f % 20 >= 18 ? BLINK : NEUTRAL, (f / 4) % 2); label("DESKY", "hold the button to talk"); break;
      case S_LISTEN:  drawDesky(FOCUS, 0); label("LISTENING", (f / 3) % 2 ? "let go when done" : ""); break;
      case S_THINK:   drawDesky(FOCUS, (f / 2) % 2); label(f % 4 == 0 ? "THINKING" : f % 4 == 1 ? "THINKING." : f % 4 == 2 ? "THINKING.." : "THINKING...", ""); break;
      case S_TALK:    drawTyped(); break;
      case S_HAPPY:   drawDesky(HAPPY, (f / 3) % 2); label(":)", ""); break;
      case S_SAD:     drawDesky(SAD, 0); label("OOPS", "something failed, see Serial"); break;
      case S_OFFLINE: drawDesky(DIZZY, 0); label("NO WIFI", WIFI_NAME); break;
    }
    tft.drawRGBBitmap(0, 0, canvas->getBuffer(), 160, 128);
    vTaskDelay(pdMS_TO_TICKS(s == S_TALK ? 40 : 150));
  }
}

// keep plain letters only, then wrap into lines that fit the screen
bool wrapInto(int cols, int maxLines) {
  lineCount = 0; totalChars = 0;
  char cur[27] = ""; int len = 0;
  for (const char* p = sayText; *p;) {
    while (*p == ' ') p++;
    if (!*p) break;
    const char* w = p;
    while (*p && *p != ' ') p++;
    int wl = min((int)(p - w), cols);
    if ((len ? len + 1 + wl : wl) > cols) {
      if (lineCount >= maxLines) return false;
      strcpy(lines[lineCount++], cur); totalChars += len; len = 0; cur[0] = 0;
    }
    if (len) cur[len++] = ' ';
    memcpy(cur + len, w, wl); len += wl; cur[len] = 0;
  }
  if (len) { if (lineCount >= maxLines) return false; strcpy(lines[lineCount++], cur); totalChars += len; }
  return true;
}

void prepareText(const String& s, uint32_t audioMs) {
  size_t o = 0;
  for (size_t i = 0; i < s.length() && o < sizeof(sayText) - 1; i++) {
    char c = s[i];
    if ((uint8_t)c >= 32 && (uint8_t)c < 127) sayText[o++] = c;   // drops emoji and curly quotes
  }
  sayText[o] = 0;
  textSize = 2;
  if (!wrapInto(13, 6)) { textSize = 1; wrapInto(26, 12); }
  msPerChar = (audioMs && totalChars) ? max(15UL, (unsigned long)(audioMs * 9 / 10 / totalChars)) : 45;
  typeStart = millis();
}

// ============ sound ============
void play(const int16_t* buf, size_t n) {
  int16_t out[256];
  for (size_t off = 0; off < n; off += 256) {
    size_t k = min((size_t)256, n - off);
    for (size_t i = 0; i < k; i++) out[i] = buf[off + i] * VOLUME;
    spk.write((uint8_t*)out, k * 2);
  }
}

void tone(int hz, int ms) {
  int16_t buf[256];
  float ph = 0, step = 2 * PI * hz / RATE;
  for (int done = 0; done < RATE * ms / 1000; done += 256) {
    for (int i = 0; i < 256; i++) { buf[i] = sin(ph) * 6000; ph += step; }
    play(buf, 256);
  }
}

size_t listen(int& peak) {
  size_t n = 0; peak = 0;
  int32_t raw[256];
  while (digitalRead(BTN) == LOW && n < voiceMax) {
    size_t got = mic.readBytes((char*)raw, sizeof(raw)) / 4;
    for (size_t i = 0; i < got && n < voiceMax; i++) {
      int32_t v = constrain(raw[i] >> MIC_SHIFT, -32768, 32767);
      voice[n++] = v;
      peak = max(peak, (int)abs(v));
    }
  }
  return n;
}

void wavHeader(size_t samples) {
  uint32_t bytes = samples * 2;
  uint8_t* h = wavBuf;
  memcpy(h, "RIFF", 4);         *(uint32_t*)(h + 4) = 36 + bytes;
  memcpy(h + 8, "WAVEfmt ", 8); *(uint32_t*)(h + 16) = 16;
  *(uint16_t*)(h + 20) = 1;     *(uint16_t*)(h + 22) = 1;
  *(uint32_t*)(h + 24) = RATE;  *(uint32_t*)(h + 28) = RATE * 2;
  *(uint16_t*)(h + 32) = 2;     *(uint16_t*)(h + 34) = 16;
  memcpy(h + 36, "data", 4);    *(uint32_t*)(h + 40) = bytes;
}

// ============ internet ============
// collects a download straight into a memory buffer
struct Sink : Stream {
  uint8_t* buf; size_t cap, len = 0;
  Sink(uint8_t* b, size_t c) : buf(b), cap(c) {}
  size_t write(uint8_t b) override { if (len < cap) buf[len++] = b; return 1; }
  size_t write(const uint8_t* d, size_t n) override { size_t k = min(n, cap - len); memcpy(buf + len, d, k); len += k; return n; }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};

// one POST with a fresh secure connection and one retry; returns the http code
int post(const char* tag, const String& url, const char* keyHeader, const String& key,
         const uint8_t* body, size_t len, String* text, Sink* bin = nullptr) {
  for (int attempt = 1; attempt <= 2; attempt++) {
    WiFiClientSecure net;
    net.setInsecure();   // no certificate check; fine for a hackathon demo
    HTTPClient http;
    http.setConnectTimeout(10000);
    http.setTimeout(30000);
    if (!http.begin(net, url)) return -100;
    http.addHeader("Content-Type", "application/json");
    http.addHeader(keyHeader, key);
    int code = http.POST((uint8_t*)body, len);
    if (code == 200 && bin) http.writeToStream(bin);
    else if (code > 0 && text) *text = http.getString();
    http.end();
    if (code > 0) return code;
    LOG(tag, "attempt %d failed: %s (wifi %d dBm)", attempt, HTTPClient::errorToString(code).c_str(), WiFi.RSSI());
    delay(800);
  }
  return -1;
}

String speechToText(size_t samples) {
  wavHeader(samples);
  size_t wavLen = 44 + samples * 2, b64 = 4 * ((wavLen + 2) / 3);
  char head[128];
  snprintf(head, sizeof(head), "{\"model\":\"%s\",\"input_audio\":{\"format\":\"wav\",\"data\":\"", STT_MODEL);
  size_t hl = strlen(head);
  char* body = (char*)ps_malloc(hl + b64 + 8);
  if (!body) { LOG("STT", "no memory"); return ""; }
  memcpy(body, head, hl);
  size_t used = 0;
  mbedtls_base64_encode((uint8_t*)body + hl, b64 + 1, &used, wavBuf, wavLen);
  strcpy(body + hl + used, "\"}}");
  String resp;
  uint32_t t = millis();
  int code = post("STT", "https://openrouter.ai/api/v1/audio/transcriptions", "Authorization",
                  String("Bearer ") + OPENROUTER_KEY, (uint8_t*)body, strlen(body), &resp);
  free(body);
  LOG("STT", "http %d in %lu ms", code, millis() - t);
  if (code != 200) { LOG("STT", "server said: %s", resp.c_str()); return ""; }
  JsonDocument doc;
  if (deserializeJson(doc, resp)) return "";
  String words = doc["text"] | "";
  words.trim();
  LOG("STT", "you said: \"%s\"", words.c_str());
  return words;
}

// the model has no clock, so every question carries the real date and time (india time, from the internet)
String nowText() {
  struct tm t;
  if (!getLocalTime(&t, 200)) return "";
  char buf[80];
  strftime(buf, sizeof(buf), " Right now it is %A, %d %B %Y, %I:%M %p in India.", &t);
  return buf;
}

// the last few turns, so desky remembers the conversation
String memRole[6], memText[6];
int memCount = 0;
void remember(const char* role, const String& t) {
  if (memCount == 6) { for (int i = 1; i < 6; i++) { memRole[i - 1] = memRole[i]; memText[i - 1] = memText[i]; } memCount--; }
  memRole[memCount] = role; memText[memCount] = t; memCount++;
}

String reply(const String& said) {
  JsonDocument req;
  req["model"] = CHAT_MODEL;
  req["max_tokens"] = 120;
  JsonArray m = req["messages"].to<JsonArray>();
  JsonObject sys = m.add<JsonObject>(); sys["role"] = "system"; sys["content"] = String(PERSONA) + nowText();
  for (int i = 0; i < memCount; i++) { JsonObject o = m.add<JsonObject>(); o["role"] = memRole[i]; o["content"] = memText[i]; }
  JsonObject u = m.add<JsonObject>(); u["role"] = "user"; u["content"] = said;
  String body, resp;
  serializeJson(req, body);
  uint32_t t = millis();
  int code = post("CHAT", "https://openrouter.ai/api/v1/chat/completions", "Authorization",
                  String("Bearer ") + OPENROUTER_KEY, (uint8_t*)body.c_str(), body.length(), &resp);
  LOG("CHAT", "http %d in %lu ms", code, millis() - t);
  if (code != 200) { LOG("CHAT", "server said: %s", resp.c_str()); return ""; }
  JsonDocument doc;
  if (deserializeJson(doc, resp)) return "";
  String answer = doc["choices"][0]["message"]["content"] | "";
  if (answer.length()) { remember("user", said); remember("assistant", answer); }
  LOG("CHAT", "desky says: \"%s\"", answer.c_str());
  return answer;
}

size_t textToSpeech(const String& words) {
  JsonDocument req;
  req["text"] = words;
  req["model_id"] = TTS_MODEL;
  String body, resp;
  serializeJson(req, body);
  Sink out(replyBuf, replyMax);
  uint32_t t = millis();
  int code = post("TTS", String("https://api.elevenlabs.io/v1/text-to-speech/") + VOICE_ID + "?output_format=pcm_16000",
                  "xi-api-key", ELEVENLABS_KEY, (uint8_t*)body.c_str(), body.length(), &resp, &out);
  LOG("TTS", "http %d, %u bytes in %lu ms", code, (unsigned)out.len, millis() - t);
  if (code != 200) { LOG("TTS", "server said: %s", resp.c_str()); return 0; }
  return out.len / 2;
}

// "[happy] Hi!" -> the words, plus which face to show after speaking
int splitMood(String& s) {
  s.trim();
  int mood = S_HAPPY;
  if (s.startsWith("[")) {
    int e = s.indexOf(']');
    if (e > 0) {
      String tag = s.substring(1, e); tag.toLowerCase();
      if (tag == "sad") mood = S_SAD;
      else if (tag == "thinking" || tag == "neutral") mood = S_IDLE;
      s = s.substring(e + 1); s.trim();
    }
  }
  return mood;
}

void oops(const char* why) {
  LOG("FAIL", "%s", why);
  state = S_SAD;
  tone(300, 250);
  delay(1800);
  state = S_IDLE;
}

// iphone hotspots use a curly apostrophe and their own capitals, so match names loosely
String looseName(const String& s) {
  String o = s;
  o.replace("\xE2\x80\x99", "'");   // curly apostrophe to a plain one
  o.toLowerCase();
  return o;
}

String findNetwork() {
  int n = WiFi.scanNetworks();
  LOG("WIFI", "%d networks nearby:", n);
  String want = looseName(WIFI_NAME), found = WIFI_NAME;
  for (int i = 0; i < n; i++) {
    LOG("WIFI", "  \"%s\" %d dBm", WiFi.SSID(i).c_str(), WiFi.RSSI(i));
    if (looseName(WiFi.SSID(i)) == want) found = WiFi.SSID(i);
  }
  if (found == WIFI_NAME && n > 0) LOG("WIFI", "did not see \"%s\". Is the hotspot on?", WIFI_NAME);
  return found;
}

// ============ setup and loop ============
void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== Desky talk ===");
  pinMode(BTN, INPUT_PULLUP);
  if (AMP_SD >= 0) { pinMode(AMP_SD, OUTPUT); digitalWrite(AMP_SD, HIGH); }

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  BG = tft.color565(196, 207, 161); UNLIT = tft.color565(184, 196, 150); INK = tft.color565(38, 46, 32);
  canvas = new GFXcanvas16(160, 128);

  voiceMax = RATE * MAX_TALK_S;
  wavBuf = (uint8_t*)ps_malloc(44 + voiceMax * 2);
  voice = (int16_t*)(wavBuf + 44);
  replyMax = RATE * 2 * MAX_REPLY_S;
  replyBuf = (uint8_t*)ps_malloc(replyMax);
  if (!wavBuf || !replyBuf || !canvas->getBuffer()) {
    LOG("BOOT", "no memory. Set Tools > PSRAM: OPI PSRAM");
    tft.fillScreen(ST77XX_RED);
    while (true) delay(1000);
  }
  xTaskCreatePinnedToCore(faceTask, "face", 6144, nullptr, 1, nullptr, 0);

  spk.setPins(AMP_BCLK, AMP_LRC, AMP_DIN);
  LOG("BOOT", "speaker %s", spk.begin(I2S_MODE_STD, RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO) ? "ready" : "FAILED");
  mic.setPins(MIC_SCK, MIC_WS, -1, MIC_SD);
  LOG("BOOT", "mic %s", mic.begin(I2S_MODE_STD, RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT) ? "ready" : "FAILED");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  String ssid = findNetwork();
  WiFi.begin(ssid.c_str(), WIFI_PASSWORD);
  LOG("WIFI", "joining \"%s\"", ssid.c_str());
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(500);
  configTzTime("IST-5:30", "pool.ntp.org", "time.google.com");   // keeps the clock right once wi-fi is up
  if (WiFi.status() == WL_CONNECTED) LOG("WIFI", "connected, ip %s, %d dBm", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  else LOG("WIFI", "not connected yet. On the iPhone turn on Personal Hotspot and Maximize Compatibility");

  state = S_HAPPY;
  tone(880, 120); delay(60); tone(1320, 120);
  delay(700);
  state = S_IDLE;
  LOG("BOOT", "clock:%s", nowText().c_str());
  LOG("BOOT", "ready: hold the button, talk, let go");
}

void loop() {
  if (digitalRead(BTN) != LOW) { delay(10); return; }
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    oops("no wifi yet");
    while (digitalRead(BTN) == LOW) delay(10);
    return;
  }

  state = S_LISTEN;
  int peak;
  size_t samples = listen(peak);
  LOG("MIC", "%.1f s, peak %d", samples / (float)RATE, peak);
  if (samples < RATE / 2) { state = S_IDLE; return; }   // a tap, not speech
  if (peak < 500) LOG("MIC", "very quiet: check the mic wires, or set MIC_SHIFT to 14");

  state = S_THINK;
  uint32_t t0 = millis();
  String said = speechToText(samples);
  if (!said.length()) { oops("speech to text failed, see [STT]"); return; }
  String answer = reply(said);
  if (!answer.length()) { oops("reply failed, see [CHAT]"); return; }
  int mood = splitMood(answer);
  size_t n = textToSpeech(answer);
  uint32_t audioMs = n * 1000UL / RATE;
  LOG("PLAY", "%.1f s of voice, %lu ms after you let go", audioMs / 1000.0, millis() - t0);

  prepareText(answer, audioMs);
  state = S_TALK;
  if (n) play((int16_t*)replyBuf, n);
  uint32_t until = typeStart + totalChars * msPerChar + 1500;
  while (millis() < until) delay(20);   // let the typing finish, then a moment to read
  delay(1200);

  state = mood;
  delay(1500);
  state = S_IDLE;
}
