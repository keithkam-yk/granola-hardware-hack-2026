// Dogfight controller.
//
// Display bring-up: proves the panel, the touch input and the HUD layout before
// there is a game to feed them. The mock state below is the only game logic that
// will ever live on the device; it goes away once the host is sending real state.
#include <stdio.h>

#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hud.h"

static void weapon_tapped(int side)
{
    printf("!sel side=%c\n", side == 0 ? 'L' : 'R');
}

void app_main(void)
{
    ESP_ERROR_CHECK(bsp_i2c_init());
    ESP_ERROR_CHECK(hud_start(weapon_tapped));

    hud_state_t mock = {
        .hull = 100,
        .wing_l = 100,
        .wing_r = 100,
        .speed = 0,
        .left = {"CANNON", 240},
        .right = {"MISSILE", 4},
    };

    // Walks the damage down so every colour step gets shown once, which is the
    // whole point of this build: see the panel do the thing before trusting it.
    while (1) {
        hud_set(&mock);
        vTaskDelay(pdMS_TO_TICKS(500));

        mock.hull = mock.hull > 0 ? mock.hull - 4 : 100;
        mock.wing_l = mock.wing_l > 0 ? mock.wing_l - 7 : 100;
        mock.wing_r = mock.wing_r > 0 ? mock.wing_r - 2 : 100;
        mock.speed = 180 + (mock.hull % 7) * 20;
        if (mock.left.ammo > 0) mock.left.ammo -= 3;
    }
}
