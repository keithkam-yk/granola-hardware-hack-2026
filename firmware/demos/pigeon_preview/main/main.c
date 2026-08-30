#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_check.h"
#include "esp_err.h"
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SCREEN_WIDTH 448
#define SCREEN_HEIGHT 368
#define DISPLAY_BUFFER_LINES 100
#define BUTTON_POLL_MS 20
#define BUTTON_DEBOUNCE_SAMPLES 2
#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define AXP2101_I2C_ADDRESS 0x34
#define AXP2101_INTEN2_REGISTER 0x41
#define AXP2101_INTSTS2_REGISTER 0x49
#define AXP2101_PKEY_SHORT_IRQ 0x08
#define AXP2101_PKEY_LONG_IRQ 0x04
#define PWR_EVENT_PULSE_MS 650
#define I2C_TIMEOUT_MS 100

extern const uint8_t pigeon_screen_start[] asm("_binary_pigeon_screen_rgb565_start");
extern const uint8_t pigeon_screen_end[] asm("_binary_pigeon_screen_rgb565_end");

static const char *TAG = "pigeon_preview";
static volatile bool s_boot_pressed;
static volatile bool s_pwr_pressed;
static i2c_master_dev_handle_t s_axp2101;
static lv_obj_t *s_flap_highlight;
static lv_obj_t *s_deuce_highlight;
static lv_obj_t *s_action_label;
static lv_obj_t *s_flap_marks;
static lv_obj_t *s_deuce_payload;

static lv_image_dsc_t s_screen_image = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RGB565,
        .flags = 0,
        .w = SCREEN_WIDTH,
        .h = SCREEN_HEIGHT,
        .stride = SCREEN_WIDTH * sizeof(uint16_t),
        .reserved_2 = 0,
    },
    .data_size = SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t),
    .data = pigeon_screen_start,
    .reserved = NULL,
};

static void display_rounder(lv_area_t *area)
{
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}

static lv_display_t *start_safe_display(void)
{
    const lvgl_port_cfg_t lvgl_config = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_config));

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t panel_io = NULL;
    const bsp_display_config_t panel_config = {
        .max_transfer_sz = BSP_LCD_H_RES * DISPLAY_BUFFER_LINES * sizeof(lv_color16_t),
    };
    ESP_ERROR_CHECK(bsp_display_new(&panel_config, &panel, &panel_io));

    const lvgl_port_display_cfg_t display_config = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .buffer_size = BSP_LCD_H_RES * DISPLAY_BUFFER_LINES,
        .double_buffer = false,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .monochrome = false,
        .rounder_cb = display_rounder,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = true,
            .swap_bytes = true,
        },
    };
    lv_display_t *display = lvgl_port_add_disp(&display_config);
    if (display == NULL) {
        ESP_LOGE(TAG, "could not register display");
        return NULL;
    }

    ESP_ERROR_CHECK(bsp_display_brightness_init());
    ESP_ERROR_CHECK(bsp_display_brightness_set(100));
    return display;
}

