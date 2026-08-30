#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "bsp/esp-bsp.h"
#include "esp_bt.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_hidd.h"
#include "esp_hid_common.h"
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

#define TOUCH_HOLD_MS 120

#define GYRO_CALIBRATION_SAMPLES 100
#define GYRO_CALIBRATION_STILL_DPS 8.0f
#define GYRO_DEAD_ZONE_DPS 1.5f
#define GYRO_SMOOTHING_ALPHA 0.24f
#define DEFAULT_SENSITIVITY 0.18f
#define MIN_SENSITIVITY 0.05f
#define MAX_SENSITIVITY 0.80f

#define HID_BATTERY_LEVEL 100
#define BLE_HID_SERVICE_UUID 0x1812

static const char *TAG = "motion_airmouse";
static esp_hidd_dev_t *s_hid_device;
static volatile bool s_ble_connected;
static volatile bool s_touch_available;
static volatile bool s_calibrated;
static volatile bool s_clutch_arming;
static volatile bool s_clutch_active;
static volatile bool s_touch_held;
static volatile float s_sensitivity = DEFAULT_SENSITIVITY;
static float s_gyro_bias[3];
static uint32_t s_mouse_reports;
static lv_obj_t *s_status_label;

typedef enum {
    CONTROL_NONE = 0,
    CONTROL_MOVE,
    CONTROL_LASER,
} control_mode_t;

static volatile control_mode_t s_control_mode;

static const char *control_mode_name(control_mode_t mode)
{
    return mode == CONTROL_LASER ? "laser" : (mode == CONTROL_MOVE ? "move" : "none");
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
    const char *clutch = s_clutch_active ? "active" : (s_clutch_arming ? "arming" : "idle");
    printf(
        "{\"type\":\"airmouse\",\"ble\":\"%s\",\"clutch\":\"%s\""
        ",\"mode\":\"%s\",\"calibrated\":%s,\"touch\":%s,\"sensitivity\":%.3f"
        ",\"bias\":[%.4f,%.4f,%.4f],\"reports\":%" PRIu32 "}\n",
        s_ble_connected ? "connected" : "advertising", clutch,
        control_mode_name(s_control_mode),
        s_calibrated ? "true" : "false", s_touch_available ? "true" : "false",
        (double)s_sensitivity, (double)s_gyro_bias[0], (double)s_gyro_bias[1],
        (double)s_gyro_bias[2], s_mouse_reports);
    fflush(stdout);
}

static void control_button_event(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    const control_mode_t mode = (control_mode_t)(intptr_t)lv_event_get_user_data(event);
    if (code == LV_EVENT_PRESSED) {
        s_control_mode = mode;
        s_touch_held = true;
        emit_airmouse_state();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_touch_held = false;
        s_control_mode = CONTROL_NONE;
        emit_airmouse_state();
    }
}

static lv_obj_t *create_control_button(lv_obj_t *parent, control_mode_t mode,
                                       const char *title, const char *subtitle,
                                       lv_color_t color)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, 324, 126);
    lv_obj_set_style_radius(button, 20, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x24202D), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, 3, 0);
    lv_obj_set_style_border_color(button, color, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_bg_color(button, color, LV_STATE_PRESSED);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(button, control_button_event, LV_EVENT_ALL, (void *)(intptr_t)mode);

    lv_obj_t *title_label = lv_label_create(button);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xFFF4DE), 0);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_14, 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 4, 14);

    lv_obj_t *subtitle_label = lv_label_create(button);
    lv_label_set_text(subtitle_label, subtitle);
    lv_obj_set_style_text_color(subtitle_label, lv_color_hex(0xBDB1C8), 0);
    lv_obj_align(subtitle_label, LV_ALIGN_BOTTOM_LEFT, 4, -14);

    lv_obj_t *icon = lv_obj_create(button);
    lv_obj_set_size(icon, 34, 34);
    lv_obj_set_style_radius(icon, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(icon, color, 0);
    lv_obj_set_style_border_width(icon, 0, 0);
    lv_obj_align(icon, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return button;
}

static void ui_status_timer(lv_timer_t *timer)
{
    (void)timer;
    if (s_status_label == NULL) {
        return;
    }
    const char *text = "KEEP STILL  /  CALIBRATING";
    if (s_calibrated) {
        if (s_touch_held && s_control_mode == CONTROL_LASER) {
            text = "LASER ON  /  TILT TO AIM";
        } else if (s_touch_held && s_control_mode == CONTROL_MOVE) {
            text = "MOVE ON  /  TILT TO AIM";
        } else if (s_ble_connected) {
            text = "READY  /  BLE CONNECTED";
        } else {
            text = "READY  /  USB LIVE";
        }
    }
    lv_label_set_text(s_status_label, text);
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
    lv_label_set_text(eyebrow, "POCKET CAT CONTROLLER");
    lv_obj_set_style_text_color(eyebrow, lv_color_hex(0xE9AFAF), 0);
    lv_obj_align(eyebrow, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *hint = lv_label_create(screen);
    lv_label_set_text(hint, "HOLD A BUTTON + TILT");
    lv_obj_set_style_text_color(hint, lv_color_hex(0xFFF4DE), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 54);

    lv_obj_t *move = create_control_button(screen, CONTROL_MOVE, "MOVE", "pointer only",
                                           lv_color_hex(0x6AA7A2));
    lv_obj_align(move, LV_ALIGN_TOP_MID, 0, 92);

    lv_obj_t *laser = create_control_button(screen, CONTROL_LASER, "LASER", "pointer + cat chase",
                                            lv_color_hex(0xE85D75));
    lv_obj_align(laser, LV_ALIGN_TOP_MID, 0, 232);

    s_status_label = lv_label_create(screen);
    lv_label_set_text(s_status_label, "KEEP STILL  /  CALIBRATING");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x9A8FA6), 0);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, -28);
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
        s_clutch_active = false;
        s_clutch_arming = false;
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
        s_clutch_active = false;
        s_clutch_arming = false;
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
        }
    }
}

