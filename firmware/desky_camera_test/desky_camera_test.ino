// desky camera test: shows the live camera picture on desky's screen.
// pass when you see yourself on the screen. hold BOOT to test jpeg capture, which the real build uploads.
// tools: ESP32S3 Dev Module, PSRAM: OPI PSRAM, Flash Size: 16MB, USB-UART port, USB CDC On Boot: Disabled.

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include "esp_camera.h"
#include "mbedtls/base64.h"

// flip these if the picture is upside down or mirrored
const bool FLIP_V   = false;
const bool MIRROR_H = true;

// screen pins. LAYOUT 1: the current wiring, with 8 jumper wires to the screen.
// LAYOUT 2: the compact two-breadboard build (guide, "Compact build").
#define LAYOUT 2
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
#define BTN      0

// camera, from the xiaozhi "bread-compact-wifi-s3cam" board file (same board layout)
#define CAM_XCLK  15
#define CAM_SIOD  4
#define CAM_SIOC  5
#define CAM_D0    11
#define CAM_D1    9
#define CAM_D2    8
#define CAM_D3    10
#define CAM_D4    12
#define CAM_D5    18
#define CAM_D6    17
#define CAM_D7    16
#define CAM_VSYNC 6
#define CAM_HREF  7
#define CAM_PCLK  13

Adafruit_ST7735 tft = Adafruit_ST7735(&SPI, TFT_CS, TFT_DC, TFT_RST);

#define LOG(tag, fmt, ...) Serial.printf("[%6lu ms] [%s] " fmt "\n", millis(), tag, ##__VA_ARGS__)

void showMessage(const char* line1, const char* line2, uint16_t color) {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(color);
  tft.setTextSize(1);
  tft.setCursor(4, 50);
  tft.println(line1);
  tft.setCursor(4, 64);
  tft.println(line2);
}

bool startCamera(pixformat_t format, framesize_t size) {
  camera_config_t c = {};
  c.pin_pwdn = -1;
  c.pin_reset = -1;
  c.pin_xclk = CAM_XCLK;
  c.pin_sccb_sda = CAM_SIOD;
  c.pin_sccb_scl = CAM_SIOC;
  c.pin_d0 = CAM_D0; c.pin_d1 = CAM_D1; c.pin_d2 = CAM_D2; c.pin_d3 = CAM_D3;
  c.pin_d4 = CAM_D4; c.pin_d5 = CAM_D5; c.pin_d6 = CAM_D6; c.pin_d7 = CAM_D7;
  c.pin_vsync = CAM_VSYNC;
  c.pin_href = CAM_HREF;
  c.pin_pclk = CAM_PCLK;
  c.xclk_freq_hz = 20000000;
  c.ledc_timer = LEDC_TIMER_0;
  c.ledc_channel = LEDC_CHANNEL_0;
  c.pixel_format = format;
  c.frame_size = size;
  c.jpeg_quality = 12;              // 10 sharper ... 20 smaller
  c.fb_count = 2;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  c.grab_mode = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    LOG("CAM", "FAIL: camera did not start, error 0x%x", err);
    return false;
  }
  sensor_t* s = esp_camera_sensor_get();
  s->set_vflip(s, FLIP_V);
  s->set_hmirror(s, MIRROR_H);
  LOG("CAM", "ready, sensor id 0x%x (0x26 = OV2640, 0x3660 = OV3660, 0x5640 = OV5640)", s->id.PID);
  return true;
}

// prints the jpeg as base64 between markers, so a computer on the usb cable can save it as a file
void sendPhoto(camera_fb_t* fb) {
  size_t len = 0;
  mbedtls_base64_encode(nullptr, 0, &len, fb->buf, fb->len);
  uint8_t* b64 = (uint8_t*)ps_malloc(len + 1);
  if (!b64) return;
  mbedtls_base64_encode(b64, len + 1, &len, fb->buf, fb->len);
  Serial.println("PHOTO_START");
  for (size_t i = 0; i < len; i += 76) { Serial.write(b64 + i, min((size_t)76, len - i)); Serial.println(); }
  Serial.println("PHOTO_END");
  free(b64);
}

// takes a few jpeg pictures at the size the real build will upload
void jpegTest() {
  showMessage("JPEG test running", "see the Serial Monitor", ST77XX_YELLOW);
  esp_camera_deinit();
  if (startCamera(PIXFORMAT_JPEG, FRAMESIZE_QVGA)) {
    for (int i = 1; i <= 5; i++) {
      camera_fb_t* fb = esp_camera_fb_get();
      if (!fb) { LOG("JPEG", "FAIL: no picture"); continue; }
      LOG("JPEG", "picture %d: %ux%u, %u bytes, starts %02X %02X (FF D8 means a valid jpeg)",
          i, fb->width, fb->height, (unsigned)fb->len, fb->buf[0], fb->buf[1]);
      if (i == 5) sendPhoto(fb);   // last one: the exposure has settled by now
      esp_camera_fb_return(fb);
      delay(300);
    }
  }
  esp_camera_deinit();
  startCamera(PIXFORMAT_RGB565, FRAMESIZE_QQVGA);
  tft.fillScreen(ST77XX_BLACK);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Desky camera test ===");
  pinMode(BTN, INPUT_PULLUP);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);               // landscape, 160 x 128
  showMessage("Starting camera...", "", ST77XX_WHITE);

  if (!psramFound()) {
    LOG("BOOT", "FAIL: no PSRAM. Set Tools > PSRAM: OPI PSRAM");
    showMessage("No PSRAM found.", "Tools > PSRAM: OPI PSRAM", ST77XX_RED);
    while (true) delay(1000);
  }
  // 160 x 120 colour pictures fit the 160 x 128 screen with no scaling
  if (!startCamera(PIXFORMAT_RGB565, FRAMESIZE_QQVGA)) {
    showMessage("Camera failed. Check the", "ribbon: contacts + latch", ST77XX_RED);
    while (true) delay(1000);
  }
  jpegTest();   // once at boot, so the photo reaches the computer without a button press
  LOG("BOOT", "ready. You should see the camera picture. Hold BOOT for the jpeg test.");
}

void loop() {
  if (digitalRead(BTN) == LOW) {
    jpegTest();
    while (digitalRead(BTN) == LOW) delay(10);
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) { LOG("CAM", "WARN: no picture"); delay(100); return; }

  // the camera sends each pixel high byte first; the screen library wants it the other way round
  uint16_t* px = (uint16_t*)fb->buf;
  size_t count = fb->len / 2;
  for (size_t i = 0; i < count; i++) px[i] = (px[i] >> 8) | (px[i] << 8);
  tft.drawRGBBitmap(0, (128 - fb->height) / 2, px, fb->width, fb->height);
  esp_camera_fb_return(fb);

  static uint32_t frames = 0, since = millis();
  frames++;
  if (millis() - since >= 3000) {
    LOG("CAM", "%.1f pictures per second", frames * 1000.0 / (millis() - since));
    frames = 0;
    since = millis();
  }
}
