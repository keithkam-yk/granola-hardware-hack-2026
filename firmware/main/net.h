// Joins wifi and keeps a socket to the game host. Requires link_start() first,
// since it reports progress over whatever line is currently up.
#pragma once

#include "esp_err.h"

esp_err_t net_start(void);

// Stores a network and joins it. The password is written to NVS and never
// echoed back over the link.
void net_set_credentials(const char *ssid, const char *password);
