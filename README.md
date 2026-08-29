# Lionel Wi-Fi Train Controller

Hello — I built this for people who have older Lionel train sets and want an easy way to run them wirelessly. The firmware runs on an ESP32-S3, starts a web server on your home Wi-Fi, and lets you set train speed from a browser. I hope you enjoy it.

This project is currently set up for a **Seeed XIAO ESP32-S3**. Open the board’s IP address in a phone or laptop browser and you get a throttle page: **OFF** plus speeds **1–6**.

The chip itself only produces a **control signal**. It cannot power a locomotive. A motor driver and a buck converter sit between the ESP32, your 12/24 V supply, and the train.

## What it does

- Joins your Wi-Fi network and serves a simple control page at `http://<board-ip>/`
- Maps each throttle button to a PWM duty cycle on GPIO 2, which the Cytron motor driver turns into track/motor voltage
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

Think of two power domains:

1. **Logic / Wi-Fi** — the XIAO ESP32-S3, running this firmware at 5 V.
2. **Track / motor** — 12 V or 24 V at several amps, which only the Cytron driver should switch.

The ESP32 PWM pin is a small 3.3 V signal. Connecting it straight to a Lionel transformer or the rails will destroy the board. The parts below keep those worlds separate.

| Part | Role |
|------|------|
| **Seeed XIAO ESP32-S3** | Runs the web server and generates the speed PWM. |
| **Cytron 13A DC motor driver** | The muscle. It takes the ESP32 PWM (and a direction input, if you use one) and switches the 12/24 V supply to the motor/track at up to 13 A. The firmware’s speed buttons only change the PWM going *into* this driver; the driver is what actually moves the train. |
| **12/24 V → 5 V buck converter** | Drops the same layout supply down to 5 V to power the ESP32. The XIAO cannot run from 12 V or 24 V. Keep the buck’s 5 V output on the ESP32’s 5 V pin, not on GPIO. |

### XIAO pinout used by this firmware

| Function | XIAO pin | GPIO | Goes to |
|----------|----------|------|---------|
| PWM output | D1 | 2 | Cytron PWM input |
| Speed/status LED | D0 | 1 | LED (and resistor) |

PWM is 10 kHz, 10-bit LEDC, capped at 90%. Wire GPIO 2 only to the Cytron PWM input — never to the track.

A typical hookup:

- 12/24 V supply → Cytron motor-power terminals **and** the buck converter input
- Buck 5 V output → XIAO 5 V (and grounds common with the Cytron logic ground)
- XIAO D1 / GPIO 2 → Cytron PWM
- Cytron motor output → locomotive / track (through whatever isolation your layout needs)

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
- `main/train_pwm.c` — LEDC PWM into the Cytron driver
- `main/speed_led.c` — status LED
- `images/lionel-trains.jpg` — image served on the page
