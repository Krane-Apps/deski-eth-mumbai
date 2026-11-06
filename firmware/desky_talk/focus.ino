// focus mode: a staked focus session with camera attention checks, then an exercise to win the stake back.
// talks to the deski backend (POST BACKEND_URL/tool, see docs/tool-api.html) and to 0G's vision model.
// flow: "stake 2 ATTN for 25 minutes" -> backend stakes -> camera check every CHECK_EVERY_MS ->
// time up -> exercise photos -> 0G says yes or no -> yes asks the backend to unstake.

const int      FOCUS_DEFAULT_MIN = 2;       // when the user says no number of minutes
const uint32_t CHECK_EVERY_MS    = 60000;   // camera attention check interval
const int      EXERCISE_WAIT_S   = 6;       // time to start moving before the photos
const int      EXERCISE_PHOTOS   = 3;
const int      PHOTO_GAP_MS      = 2500;
const char*    EXERCISE          = "three slow neck rotations";

const char* LOOK_QUESTION =
  "This photo is from a desk robot's camera, pointed at its user. First check if a person's face or body is clearly visible. "
  "If not, answer exactly: Nobody is at the desk. If yes, say in one short sentence what they are doing and whether they look focused on their work.";

bool camOk = false;
volatile uint32_t focusEnd = 0;      // millis when the session ends; 0 = no session
volatile uint32_t exerciseEnd = 0;   // millis when the exercise photos are done
uint32_t nextCheck = 0;
volatile int checksPassed = 0, checksFailed = 0;

bool focusOn() { return focusEnd != 0; }

// ---------- camera ----------
void cameraStart() {
  camera_config_t c = {};
  c.pin_pwdn = -1; c.pin_reset = -1;
  c.pin_xclk = 15; c.pin_sccb_sda = 4; c.pin_sccb_scl = 5;
  c.pin_d0 = 11; c.pin_d1 = 9; c.pin_d2 = 8; c.pin_d3 = 10;
  c.pin_d4 = 12; c.pin_d5 = 18; c.pin_d6 = 17; c.pin_d7 = 16;
  c.pin_vsync = 6; c.pin_href = 7; c.pin_pclk = 13;
  c.xclk_freq_hz = 20000000;
  c.ledc_timer = LEDC_TIMER_0;
  c.ledc_channel = LEDC_CHANNEL_0;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = FRAMESIZE_QVGA;   // 320 x 240, about 8 KB
  c.jpeg_quality = 12;
  c.fb_count = 1;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  c.grab_mode = CAMERA_GRAB_LATEST;
  esp_err_t err = esp_camera_init(&c);
  camOk = err == ESP_OK;
  if (camOk) LOG("CAM", "ready, sensor 0x%x", esp_camera_sensor_get()->id.PID);
  else LOG("CAM", "FAILED (0x%x): focus sessions run without camera checks", err);
}

// one fresh photo as a data url for the vision model ("" if the camera fails)
String photo() {
  if (!camOk) return "";
  camera_fb_t* fb = esp_camera_fb_get();
  if (fb) esp_camera_fb_return(fb);   // drop the stale frame
  fb = esp_camera_fb_get();
  if (!fb) { LOG("CAM", "no picture"); return ""; }
  size_t len = 0;
  mbedtls_base64_encode(nullptr, 0, &len, fb->buf, fb->len);
  char* b64 = (char*)ps_malloc(len + 1);
  String url;
  if (b64 && mbedtls_base64_encode((uint8_t*)b64, len + 1, &len, fb->buf, fb->len) == 0) {
    b64[len] = 0;
    url = String("data:image/jpeg;base64,") + b64;
  }
  LOG("CAM", "photo %ux%u, %u bytes", fb->width, fb->height, (unsigned)fb->len);
  free(b64);
  esp_camera_fb_return(fb);
  return url;
}

// asks 0G's vision model (openrouter as fallback) a question about one or more photos
String vision(const char* question, const String* photos, int count) {
  JsonDocument req;
  req["max_tokens"] = 100;
  JsonObject msg = req["messages"].to<JsonArray>().add<JsonObject>();
  msg["role"] = "user";
  JsonArray content = msg["content"].to<JsonArray>();
  JsonObject t = content.add<JsonObject>();
  t["type"] = "text"; t["text"] = question;
  for (int i = 0; i < count; i++) {
    if (!photos[i].length()) continue;
    JsonObject im = content.add<JsonObject>();
    im["type"] = "image_url"; im["image_url"]["url"] = photos[i];
  }
  String answer = chat("VISION", true, req);
  if (!answer.length()) { LOG("VISION", "trying OpenRouter"); answer = chat("VISION", false, req); }
  LOG("VISION", "\"%s\"", answer.c_str());
  return answer;
}

