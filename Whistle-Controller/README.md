# Whistleblower

Standalone ESP-IDF firmware for a Seeed Studio XIAO ESP32-S3 talking to a DFRobot DFPlayer Pro (DFR0768). A short press on GPIO1 (D0) plays the first sound in `sounds.json`. Holding that button for about 5 seconds opens Wi-Fi setup. The train controller, LEDs, and the battery board are not part of this firmware.

Hardware communication and audible playback have to be checked on the bench. A successful compile does not mean those checks passed.

## Wiring

| XIAO ESP32-S3 | DFPlayer Pro |
| --- | --- |
| 5V | VIN |
| GND | GND |
| GPIO43 (D6, TX) | RX |
| GPIO44 (D7, RX) | TX |
|  | Speaker on L+ and L− |

GPIO43 and GPIO44 are the ESP32-S3 UART0 default pins. This firmware moves the log console to the board's native USB Serial/JTAG port and uses UART1 for the DFPlayer, on those same pins. Do not attach a USB-to-UART adapter to GPIO43/GPIO44.

The ROM may emit a short message on GPIO43 before the application starts. The firmware waits two seconds and retries `AT` so that burst does not have to be a clean command.

Copy the MP3s named in `sounds.json` onto the module with its own USB port before flashing. The names are case-sensitive. Disconnect the ESP 5V and UART while the module USB is plugged into the computer. Wire a normally open button from GPIO1 (D0) to GND. Nothing plays at boot.

Flash and monitor from USB with the external 9V battery disconnected. Do not power the XIAO from USB and the battery at the same time.

## Wi-Fi

Credentials are stored in NVS namespace `wifi_cfg`, keys `ssid` and `pass`. There is no SSID in the source and nothing to set in menuconfig.

On a fresh board the setup access point is `Whistleblower-` plus the last two bytes of the station MAC. Its WPA2 password is `whistlesetup`. The setup page is `http://192.168.4.1` (the same page is also at `/wifi`). It scans for nearby 2.4 GHz networks. Tap one, enter the password, and press Connect. Saving a network keeps the setup AP up for 20 seconds after the home network assigns an address, then the board becomes a station only.

A short press still plays the horn when Wi-Fi is down. A 5 second hold opens the setup AP again and does not erase the saved network and does not play the horn. The setup page has a separate Forget button that erases only the saved network.

The DHCP hostname is `whistleblower`. This ESP-IDF tree has no mDNS responder, and the train controller does not advertise one either, so `whistleblower.local` is logged as the intended name and is not actually advertised. After the station connects, the log prints the SSID and the assigned IP. The home Wi-Fi password is never logged.

Open `http://<station-ip>/` in a browser on the same network. That page calls health, sounds, horn, play, and status, and shows each response code. Wi-Fi setup stays at `/wifi`.

`GET /api/health` returns the service state, including whether station Wi-Fi and the DFPlayer AT handshake succeeded:

```json
{
  "service": "whistleblower",
  "status": "ok",
  "wifi_connected": true,
  "dfplayer_connected": true
}
```

## Sound catalog

`sounds.json` in the project root is the list of sounds. The first entry is the power-on horn. Edit the file to add, remove, or reorder sounds, then rebuild and flash. Copy the same MP3s onto the DFPlayer with its USB port. The firmware does not check that those files are on the module.

The selected horn is kept in RAM for the current session. `GET /api/selection` returns it, and `POST /api/selection` with `{"name":"Horn4.mp3"}` changes it without playing. A reboot selects the first catalog entry again. Reconnecting Wi-Fi does not.

`POST /api/horn` and a short press on GPIO1 play the selected horn. `POST /api/sounds/play` plays a named catalog file and does not change the selection. `202` means the request was accepted. `409` means a sound is already active. `503` means the DFPlayer did not answer `AT` at boot, or `sounds.json` failed to load. An unknown name returns `404`.

