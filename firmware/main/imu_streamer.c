#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "esp_bt.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_hidd.h"
#include "esp_hid_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#undef M_PI
#include "qmi8658.h"

#define IMU_PROBE_TIMEOUT_MS 100
#define SAMPLE_PERIOD_MS 20
#define QMI8658_RESET_REGISTER 0x60
#define QMI8658_RESET_COMMAND 0xB0
#define QMI8658_CTRL1_VALUE 0x60
#define QMI8658_RESET_DELAY_MS 20

#define GYRO_CALIBRATION_SAMPLES 100
#define GYRO_CALIBRATION_STILL_DPS 8.0f
#define GYRO_DEAD_ZONE_DPS 1.5f
#define GYRO_SMOOTHING_ALPHA 0.24f
#define DEFAULT_SENSITIVITY 0.18f
#define MIN_SENSITIVITY 0.05f
#define MAX_SENSITIVITY 0.80f
#define FLIGHT_ALTITUDE_MIN_M 120
#define FLIGHT_ALTITUDE_MAX_M 2400
#define FLIGHT_ALTITUDE_INITIAL_M 840
#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define BUTTON_DEBOUNCE_SAMPLES 2
#define AXP2101_I2C_ADDRESS 0x34
#define AXP2101_INTEN2_REGISTER 0x41
#define AXP2101_INTSTS2_REGISTER 0x49
#define AXP2101_PKEY_SHORT_IRQ 0x08
#define AXP2101_PKEY_LONG_IRQ 0x04
#define PWR_EVENT_PULSE_MS 300
#define I2C_TIMEOUT_MS 100
#define DISPLAY_BUFFER_LINES 100
#define ORIENTATION_COMPLEMENTARY_ALPHA 0.98f
#define ORIENTATION_EMIT_PERIOD_MS 40

#define HID_BATTERY_LEVEL 100
#define BLE_HID_SERVICE_UUID 0x1812

static const char *TAG = "motion_airmouse";
static esp_hidd_dev_t *s_hid_device;
static volatile bool s_ble_connected;
static volatile bool s_touch_available;
static volatile bool s_calibrated;
static volatile bool s_pwr_pressed;
static volatile bool s_boot_pressed;
static volatile float s_sensitivity = DEFAULT_SENSITIVITY;
static volatile int s_flight_altitude_m = FLIGHT_ALTITUDE_INITIAL_M;
static float s_gyro_bias[3];
static uint32_t s_mouse_reports;
static lv_obj_t *s_status_label;
static lv_obj_t *s_altitude_label;
static lv_obj_t *s_altitude_bar;
static lv_obj_t *s_calibration_overlay;
static lv_obj_t *s_calibration_step_label;
static lv_obj_t *s_calibration_prompt_label;
static lv_obj_t *s_calibration_values_label;
static i2c_master_dev_handle_t s_axp2101;
static lv_indev_t *s_display_input;

typedef enum {
    ORIENTATION_CAL_GYRO = 0,
    ORIENTATION_CAL_CENTER,
    ORIENTATION_CAL_PITCH_FORWARD,
    ORIENTATION_CAL_PITCH_BACK,
    ORIENTATION_CAL_ROLL_RIGHT,
    ORIENTATION_CAL_ROLL_LEFT,
    ORIENTATION_CAL_YAW_CENTER,
    ORIENTATION_CAL_YAW_RIGHT,
    ORIENTATION_CAL_YAW_RETURN_CENTER,
    ORIENTATION_CAL_YAW_LEFT,
    ORIENTATION_CAL_DONE,
} orientation_cal_step_t;

typedef struct {
    float pitch_center;
    float pitch_forward;
    float pitch_back;
    float roll_center;
    float roll_right;
    float roll_left;
    float yaw_right;
    float yaw_left;
} orientation_calibration_t;

static volatile orientation_cal_step_t s_orientation_cal_step = ORIENTATION_CAL_GYRO;
static volatile bool s_orientation_initialized;
static volatile float s_roll_deg;
static volatile float s_pitch_deg;
static volatile float s_yaw_deg;
static volatile float s_roll_normalized;
static volatile float s_pitch_normalized;
static volatile float s_yaw_normalized;
static volatile bool s_orientation_capture_rejected;
static orientation_calibration_t s_orientation_calibration;

static const char *control_mode_name(void)
{
    if (s_pwr_pressed && s_boot_pressed) {
        return "both";
    }
    return s_boot_pressed ? "boot" : (s_pwr_pressed ? "pwr" : "none");
}

static const char *orientation_step_name(orientation_cal_step_t step)
{
    switch (step) {
    case ORIENTATION_CAL_CENTER: return "1 / 9  CENTER";
    case ORIENTATION_CAL_PITCH_FORWARD: return "2 / 9  PITCH FORWARD";
    case ORIENTATION_CAL_PITCH_BACK: return "3 / 9  PITCH BACK";
    case ORIENTATION_CAL_ROLL_RIGHT: return "4 / 9  ROLL RIGHT";
    case ORIENTATION_CAL_ROLL_LEFT: return "5 / 9  ROLL LEFT";
    case ORIENTATION_CAL_YAW_CENTER: return "6 / 9  CENTER YAW";
    case ORIENTATION_CAL_YAW_RIGHT: return "7 / 9  YAW RIGHT";
    case ORIENTATION_CAL_YAW_RETURN_CENTER: return "8 / 9  RETURN CENTER";
    case ORIENTATION_CAL_YAW_LEFT: return "9 / 9  YAW LEFT";
    case ORIENTATION_CAL_DONE: return "CALIBRATED";
    default: return "GYRO CALIBRATION";
    }
}

