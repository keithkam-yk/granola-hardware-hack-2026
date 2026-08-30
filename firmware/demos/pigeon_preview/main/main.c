#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
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
#define PWR_EVENT_PULSE_MS 700
#define I2C_TIMEOUT_MS 100
#define ACTION_FRAME_COUNT 4
#define FLAP_WIDTH 84
#define FLAP_HEIGHT 67
#define FLAP_DURATION_MS 400
#define DEUCE_WIDTH 56
#define DEUCE_HEIGHT 84
#define DEUCE_DURATION_MS 700

extern const uint8_t pigeon_screen_start[] asm("_binary_pigeon_screen_rgb565_start");
extern const uint8_t pigeon_screen_end[] asm("_binary_pigeon_screen_rgb565_end");
extern const uint8_t flap_frames_start[] asm("_binary_flap_wings_animation_v1_argb8888_start");
extern const uint8_t flap_frames_end[] asm("_binary_flap_wings_animation_v1_argb8888_end");
extern const uint8_t deuce_frames_start[] asm("_binary_deuce_payload_animation_v1_argb8888_start");
extern const uint8_t deuce_frames_end[] asm("_binary_deuce_payload_animation_v1_argb8888_end");

static const char *TAG = "pigeon_preview";
static volatile bool s_boot_pressed;
static volatile bool s_pwr_pressed;
static i2c_master_dev_handle_t s_axp2101;
static lv_obj_t *s_flap_highlight;
static lv_obj_t *s_deuce_highlight;
static lv_obj_t *s_action_label;
static lv_obj_t *s_flap_animation;
static lv_obj_t *s_deuce_animation;

typedef struct {
    lv_image_dsc_t frames[ACTION_FRAME_COUNT];
    const uint16_t *durations_ms;
    uint16_t total_duration_ms;
} action_animation_t;

static const uint16_t s_flap_durations_ms[ACTION_FRAME_COUNT] = {110, 90, 110, 90};
static const uint16_t s_deuce_durations_ms[ACTION_FRAME_COUNT] = {140, 120, 180, 260};
static action_animation_t s_flap = {
    .durations_ms = s_flap_durations_ms,
    .total_duration_ms = FLAP_DURATION_MS,
};
static action_animation_t s_deuce = {
    .durations_ms = s_deuce_durations_ms,
    .total_duration_ms = DEUCE_DURATION_MS,
};

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

static bool init_action_animation(action_animation_t *animation, const uint8_t *data,
                                  const uint8_t *data_end, uint16_t width, uint16_t height,
                                  const char *name)
{
    const uint32_t source_frame_size = (uint32_t)width * height * sizeof(lv_color32_t);
    const uint32_t display_frame_size = SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t);
    const size_t actual_size = (size_t)(data_end - data);
    const size_t expected_size = source_frame_size * ACTION_FRAME_COUNT;
    if (actual_size != expected_size) {
        ESP_LOGE(TAG, "%s frames have %zu bytes; expected %zu", name, actual_size,
                 expected_size);
        return false;
    }

    uint16_t *display_frames = heap_caps_malloc(display_frame_size * ACTION_FRAME_COUNT,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (display_frames == NULL) {
        ESP_LOGE(TAG, "could not allocate full-screen %s frames in PSRAM", name);
        return false;
    }

    for (uint32_t index = 0; index < ACTION_FRAME_COUNT; ++index) {
        const uint8_t *source = data + index * source_frame_size;
        uint16_t *destination = display_frames + index * SCREEN_WIDTH * SCREEN_HEIGHT;
        for (uint32_t y = 0; y < SCREEN_HEIGHT; ++y) {
            const uint32_t source_y = y * height / SCREEN_HEIGHT;
            for (uint32_t x = 0; x < SCREEN_WIDTH; ++x) {
                const uint32_t source_x = x * width / SCREEN_WIDTH;
                const uint8_t *pixel = source + (source_y * width + source_x) * 4;
                if (pixel[3] < 128) {
                    destination[y * SCREEN_WIDTH + x] = 0;
                } else {
                    const uint16_t red = pixel[2] >> 3;
                    const uint16_t green = pixel[1] >> 2;
                    const uint16_t blue = pixel[0] >> 3;
                    destination[y * SCREEN_WIDTH + x] = (red << 11) | (green << 5) | blue;
                }
            }
        }

        animation->frames[index] = (lv_image_dsc_t) {
            .header = {
                .magic = LV_IMAGE_HEADER_MAGIC,
                .cf = LV_COLOR_FORMAT_RGB565,
                .flags = 0,
                .w = SCREEN_WIDTH,
                .h = SCREEN_HEIGHT,
                .stride = SCREEN_WIDTH * sizeof(uint16_t),
                .reserved_2 = 0,
            },
            .data_size = display_frame_size,
            .data = (const uint8_t *)destination,
            .reserved = NULL,
        };
    }
    ESP_LOGI(TAG, "%s animation ready: %d full-screen frames, %u ms", name,
             ACTION_FRAME_COUNT, animation->total_duration_ms);
    return true;
}