`GET /api/sounds` returns the catalog in file order, plus `default_sound` and `module_file_count`. The file count comes from `AT+QUERY=2` at boot. It is not a file list.

On the home network the board advertises `_whistleblower._tcp` and the hostname `whistleblower.local`. The train controller uses that advertisement, then checks `/api/health`, before it tells the train page where to send horn commands.

Browser calls from `WHISTLE_CORS_ORIGIN` (`http://localhost`) are allowed. Browser calls whose Origin is `http://` plus a loopback or private LAN address are also allowed, so the train page can call this board at whatever address DHCP assigned. Other origins are rejected. PowerShell does not send an Origin header. The diagnostic page at `/` remains available.

`GET /api/status` reports the playback task. `playing` clears when the elapsed-time clock (`AT+QUERY=3` and `AT+QUERY=4`) indicates the file ended, or when that wait fails. The module has no finished event, so completion is estimated from that clock.

```powershell
$base = "http://192.168.1.100"   # station IP from the serial log

Invoke-RestMethod "$base/api/health"
Invoke-RestMethod "$base/api/sounds"
Invoke-RestMethod "$base/api/selection"
Invoke-RestMethod -Method Post "$base/api/horn"
Invoke-RestMethod -Method Post `
  -Uri "$base/api/selection" `
  -ContentType "application/json" `
  -Body '{"name":"Horn4.mp3"}'
Invoke-RestMethod -Method Post `
  -Uri "$base/api/sounds/play" `
  -ContentType "application/json" `
  -Body '{"name":"Horn4.mp3"}'
Invoke-RestMethod "$base/api/status"
Invoke-RestMethod -Method Post `
  -Uri "$base/api/sounds/play" `
  -ContentType "application/json" `
  -Body '{"name":"missing.mp3"}'
```

## Build, flash, and monitor

Open the ESP-IDF 6.1 PowerShell environment, then build and flash. On this machine that environment is:

```powershell
. C:\Espressif\tools\Microsoft.v6.1-rc1.PowerShell_profile.ps1
cd C:\Users\ernest\Desktop\Train-HTTPServer\Lionel-WIFI-Train-Controller\Whistle-Controller
idf.py build
idf.py -p COM5 flash monitor
```

Replace `COM5` with the XIAO's USB Serial/JTAG port. In Device Manager it appears as "USB JTAG/serial debug unit" or "USB Serial Device" on the USB-C connector. That same port is used for flashing and for the log output.

Leave the monitor with Ctrl+].

If the flash download never connects, hold the XIAO BOOT button, tap RESET, release BOOT, and run the flash command again.

To erase only the saved Wi-Fi network, join the setup AP and use Forget on the page. A full chip erase also clears it:

```powershell
idf.py -p COM5 erase-flash
```

## What to check

The monitor prints `DFPlayer connected` and `Horn button ready on GPIO1`. With no saved network it also prints the setup AP name and `open http://192.168.4.1`.

1. Join `Whistleblower-XXXX` with password `whistlesetup`.
2. Open `http://192.168.4.1`. The page lists nearby networks. Tap one, enter its password, and press Connect. The serial log should show `saved network` and then `STA connected` with an IP. It must not print the home password.
3. Reboot. The board should join that network without opening setup.
4. Short-press GPIO1: the first sound in `sounds.json` plays once. Release is required before another press.
5. Hold GPIO1 for about 5 seconds: the log says `Provisioning requested` and the setup AP returns. Releasing the button does not play the horn.
6. From the home network, open `http://<ip>/api/health`.
7. If the home network is off, the board should return to the setup AP after about 30 seconds and keep the saved credentials. It should not reboot in a loop. The horn button still works.

A press during playback is ignored. After flashing, drop 5V so the DFPlayer restarts with the ESP; a monitor reset alone leaves the module powered.

Phones do not automatically open the setup page. This firmware only redirects unknown setup-AP requests to `http://192.168.4.1/`. That behavior was not verified as a captive portal.
