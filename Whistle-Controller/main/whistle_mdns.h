#pragma once

#include <stdbool.h>

/* Prepare mDNS after Wi-Fi init. Does not advertise the Horn service. */
void whistle_mdns_start(void);

/* Advertise _whistleblower._tcp only while the DFPlayer initialization sequence has succeeded. */
void whistle_mdns_set_horn_ready(bool ready);
