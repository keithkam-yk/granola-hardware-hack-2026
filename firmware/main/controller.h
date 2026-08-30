// Reads the stick and streams it to the host. Requires bsp_i2c_init() and
// link_start() to have run first.
#pragma once

#include "esp_err.h"

esp_err_t controller_start(void);
