#pragma once

/* Momentary horn button on XIAO D7 / GPIO44. One press queues one POST /api/horn. */
void horn_button_init(void);
