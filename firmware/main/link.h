// The line to the game host.
//
// Text, one message per line, in both directions. Samples and events go up, HUD
// state comes down. The line runs over USB until a socket is attached, and falls
// back to USB the moment one breaks, so a board is never mute.
#pragma once

#include <stdbool.h>

#include "esp_err.h"

// Called once per line received, newline stripped, on whichever task read it.
typedef void (*link_line_cb_t)(const char *line);

esp_err_t link_start(link_line_cb_t on_line);

// Sends one line. The newline is added here; do not include one.
void link_sendf(const char *fmt, ...);

// Hands a received line to the callback. For transports that own their reader.
void link_deliver(const char *line);

// Routes traffic over a connected socket, or back to USB.
void link_attach_socket(int fd);
void link_detach_socket(void);
bool link_is_wireless(void);
