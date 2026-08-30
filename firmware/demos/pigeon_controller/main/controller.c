// The stick: what the pilot's hands and wrist are doing.
//
// Raw samples only. No filtering, no orientation fusion, no notion of a plane —
// the host turns these numbers into flight, so the flight model can be retuned
// without a reflash. The one thing decided here is the axis frame, which is
// squared to the panel's rotation so "bank left" means the same to the game as
// it does to the picture the pilot is looking at.
#include <stdio.h>

#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "controller.h"
#include "link.h"

#define QMI_ADDR       0x6B
#define AXP_ADDR       0x34
#define I2C_HZ         400000

#define PIN_BOOT       GPIO_NUM_0    // active low, measured

// PWR is a key on the PMU rather than a pin. Its interrupt status register
// latches an edge until acknowledged: bit1 on the way down, bit0 on the way up.
// Both were read off real presses; the datasheet is not the source here.
#define AXP_IRQ_STA1   0x49
#define AXP_PWRON_DOWN 0x02
#define AXP_PWRON_UP   0x01

// QMI8658 registers.
#define REG_WHO_AM_I   0x00
#define REG_CTRL1      0x02
#define REG_CTRL2      0x03   // accel: full-scale [6:4], ODR [3:0]
#define REG_CTRL3      0x04   // gyro:  full-scale [6:4], ODR [3:0]
#define REG_CTRL5      0x06   // low-pass filters
#define REG_CTRL7      0x08   // enable bits
#define REG_AX_L       0x35   // 12 bytes: ax ay az gx gy gz, little-endian

// A hard bank clips 4g and saturates a 1000 dps gyro, so both ranges are opened
// up rather than trading headroom for resolution that a wrist cannot use.
#define ACC_FS_8G      (0x02 << 4)
#define GYR_FS_2048DPS (0x07 << 4)
#define ODR_500HZ      0x04
#define ACC_SCALE_G    (8.0f / 32768.0f)
#define GYR_SCALE_DPS  (2048.0f / 32768.0f)

// 50 Hz. A wrist cannot produce anything a plane needs faster than this, and
// the display runs its own clock and eases between readings, so resolution here
// buys nothing that airtime on a busy 2.4 GHz channel does not cost more. The
// rate travels in the banner, so raising it later costs the host nothing.
#define SAMPLE_HZ      50

// The PMU only has to be asked often enough that a trigger never feels late.
#define PMU_EVERY_N    2

static i2c_master_dev_handle_t s_imu;
static i2c_master_dev_handle_t s_pmu;

static esp_err_t imu_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_imu, buf, 2, 100);
}

static esp_err_t imu_read(uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_transmit_receive(s_imu, &reg, 1, out, len, 100);
}

static bool imu_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = QMI_ADDR,
        .scl_speed_hz = I2C_HZ,
    };
    if (i2c_master_bus_add_device(bus, &dev, &s_imu) != ESP_OK) return false;

    // A reset landing mid-transaction can leave the bus briefly unreadable, so
    // give the part a few attempts before declaring it missing.
    uint8_t who = 0;
    bool present = false;
    for (int attempt = 0; attempt < 10 && !present; attempt++) {
        if (imu_read(REG_WHO_AM_I, &who, 1) == ESP_OK && who == 0x05) present = true;
        else vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!present) {
        link_sendf("# ERROR imu who_am_i=0x%02X expected 0x05", who);
        return false;
    }

    imu_write(REG_CTRL7, 0x00);                    // stop while configuring
    imu_write(REG_CTRL1, 0x40);                    // register address auto-increment
    imu_write(REG_CTRL2, ACC_FS_8G | ODR_500HZ);
    imu_write(REG_CTRL3, GYR_FS_2048DPS | ODR_500HZ);
    imu_write(REG_CTRL5, 0x11);                    // both low-pass filters, widest band
    imu_write(REG_CTRL7, 0x03);                    // accel + gyro enable
    vTaskDelay(pdMS_TO_TICKS(50));
    return true;
}

static void pmu_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP_ADDR,
        .scl_speed_hz = I2C_HZ,
    };
    i2c_master_bus_add_device(bus, &dev, &s_pmu);
}

// Turns the PMU's latched edges back into a held state. Returns the button's
// level so the host sees the same shape from both triggers.
static int pwr_level(void)
{
    static int held;

    uint8_t reg = AXP_IRQ_STA1, status = 0;
    if (i2c_master_transmit_receive(s_pmu, &reg, 1, &status, 1, 50) != ESP_OK) return held;
    if (status == 0) return held;

    if (status & AXP_PWRON_DOWN) held = 1;
    if (status & AXP_PWRON_UP) held = 0;

    uint8_t ack[2] = {AXP_IRQ_STA1, status};
    i2c_master_transmit(s_pmu, ack, 2, 50);
    return held;
}

static void controller_task(void *arg)
{
    uint32_t seq = 0;
    int pwr = 0;

    TickType_t next = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(1000 / SAMPLE_HZ);

    while (1) {
        vTaskDelayUntil(&next, period);

        // Stamp the scheduled instant rather than the moment the reads finish:
        // the PMU is polled on some iterations and not others, and letting that
        // leak into the timestamp shows up as fake jitter to anything
        // integrating these samples.
        uint32_t t_us = (uint32_t)(esp_timer_get_time() & 0xFFFFFFFF);

        uint8_t raw[12];
        if (imu_read(REG_AX_L, raw, sizeof(raw)) != ESP_OK) continue;

        if (seq % PMU_EVERY_N == 0) pwr = pwr_level();

        int16_t v[6];
        for (int i = 0; i < 6; i++) {
            v[i] = (int16_t)((uint16_t)raw[2 * i] | ((uint16_t)raw[2 * i + 1] << 8));
        }

        // Re-announce the wire contract periodically so a host that attaches to
        // an already-running board still learns the scale factors.
        if (seq % (SAMPLE_HZ * 2) == 0) {
            link_sendf("#DOGFIGHT v3 imu=QMI8658 rate_hz=%d acc_scale_g=%.9f gyr_scale_dps=%.9f link_dropped=%lu",
                       SAMPLE_HZ, ACC_SCALE_G, GYR_SCALE_DPS, (unsigned long)link_dropped());
            link_sendf("#cols seq,t_us,ax,ay,az,gx,gy,gz,btn_l,btn_r");
        }

        link_sendf("%lu,%lu,%d,%d,%d,%d,%d,%d,%d,%d",
                   (unsigned long)seq++, (unsigned long)t_us,
                   v[0], v[1], v[2], v[3], v[4], v[5],
                   gpio_get_level(PIN_BOOT) == 0 ? 1 : 0, pwr);
    }
}

esp_err_t controller_start(void)
{
    // The BSP already owns the bus these parts sit on. Opening a second one on
    // the same pins is how you get a board that works until the panel is
    // touched at the wrong moment.
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) return ESP_ERR_INVALID_STATE;

    const gpio_config_t boot = {
        .pin_bit_mask = 1ULL << PIN_BOOT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&boot);

    pmu_init(bus);
    if (!imu_init(bus)) return ESP_FAIL;

    return xTaskCreatePinnedToCore(controller_task, "controller", 4096, NULL, 6, NULL, 0) == pdPASS
           ? ESP_OK : ESP_ERR_NO_MEM;
}
