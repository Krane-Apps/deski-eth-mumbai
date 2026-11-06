// desky pets: desky's original pixel characters for focus-pet mode, shown one scene after another.
// the pet grows with finished focus sessions: seed (egg) -> blip (baby) -> doodle (kid) -> desky (adult).
// desky's face changes with its mood, and a small icon shows what is happening.
// tools: ESP32S3 Dev Module, PSRAM: OPI PSRAM, Flash Size: 16MB, USB-UART port, USB CDC On Boot: Disabled.

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>

// ---------- settings ----------
const bool LCD_LOOK = true;   // true: classic dot-matrix lcd look. false: colour pixels on black
const int  SCENE_MS = 4000;   // how long each scene shows
const int  FRAME_MS = 150;    // animation speed

// screen pins. LAYOUT 1: the current wiring, with 8 jumper wires to the screen.
// LAYOUT 2: the compact two-breadboard build (guide, "Compact build").
#define LAYOUT 1
#if LAYOUT == 2
#define TFT_SCLK 20   // screen SCK
#define TFT_MOSI 21   // screen SDA
#define TFT_RST  48   // also drives the board's rgb led, so that led may light up
#define TFT_DC   47   // screen A0
#define TFT_CS   45
#define TFT_BL   19   // screen LED
#else
#define TFT_SCLK 19
#define TFT_MOSI 20
#define TFT_RST  21
#define TFT_DC   47
#define TFT_CS   45
#define TFT_BL   38
#endif

Adafruit_ST7735 tft = Adafruit_ST7735(&SPI, TFT_CS, TFT_DC, TFT_RST);
GFXcanvas16 canvas(160, 128);   // draw each frame off screen, then send it in one go (no flicker)

// ---------- characters: 16 x 16, '#' is a lit pixel ----------
const char* SEED[16] = {
  "................",
  "......####......",
  ".....#....#.....",
  "....#......#....",
  "...#........#...",
  "...#..##....#...",
  "..#...##.....#..",
  "..#..........#..",
  "..#.#.#.#.#.##..",
  "..##.#.#.#.#.#..",
  "..#..........#..",
  "..#.....##...#..",
  "...#....##..#...",
  "....#......#....",
  ".....######.....",
  "................",
};

const char* BLIP[16] = {
  "................",
  "................",
  "........#.......",
  ".......#........",
  ".....######.....",
  "....#......#....",
  "...#........#...",
  "...#.##..##.#...",
  "...#.##..##.#...",
  "...#........#...",
  "...#..#..#..#...",
  "...#...##...#...",
  "....#......#....",
  ".....######.....",
  ".....##..##.....",
  "................",
};

const char* DOODLE[16] = {
  "...#........#...",
  "....#......#....",
  ".....######.....",
  "....#......#....",
  "...#........#...",
  "...#.##..##.#...",
  "...#.##..##.#...",
  "...#........#...",
  "...#...##...#...",
  "..##........##..",
  ".#.#........#.#.",
  "...#........#...",
  "...#........#...",
  "....#......#....",
  "....###..###....",
  "................",
};

// desky has a screen for a face, like the real robot; the mood faces below are drawn inside it
const char* DESKY[16] = {
  ".......##.......",
  ".......##.......",
  "........#.......",
  "..############..",
  "..#..........#..",
  "..#..........#..",
  "..#..........#..",
  "..#..........#..",
  "..#..........#..",
  "..#..........#..",
  "..#..........#..",
  "..############..",
  "......#..#......",
  ".....##..##.....",
  "................",
  "................",
};

const char* GHOST[16] = {
  "................",
  ".....######.....",
  "....#......#....",
  "...#........#...",
  "..#..........#..",
  "..#..##..##..#..",
  "..#..##..##..#..",
  "..#..........#..",
  "..#....##....#..",
  "..#...#..#...#..",
  "..#....##....#..",
  "..#..........#..",
  "..#..........#..",
  "..#.##.##.##.#..",
  "..##..#..#..##..",
  "................",
};