static const char *orientation_step_prompt(orientation_cal_step_t step)
{
    switch (step) {
    case ORIENTATION_CAL_CENTER: return "Hold naturally and level";
    case ORIENTATION_CAL_PITCH_FORWARD: return "Pitch fully forward";
    case ORIENTATION_CAL_PITCH_BACK: return "Pitch fully backward";
    case ORIENTATION_CAL_ROLL_RIGHT: return "Roll fully right";
    case ORIENTATION_CAL_ROLL_LEFT: return "Roll fully left";
    case ORIENTATION_CAL_YAW_CENTER: return "Return to center";
    case ORIENTATION_CAL_YAW_RIGHT: return "Turn fully right";
    case ORIENTATION_CAL_YAW_RETURN_CENTER: return "Return to center";
    case ORIENTATION_CAL_YAW_LEFT: return "Turn fully left";
    case ORIENTATION_CAL_DONE: return "Roll, pitch and yaw are live";
    default: return "Keep the controller still";
    }
}

static const char *orientation_step_key(orientation_cal_step_t step)
{
    switch (step) {
    case ORIENTATION_CAL_CENTER: return "center";
    case ORIENTATION_CAL_PITCH_FORWARD: return "pitch_forward";
    case ORIENTATION_CAL_PITCH_BACK: return "pitch_back";
    case ORIENTATION_CAL_ROLL_RIGHT: return "roll_right";
    case ORIENTATION_CAL_ROLL_LEFT: return "roll_left";
    case ORIENTATION_CAL_YAW_CENTER: return "yaw_center";
    case ORIENTATION_CAL_YAW_RIGHT: return "yaw_right";
    case ORIENTATION_CAL_YAW_RETURN_CENTER: return "yaw_return_center";
    case ORIENTATION_CAL_YAW_LEFT: return "yaw_left";
    case ORIENTATION_CAL_DONE: return "done";
    default: return "gyro";
    }
}

static const uint8_t s_mouse_report_map[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01,
    0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
    0xC0, 0xC0,
};

static esp_hid_raw_report_map_t s_report_maps[] = {
    {.data = s_mouse_report_map, .len = sizeof(s_mouse_report_map)},
};

static esp_hid_device_config_t s_hid_config = {
    .vendor_id = 0x303A,
    .product_id = 0x8658,
    .version = 0x0100,
    .device_name = "QMI8658 Air Mouse",
    .manufacturer_name = "Granola Hardware Hack",
    .serial_number = "ESP32S3-QMI8658",
    .report_maps = s_report_maps,
    .report_maps_len = 1,
};

static void emit_airmouse_state(void)
{
    printf(
        "{\"type\":\"airmouse\",\"ble\":\"%s\",\"tracking\":%s"
        ",\"clutch\":\"%s\",\"mode\":\"%s\",\"pwr\":%s,\"boot\":%s"
        ",\"calibrated\":%s,\"touch\":%s,\"sensitivity\":%.3f"
        ",\"altitude\":%d,\"bias\":[%.4f,%.4f,%.4f],\"reports\":%" PRIu32 "}\n",
        s_ble_connected ? "connected" : "advertising", s_calibrated ? "true" : "false",
        s_calibrated ? "active" : "arming", control_mode_name(),
        s_pwr_pressed ? "true" : "false", s_boot_pressed ? "true" : "false",
        s_calibrated ? "true" : "false", s_touch_available ? "true" : "false",
        (double)s_sensitivity, s_flight_altitude_m,
        (double)s_gyro_bias[0], (double)s_gyro_bias[1],
        (double)s_gyro_bias[2], s_mouse_reports);
    fflush(stdout);
}

static float wrap_degrees(float angle)
{
    while (angle > 180.0f) {
        angle -= 360.0f;
    }
    while (angle < -180.0f) {
        angle += 360.0f;
    }
    return angle;
}

static float angle_from_center(float angle, float center)
{
    return wrap_degrees(angle - center);
}

static float normalize_orientation_axis(float angle, float center,
                                        float positive_endpoint, float negative_endpoint)
{
    const float value = angle_from_center(angle, center);
    const float positive = angle_from_center(positive_endpoint, center);
    const float negative = angle_from_center(negative_endpoint, center);
    float normalized = 0.0f;
    if (value * positive >= 0.0f && fabsf(positive) >= 1.0f) {
        normalized = value / positive;
    } else if (fabsf(negative) >= 1.0f) {
        normalized = -(value / negative);
    }
    return fmaxf(-1.0f, fminf(1.0f, normalized));
}

