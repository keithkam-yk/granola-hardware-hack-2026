#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "lvgl.h"

#include "hud.h"

static const char *TAG = "hud";

// Damage reads as a colour before it reads as a number, so the same three
// steps are used everywhere: intact, hurt, failing.
#define COLOR_OK      0x35D07F
#define COLOR_WARN    0xF5A623
#define COLOR_CRIT    0xFF4D4D
#define COLOR_BG      0x05070A
#define COLOR_PANEL   0x121820
#define COLOR_DIM     0x6A7686

// The board is flown on its side, so the panel runs landscape: 448 wide by 368
// tall. Every coordinate below is in that frame. The IMU has to be rotated the
// same way, and does it against this constant so the two cannot drift apart.
#define HUD_ROTATION  LV_DISPLAY_ROTATION_270
#define DISP_W        BSP_LCD_V_RES
#define DISP_H        BSP_LCD_H_RES

// One card per hardpoint: the weapon, its ammo, and which button fires it.
typedef struct {
    lv_obj_t *card;
    lv_obj_t *name;
    lv_obj_t *ammo;
} weapon_card_t;

static struct {
    lv_obj_t *hull_bar;
    lv_obj_t *hull_pct;
    lv_obj_t *wing_l;
    lv_obj_t *wing_r;
    lv_obj_t *fuselage;
    lv_obj_t *speed;
    weapon_card_t weapon[2];
    hud_tap_cb_t on_tap;
} s_hud;

static lv_color_t damage_color(int health)
{
    if (health > 66) return lv_color_hex(COLOR_OK);
    if (health > 33) return lv_color_hex(COLOR_WARN);
    return lv_color_hex(COLOR_CRIT);
}

static void card_clicked(lv_event_t *event)
{
    if (!s_hud.on_tap) return;
    s_hud.on_tap((int)(intptr_t)lv_event_get_user_data(event));
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

// A wing or the fuselage: a plain block whose colour is its condition. Abstract
// on purpose — a recognisable plane silhouette reads worse at a glance than
// three blocks in the same arrangement as the real thing.
static lv_obj_t *make_section(lv_obj_t *parent, int w, int h, int x, int y)
{
    lv_obj_t *block = lv_obj_create(parent);
    lv_obj_remove_style_all(block);
    lv_obj_set_size(block, w, h);
    lv_obj_set_pos(block, x, y);
    lv_obj_set_style_radius(block, 4, 0);
    lv_obj_set_style_bg_opa(block, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(block, lv_color_hex(COLOR_OK), 0);
    return block;
}

static void build_weapon_card(int side, int x)
{
    weapon_card_t *card = &s_hud.weapon[side];

    card->card = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(card->card);
    lv_obj_set_size(card->card, 200, 104);
    lv_obj_set_pos(card->card, x, DISP_H - 120);
    lv_obj_set_style_radius(card->card, 10, 0);
    lv_obj_set_style_bg_opa(card->card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card->card, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_border_width(card->card, 1, 0);
    lv_obj_set_style_border_color(card->card, lv_color_hex(COLOR_DIM), 0);
    lv_obj_add_flag(card->card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card->card, card_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)side);

    lv_obj_t *button = make_label(card->card, &lv_font_montserrat_14, COLOR_DIM,
                                 side == 0 ? "BOOT" : "PWR");
    lv_obj_align(button, LV_ALIGN_TOP_LEFT, 12, 10);

    card->name = make_label(card->card, &lv_font_montserrat_20, 0xFFFFFF, "-");
    lv_obj_align(card->name, LV_ALIGN_TOP_LEFT, 12, 34);

    card->ammo = make_label(card->card, &lv_font_montserrat_28, COLOR_OK, "0");
    lv_obj_align(card->ammo, LV_ALIGN_BOTTOM_LEFT, 12, -12);
}

esp_err_t hud_start(hud_tap_cb_t on_tap)
{
    s_hud.on_tap = on_tap;

    // The BSP builds its own LVGL display config and ignores the buffer fields
    // of bsp_display_cfg_t, so there is nothing to pass here: draw-buffer size
    // and placement are Kconfig settings (CONFIG_BSP_DISPLAY_LVGL_*), not
    // arguments.
    lv_display_t *display = bsp_display_start();
    if (display == NULL) {
        ESP_LOGE(TAG, "display init failed");
        return ESP_FAIL;
    }
    bsp_display_brightness_set(100);

    bsp_display_lock(0);

    // The LVGL task is already running by now, so the rotation has to be set
    // under the same mutex as every other lv_ call.
    bsp_display_rotate(display, HUD_ROTATION);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    lv_obj_t *title = make_label(scr, &lv_font_montserrat_14, COLOR_DIM, "HULL");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 14);

    s_hud.hull_pct = make_label(scr, &lv_font_montserrat_28, 0xFFFFFF, "--%");
    lv_obj_align(s_hud.hull_pct, LV_ALIGN_TOP_RIGHT, -16, 6);

    s_hud.hull_bar = lv_bar_create(scr);
    lv_obj_set_size(s_hud.hull_bar, DISP_W - 32, 14);
    lv_obj_set_pos(s_hud.hull_bar, 16, 48);
    lv_obj_set_style_bg_color(s_hud.hull_bar, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_bg_color(s_hud.hull_bar, lv_color_hex(COLOR_OK), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_hud.hull_bar, 7, 0);
    lv_obj_set_style_radius(s_hud.hull_bar, 7, LV_PART_INDICATOR);
    lv_bar_set_value(s_hud.hull_bar, 100, LV_ANIM_OFF);

    // Wings sit either side of the fuselage in the same layout as the plane, so
    // "the left one is red" needs no translating while you are flying.
    s_hud.wing_l   = make_section(scr, 140, 32, 30, 126);
    s_hud.fuselage = make_section(scr, 90, 92, 179, 96);
    s_hud.wing_r   = make_section(scr, 140, 32, 278, 126);

    s_hud.speed = make_label(scr, &lv_font_montserrat_20, COLOR_DIM, "--- kt");
    lv_obj_align(s_hud.speed, LV_ALIGN_TOP_MID, 0, 210);

    build_weapon_card(0, 16);
    build_weapon_card(1, DISP_W - 216);

    bsp_display_unlock();
    return ESP_OK;
}

static void set_weapon(int side, const hud_weapon_t *weapon)
{
    weapon_card_t *card = &s_hud.weapon[side];
    lv_label_set_text(card->name, weapon->name);
    lv_label_set_text_fmt(card->ammo, "%d", weapon->ammo);
    lv_obj_set_style_text_color(card->ammo,
                               weapon->ammo == 0 ? lv_color_hex(COLOR_CRIT) : lv_color_hex(COLOR_OK), 0);
}

void hud_set(const hud_state_t *state)
{
    bsp_display_lock(0);

    lv_bar_set_value(s_hud.hull_bar, state->hull, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_hud.hull_bar, damage_color(state->hull), LV_PART_INDICATOR);
    lv_label_set_text_fmt(s_hud.hull_pct, "%d%%", state->hull);

    lv_obj_set_style_bg_color(s_hud.wing_l, damage_color(state->wing_l), 0);
    lv_obj_set_style_bg_color(s_hud.wing_r, damage_color(state->wing_r), 0);
    lv_obj_set_style_bg_color(s_hud.fuselage, damage_color(state->hull), 0);

    lv_label_set_text_fmt(s_hud.speed, "%d kt", state->speed);

    set_weapon(0, &state->left);
    set_weapon(1, &state->right);

    bsp_display_unlock();
}