static uint32_t animation_frame_at(const action_animation_t *animation, uint32_t elapsed_ms)
{
    uint32_t phase_ms = elapsed_ms % animation->total_duration_ms;
    for (uint32_t index = 0; index < ACTION_FRAME_COUNT; ++index) {
        if (phase_ms < animation->durations_ms[index]) {
            return index;
        }
        phase_ms -= animation->durations_ms[index];
    }
    return ACTION_FRAME_COUNT - 1;
}

static lv_obj_t *create_action_animation(lv_obj_t *parent, const action_animation_t *animation)
{
    lv_obj_t *image = lv_image_create(parent);
    lv_image_set_src(image, &animation->frames[0]);
    lv_obj_set_pos(image, 0, 0);
    lv_obj_clear_flag(image, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
    return image;
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

    s_flap_animation = create_action_animation(screen, &s_flap);
    s_deuce_animation = create_action_animation(screen, &s_deuce);

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
    static uint32_t flap_started_at_ms;
    static uint32_t deuce_started_at_ms;
    static int32_t displayed_flap_frame = -1;
    static int32_t displayed_deuce_frame = -1;
    static uint32_t flap_frame_changes;
    static uint32_t deuce_frame_changes;
    const uint32_t now_ms = lv_tick_get();
    const bool flap = s_boot_pressed;
    const bool deuce = s_pwr_pressed;

    if (flap != previous_flap) {
        if (flap) {
            lv_obj_clear_flag(s_flap_highlight, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_flap_animation, LV_OBJ_FLAG_HIDDEN);
            flap_started_at_ms = now_ms;
            displayed_flap_frame = -1;
            flap_frame_changes = 0;
            lv_label_set_text(s_action_label, "BOOT // FLAP FLAP!");
            lv_obj_set_style_text_color(s_action_label, lv_color_hex(0xFFC93D), 0);
            lv_obj_clear_flag(s_action_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_flap_highlight, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_flap_animation, LV_OBJ_FLAG_HIDDEN);
            displayed_flap_frame = -1;
            ESP_LOGI(TAG, "FLAP playback completed with %" PRIu32 " frame changes",
                     flap_frame_changes);
            if (!deuce) {
                lv_obj_add_flag(s_action_label, LV_OBJ_FLAG_HIDDEN);
            }
        }
        previous_flap = flap;
    }

    if (deuce != previous_deuce) {
        if (deuce) {
            lv_obj_clear_flag(s_deuce_highlight, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_deuce_animation, LV_OBJ_FLAG_HIDDEN);
            deuce_started_at_ms = now_ms;
            displayed_deuce_frame = -1;
            deuce_frame_changes = 0;
            lv_label_set_text(s_action_label, "PWR // DEUCE AWAY!");
            lv_obj_set_style_text_color(s_action_label, lv_color_hex(0x61E0D2), 0);
            lv_obj_clear_flag(s_action_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_deuce_highlight, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_deuce_animation, LV_OBJ_FLAG_HIDDEN);
            displayed_deuce_frame = -1;
            ESP_LOGI(TAG, "DEUCE playback completed with %" PRIu32 " frame changes",
                     deuce_frame_changes);
            if (!flap) {
                lv_obj_add_flag(s_action_label, LV_OBJ_FLAG_HIDDEN);
            }
        }
        previous_deuce = deuce;
    }

    if (flap) {
        const uint32_t frame = animation_frame_at(&s_flap, now_ms - flap_started_at_ms);
        if ((int32_t)frame != displayed_flap_frame) {
            lv_image_set_src(s_flap_animation, &s_flap.frames[frame]);
            displayed_flap_frame = (int32_t)frame;
            ++flap_frame_changes;
        }
    }
    if (deuce) {
        const uint32_t frame = animation_frame_at(&s_deuce, now_ms - deuce_started_at_ms);
        if ((int32_t)frame != displayed_deuce_frame) {
            lv_image_set_src(s_deuce_animation, &s_deuce.frames[frame]);
            displayed_deuce_frame = (int32_t)frame;
            ++deuce_frame_changes;
        }
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
    if (!init_action_animation(&s_flap, flap_frames_start, flap_frames_end,
                               FLAP_WIDTH, FLAP_HEIGHT, "FLAP") ||
        !init_action_animation(&s_deuce, deuce_frames_start, deuce_frames_end,
                               DEUCE_WIDTH, DEUCE_HEIGHT, "DEUCE")) {
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