static void update_orientation(const qmi8658_data_t *data, float dt_seconds)
{
    const float accel_pitch = atan2f(-data->accelY, -data->accelZ) * 57.2957795f;
    const float accel_roll = atan2f(data->accelX, -data->accelZ) * 57.2957795f;
    if (!s_orientation_initialized) {
        s_pitch_deg = accel_pitch;
        s_roll_deg = accel_roll;
        s_yaw_deg = 0.0f;
        s_orientation_initialized = true;
    } else {
        const float predicted_pitch = wrap_degrees(
            s_pitch_deg + (data->gyroX - s_gyro_bias[0]) * dt_seconds);
        const float predicted_roll = wrap_degrees(
            s_roll_deg + (data->gyroY - s_gyro_bias[1]) * dt_seconds);
        s_pitch_deg = wrap_degrees(predicted_pitch +
            (1.0f - ORIENTATION_COMPLEMENTARY_ALPHA) *
            angle_from_center(accel_pitch, predicted_pitch));
        s_roll_deg = wrap_degrees(predicted_roll +
            (1.0f - ORIENTATION_COMPLEMENTARY_ALPHA) *
            angle_from_center(accel_roll, predicted_roll));
        s_yaw_deg = wrap_degrees(
            s_yaw_deg + (data->gyroZ - s_gyro_bias[2]) * dt_seconds);
    }

    if (s_orientation_cal_step == ORIENTATION_CAL_DONE) {
        s_pitch_normalized = normalize_orientation_axis(
            s_pitch_deg, s_orientation_calibration.pitch_center,
            s_orientation_calibration.pitch_forward,
            s_orientation_calibration.pitch_back);
        s_roll_normalized = normalize_orientation_axis(
            s_roll_deg, s_orientation_calibration.roll_center,
            s_orientation_calibration.roll_right,
            s_orientation_calibration.roll_left);
        s_yaw_normalized = normalize_orientation_axis(
            s_yaw_deg, 0.0f, s_orientation_calibration.yaw_right,
            s_orientation_calibration.yaw_left);
    }
}

static bool orientation_endpoint_is_valid(float value, float center)
{
    return fabsf(angle_from_center(value, center)) >= 12.0f;
}

static bool orientation_endpoints_are_opposed(float first, float second, float center)
{
    return angle_from_center(first, center) * angle_from_center(second, center) < 0.0f;
}

static void capture_orientation_calibration(void)
{
    orientation_cal_step_t next = s_orientation_cal_step;
    bool accepted = true;
    switch (s_orientation_cal_step) {
    case ORIENTATION_CAL_CENTER:
        s_orientation_calibration.pitch_center = s_pitch_deg;
        s_orientation_calibration.roll_center = s_roll_deg;
        s_yaw_deg = 0.0f;
        next = ORIENTATION_CAL_PITCH_FORWARD;
        break;
    case ORIENTATION_CAL_PITCH_FORWARD:
        accepted = orientation_endpoint_is_valid(
            s_pitch_deg, s_orientation_calibration.pitch_center);
        if (accepted) {
            s_orientation_calibration.pitch_forward = s_pitch_deg;
            next = ORIENTATION_CAL_PITCH_BACK;
        }
        break;
    case ORIENTATION_CAL_PITCH_BACK:
        accepted = orientation_endpoint_is_valid(
            s_pitch_deg, s_orientation_calibration.pitch_center) &&
            orientation_endpoints_are_opposed(
                s_orientation_calibration.pitch_forward, s_pitch_deg,
                s_orientation_calibration.pitch_center);
        if (accepted) {
            s_orientation_calibration.pitch_back = s_pitch_deg;
            next = ORIENTATION_CAL_ROLL_RIGHT;
        }
        break;
    case ORIENTATION_CAL_ROLL_RIGHT:
        accepted = orientation_endpoint_is_valid(
            s_roll_deg, s_orientation_calibration.roll_center);
        if (accepted) {
            s_orientation_calibration.roll_right = s_roll_deg;
            next = ORIENTATION_CAL_ROLL_LEFT;
        }
        break;
    case ORIENTATION_CAL_ROLL_LEFT:
        accepted = orientation_endpoint_is_valid(
            s_roll_deg, s_orientation_calibration.roll_center) &&
            orientation_endpoints_are_opposed(
                s_orientation_calibration.roll_right, s_roll_deg,
                s_orientation_calibration.roll_center);
        if (accepted) {
            s_orientation_calibration.roll_left = s_roll_deg;
            next = ORIENTATION_CAL_YAW_CENTER;
        }
        break;
    case ORIENTATION_CAL_YAW_CENTER:
        s_yaw_deg = 0.0f;
        next = ORIENTATION_CAL_YAW_RIGHT;
        break;
    case ORIENTATION_CAL_YAW_RIGHT:
        accepted = orientation_endpoint_is_valid(s_yaw_deg, 0.0f);
        if (accepted) {
            s_orientation_calibration.yaw_right = s_yaw_deg;
            next = ORIENTATION_CAL_YAW_RETURN_CENTER;
        }
        break;
    case ORIENTATION_CAL_YAW_RETURN_CENTER:
        s_yaw_deg = 0.0f;
        next = ORIENTATION_CAL_YAW_LEFT;
        break;
    case ORIENTATION_CAL_YAW_LEFT:
        accepted = orientation_endpoint_is_valid(s_yaw_deg, 0.0f) &&
            orientation_endpoints_are_opposed(
                s_orientation_calibration.yaw_right, s_yaw_deg, 0.0f);
        if (accepted) {
            s_orientation_calibration.yaw_left = s_yaw_deg;
            next = ORIENTATION_CAL_DONE;
        }
        break;
    default:
        accepted = false;
        break;
    }
    s_orientation_capture_rejected = !accepted;
    if (accepted) {
        s_orientation_cal_step = next;
        ESP_LOGI(TAG, "Orientation calibration: %s", orientation_step_key(next));
    }
}