static lv_obj_t *make_button_highlight(lv_obj_t *parent, int x, lv_color_t color)
{
    lv_obj_t *highlight = lv_obj_create(parent);
    lv_obj_set_pos(highlight, x, 62);
    lv_obj_set_size(highlight, 113, 250);
    lv_obj_set_style_radius(highlight, 20, 0);
    lv_obj_set_style_bg_color(highlight, color, 0);
    lv_obj_set_style_bg_opa(highlight, LV_OPA_30, 0);
    lv_obj_set_style_border_color(highlight, color, 0);
    lv_obj_set_style_border_width(highlight, 5, 0);
    lv_obj_set_style_shadow_color(highlight, color, 0);
    lv_obj_set_style_shadow_width(highlight, 18, 0);
    lv_obj_set_style_shadow_opa(highlight, LV_OPA_70, 0);
    lv_obj_clear_flag(highlight, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(highlight, LV_OBJ_FLAG_HIDDEN);
    return highlight;
}

static void create_ui(lv_display_t *display)
{
    lv_display_set_rotation(display, LV_DISPLAY_ROTATION_270);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0B1821), 0);

    lv_obj_t *background = lv_image_create(screen);
    lv_image_set_src(background, &s_screen_image);
    lv_obj_set_pos(background, 0, 0);
    lv_obj_clear_flag(background, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_flap_highlight = make_button_highlight(screen, 16, lv_color_hex(0xFFC93D));
    s_deuce_highlight = make_button_highlight(screen, 319, lv_color_hex(0x61E0D2));

    s_flap_marks = lv_label_create(screen);
    lv_label_set_text(s_flap_marks, "^  ^");
    lv_obj_set_style_text_color(s_flap_marks, lv_color_hex(0xFFC93D), 0);
    lv_obj_set_style_text_font(s_flap_marks, &lv_font_montserrat_20, 0);
    lv_obj_align(s_flap_marks, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_add_flag(s_flap_marks, LV_OBJ_FLAG_HIDDEN);

    s_deuce_payload = lv_label_create(screen);
    lv_label_set_text(s_deuce_payload, "v");
    lv_obj_set_style_text_color(s_deuce_payload, lv_color_hex(0xF6D071), 0);
    lv_obj_set_style_text_font(s_deuce_payload, &lv_font_montserrat_20, 0);
    lv_obj_align(s_deuce_payload, LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_obj_add_flag(s_deuce_payload, LV_OBJ_FLAG_HIDDEN);

    s_action_label = lv_label_create(screen);
    lv_obj_set_style_bg_color(s_action_label, lv_color_hex(0x111D28), 0);
    lv_obj_set_style_bg_opa(s_action_label, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_action_label, 8, 0);
    lv_obj_set_style_pad_hor(s_action_label, 10, 0);
    lv_obj_set_style_pad_ver(s_action_label, 5, 0);
    lv_obj_set_style_text_font(s_action_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_action_label, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_add_flag(s_action_label, LV_OBJ_FLAG_HIDDEN);
}

static void ui_timer(lv_timer_t *timer)
{
    (void)timer;
    static bool previous_flap;
    static bool previous_deuce;
    const bool flap = s_boot_pressed;
    const bool deuce = s_pwr_pressed;

    if (flap != previous_flap) {
        if (flap) {
            lv_obj_clear_flag(s_flap_highlight, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_flap_marks, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_action_label, "BOOT // FLAP FLAP!");
            lv_obj_set_style_text_color(s_action_label, lv_color_hex(0xFFC93D), 0);
            lv_obj_clear_flag(s_action_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_flap_highlight, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_flap_marks, LV_OBJ_FLAG_HIDDEN);
            if (!deuce) {
                lv_obj_add_flag(s_action_label, LV_OBJ_FLAG_HIDDEN);
            }
        }
        previous_flap = flap;
    }

    if (deuce != previous_deuce) {
        if (deuce) {
            lv_obj_clear_flag(s_deuce_highlight, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_deuce_payload, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_action_label, "PWR // DEUCE AWAY!");
            lv_obj_set_style_text_color(s_action_label, lv_color_hex(0x61E0D2), 0);
            lv_obj_clear_flag(s_action_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_deuce_highlight, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_deuce_payload, LV_OBJ_FLAG_HIDDEN);
            if (!flap) {
                lv_obj_add_flag(s_action_label, LV_OBJ_FLAG_HIDDEN);
            }
        }
        previous_deuce = deuce;
    }

    if (flap) {
        const uint32_t phase = lv_tick_get() / 120;
        lv_obj_set_y(s_flap_marks, 48 + (phase & 1U) * 6);
    }
    if (deuce) {
        const uint32_t phase = (lv_tick_get() / 50) % 7;
        lv_obj_align(s_deuce_payload, LV_ALIGN_BOTTOM_MID, 0, -58 + (int32_t)phase * 6);
    }
}

static esp_err_t axp2101_read_register(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_axp2101, &reg, sizeof(reg), value, sizeof(*value),
                                       I2C_TIMEOUT_MS);
}

static esp_err_t axp2101_write_register(uint8_t reg, uint8_t value)
{
    const uint8_t command[] = {reg, value};
    return i2c_master_transmit(s_axp2101, command, sizeof(command), I2C_TIMEOUT_MS);
}

static esp_err_t init_buttons(i2c_master_bus_handle_t bus_handle)
{
    const gpio_config_t boot_config = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&boot_config), TAG, "BOOT GPIO setup failed");

    const i2c_device_config_t axp2101_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_I2C_ADDRESS,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus_handle, &axp2101_config, &s_axp2101),
                        TAG, "AXP2101 setup failed");

    uint8_t interrupts_enabled = 0;
    ESP_RETURN_ON_ERROR(axp2101_read_register(AXP2101_INTEN2_REGISTER, &interrupts_enabled),
                        TAG, "AXP2101 INTEN2 read failed");
    const uint8_t events = AXP2101_PKEY_SHORT_IRQ | AXP2101_PKEY_LONG_IRQ;
    ESP_RETURN_ON_ERROR(axp2101_write_register(AXP2101_INTEN2_REGISTER,
                                               interrupts_enabled | events),
                        TAG, "AXP2101 INTEN2 write failed");
    return axp2101_write_register(AXP2101_INTSTS2_REGISTER, events);
}

