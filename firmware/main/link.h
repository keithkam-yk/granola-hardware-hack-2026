// The line to the game host.
//
// Text, one message per line, in both directions. Samples and events go up,
// HUD state comes down. USB CDC today; the wifi transport replaces this file's
// implementation without any caller noticing, which is the whole reason the
// protocol is lines of text rather than a packed struct.
#pragma once

#include "esp_err.h"

// Called on the link's reader task, once per line received, newline stripped.
typedef void (*link_line_cb_t)(const char *line);

esp_err_t link_start(link_line_cb_t on_line);

// Sends one line. The newline is added here; do not include one.
void link_sendf(const char *fmt, ...);