// ---------- backend ----------
// sends one turn to POST /tool; returns success, and fills the tool name and message
bool askBackend(const String& userText, const String& aiRes, String& tool, String& message) {
  tool = ""; message = "";
  if (!strlen(BACKEND_URL)) { LOG("TOOL", "skipped: set BACKEND_URL in secrets.h"); return false; }
  JsonDocument req;
  req["user_text"] = userText;
  req["ai_res"] = aiRes;
  String body, resp;
  serializeJson(req, body);
  uint32_t t = millis();
  int code = post("TOOL", String(BACKEND_URL) + "/tool", "Accept", "application/json",
                  (uint8_t*)body.c_str(), body.length(), &resp, nullptr, "application/json", 1, 20000);
  JsonDocument doc;
  if (code != 200 || deserializeJson(doc, resp)) { LOG("TOOL", "backend failed (http %d: %.120s)", code, resp.c_str()); return false; }
  tool = doc["tool"] | "";
  message = doc["message"] | "";
  bool ok = doc["success"] | false;
  LOG("TOOL", "tool=%s success=%s message=\"%s\" in %lu ms", tool.length() ? tool.c_str() : "none",
      ok ? "yes" : "no", message.c_str(), (unsigned long)(millis() - t));
  return ok;
}

// "stake 2 ATTN for 25 minutes" -> 25; also "five minutes"; no number -> FOCUS_DEFAULT_MIN
int minutesFrom(const String& said) {
  String s = said;
  s.toLowerCase();
  int m = s.indexOf("min");
  if (m < 0) return FOCUS_DEFAULT_MIN;
  int e = m;
  while (e > 0 && s[e - 1] == ' ') e--;
  int b = e;
  while (b > 0 && isDigit(s[b - 1])) b--;
  if (b < e) return max(1L, s.substring(b, e).toInt());
  while (b > 0 && isAlpha(s[b - 1])) b--;   // the word before "minutes"
  String w = s.substring(b, e);
  const char* names[] = { "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten" };
  for (int i = 0; i < 10; i++) if (w == names[i]) return i + 1;
  if (w == "fifteen") return 15;
  if (w == "twenty") return 20;
  if (w == "thirty") return 30;
  return FOCUS_DEFAULT_MIN;
}

// boot self-check for the parser above; logs FAIL if a case breaks
void checkMinutesParser() {
  struct { const char* in; int out; } cases[] = {
    { "stake 2 ATTN for 25 minutes", 25 }, { "Stake 1 for 5 min.", 5 }, { "focus for ten minutes", 10 },
    { "start a focus session", FOCUS_DEFAULT_MIN }, { "one minute please", 1 },
  };
  for (auto& c : cases)
    if (minutesFrom(c.in) != c.out) { LOG("TEST", "FAIL minutesFrom(\"%s\") = %d, want %d", c.in, minutesFrom(c.in), c.out); return; }
  LOG("TEST", "minutes parser ok");
}

// ---------- session ----------
void startFocus(int minutes) {
  focusEnd = millis() + minutes * 60000UL;
  if (!focusEnd) focusEnd = 1;
  nextCheck = millis() + CHECK_EVERY_MS;
  checksPassed = checksFailed = 0;
  LOG("FOCUS", "session started: %d min, camera check every %lu s", minutes, (unsigned long)(CHECK_EVERY_MS / 1000));
}

// after every voice turn: the backend decides if it was a stake or unstake request
void focusAfterTurn(const String& said, const String& answer) {
  state = S_THINK;
  String tool, msg;
  bool ok = askBackend(said, answer, tool, msg);
  state = S_IDLE;
  if (tool == "stake" && ok) {
    startFocus(minutesFrom(said));
    say(msg.length() ? msg : String("Focus session started."), S_HAPPY);
  } else if (tool == "unstake" && ok) {
    LOG("FOCUS", "session ended by the user: %d checks passed, %d failed", checksPassed, checksFailed);
    focusEnd = 0;
    say(msg, S_HAPPY);
  } else if (tool.length() && msg.length()) {
    say(msg, S_SAD);   // a tool ran but failed
  }
}

void attentionCheck() {
  state = S_THINK;
  String shot = photo();
  String desc = shot.length() ? vision(LOOK_QUESTION, &shot, 1) : "";
  String tool, msg;
  bool ok = desc.length() && askBackend("img", desc, tool, msg);
  state = S_IDLE;
  if (tool != "attention") return;   // no camera, or no answer from the backend: no verdict
  if (ok) { checksPassed++; state = S_HAPPY; delay(1200); state = S_IDLE; }
  else {
    // backend warns first (neck exercise), then slashes on the second miss
    checksFailed++;
    say(msg.length() ? msg : String("Eyes back on your work, please. I am watching."), S_SAD);
    if (msg.indexOf("slashed") >= 0) { focusEnd = 0; LOG("FOCUS", "stake slashed by the backend, session over"); }
  }
  LOG("FOCUS", "attention %s: %d passed, %d failed", ok ? "passed" : "failed", checksPassed, checksFailed);
}