static void emit_orientation(void)
{
    printf(
        "{\"type\":\"orientation\",\"step\":\"%s\",\"calibrated\":%s"
        ",\"roll\":%.2f,\"pitch\":%.2f,\"yaw\":%.2f"
        ",\"rollNorm\":%.4f,\"pitchNorm\":%.4f,\"yawNorm\":%.4f}\n",
        orientation_step_key(s_orientation_cal_step),
        s_orientation_cal_step == ORIENTATION_CAL_DONE ? "true" : "false",
        (double)s_roll_deg, (double)s_pitch_deg, (double)s_yaw_deg,
        (double)s_roll_normalized, (double)s_pitch_normalized,
        (double)s_yaw_normalized);
    fflush(stdout);
}

static lv_obj_t *create_control_card(lv_obj_t *parent, const char *title,
                                     const char *subtitle, lv_color_t color)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 324, 126);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x24202D), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 3, 0);
    lv_obj_set_style_border_color(card, color, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title_label = lv_label_create(card);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xFFF4DE), 0);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_14, 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 4, 14);

    lv_obj_t *subtitle_label = lv_label_create(card);
    lv_label_set_text(subtitle_label, subtitle);
    lv_obj_set_style_text_color(subtitle_label, lv_color_hex(0xBDB1C8), 0);
    lv_obj_align(subtitle_label, LV_ALIGN_BOTTOM_LEFT, 4, -14);

    lv_obj_t *icon = lv_obj_create(card);
    lv_obj_set_size(icon, 34, 34);
    lv_obj_set_style_radius(icon, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(icon, color, 0);
    lv_obj_set_style_border_width(icon, 0, 0);
    lv_obj_align(icon, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return card;
}

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
    if (lvgl_port_init(&lvgl_config) != ESP_OK) {
        ESP_LOGE(TAG, "Could not initialize LVGL");
        return NULL;
    }

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t panel_io = NULL;
    const bsp_display_config_t panel_config = {
        .max_transfer_sz = BSP_LCD_H_RES * DISPLAY_BUFFER_LINES * sizeof(lv_color16_t),
    };
    if (bsp_display_new(&panel_config, &panel, &panel_io) != ESP_OK) {
        ESP_LOGE(TAG, "Could not initialize CO5300 panel");
        return NULL;
    }

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
        ESP_LOGE(TAG, "Could not register QSPI display with LVGL");
        return NULL;
    }

    esp_lcd_touch_handle_t touch = NULL;
    if (bsp_touch_new(NULL, &touch) == ESP_OK) {
        const lvgl_port_touch_cfg_t touch_config = {
            .disp = display,
            .handle = touch,
        };
        s_display_input = lvgl_port_add_touch(&touch_config);
    } else {
        ESP_LOGW(TAG, "Touch controller unavailable");
    }
    if (bsp_display_brightness_init() != ESP_OK) {
        ESP_LOGW(TAG, "Display brightness control unavailable");
    }
    ESP_LOGI(TAG, "QSPI display uses transfer-complete flush with internal DMA buffer");
    return display;
}

static void ui_status_timer(lv_timer_t *timer)
{
    (void)timer;
    static int displayed_altitude_m = -1;
    if (s_status_label == NULL) {
        return;
    }
    if (s_calibration_overlay != NULL) {
        if (s_orientation_cal_step == ORIENTATION_CAL_DONE) {
            lv_obj_add_flag(s_calibration_overlay, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_calibration_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_calibration_step_label,
                              orientation_step_name(s_orientation_cal_step));
            if (s_orientation_capture_rejected) {
                lv_label_set_text(s_calibration_prompt_label,
                                  "Move farther in the opposite direction\n\nPRESS BOOT AGAIN");
            } else if (s_orientation_cal_step == ORIENTATION_CAL_GYRO) {
                lv_label_set_text(s_calibration_prompt_label,
                                  "Keep the controller still\n\nCALIBRATING GYRO");
            } else {
                lv_label_set_text_fmt(s_calibration_prompt_label, "%s\n\nPRESS BOOT TO CAPTURE",
                                      orientation_step_prompt(s_orientation_cal_step));
            }
            lv_label_set_text_fmt(s_calibration_values_label, "ROLL %+.0f   PITCH %+.0f   YAW %+.0f",
                                  (double)s_roll_deg, (double)s_pitch_deg, (double)s_yaw_deg);
        }
    }

    const char *text = "KEEP STILL  /  CALIBRATING";
    if (s_calibrated) {
        if (s_pwr_pressed && s_boot_pressed) {
            text = "PWR + BOOT  /  ACTIVE";
        } else if (s_boot_pressed) {
            text = "BOOT  /  ACTIVE";
        } else if (s_pwr_pressed) {
            text = "PWR  /  ACTIVE";
        } else if (s_ble_connected) {
            text = "MOTION ON  /  BLE CONNECTED";
        } else {
            text = "MOTION ON  /  USB LIVE";
        }
    }
    lv_label_set_text(s_status_label, text);
    if (s_altitude_label != NULL && displayed_altitude_m != s_flight_altitude_m) {
        lv_label_set_text_fmt(s_altitude_label, "ALT  %04d m", s_flight_altitude_m);
        if (s_altitude_bar != NULL) {
            lv_bar_set_value(s_altitude_bar, s_flight_altitude_m, LV_ANIM_ON);
        }
        displayed_altitude_m = s_flight_altitude_m;
    }
}