static void button_task(void *argument)
{
    (void)argument;
    bool previous_raw_boot = false;
    uint8_t stable_samples = 0;
    int64_t pwr_release_at_ms = 0;

    while (true) {
        const int64_t now_ms = esp_timer_get_time() / 1000;
        const bool raw_boot = gpio_get_level(BOOT_BUTTON_GPIO) == 0;
        if (raw_boot == previous_raw_boot) {
            if (stable_samples < BUTTON_DEBOUNCE_SAMPLES) {
                ++stable_samples;
            }
        } else {
            previous_raw_boot = raw_boot;
            stable_samples = 1;
        }
        if (stable_samples >= BUTTON_DEBOUNCE_SAMPLES && s_boot_pressed != raw_boot) {
            s_boot_pressed = raw_boot;
            ESP_LOGI(TAG, "BOOT / flap %s", raw_boot ? "on" : "off");
        }

        uint8_t status = 0;
        if (axp2101_read_register(AXP2101_INTSTS2_REGISTER, &status) == ESP_OK) {
            const uint8_t events = status & (AXP2101_PKEY_SHORT_IRQ | AXP2101_PKEY_LONG_IRQ);
            if (events != 0) {
                axp2101_write_register(AXP2101_INTSTS2_REGISTER, events);
                s_pwr_pressed = true;
                pwr_release_at_ms = now_ms + PWR_EVENT_PULSE_MS;
                ESP_LOGI(TAG, "PWR / deuce triggered");
            }
        }
        if (s_pwr_pressed && now_ms >= pwr_release_at_ms) {
            s_pwr_pressed = false;
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

void app_main(void)
{
    const size_t asset_size = (size_t)(pigeon_screen_end - pigeon_screen_start);
    if (asset_size != s_screen_image.data_size) {
        ESP_LOGE(TAG, "screen asset has %zu bytes; expected %" PRIu32,
                 asset_size, s_screen_image.data_size);
        return;
    }

    ESP_ERROR_CHECK(bsp_i2c_init());
    lv_display_t *display = start_safe_display();
    if (display == NULL) {
        return;
    }

    if (bsp_display_lock(0)) {
        create_ui(display);
        lv_timer_create(ui_timer, 30, NULL);
        bsp_display_unlock();
    } else {
        ESP_LOGE(TAG, "could not lock display for UI setup");
        return;
    }

    ESP_ERROR_CHECK(init_buttons(bsp_i2c_get_handle()));
    xTaskCreate(button_task, "pigeon_buttons", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "ready: hold BOOT to flap, tap PWR for deuce");
}
