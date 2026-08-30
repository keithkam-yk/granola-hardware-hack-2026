// The instrument panel.
//
// Everything drawn here is computed on the host and sent down the USB link the
// IMU samples go up. The device decides nothing about the game; it only shows
// what it is told and reports what you touch.
#pragma once

#include "esp_err.h"

#define HUD_WEAPON_NAME_MAX 12

typedef struct {
    char name[HUD_WEAPON_NAME_MAX];
    int  ammo;
} hud_weapon_t;

typedef struct {
    int hull;               // 0-100 overall airframe integrity
    int wing_l, wing_r;     // 0-100 per side; damage here costs turn rate that way
    int speed;              // knots, for the pilot's benefit only
    hud_weapon_t left, right;
} hud_state_t;

// Fired when a weapon card is tapped. side is 0 for left, 1 for right.
typedef void (*hud_tap_cb_t)(int side);

// Brings up the panel and the touch input. Must be called once, before hud_set.
esp_err_t hud_start(hud_tap_cb_t on_tap);

// Redraws the panel. Safe to call from any task except an LVGL callback.
void hud_set(const hud_state_t *state);

// One line on the panel saying where the board is up to in finding a game.
// Without it, a controller that will not connect is a black box you have to
// take back to a laptop to interrogate.
void hud_set_link(const char *status);
