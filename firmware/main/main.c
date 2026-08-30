// Dogfight controller.
//
// The board is a client: it reports what the pilot is doing and draws the panel
// state the host sends back. No game rule is decided here.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "controller.h"
#include "hud.h"
#include "link.h"

static hud_state_t s_panel = {
    .hull = 100, .wing_l = 100, .wing_r = 100, .speed = 0,
    .left = {"---", 0}, .right = {"---", 0},
};

static void assign(const char *key, const char *value)
{
    if (!strcmp(key, "hp")) s_panel.hull = atoi(value);
    else if (!strcmp(key, "wl")) s_panel.wing_l = atoi(value);
    else if (!strcmp(key, "wr")) s_panel.wing_r = atoi(value);
    else if (!strcmp(key, "spd")) s_panel.speed = atoi(value);
    else if (!strcmp(key, "gl")) snprintf(s_panel.left.name, HUD_WEAPON_NAME_MAX, "%s", value);
    else if (!strcmp(key, "gr")) snprintf(s_panel.right.name, HUD_WEAPON_NAME_MAX, "%s", value);
    else if (!strcmp(key, "al")) s_panel.left.ammo = atoi(value);
    else if (!strcmp(key, "ar")) s_panel.right.ammo = atoi(value);
    // Anything unrecognised is ignored on purpose: the host can add fields
    // before the firmware knows about them without bricking the panel.
}

// "!hud hp=87 wl=40 gl=CANNON al=120 ..." — every field optional, so the host
// may send only what changed.
static void host_said(const char *line)
{
    if (strncmp(line, "!hud ", 5) != 0) return;

    char buffer[256];
    snprintf(buffer, sizeof(buffer), "%s", line + 5);

    char *save = NULL;
    for (char *token = strtok_r(buffer, " ", &save); token; token = strtok_r(NULL, " ", &save)) {
        char *equals = strchr(token, '=');
        if (!equals) continue;
        *equals = '\0';
        assign(token, equals + 1);
    }
    hud_set(&s_panel);
}

static void weapon_tapped(int side)
{
    link_sendf("!sel side=%c", side == 0 ? 'L' : 'R');
}

void app_main(void)
{
    ESP_ERROR_CHECK(bsp_i2c_init());
    ESP_ERROR_CHECK(link_start(host_said));
    ESP_ERROR_CHECK(hud_start(weapon_tapped));

    hud_set(&s_panel);

    ESP_ERROR_CHECK(controller_start());
}
