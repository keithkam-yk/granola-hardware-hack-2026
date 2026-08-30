#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "lvgl.h"

#include "hud.h"

static const char *TAG = "hud";

// The panel art, embedded by CMakeLists and shared with demos/pigeon_preview.
extern const uint8_t pigeon_screen_start[] asm("_binary_pigeon_screen_rgb565_start");
extern const uint8_t flap_frames_start[] asm("_binary_flap_wings_animation_v1_argb8888_start");
extern const uint8_t deuce_frames_start[] asm("_binary_deuce_payload_animation_v1_argb8888_start");

#define ART_W 448
#define ART_H 368
#define ACTION_FRAMES 4
#define FLAP_W 84
#define FLAP_H 67
#define DEUCE_W 56
#define DEUCE_H 84

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
    lv_obj_t *link;
    weapon_card_t weapon[2];
    hud_tap_cb_t on_tap;
} s_hud;

// One action loop: four predecoded frames and where on the panel it sits.
typedef struct {
    lv_image_dsc_t frames[ACTION_FRAMES];
    lv_obj_t *image;
    lv_timer_t *timer;
    int frame;
    const uint16_t *durations_ms;
} action_t;

static const uint16_t s_flap_durations_ms[ACTION_FRAMES] = {110, 90, 110, 90};
static const uint16_t s_deuce_durations_ms[ACTION_FRAMES] = {140, 120, 180, 260};
static action_t s_flap = { .durations_ms = s_flap_durations_ms };
static action_t s_deuce = { .durations_ms = s_deuce_durations_ms };

static lv_image_dsc_t s_screen_image = {
    .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                .flags = 0, .w = ART_W, .h = ART_H,
                .stride = ART_W * sizeof(uint16_t), .reserved_2 = 0 },
    .data_size = ART_W * ART_H * sizeof(uint16_t),
    .data = pigeon_screen_start,
    .reserved = NULL,
};

// Frames are stored back to back, so each one is just an offset into the blob.
static void describe_frames(action_t *action, const uint8_t *blob, uint32_t w, uint32_t h)
{
    const uint32_t bytes = w * h * 4;
    for (int i = 0; i < ACTION_FRAMES; i++) {
        action->frames[i] = (lv_image_dsc_t){
            .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888,
                        .flags = 0, .w = w, .h = h, .stride = w * 4, .reserved_2 = 0 },
            .data_size = bytes,
            .data = blob + (uint32_t)i * bytes,
            .reserved = NULL,
        };
    }
}

// Advances one frame, and hides the loop again once it has played out. A press
// during playback restarts it rather than queuing, because a pigeon beating
// twice quickly should look like two beats, not like one beat played late.
static void action_tick(lv_timer_t *timer)
{
    action_t *action = lv_timer_get_user_data(timer);
    action->frame++;
    if (action->frame >= ACTION_FRAMES) {
        lv_obj_add_flag(action->image, LV_OBJ_FLAG_HIDDEN);
        lv_timer_pause(timer);
        return;
    }
    lv_image_set_src(action->image, &action->frames[action->frame]);
    lv_timer_set_period(timer, action->durations_ms[action->frame]);
}

static void action_play(action_t *action)
{
    if (!action->image) return;
    action->frame = 0;
    lv_image_set_src(action->image, &action->frames[0]);
    lv_obj_remove_flag(action->image, LV_OBJ_FLAG_HIDDEN);
    lv_timer_set_period(action->timer, action->durations_ms[0]);
    lv_timer_reset(action->timer);
    lv_timer_resume(action->timer);
}

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
    lv_obj_align(s_hud.speed, LV_ALIGN_TOP_MID, 0, 200);

    // The pigeon panel goes in behind everything. The dogfight instruments are
    // kept rather than deleted — the widgets and hud_set() still work — but
    // hidden, because this is a bird now and hull integrity means nothing to it.
    lv_obj_t *art = lv_image_create(scr);
    lv_image_set_src(art, &s_screen_image);
    lv_obj_set_pos(art, 0, 0);
    lv_obj_move_background(art);
    lv_obj_t *const instruments[] = {
        s_hud.hull_bar, s_hud.hull_pct, s_hud.wing_l, s_hud.wing_r,
        s_hud.fuselage, s_hud.speed,
        s_hud.weapon[0].card, s_hud.weapon[1].card,
    };
    for (size_t i = 0; i < sizeof(instruments)/sizeof(instruments[0]); i++) {
        if (instruments[i]) lv_obj_add_flag(instruments[i], LV_OBJ_FLAG_HIDDEN);
    }

    describe_frames(&s_flap, flap_frames_start, FLAP_W, FLAP_H);
    describe_frames(&s_deuce, deuce_frames_start, DEUCE_W, DEUCE_H);
    s_flap.image = lv_image_create(scr);
    lv_obj_align(s_flap.image, LV_ALIGN_BOTTOM_LEFT, 46, -54);
    lv_obj_add_flag(s_flap.image, LV_OBJ_FLAG_HIDDEN);
    s_flap.timer = lv_timer_create(action_tick, 110, &s_flap);
    lv_timer_pause(s_flap.timer);
    s_deuce.image = lv_image_create(scr);
    lv_obj_align(s_deuce.image, LV_ALIGN_BOTTOM_RIGHT, -60, -46);
    lv_obj_add_flag(s_deuce.image, LV_OBJ_FLAG_HIDDEN);
    s_deuce.timer = lv_timer_create(action_tick, 140, &s_deuce);
    lv_timer_pause(s_deuce.timer);

    s_hud.link = make_label(scr, &lv_font_montserrat_14, COLOR_DIM, "starting");
    lv_obj_align(s_hud.link, LV_ALIGN_TOP_MID, 0, 228);

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

void hud_action(int side)
{
    if (!s_flap.image) return;
    bsp_display_lock(0);
    action_play(side ? &s_flap : &s_deuce);
    bsp_display_unlock();
}

void hud_set_link(const char *status)
{
    if (!s_hud.link) return;
    bsp_display_lock(0);
    lv_label_set_text(s_hud.link, status);
    lv_obj_align(s_hud.link, LV_ALIGN_TOP_MID, 0, 228);
    bsp_display_unlock();
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