static void create_control_ui(void)
{
    if (!bsp_display_lock(0)) {
        return;
    }
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x17131D), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *eyebrow = lv_label_create(screen);
    lv_label_set_text(eyebrow, "FLIGHT CONTROLS");
    lv_obj_set_style_text_color(eyebrow, lv_color_hex(0xE9AFAF), 0);
    lv_obj_align(eyebrow, LV_ALIGN_TOP_LEFT, 22, 22);

    s_altitude_label = lv_label_create(screen);
    lv_label_set_text_fmt(s_altitude_label, "ALT  %04d m", s_flight_altitude_m);
    lv_obj_set_style_text_color(s_altitude_label, lv_color_hex(0x86FFD0), 0);
    lv_obj_set_style_text_font(s_altitude_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_altitude_label, LV_ALIGN_TOP_RIGHT, -22, 22);

    lv_obj_t *hint = lv_label_create(screen);
    lv_label_set_text(hint, "TILT ANYTIME  /  BUTTONS BELOW");
    lv_obj_set_style_text_color(hint, lv_color_hex(0xFFF4DE), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 54);

    s_altitude_bar = lv_bar_create(screen);
    lv_obj_set_size(s_altitude_bar, 324, 6);
    lv_bar_set_range(s_altitude_bar, FLIGHT_ALTITUDE_MIN_M, FLIGHT_ALTITUDE_MAX_M);
    lv_bar_set_value(s_altitude_bar, s_flight_altitude_m, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_altitude_bar, lv_color_hex(0x342E3C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_altitude_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_altitude_bar, lv_color_hex(0x86FFD0), LV_PART_INDICATOR);
    lv_obj_align(s_altitude_bar, LV_ALIGN_TOP_MID, 0, 77);

    lv_obj_t *pwr = create_control_card(screen, "PWR", "secondary action / short press",
                                        lv_color_hex(0x6AA7A2));
    lv_obj_align(pwr, LV_ALIGN_TOP_MID, 0, 92);

    lv_obj_t *boot = create_control_card(screen, "BOOT", "primary action",
                                         lv_color_hex(0xE85D75));
    lv_obj_align(boot, LV_ALIGN_TOP_MID, 0, 232);

    s_status_label = lv_label_create(screen);
    lv_label_set_text(s_status_label, "KEEP STILL  /  CALIBRATING");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x9A8FA6), 0);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, -28);

    s_calibration_overlay = lv_obj_create(screen);
    lv_obj_set_size(s_calibration_overlay, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_align(s_calibration_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(s_calibration_overlay, 0, 0);
    lv_obj_set_style_border_width(s_calibration_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_calibration_overlay, lv_color_hex(0x111722), 0);
    lv_obj_set_style_bg_opa(s_calibration_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_calibration_overlay, 22, 0);
    lv_obj_clear_flag(s_calibration_overlay, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *calibration_eyebrow = lv_label_create(s_calibration_overlay);
    lv_label_set_text(calibration_eyebrow, "ORIENTATION SETUP");
    lv_obj_set_style_text_color(calibration_eyebrow, lv_color_hex(0x86FFD0), 0);
    lv_obj_align(calibration_eyebrow, LV_ALIGN_TOP_LEFT, 0, 4);

    s_calibration_step_label = lv_label_create(s_calibration_overlay);
    lv_label_set_text(s_calibration_step_label, "GYRO CALIBRATION");
    lv_obj_set_style_text_color(s_calibration_step_label, lv_color_hex(0xE9AFAF), 0);
    lv_obj_set_style_text_font(s_calibration_step_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_calibration_step_label, LV_ALIGN_TOP_LEFT, 0, 58);

    s_calibration_prompt_label = lv_label_create(s_calibration_overlay);
    lv_obj_set_width(s_calibration_prompt_label, 324);
    lv_label_set_long_mode(s_calibration_prompt_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_calibration_prompt_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_calibration_prompt_label, lv_color_hex(0xFFF4DE), 0);
    lv_obj_set_style_text_font(s_calibration_prompt_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_calibration_prompt_label,
                      "Keep the controller still\n\nCALIBRATING GYRO");
    lv_obj_align(s_calibration_prompt_label, LV_ALIGN_CENTER, 0, -8);

    s_calibration_values_label = lv_label_create(s_calibration_overlay);
    lv_label_set_text(s_calibration_values_label, "ROLL --   PITCH --   YAW --");
    lv_obj_set_style_text_color(s_calibration_values_label, lv_color_hex(0x8191A8), 0);
    lv_obj_align(s_calibration_values_label, LV_ALIGN_BOTTOM_MID, 0, -18);

    lv_timer_create(ui_status_timer, 150, NULL);
    bsp_display_unlock();
}

static esp_err_t detect_imu_address(i2c_master_bus_handle_t bus_handle, uint8_t *address)
{
    const uint8_t candidates[] = {QMI8658_ADDRESS_HIGH, QMI8658_ADDRESS_LOW};
    if (bus_handle == NULL || address == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (i2c_master_probe(bus_handle, candidates[i], IMU_PROBE_TIMEOUT_MS) == ESP_OK) {
            *address = candidates[i];
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t configure_imu(qmi8658_dev_t *imu)
{
    esp_err_t result = qmi8658_write_register(imu, QMI8658_RESET_REGISTER, QMI8658_RESET_COMMAND);
    if (result != ESP_OK) {
        return result;
    }
    vTaskDelay(pdMS_TO_TICKS(QMI8658_RESET_DELAY_MS));
    result = qmi8658_write_register(imu, QMI8658_CTRL1, QMI8658_CTRL1_VALUE);
    if (result != ESP_OK) {
        return result;
    }
    if ((result = qmi8658_set_accel_range(imu, QMI8658_ACCEL_RANGE_4G)) != ESP_OK ||
        (result = qmi8658_set_accel_odr(imu, QMI8658_ACCEL_ODR_250HZ)) != ESP_OK ||
        (result = qmi8658_set_gyro_range(imu, QMI8658_GYRO_RANGE_256DPS)) != ESP_OK ||
        (result = qmi8658_set_gyro_odr(imu, QMI8658_GYRO_ODR_250HZ)) != ESP_OK) {
        return result;
    }
    qmi8658_set_accel_unit_mps2(imu, true);
    qmi8658_set_gyro_unit_dps(imu, true);
    return qmi8658_enable_sensors(imu, QMI8658_ENABLE_ACCEL | QMI8658_ENABLE_GYRO);
}

static float apply_dead_zone(float value)
{
    const float magnitude = fabsf(value);
    return magnitude <= GYRO_DEAD_ZONE_DPS ? 0.0f : copysignf(magnitude - GYRO_DEAD_ZONE_DPS, value);
}

static int8_t report_delta(float value, float *remainder)
{
    const float total = value + *remainder;
    long rounded = lroundf(total);
    if (rounded > 127) {
        rounded = 127;
    } else if (rounded < -127) {
        rounded = -127;
    }
    *remainder = total - (float)rounded;
    return (int8_t)rounded;
}

static void send_mouse_report(int8_t dx, int8_t dy)
{
    if (!s_ble_connected || s_hid_device == NULL || (dx == 0 && dy == 0)) {
        return;
    }
    uint8_t report[] = {0, (uint8_t)dx, (uint8_t)dy};
    if (esp_hidd_dev_input_set(s_hid_device, 0, 0, report, sizeof(report)) == ESP_OK) {
        ++s_mouse_reports;
    }
}

static int ble_gap_event(struct ble_gap_event *event, void *arg);

static esp_err_t start_advertising(void)
{
    static const ble_uuid16_t hid_uuid = BLE_UUID16_INIT(BLE_HID_SERVICE_UUID);
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.appearance = ESP_HID_APPEARANCE_MOUSE;
    fields.appearance_is_present = 1;
    fields.name = (uint8_t *)s_hid_config.device_name;
    fields.name_len = strlen(s_hid_config.device_name);
    fields.name_is_complete = 1;
    fields.uuids16 = (ble_uuid16_t *)&hid_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    int result = ble_gap_adv_set_fields(&fields);
    if (result != 0) {
        ESP_LOGE(TAG, "Could not set BLE advertisement fields: rc=%d", result);
        return ESP_FAIL;
    }
    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = BLE_GAP_ADV_ITVL_MS(30);
    params.itvl_max = BLE_GAP_ADV_ITVL_MS(50);
    result = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &params, ble_gap_event, NULL);
    if (result != 0) {
        ESP_LOGE(TAG, "Could not start BLE advertising: rc=%d", result);
    }
    return result == 0 ? ESP_OK : ESP_FAIL;
}

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ble_gap_security_initiate(event->connect.conn_handle);
        } else {
            start_advertising();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_ble_connected = false;
        emit_airmouse_state();
        start_advertising();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (!s_ble_connected) {
            start_advertising();
        }
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default:
        break;
    }
    return 0;
}

static void hid_event_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)handler_args;
    (void)base;
    (void)event_data;
    switch ((esp_hidd_event_t)id) {
    case ESP_HIDD_START_EVENT:
        ESP_LOGI(TAG, "BLE HID ready; advertising as %s", s_hid_config.device_name);
        if (start_advertising() != ESP_OK) {
            ESP_LOGE(TAG, "BLE advertising unavailable; USB telemetry remains active");
        }
        emit_airmouse_state();
        break;
    case ESP_HIDD_CONNECT_EVENT:
        s_ble_connected = true;
        emit_airmouse_state();
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        s_ble_connected = false;
        emit_airmouse_state();
        break;
    default:
        break;
    }
}

