# Lionel Wi-Fi Train Controller

Hello — I built this for people who have older Lionel train sets and want an easy way to run them wirelessly. The firmware runs on an ESP32-S3. First boot it opens a setup Wi-Fi network so you can pick your home network from a phone; after that it serves a throttle page on that LAN. You can set train speed from a browser **or** from a physical knob. I hope you enjoy it.

This project is currently set up for a **Seeed XIAO ESP32-S3**. Open the board’s IP address in a phone or laptop browser and you get a throttle page: **OFF** plus speeds **1–6**. A three-position **WEB / OFF / KNOB** switch chooses who is allowed to command speed.

The chip itself only produces a **control signal**. It cannot power a locomotive. A motor driver and a buck converter sit between the ESP32, your 12/24 V supply, and the train.

## What it does

- Opens a setup AP (`Train-xxxx`) until you pick a home network, then serves a simple control page at `http://<board-ip>/`
- Lets you pick the control source with a maintained ON-OFF-ON switch: **WEB**, **OFF**, or **KNOB**
- In WEB mode, the browser throttle buttons set speed
- In KNOB mode, a 10 kΩ potentiometer selects the same six speed levels
- In OFF mode, PWM is forced to 0% (this is a software stop, not a power disconnect)
- Maps each speed to a PWM duty cycle on GPIO 2, which the Cytron motor driver turns into track/motor voltage
- Starts at **0% PWM** on boot so the train does not take off when power is applied
- Never restores the last speed after reboot or after switching WEB ↔ KNOB
- Blinks a status LED faster as the commanded speed increases, and flashes three times when the web server is ready

Speed levels in firmware (percentages are labeled as provisional and easy to retune in `main/train_control.h`). WEB and KNOB share this same table:

| Level | PWM |
|-------|-----|
| OFF   | 0%  |
| 1     | 65% |
| 2     | 70% |
| 3     | 75% |
| 4     | 80% |
| 5     | 85% |
| 6     | 90% (maximum; also clamped in the PWM driver) |

## Hardware

Think of two power domains:

1. **Logic / Wi-Fi** — the XIAO ESP32-S3, running this firmware at 5 V.
2. **Track / motor** — 12 V or 24 V at several amps, which only the Cytron driver should switch.

The ESP32 PWM pin is a small 3.3 V signal. Connecting it straight to a Lionel transformer or the rails will destroy the board. The parts below keep those worlds separate.

| Part | Role |
|------|------|
| **Seeed XIAO ESP32-S3** | Runs the web server, reads the switch and knob, and generates the speed PWM. |
| **Cytron 13A DC motor driver** (MD13S) | The muscle. It takes the ESP32 PWM and switches the 12/24 V supply to the motor/track at up to 13 A. The firmware only changes the PWM going *into* this driver; the driver is what actually moves the train. |
| **12/24 V → 5 V buck converter** | Drops the same layout supply down to 5 V to power the ESP32. The XIAO cannot run from 12 V or 24 V. Keep the buck’s 5 V output on the ESP32’s 5 V pin, not on GPIO. |
| **10 kΩ linear potentiometer (B10K)** | Manual throttle in KNOB mode. |
| **SPDT maintained ON-OFF-ON switch** | Selects WEB, OFF (center), or KNOB. |
| **Momentary pushbutton** | Hold to GND for ~4 s to forget the saved Wi-Fi network. |

### XIAO pinout used by this firmware

| Function | XIAO pin | GPIO | Goes to |
|----------|----------|------|---------|
| Speed/status LED | D0 | 1 | Blue LED through ~330 Ω |
| PWM output | D1 | 2 | Cytron MD13S PWM input |
| Potentiometer ADC | D3 | 4 | Pot wiper through ~1 kΩ; 100 nF from GPIO4 to GND |
| WEB selector | D4 | 5 | Switch WEB throw (internal pull-up; grounds this pin in WEB) |
| KNOB selector | D5 | 6 | Switch KNOB throw (internal pull-up; grounds this pin in KNOB) |
| Forget Wi-Fi | D8 | 7 | Momentary button to GND (internal pull-up; hold ~4 s) |

D2, D9, and D10 are unused. **D6 / GPIO43 and D7 / GPIO44** are the USB serial console — leave them alone if you want `idf.py monitor`.

PWM is 10 kHz, 10-bit LEDC, capped at 90%. Wire GPIO 2 only to the Cytron PWM input — never to the track.

Selector (active low, internal pull-ups):

| GPIO5 / D4 | GPIO6 / D5 | Mode |
|------------|------------|------|
| LOW | HIGH | WEB |
| HIGH | HIGH | OFF (center) |
| HIGH | LOW | KNOB |
| LOW | LOW | INVALID — treated as OFF |

