// Magic wand IMU streamer.
//
// Reads the QMI8658 and the CST816 touch panel and emits one CSV line per sample
// over USB CDC. Touch is what starts and stops a gesture, so the wand itself is
// the controller and the host only listens. Deliberately dumb: no filtering, no fusion, no gesture
// logic on-device, so the host tool can iterate on all of that without a reflash.
//
// Discovered by tools/i2c_discovery.c.bak on this board:
//   QMI8658 @ 0x6B, SDA=15, SCL=14
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "esp_log.h"

#define PIN_SDA        15
#define PIN_SCL        14
#define QMI_ADDR       0x6B
#define TOUCH_ADDR     0x15   // CST816-family capacitive touch, chip id 0xB7
#define I2C_HZ         400000

// Touch is the wand's trigger: hold a finger on the LCD to draw. Polled at a
// fraction of the IMU rate because a finger cannot move fast enough to care,
// and it keeps I2C well inside the 2 ms sample budget.
#define TOUCH_EVERY_N  5

// QMI8658 registers
#define REG_WHO_AM_I   0x00
#define REG_CTRL1      0x02
#define REG_CTRL2      0x03   // accel: full-scale [6:4], ODR [3:0]
#define REG_CTRL3      0x04   // gyro:  full-scale [6:4], ODR [3:0]
#define REG_CTRL5      0x06   // low-pass filters
#define REG_CTRL7      0x08   // enable bits
#define REG_AX_L       0x35   // 12 bytes: ax ay az gx gy gz, little-endian

// ±8g and ±2048dps: a wand flick clips ±4g and saturates a 1000dps gyro.
#define ACC_FS_8G      (0x02 << 4)
#define GYR_FS_2048DPS (0x07 << 4)
#define ODR_500HZ      0x04
#define SAMPLE_HZ      500
#define ACC_SCALE_G    (8.0f / 32768.0f)
#define GYR_SCALE_DPS  (2048.0f / 32768.0f)

static i2c_master_dev_handle_t s_imu;
static i2c_master_dev_handle_t s_touch;

static esp_err_t imu_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_imu, buf, 2, 100);
}

static esp_err_t imu_read(uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_transmit_receive(s_imu, &reg, 1, out, len, 100);
}

static esp_err_t touch_read(uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_transmit_receive(s_touch, &reg, 1, out, len, 50);
}

// Returns true if the panel answered; fills in contact state and position.
static bool touch_poll(int *pressed, int *x, int *y)
{
    uint8_t d[7];
    if (touch_read(0x00, d, sizeof(d)) != ESP_OK) return false;
    *pressed = (d[2] & 0x0F) > 0;
    *x = ((d[3] & 0x0F) << 8) | d[4];
    *y = ((d[5] & 0x0F) << 8) | d[6];
    return true;
}

// This panel auto-sleeps and then NACKs until something wakes it, so a one-shot
// init that gives up on first silence leaves touch dead for the whole session.
// Attaching the device handle never touches the bus, so it always succeeds; the
// configuration below is re-applied whenever the panel comes back.
static void touch_attach(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOUCH_ADDR,
        .scl_speed_hz = 100000,
    };
    i2c_master_bus_add_device(bus, &cfg, &s_touch);
}

static void touch_configure(void)
{
    uint8_t disable_auto_sleep[2] = {0xFE, 0xFF};
    i2c_master_transmit(s_touch, disable_auto_sleep, 2, 50);
    uint8_t id[3] = {0};
    if (touch_read(0xA7, id, 3) == ESP_OK) {
        printf("# touch online chipid=0x%02X projid=0x%02X fw=0x%02X\n", id[0], id[1], id[2]);
    }
}

static bool imu_init(void)
{
    i2c_master_bus_config_t bcfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bcfg, &bus) != ESP_OK) return false;
    touch_attach(bus);

    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = QMI_ADDR,
        .scl_speed_hz = I2C_HZ,
    };
    if (i2c_master_bus_add_device(bus, &dcfg, &s_imu) != ESP_OK) return false;

    // A reset landing mid-transaction can leave the bus briefly unreadable,
    // so give the part a few attempts before declaring it missing.
    uint8_t who = 0;
    bool present = false;
    for (int attempt = 0; attempt < 10 && !present; attempt++) {
        if (imu_read(REG_WHO_AM_I, &who, 1) == ESP_OK && who == 0x05) present = true;
        else vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!present) {
        printf("# ERROR: WHO_AM_I=0x%02X, expected 0x05\n", who);
        return false;
    }

    imu_write(REG_CTRL7, 0x00);                          // stop while configuring
    imu_write(REG_CTRL1, 0x40);                          // register address auto-increment
    imu_write(REG_CTRL2, ACC_FS_8G | ODR_500HZ);
    imu_write(REG_CTRL3, GYR_FS_2048DPS | ODR_500HZ);
    imu_write(REG_CTRL5, 0x11);                          // both low-pass filters on, widest band
    imu_write(REG_CTRL7, 0x03);                          // accel + gyro enable
    vTaskDelay(pdMS_TO_TICKS(50));
    return true;
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(1200));  // let USB CDC enumerate before the header

    if (!imu_init()) {
        printf("# ERROR: IMU init failed\n");
        return;
    }


    uint32_t seq = 0;
    char line[128];
    int pressed = 0, tx = 0, ty = 0;
    bool touch_online = false;

    touch_configure();

    // This part leaves STATUS0 at 0x00 even while streaming, so data-ready
    // polling never fires. Read at a fixed 500 Hz to match the configured ODR
    // instead; the host derives the true rate from t_us.
    TickType_t next = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(1000 / SAMPLE_HZ);

    while (1) {
        vTaskDelayUntil(&next, period);

        // Stamp the scheduled instant, not the moment the I2C reads happen to
        // finish: the touch poll runs on some iterations and not others, and
        // letting that leak into the timestamp shows up as 1 ms of fake jitter
        // in whatever integrates these samples.
        uint32_t t_us = (uint32_t)(esp_timer_get_time() & 0xFFFFFFFF);

        uint8_t raw[12];
        if (imu_read(REG_AX_L, raw, sizeof(raw)) != ESP_OK) continue;

        if (seq % TOUCH_EVERY_N == 0) {
            bool ok = touch_poll(&pressed, &tx, &ty);
            if (ok != touch_online) {
                touch_online = ok;
                if (ok) touch_configure();      // came back: re-disable auto sleep
                else pressed = 0;               // never leave a stroke stuck open
            }
        }

        int16_t v[6];
        for (int i = 0; i < 6; i++) {
            v[i] = (int16_t)((uint16_t)raw[2 * i] | ((uint16_t)raw[2 * i + 1] << 8));
        }

        // Re-announce the wire contract periodically so a host that attaches to
        // an already-running board still learns the scale factors.
        if (seq % (SAMPLE_HZ * 2) == 0) {
            printf("#WAND v2 imu=QMI8658 touch=CST816 rate_hz=%d acc_scale_g=%.9f gyr_scale_dps=%.9f\n",
                   SAMPLE_HZ, ACC_SCALE_G, GYR_SCALE_DPS);
            printf("#cols seq,t_us,ax,ay,az,gx,gy,gz,touch,tx,ty\n");
        }

        int n = snprintf(line, sizeof(line), "%lu,%lu,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                         (unsigned long)seq++,
                         (unsigned long)t_us,
                         v[0], v[1], v[2], v[3], v[4], v[5],
                         pressed, tx, ty);
        fwrite(line, 1, n, stdout);
    }
}