// ---------- desky's mood faces: 10 x 5, drawn inside its screen ----------
enum Face { NO_FACE = -1, NEUTRAL, BLINK, HAPPY, SAD, FOCUS, ANGRY, DIZZY, SLEEP };
const char* FACES[][5] = {
  { "..##..##..", "..##..##..", "..........", "...####...", ".........." },  // neutral
  { "..........", "..##..##..", "..........", "...####...", ".........." },  // blink
  { "..#....#..", ".#.#..#.#.", "..........", "..#....#..", "...####..." },  // happy
  { "..........", "..##..##..", "..........", "...####...", "..#....#.." },  // sad
  { ".###..###.", "..##..##..", "..........", "...####...", ".........." },  // focus
  { ".#......#.", "..#....#..", "..##..##..", "..........", "..######.." },  // angry
  { ".#.#..#.#.", "..#....#..", ".#.#..#.#.", "..........", "...####..." },  // dizzy
  { "..........", ".###..###.", "..........", "....##....", "....##...." },  // sleep
};

// ---------- small icons: 7 x 7 ----------
enum Icon { NO_ICON = -1, HEART, DROP, ZZZ, BANG, QUESTION, SPARKLE, COIN };
const char* ICONS[][7] = {
  { ".##.##.", "#######", "#######", ".#####.", "..###..", "...#...", "......." },  // heart
  { "...#...", "..###..", ".#####.", ".#####.", "..###..", ".......", "......." },  // drop
  { "####...", "..#....", ".#.....", "####...", "....###", ".....#.", "....###" },  // zzz
  { "...#...", "...#...", "...#...", "...#...", ".......", "...#...", "......." },  // !
  { ".####..", "#....#.", "....#..", "...#...", "...#...", ".......", "...#..." },  // ?
  { "...#...", "..###..", "#######", "..###..", "...#...", ".......", "......." },  // sparkle
  { "..###..", ".#...#.", "#..#..#", "#.###.#", "#..#..#", ".#...#.", "..###.." },  // coin
};

// ---------- scenes ----------
enum Anim { BOUNCE, WOBBLE, STILL, SHAKE, FLOAT };
struct Scene {
  const char** body;
  int face;
  int icon;
  const char* title;     // big line; "TIMER" shows a countdown instead
  const char* caption;   // small line
  int anim;
  uint8_t r, g, b;       // body colour when LCD_LOOK is false
};

Scene scenes[] = {
  { SEED,   NO_FACE, NO_ICON,  "SEED",      "hatches after 1 session", WOBBLE, 240, 240, 240 },
  { BLIP,   NO_FACE, HEART,    "BLIP",      "baby: 1 session done",    BOUNCE,  40, 230, 255 },
  { DOODLE, NO_FACE, SPARKLE,  "DOODLE",    "kid: 5 sessions done",    BOUNCE, 255, 220,  40 },
  { DESKY,  NEUTRAL, NO_ICON,  "DESKY",     "adult: 10 sessions done", BOUNCE, 255,  90, 170 },
  { DESKY,  FOCUS,   NO_ICON,  "TIMER",     "focus mode, 0 strikes",   STILL,  255,  90, 170 },
  { DESKY,  ANGRY,   BANG,     "STRIKE 1",  "phone down! 2 left",      SHAKE,  255,  90, 170 },
  { DESKY,  SAD,     DROP,     "STRIKE 3",  "session failed",          STILL,  255,  90, 170 },
  { DESKY,  DIZZY,   QUESTION, "OFFLINE",   "brain offline, paused",   WOBBLE, 255,  90, 170 },
  { DESKY,  HAPPY,   COIN,     "+0.001 0G", "stake returned",          BOUNCE, 255,  90, 170 },
  { DESKY,  SLEEP,   ZZZ,      "BREAK",     "5 minute break",          STILL,  255,  90, 170 },
  { GHOST,  NO_FACE, NO_ICON,  "GHOST",     "no sessions for 3 days",  FLOAT,  170, 200, 255 },
};
const int NUM_SCENES = sizeof(scenes) / sizeof(scenes[0]);

uint16_t BG, INK, UNLIT;
const uint8_t ICON_RGB[][3] = {
  { 255, 40, 60 }, { 70, 140, 255 }, { 255, 255, 255 }, { 255, 40, 40 },
  { 255, 220, 40 }, { 255, 220, 40 }, { 255, 190, 0 },
};

// ---------- drawing ----------
const int SPR_X = 40, SPR_Y = 2, SPR_CELL = 5;   // sprite: 16 x 17 cells of 5 px (one spare row for bouncing)
const int ICN_X = 126, ICN_Y = 6, ICN_CELL = 4;  // icon: 7 x 7 cells of 4 px
char px[17][16];                                 // the composed sprite for this frame

