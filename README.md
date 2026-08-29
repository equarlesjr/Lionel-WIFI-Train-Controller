# Lionel Wi-Fi Train Controller

Hello — I built this for people who have older Lionel train sets and want an easy way to run them wirelessly. The firmware runs on an ESP32-S3, starts a web server on your home Wi-Fi, and lets you set train speed from a browser. I hope you enjoy it.

This project is currently set up for a **Seeed XIAO ESP32-S3**. Open the board’s IP address in a phone or laptop browser and you get a throttle page: **OFF** plus speeds **1–6**.

## What it does

- Joins your Wi-Fi network and serves a simple control page at `http://<board-ip>/`
- Maps each throttle button to a PWM duty cycle that drives the track (or your motor driver)
- Starts at **0% PWM** on boot so the train does not take off when power is applied
- Blinks a status LED faster as the commanded speed increases, and flashes three times when the web server is ready

Speed levels in firmware (percentages are labeled as provisional and easy to retune in `main/train_control.h`):

| Button | PWM |
|--------|-----|
| OFF    | 0%  |
| 1      | 65% |
| 2      | 70% |
| 3      | 75% |
| 4      | 80% |
| 5      | 85% |
| 6      | 90% (maximum) |

## Hardware

| Function        | XIAO pin | GPIO |
|-----------------|----------|------|
| PWM output      | D1       | 2    |
| Speed/status LED| D0       | 1    |

PWM is 10 kHz, 10-bit LEDC. Wire GPIO 2 through an appropriate driver/amp for your layout — do not hang a transformer or track load directly on the ESP32 pin.

## How to use it

1. Set your Wi-Fi SSID and password in `idf.py menuconfig` under **Example Connection Configuration**.
2. Build and flash for ESP32-S3:

   ```
   idf.py -p PORT flash monitor
   ```

3. Watch the serial log for the IPv4 address, then open that address in a browser.
4. Use the throttle buttons. The page also polls `/status` so the highlighted speed stays in sync if you reload.

When the LED flashes three times, the server is up.

## HTTP API

The page is enough for day-to-day use. These endpoints are what it calls:

| Path | Description |
|------|-------------|
| `GET /` | Control page |
| `GET /speed?mode=off` or `1`–`6` | Set speed; JSON `{ "mode", "percent" }` |
| `GET /status` | Current mode and percent |
| `GET /train.jpg` | Embedded header image |

## Project layout

- `main/train_http.c` — web UI and URI handlers
- `main/train_control.c` — speed modes
- `main/train_pwm.c` — LEDC PWM
- `main/speed_led.c` — status LED
- `images/lionel-trains.jpg` — image served on the page