static void nimble_host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_store_config_init(void);

static esp_err_t init_ble_mouse(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    if (result != ESP_OK) {
        return result;
    }
    result = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (result != ESP_OK) {
        return result;
    }
    esp_bt_controller_config_t bt_config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if ((result = esp_bt_controller_init(&bt_config)) != ESP_OK ||
        (result = esp_bt_controller_enable(ESP_BT_MODE_BLE)) != ESP_OK ||
        (result = esp_nimble_init()) != ESP_OK) {
        return result;
    }
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_ENC;
    result = esp_hidd_dev_init(&s_hid_config, ESP_HID_TRANSPORT_BLE,
                               hid_event_callback, &s_hid_device);
    if (result != ESP_OK) {
        return result;
    }
    ESP_ERROR_CHECK(esp_hidd_dev_battery_set(s_hid_device, HID_BATTERY_LEVEL));
    ble_store_config_init();
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    nimble_port_freertos_init(nimble_host_task);
    return ESP_OK;
}

static void command_task(void *arg)
{
    (void)arg;
    char line[64];
    while (true) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        float requested = 0.0f;
        if (sscanf(line, "SENS %f", &requested) == 1 &&
            requested >= MIN_SENSITIVITY && requested <= MAX_SENSITIVITY) {
            s_sensitivity = requested;
            emit_airmouse_state();
            continue;
        }
        int requested_altitude = 0;
        if (sscanf(line, "ALT %d", &requested_altitude) == 1 &&
            requested_altitude >= FLIGHT_ALTITUDE_MIN_M &&
            requested_altitude <= FLIGHT_ALTITUDE_MAX_M) {
            s_flight_altitude_m = requested_altitude;
            emit_airmouse_state();
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

static esp_err_t init_physical_buttons(i2c_master_bus_handle_t bus_handle)
{
    const gpio_config_t boot_config = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t result = gpio_config(&boot_config);
    if (result != ESP_OK) {
        return result;
    }
    const i2c_device_config_t axp2101_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_I2C_ADDRESS,
        .scl_speed_hz = 400000,
    };
    result = i2c_master_bus_add_device(bus_handle, &axp2101_config, &s_axp2101);
    if (result != ESP_OK) {
        return result;
    }
    uint8_t interrupts_enabled = 0;
    result = axp2101_read_register(AXP2101_INTEN2_REGISTER, &interrupts_enabled);
    if (result != ESP_OK) {
        return result;
    }
    const uint8_t pkey_interrupts = AXP2101_PKEY_SHORT_IRQ | AXP2101_PKEY_LONG_IRQ;
    result = axp2101_write_register(AXP2101_INTEN2_REGISTER,
                                    interrupts_enabled | pkey_interrupts);
    if (result != ESP_OK) {
        return result;
    }
    return axp2101_write_register(AXP2101_INTSTS2_REGISTER, pkey_interrupts);
}

static void poll_physical_buttons(int64_t elapsed_ms, bool *state_changed)
{
    static bool raw_boot_previous;
    static uint8_t boot_stable_samples;
    static int64_t pwr_release_at_ms;
    const bool raw_boot = gpio_get_level(BOOT_BUTTON_GPIO) == 0;
    uint8_t pwr_status = 0;
    if (axp2101_read_register(AXP2101_INTSTS2_REGISTER, &pwr_status) == ESP_OK) {
        const uint8_t pwr_events = pwr_status & (AXP2101_PKEY_SHORT_IRQ | AXP2101_PKEY_LONG_IRQ);
        if (pwr_events != 0) {
            axp2101_write_register(AXP2101_INTSTS2_REGISTER, pwr_events);
            if (!s_pwr_pressed) {
                s_pwr_pressed = true;
                *state_changed = true;
            }
            pwr_release_at_ms = elapsed_ms + PWR_EVENT_PULSE_MS;
        }
    }
    if (s_pwr_pressed && elapsed_ms >= pwr_release_at_ms) {
        s_pwr_pressed = false;
        *state_changed = true;
    }
    if (raw_boot == raw_boot_previous) {
        if (boot_stable_samples < BUTTON_DEBOUNCE_SAMPLES) {
            ++boot_stable_samples;
        }
    } else {
        raw_boot_previous = raw_boot;
        boot_stable_samples = 1;
    }
    if (boot_stable_samples >= BUTTON_DEBOUNCE_SAMPLES && s_boot_pressed != raw_boot) {
        s_boot_pressed = raw_boot;
        *state_changed = true;
    }
}

static void stream_samples(qmi8658_dev_t *imu)
{
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t sequence = 0;
    uint32_t calibration_count = 0;
    float calibration_sum[3] = {0};
    float filtered_x = 0.0f;
    float filtered_y = 0.0f;
    float remainder_x = 0.0f;
    float remainder_y = 0.0f;
    int64_t last_state_emit_ms = 0;
    int64_t last_orientation_emit_ms = 0;
    bool was_boot_pressed = false;

    emit_airmouse_state();
    while (true) {
        bool ready = false;
        esp_err_t result = qmi8658_is_data_ready(imu, &ready);
        if (result == ESP_OK && ready) {
            qmi8658_data_t data = {0};
            result = qmi8658_read_sensor_data(imu, &data);
            if (result == ESP_OK) {
                const int64_t elapsed_ms = esp_timer_get_time() / 1000;
                bool button_state_changed = false;
                poll_physical_buttons(elapsed_ms, &button_state_changed);
                const bool boot_pressed_edge = s_boot_pressed && !was_boot_pressed;
                was_boot_pressed = s_boot_pressed;
                if (button_state_changed) {
                    emit_airmouse_state();
                }
                printf(
                    "{\"seq\":%" PRIu32 ",\"t\":%" PRIi64
                    ",\"ax\":%.4f,\"ay\":%.4f,\"az\":%.4f"
                    ",\"gx\":%.4f,\"gy\":%.4f,\"gz\":%.4f,\"temp\":%.2f}\n",
                    sequence++, elapsed_ms, data.accelX, data.accelY, data.accelZ,
                    data.gyroX, data.gyroY, data.gyroZ, data.temperature);
                fflush(stdout);

                if (!s_calibrated) {
                    const float magnitude = sqrtf(data.gyroX * data.gyroX +
                                                  data.gyroY * data.gyroY +
                                                  data.gyroZ * data.gyroZ);
                    if (magnitude <= GYRO_CALIBRATION_STILL_DPS) {
                        calibration_sum[0] += data.gyroX;
                        calibration_sum[1] += data.gyroY;
                        calibration_sum[2] += data.gyroZ;
                        ++calibration_count;
                    } else {
                        calibration_count = 0;
                        memset(calibration_sum, 0, sizeof(calibration_sum));
                    }
                    if (calibration_count >= GYRO_CALIBRATION_SAMPLES) {
                        for (size_t axis = 0; axis < 3; ++axis) {
                            s_gyro_bias[axis] = calibration_sum[axis] / calibration_count;
                        }
                        s_calibrated = true;
                        s_orientation_cal_step = ORIENTATION_CAL_CENTER;
                        ESP_LOGI(TAG, "Gyro calibrated: %.4f %.4f %.4f dps",
                                 s_gyro_bias[0], s_gyro_bias[1], s_gyro_bias[2]);
                        emit_airmouse_state();
                    }
                }

                if (s_calibrated) {
                    update_orientation(&data, SAMPLE_PERIOD_MS / 1000.0f);
                    if (boot_pressed_edge && s_orientation_initialized &&
                        s_orientation_cal_step != ORIENTATION_CAL_DONE) {
                        capture_orientation_calibration();
                        emit_orientation();
                    }

                    // Screen yaw is gyro Z; screen pitch is gyro X. Linear acceleration is never used.
                    const float rate_x = apply_dead_zone(data.gyroZ - s_gyro_bias[2]);
                    const float rate_y = apply_dead_zone(data.gyroX - s_gyro_bias[0]);
                    filtered_x += GYRO_SMOOTHING_ALPHA * (rate_x - filtered_x);
                    filtered_y += GYRO_SMOOTHING_ALPHA * (rate_y - filtered_y);
                    const int8_t dx = report_delta(filtered_x * s_sensitivity, &remainder_x);
                    const int8_t dy = report_delta(filtered_y * s_sensitivity, &remainder_y);
                    if (dx != 0 || dy != 0) {
                        printf("{\"type\":\"cursor\",\"dx\":%d,\"dy\":%d,\"mode\":\"%s\""
                               ",\"pwr\":%s,\"boot\":%s}\n",
                               dx, dy, control_mode_name(),
                               s_pwr_pressed ? "true" : "false",
                               s_boot_pressed ? "true" : "false");
                        fflush(stdout);
                    }
                    send_mouse_report(dx, dy);
                }
                if (s_orientation_initialized &&
                    elapsed_ms - last_orientation_emit_ms >= ORIENTATION_EMIT_PERIOD_MS) {
                    emit_orientation();
                    last_orientation_emit_ms = elapsed_ms;
                }
                if (elapsed_ms - last_state_emit_ms >= 1000) {
                    emit_airmouse_state();
                    last_state_emit_ms = elapsed_ms;
                }
            }
        } else if (result != ESP_OK) {
            ESP_LOGW(TAG, "Data-ready read failed: %s", esp_err_to_name(result));
        }
        xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

void app_main(void)
{
    lv_display_t *display = start_safe_display();
    s_touch_available = display != NULL && s_display_input != NULL;
    if (!s_touch_available) {
        ESP_LOGE(TAG, "Display or touch controller unavailable");
    } else {
        create_control_ui();
        ESP_LOGI(TAG, "Display ready; motion is always active after calibration");
    }
    i2c_master_bus_handle_t bus_handle = bsp_i2c_get_handle();
    ESP_ERROR_CHECK(bus_handle == NULL ? ESP_FAIL : ESP_OK);
    ESP_ERROR_CHECK(init_physical_buttons(bus_handle));
    ESP_LOGI(TAG, "Physical controls ready: PWR via AXP2101 PKEY, BOOT on GPIO0");

    uint8_t address = 0;
    esp_err_t result = detect_imu_address(bus_handle, &address);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "QMI8658 not found at 0x%02x or 0x%02x",
                 QMI8658_ADDRESS_HIGH, QMI8658_ADDRESS_LOW);
        return;
    }
    qmi8658_dev_t imu = {0};
    ESP_ERROR_CHECK(qmi8658_init(&imu, bus_handle, address));
    uint8_t who_am_i = 0;
    ESP_ERROR_CHECK(qmi8658_get_who_am_i(&imu, &who_am_i));
    ESP_ERROR_CHECK(configure_imu(&imu));
    ESP_ERROR_CHECK(init_ble_mouse());
    xTaskCreate(command_task, "serial_commands", 3072, NULL, 4, NULL);

    ESP_LOGI(TAG, "QMI8658 ready at 0x%02x (WHO_AM_I=0x%02x); streaming JSON at %d Hz",
             address, who_am_i, 1000 / SAMPLE_PERIOD_MS);
    ESP_LOGI(TAG, "Keep the board still for %.1f seconds of gyro calibration",
             GYRO_CALIBRATION_SAMPLES * SAMPLE_PERIOD_MS / 1000.0f);
    stream_samples(&imu);
}
