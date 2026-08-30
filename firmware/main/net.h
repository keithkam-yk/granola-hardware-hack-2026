// Joins a known network and keeps a link to the game host, without being asked
// anything at play time. Requires link_start() and hud_start() first.
#pragma once

#include "esp_err.h"

esp_err_t net_start(void);

// Teaches the board a network. It keeps several and joins whichever is in the
// room, so this is said once per place rather than once per session. The
// password goes to NVS and is never echoed back over the link.
void net_add_network(const char *ssid, const char *password);

// Names the game host directly, for a network where nothing can be found by
// asking. Normally unnecessary: a host that answers once is remembered.
void net_set_host(const char *ip, uint16_t port);