Potentiometer: outside terminals to **3V3** and **GND**, wiper through the series resistor to **D3 / GPIO4**. Full counterclockwise should read near 0% (OFF); full clockwise near 100% (speed 6).

Knob position mapping (same PWM table as the browser):

| Knob | Speed |
|------|-------|
| 0–5% | OFF |
| 5–20% | 1 |
| 20–35% | 2 |
| 35–50% | 3 |
| 50–65% | 4 |
| 65–80% | 5 |
| 80–100% | 6 |

A typical hookup:

- 12/24 V supply → Cytron motor-power terminals **and** the buck converter input
- Buck 5 V output → XIAO 5 V (grounds common with the Cytron logic ground)
- XIAO D1 / GPIO 2 → Cytron PWM
- Cytron motor output → locomotive / track (through whatever isolation your layout needs)
- Switch common → GND; WEB throw → D4; KNOB throw → D5
- Forget button between D8 and GND
- Pot 3V3 / GND / wiper as above

## Safety

The locomotive must not start unexpectedly:

- PWM is 0% on boot, before the selector is read, and whenever the source is OFF or INVALID
- Switching WEB → KNOB or KNOB → WEB immediately commands OFF
- After entering KNOB, the pot must return to OFF before a nonzero manual speed is accepted (`armed=1` in the serial log)
- After entering WEB, a fresh browser button press is required; the last speed is not restored
- Browser `/speed` commands are rejected with HTTP 403 unless the switch is in WEB
- ADC failure commands OFF rather than holding an old speed

Center OFF on the switch is a **software** zero-PWM command, not a physical power cut.

## How to use it

1. Build and flash for ESP32-S3 from this project folder (not a different `TrainController` tree):

   ```
   idf.py -p PORT flash monitor
   ```

   House Wi-Fi is **not** set in menuconfig. Credentials are stored in NVS after phone setup. Do not put the house password in this README.

2. On first boot (or after forgetting the network), the serial log shows a setup AP such as `Train-A1B2` with password `trainsetup`. Join that AP from a phone and open `http://192.168.4.1/wifi` (many phones jump there automatically).
3. Pick your house network, enter its password, and wait until the page shows the train’s LAN IP. Rejoin the house Wi-Fi and open `http://<board-ip>/`.
4. Put the switch in **WEB** and use the throttle buttons, or **KNOB** and turn the pot (fully CCW first if you just switched into KNOB).
5. The page shows **WEB / OFF / KNOB**, polls `/status` about every 400 ms, and greys out the buttons when the switch is not WEB.
6. To change networks later, hold the D8 button to GND for about 4 seconds (or use **Forget network** on `/wifi`). The setup AP comes back.

When the LED flashes three times, the HTTP server is up (including on the setup AP, before the house LAN is joined). WEB/OFF/KNOB still work with no house Wi-Fi.

Serial lines tagged `wifi` report the setup AP name and the station IP after a successful join. Lines tagged `manual_control` report switch GPIO levels, decoded mode, and pot ADC (`raw` / `pct` / `maps_to` / `armed`). `GPIO6/KNOB=0` means the KNOB throw is grounding D5.

## HTTP API

The page is enough for day-to-day use. These endpoints are what it calls:

| Path | Description |
|------|-------------|
| `GET /` | Control page |
| `GET /speed?mode=off` or `1`–`6` | Set speed; only allowed in WEB. JSON includes `control_source`, `mode`, `percent`. HTTP 403 if the switch is OFF or KNOB. |
| `GET /status` | `{ "control_source": "web"\|"off"\|"knob", "mode": "...", "percent": ... }` |
| `GET /train.jpg` | Embedded header image |
| `GET /wifi` | Phone setup page: scan nearby SSIDs and save a network |
| `GET /wifi/status` | Setup/join state JSON (`ap_ssid`, `sta_ssid`, `ip`, …) |
| `GET /wifi/scan` | Nearby access points |
| `POST /wifi/connect` | JSON `{ "ssid", "password" }` — saves to NVS and joins |
| `POST /wifi/forget` | Erase saved network and reopen the setup AP |

`percent` is the calibrated PWM value from `train_control.h`, not a theoretical 0–100 throttle.

## Project layout

- `main/train_http.c` — web UI and URI handlers
- `main/wifi.c` — SoftAP provisioning, NVS credentials, station join
- `main/wifi_button.c` — D8 long-press to forget Wi-Fi
- `main/wifi_setup.html` — phone setup page
- `main/train_control.c` — authoritative source + speed; all motor changes go through here, then `train_pwm_set_percent()`
- `main/manual_control.c` — ADC, selector debounce, knob mapping and arming
- `main/train_pwm.c` — LEDC PWM into the Cytron driver
- `main/speed_led.c` — status LED (follows actual PWM)
- `images/lionel-trains.jpg` — image served on the page