void cell(int x, int y, int size, uint16_t color) {
  // a 1 px gap between cells gives the dot-matrix look
  if (LCD_LOOK) canvas.fillRect(x, y, size - 1, size - 1, color);
  else canvas.fillRect(x, y, size, size, color);
}

void compose(const Scene& s, int f) {
  memset(px, '.', sizeof(px));
  int dx = 0, dy = 0;
  switch (s.anim) {
    case BOUNCE: dy = (f / 3) % 2; break;
    case WOBBLE: { const int w[4] = { 0, -1, 0, 1 }; dx = w[(f / 2) % 4]; break; }
    case SHAKE:  dx = (f < 8) ? ((f % 2) ? 1 : -1) : 0; break;
    case FLOAT:  dy = (f / 4) % 2; break;
  }
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 16; x++) {
      int tx = x + dx, ty = y + dy;
      if (s.body[y][x] == '#' && tx >= 0 && tx < 16 && ty >= 0 && ty < 17) px[ty][tx] = '#';
    }
  if (s.face != NO_FACE) {
    int face = s.face;
    if (face == NEUTRAL && f % 20 >= 18) face = BLINK;          // blink now and then
    for (int y = 0; y < 5; y++)
      for (int x = 0; x < 10; x++)
        if (FACES[face][y][x] == '#') px[5 + y + dy][3 + x + dx] = '#';
  }
}

void drawFrame(const Scene& s, int f, uint32_t sceneStart) {
  canvas.fillScreen(BG);
  compose(s, f);

  uint16_t body = LCD_LOOK ? INK : tft.color565(s.r, s.g, s.b);
  for (int y = 0; y < 17; y++)
    for (int x = 0; x < 16; x++) {
      bool on = px[y][x] == '#';
      if (on || LCD_LOOK) cell(SPR_X + x * SPR_CELL, SPR_Y + y * SPR_CELL, SPR_CELL, on ? body : UNLIT);
    }

  if (s.icon != NO_ICON) {
    bool show = (s.icon == BANG || s.icon == HEART) ? (f / 2) % 2 == 0 : true;   // flash some icons
    uint16_t ic = LCD_LOOK ? INK : tft.color565(ICON_RGB[s.icon][0], ICON_RGB[s.icon][1], ICON_RGB[s.icon][2]);
    for (int y = 0; y < 7; y++)
      for (int x = 0; x < 7; x++) {
        bool on = show && ICONS[s.icon][y][x] == '#';
        if (on || LCD_LOOK) cell(ICN_X + x * ICN_CELL, ICN_Y + y * ICN_CELL, ICN_CELL, on ? ic : UNLIT);
      }
  }

  // label strip at the bottom
  char title[16];
  if (strcmp(s.title, "TIMER") == 0) {
    int left = 25 * 60 - (millis() - sceneStart) / FRAME_MS;   // fast countdown, one second per frame
    snprintf(title, sizeof(title), "%02d:%02d", left / 60, left % 60);
  } else {
    strncpy(title, s.title, sizeof(title) - 1);
    title[sizeof(title) - 1] = 0;
  }
  uint16_t text = LCD_LOOK ? INK : ST77XX_WHITE;
  canvas.setTextColor(text);
  canvas.setTextSize(2);
  canvas.setCursor((160 - (int)strlen(title) * 12) / 2, 94);
  canvas.print(title);
  canvas.setTextSize(1);
  canvas.setCursor((160 - (int)strlen(s.caption) * 6) / 2, 115);
  canvas.print(s.caption);

  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), 160, 128);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Desky pets demo ===");
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);   // landscape, 160 x 128

  if (LCD_LOOK) {
    BG    = tft.color565(196, 207, 161);   // pale lcd green
    UNLIT = tft.color565(184, 196, 150);   // faint unlit pixels
    INK   = tft.color565(38, 46, 32);      // dark lit pixels
  } else {
    BG = UNLIT = ST77XX_BLACK;
    INK = ST77XX_WHITE;
  }
}

void loop() {
  for (int i = 0; i < NUM_SCENES; i++) {
    Serial.printf("Scene %d: %s (%s)\n", i + 1, scenes[i].title, scenes[i].caption);
    uint32_t start = millis();
    for (int f = 0; millis() - start < (uint32_t)SCENE_MS; f++) {
      drawFrame(scenes[i], f, start);
      delay(FRAME_MS);
    }
  }
}