void exerciseTest() {
  LOG("FOCUS", "time is up: %d checks passed, %d failed. Exercise test", checksPassed, checksFailed);
  say(String("Time is up! To get your stake back, do ") + EXERCISE + " in front of me now.", S_IDLE);
  focusEnd = 0;
  exerciseEnd = millis() + EXERCISE_WAIT_S * 1000 + EXERCISE_PHOTOS * PHOTO_GAP_MS;
  state = S_EXERCISE;
  delay(EXERCISE_WAIT_S * 1000);
  String shots[EXERCISE_PHOTOS];
  int got = 0;
  for (int i = 0; i < EXERCISE_PHOTOS; i++) {
    tone(1200, 60);   // shutter click
    shots[i] = photo();
    if (shots[i].length()) got++;
    delay(PHOTO_GAP_MS);
  }
  exerciseEnd = 0;
  state = S_THINK;
  String q = String("These ") + EXERCISE_PHOTOS + " photos were taken about 2.5 seconds apart by a desk robot's camera. "
             "The user was asked to do " + EXERCISE + ". Did they do it? Start your answer with YES or NO, then one short sentence.";
  String verdict = got ? vision(q.c_str(), shots, EXERCISE_PHOTOS) : "";
  String v = verdict;
  v.trim();
  v.toUpperCase();
  bool did = v.startsWith("YES");
  LOG("EXERCISE", "%s (%d photos): %s", did ? "done" : "not done", got, verdict.c_str());
  state = S_IDLE;
  if (!did) {
    // ponytail: no backend "invest" tool yet; call it here when it exists
    say("I did not see the exercise, so your stake stays locked and goes into savings.", S_SAD);
    return;
  }
  String tool, msg;
  bool ok = askBackend("I finished my focus session and did the exercise. Please unstake my ATTN.",
                       "Great work! Unstaking your ATTN now.", tool, msg);
  if (ok && tool == "unstake") say(String("Great work! ") + msg, S_HAPPY);
  else say("You did it, but I could not get your stake back. Please check the backend.", S_SAD);
}

// called from loop() while nobody holds the button
void focusTick() {
  if (!focusOn() || WiFi.status() != WL_CONNECTED) return;
  if ((int32_t)(millis() - focusEnd) >= 0) { exerciseTest(); return; }
  if ((int32_t)(millis() - nextCheck) >= 0) {
    nextCheck = millis() + CHECK_EVERY_MS;
    attentionCheck();
  }
}

// ---------- screen ----------
void faceFocus(int f) {   // calm, determined eyes with brows; a slow blink
  if (f % 40 == 39) { fill(13, 14, 5, 1, GRN); fill(30, 14, 5, 1, GRN); }
  else { fill(13, 11, 5, 4, GRN); fill(30, 11, 5, 4, GRN); }
  fill(12, 8, 7, 1, GRN); fill(29, 8, 7, 1, GRN);
  fill(21, 20, 6, 1, GRN);
}

void faceExercise(int f) {   // excited bouncing face
  OY = (f / 2) % 2 ? -1 : 0;
  line(11, 13, 15, 9, 2, ORG); line(15, 9, 19, 13, 2, ORG);
  line(28, 13, 32, 9, 2, ORG); line(32, 9, 36, 13, 2, ORG);
  fill(20, 18, 8, 4, ORG); fill(22, 19, 4, 2, OFF);
  OY = 0;
}

// countdown under the face, plus the check score in the top left
void focusOverlay(bool exercising) {
  uint32_t end = exercising ? exerciseEnd : focusEnd;
  long left = end ? max(0L, (long)(end - millis()) / 1000) : 0;
  char buf[24];
  if (exercising) snprintf(buf, sizeof(buf), "MOVE! %ld", left);
  else snprintf(buf, sizeof(buf), "%02ld:%02ld", left / 60, left % 60);
  canvas->setTextSize(2);
  canvas->setTextColor(exercising ? PAL[ORG] : PAL[WHT]);
  canvas->setCursor((160 - (int)strlen(buf) * 12) / 2, 104);
  canvas->print(buf);
  if (!exercising) {
    snprintf(buf, sizeof(buf), "ok %d  miss %d", checksPassed, checksFailed);
    canvas->setTextSize(1);
    canvas->setTextColor(PAL[GRY]);
    canvas->setCursor(4, 2);
    canvas->print(buf);
  }
}