static void stream_samples(qmi8658_dev_t *imu)
{
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t sequence = 0;
    uint32_t calibration_count = 0;
    float calibration_sum[3] = {0};
    int64_t touch_started_ms = 0;
    bool was_touching = false;
    float filtered_x = 0.0f;
    float filtered_y = 0.0f;
    float remainder_x = 0.0f;
    float remainder_y = 0.0f;
    int64_t last_state_emit_ms = 0;

    emit_airmouse_state();
    while (true) {
        bool ready = false;
        esp_err_t result = qmi8658_is_data_ready(imu, &ready);
        if (result == ESP_OK && ready) {
            qmi8658_data_t data = {0};
            result = qmi8658_read_sensor_data(imu, &data);
            if (result == ESP_OK) {
                const int64_t elapsed_ms = esp_timer_get_time() / 1000;
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
                        ESP_LOGI(TAG, "Gyro calibrated: %.4f %.4f %.4f dps",
                                 s_gyro_bias[0], s_gyro_bias[1], s_gyro_bias[2]);
                        emit_airmouse_state();
                    }
                }

                const bool touching = s_touch_held;
                if (touching && !was_touching) {
                    touch_started_ms = elapsed_ms;
                    s_clutch_arming = true;
                    s_clutch_active = false;
                    filtered_x = filtered_y = remainder_x = remainder_y = 0.0f;
                    emit_airmouse_state();
                } else if (!touching && was_touching) {
                    s_clutch_arming = false;
                    s_clutch_active = false;
                    filtered_x = filtered_y = remainder_x = remainder_y = 0.0f;
                    emit_airmouse_state();
                }
                if (touching && s_clutch_arming && s_calibrated &&
                    elapsed_ms - touch_started_ms >= TOUCH_HOLD_MS) {
                    s_clutch_arming = false;
                    s_clutch_active = true;
                    filtered_x = filtered_y = remainder_x = remainder_y = 0.0f;
                    emit_airmouse_state();
                }
                if (touching && s_clutch_active) {
                    // Screen yaw is gyro Z; screen pitch is gyro X. Linear acceleration is never used.
                    const float rate_x = apply_dead_zone(data.gyroZ - s_gyro_bias[2]);
                    const float rate_y = apply_dead_zone(data.gyroX - s_gyro_bias[0]);
                    filtered_x += GYRO_SMOOTHING_ALPHA * (rate_x - filtered_x);
                    filtered_y += GYRO_SMOOTHING_ALPHA * (rate_y - filtered_y);
                    const int8_t dx = report_delta(filtered_x * s_sensitivity, &remainder_x);
                    const int8_t dy = report_delta(filtered_y * s_sensitivity, &remainder_y);
                    if (dx != 0 || dy != 0) {
                        printf("{\"type\":\"cursor\",\"dx\":%d,\"dy\":%d,\"mode\":\"%s\"}\n",
                               dx, dy, control_mode_name(s_control_mode));
                        fflush(stdout);
                    }
                    send_mouse_report(dx, dy);
                }
                if (elapsed_ms - last_state_emit_ms >= 1000) {
                    emit_airmouse_state();
                    last_state_emit_ms = elapsed_ms;
                }
                was_touching = touching;
            }
        } else if (result != ESP_OK) {
            ESP_LOGW(TAG, "Data-ready read failed: %s", esp_err_to_name(result));
        }
        xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

void app_main(void)
{
    lv_display_t *display = bsp_display_start();
    s_touch_available = display != NULL && bsp_display_get_input_dev() != NULL;
    if (!s_touch_available) {
        ESP_LOGE(TAG, "Display or touch controller unavailable");
    } else {
        create_control_ui();
        ESP_LOGI(TAG, "Touch controls ready: MOVE and LASER");
    }
    i2c_master_bus_handle_t bus_handle = bsp_i2c_get_handle();
    ESP_ERROR_CHECK(bus_handle == NULL ? ESP_FAIL : ESP_OK);

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
