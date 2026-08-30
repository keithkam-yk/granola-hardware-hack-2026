#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
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

#define BOARD_I2C_PORT I2C_NUM_0
#define BOARD_I2C_SCL_IO 14
#define BOARD_I2C_SDA_IO 15
#define IMU_PROBE_TIMEOUT_MS 100
#define SAMPLE_PERIOD_MS 20
#define QMI8658_RESET_REGISTER 0x60
#define QMI8658_RESET_COMMAND 0xB0
#define QMI8658_CTRL1_VALUE 0x60
#define QMI8658_RESET_DELAY_MS 20

#define IO_EXPANDER_ADDRESS 0x20
#define IO_EXPANDER_OUTPUT_REG 0x01
#define IO_EXPANDER_CONFIG_REG 0x03
#define IO_EXPANDER_TOUCH_RESET (1U << 2)
#define IO_EXPANDER_SD_CS (1U << 7)
#define TOUCH_ADDRESS 0x15
#define TOUCH_POINTS_REG 0x02
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
static i2c_master_dev_handle_t s_touch_device;
static esp_hidd_dev_t *s_hid_device;
static volatile bool s_ble_connected;
static volatile bool s_touch_available;
static volatile bool s_calibrated;
static volatile bool s_clutch_arming;
static volatile bool s_clutch_active;
static volatile float s_sensitivity = DEFAULT_SENSITIVITY;
static float s_gyro_bias[3];
static uint32_t s_mouse_reports;

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
        ",\"calibrated\":%s,\"touch\":%s,\"sensitivity\":%.3f"
        ",\"bias\":[%.4f,%.4f,%.4f],\"reports\":%" PRIu32 "}\n",
        s_ble_connected ? "connected" : "advertising", clutch,
        s_calibrated ? "true" : "false", s_touch_available ? "true" : "false",
        (double)s_sensitivity, (double)s_gyro_bias[0], (double)s_gyro_bias[1],
        (double)s_gyro_bias[2], s_mouse_reports);
    fflush(stdout);
}

static esp_err_t board_i2c_init(i2c_master_bus_handle_t *bus_handle)
{
    if (bus_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA_IO,
        .scl_io_num = BOARD_I2C_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {.enable_internal_pullup = true},
    };
    return i2c_new_master_bus(&bus_config, bus_handle);
}

static esp_err_t add_i2c_device(i2c_master_bus_handle_t bus, uint8_t address,
                                i2c_master_dev_handle_t *device)
{
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 400000,
    };
    return i2c_master_bus_add_device(bus, &config, device);
}

static esp_err_t write_i2c_register(i2c_master_dev_handle_t device, uint8_t reg, uint8_t value)
{
    const uint8_t payload[] = {reg, value};
    return i2c_master_transmit(device, payload, sizeof(payload), 100);
}

static esp_err_t init_touch(i2c_master_bus_handle_t bus)
{
    i2c_master_dev_handle_t expander = NULL;
    esp_err_t result = add_i2c_device(bus, IO_EXPANDER_ADDRESS, &expander);
    if (result != ESP_OK) {
        return result;
    }
    const uint8_t output_mask = IO_EXPANDER_TOUCH_RESET | IO_EXPANDER_SD_CS;
    result = write_i2c_register(expander, IO_EXPANDER_CONFIG_REG, (uint8_t)~output_mask);
    if (result == ESP_OK) {
        result = write_i2c_register(expander, IO_EXPANDER_OUTPUT_REG, IO_EXPANDER_SD_CS);
    }
    if (result == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(20));
        result = write_i2c_register(expander, IO_EXPANDER_OUTPUT_REG, output_mask);
    }
    if (result == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(150));
        result = i2c_master_probe(bus, TOUCH_ADDRESS, 100);
    }
    i2c_master_bus_rm_device(expander);
    if (result != ESP_OK) {
        return result;
    }
    return add_i2c_device(bus, TOUCH_ADDRESS, &s_touch_device);
}

static bool touch_is_held(void)
{
    uint8_t reg = TOUCH_POINTS_REG;
    uint8_t data[5] = {0};
    if (s_touch_device == NULL ||
        i2c_master_transmit_receive(s_touch_device, &reg, 1, data, sizeof(data), 20) != ESP_OK) {
        return false;
    }
    return (data[0] & 0x0FU) > 0;
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

                const bool touching = touch_is_held();
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
                        printf("{\"type\":\"cursor\",\"dx\":%d,\"dy\":%d}\n", dx, dy);
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
    i2c_master_bus_handle_t bus_handle = NULL;
    ESP_ERROR_CHECK(board_i2c_init(&bus_handle));
    esp_err_t touch_result = init_touch(bus_handle);
    s_touch_available = touch_result == ESP_OK;
    if (!s_touch_available) {
        ESP_LOGE(TAG, "CST820 touch controller unavailable: %s", esp_err_to_name(touch_result));
    } else {
        ESP_LOGI(TAG, "CST820 touch clutch ready at 0x%02x", TOUCH_ADDRESS);
    }

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
