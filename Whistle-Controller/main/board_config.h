#pragma once

/*
 * Breadboard target: Seeed Studio XIAO ESP32-S3.
 *
 * GPIO43 (TX) and GPIO44 (RX) are the ESP32-S3 UART0 default pins. The
 * DFPlayer is attached to UART1 on those pins. Application logs use the
 * native USB Serial/JTAG console so they never reach the DFPlayer.
 *
 * Move to the M5Stack Stamp-S3Bat by changing the GPIO numbers below.
 */

#define BOARD_DFPLAYER_UART_PORT        1
#define BOARD_DFPLAYER_TX_GPIO          43
#define BOARD_DFPLAYER_RX_GPIO          44
#define BOARD_DFPLAYER_BAUD             115200

#define BOARD_DFPLAYER_BOOT_DELAY_MS    2000
#define BOARD_DFPLAYER_CMD_TIMEOUT_MS   1000
#define BOARD_DFPLAYER_PLAY_TIMEOUT_MS  1500
/* Startup keeps retrying AT. This only caps the wait between full attempts. */
#define BOARD_DFPLAYER_RETRY_MAX_MS     15000

#define BOARD_DFPLAYER_VOLUME           20
#define BOARD_DFPLAYER_SINGLE_PLAY_MODE 3

#define BOARD_HORN_BUTTON_GPIO          1
#define BOARD_BUTTON_POLL_MS            10
#define BOARD_BUTTON_DEBOUNCE_MS        30
#define BOARD_BUTTON_LONG_PRESS_MS      5000

/* Exact origin always allowed. The train page is also allowed when its Origin
 * is http:// plus a loopback or private LAN address, because that address comes
 * from DHCP. PowerShell clients do not send Origin.
 */
#define WHISTLE_CORS_ORIGIN             "http://localhost"

/* QUERY=3 and QUERY=4 report whole seconds. Poll until those values show the file ended. */
#define BOARD_DFPLAYER_POLL_MS          500
#define BOARD_DFPLAYER_STALL_MS         2000
#define BOARD_DFPLAYER_SHORT_FILE_MS    3000
#define BOARD_DFPLAYER_MAX_FILE_MS      (10 * 60 * 1000)
#define BOARD_DFPLAYER_MAX_FILES        64
