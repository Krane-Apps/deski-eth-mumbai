# Deski

Deski is a small desk robot that keeps you focused, with your own money on the line. Built at ETHGlobal Mumbai 2026.

You tell Deski what you will work on and stake a little USDC. Its camera checks on you during the session, and an AI model decides if you are focused. Finish the session and your stake comes back. Lose focus and Deski does not punish you: it buys Bitcoin with the stake and locks it for you for 5 years.

## Status

| Part | State |
| --- | --- |
| Hardware: ESP32-S3, screen, mic, speaker, camera, push-to-talk button | Working on two breadboards |
| Voice: hold the button, talk, Deski replies out loud and on screen | Working (`firmware/desky_talk`) |
| Pixel characters | Working (`firmware/desky_pets`) |
| Focus sessions judged by 0G sealed inference, episode log on 0G Storage | In progress |
| Stake vault: stake back, or Bitcoin bought on Uniswap and locked for 5 years | In progress |
| Owner approval with World ID before any stake | In progress |

## Hardware

- ESP32-S3-N16R8 camera board (16 MB flash, 8 MB PSRAM)
- 1.8 inch ST7735 screen, 128 x 160
- INMP441 I2S microphone
- MAX98357A I2S amplifier and a small speaker
- 6 x 6 mm push button (push to talk)

### Wiring

| Part pin | ESP32 GPIO |
| --- | --- |
| Screen CS, RESET, A0, SDA, SCK, LED | 45, 48, 47, 21, 20, 19 |
| Screen VCC, GND | 5V, GND |
| Amp LRC, BCLK, DIN, SD | 38, 39, 40, 42 (SD is driven high to turn the amp on) |
| Amp VIN, GND | 5V, GND |
| Mic WS, SCK, SD | 3, 14, 46 |
| Mic VDD, GND, L/R | 3V3, GND, GND |
| Push-to-talk button | 0 and GND (the board's BOOT button works too) |
| Camera | Ribbon connector on the board |

## Firmware

| Sketch | What it does |
| --- | --- |
| `firmware/desky_talk` | Voice assistant: push to talk, speech to text, reply, text to speech, pixel face |
| `firmware/desky_pets` | Plays every pixel character and mood on the screen |
| `firmware/desky_camera_test` | Shows the live camera picture on the screen |

### Setup

1. Install the Arduino IDE, the ESP32 board package by Espressif (3.3.x), and the libraries "Adafruit ST7735 and ST7789" and "ArduinoJson" (v7).
2. Tools menu: ESP32S3 Dev Module, PSRAM: OPI PSRAM, Flash Size: 16MB, Partition Scheme: 16M Flash (3MB APP/9.9MB FATFS), USB CDC On Boot: Disabled. Use the USB-UART port.
3. In `firmware/desky_talk`, copy `secrets.example.h` to `secrets.h` and add your Wi-Fi and API keys. The Wi-Fi must be 2.4 GHz. On an iPhone hotspot, turn on Maximize Compatibility.
4. Each sketch has `#define LAYOUT 2` for the wiring above. Upload, open the Serial Monitor at 115200, hold the button and talk.
