// Button discovery probe.
//
// Finds which input the two onboard buttons actually land on, rather than
// trusting a pinout diagram. Waveshare documents BOOT as GPIO0 and PWR as a key
// on the AXP2101, but the PMU's exact status bits are not documented, so both
// are confirmed here by pressing them and reading what moves.
//
// Every candidate pin is configured as an *input with a pull-up and nothing
// else*. Nothing on this board is ever driven by this firmware: driving an
// unverified pin is what latched the sibling board's panel into a striped mode
// that survived reflashes.
//
// Build:  DOGFIGHT_PROBE=1 ./flash.sh
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

#define PIN_SDA   15
#define PIN_SCL   14
#define AXP_ADDR  0x34
#define I2C_HZ    400000

// Pins that are free on this board. Everything else is spoken for and left
// alone: 1-3 SD, 4-7/11/12 the QSPI panel, 8-10/16/45/46 audio, 14/15 I2C,
// 19/20 native USB, 21 touch INT, 26-37 flash and octal PSRAM.
static const int CANDIDATES[] = {0, 13, 17, 18, 38, 39, 40, 41, 42, 43, 44, 47, 48};
#define N_CANDIDATES (sizeof(CANDIDATES) / sizeof(CANDIDATES[0]))

// AXP2101 interrupt status. Write-1-to-clear, so a latched press is readable
// once and then acknowledged. The bit layout is deliberately not decoded: the
// point of the probe is to record which bits a real press sets.
#define AXP_IRQ_EN0   0x40
#define AXP_IRQ_STA0  0x48
#define N_IRQ_REGS    3

static i2c_master_dev_handle_t s_axp;

static esp_err_t axp_read(uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_transmit_receive(s_axp, &reg, 1, out, len, 100);
}

static esp_err_t axp_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_axp, buf, 2, 100);
}

static bool axp_init(void)
{
    i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus_handle;
    if (i2c_new_master_bus(&bus, &bus_handle) != ESP_OK) return false;

    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP_ADDR,
        .scl_speed_hz = I2C_HZ,
    };
    if (i2c_master_bus_add_device(bus_handle, &dev, &s_axp) != ESP_OK) return false;

    uint8_t enables[N_IRQ_REGS];
    if (axp_read(AXP_IRQ_EN0, enables, N_IRQ_REGS) != ESP_OK) {
        printf("# PMU: no response at 0x%02X\n", AXP_ADDR);
        return false;
    }
    printf("# PMU irq_enable was %02X %02X %02X; unmasking all so a key press latches\n",
           enables[0], enables[1], enables[2]);

    // Unmasking interrupts cannot change a power rail, and the vendor's own
    // settings come back on reset. Power-off timing (0x25-0x27) is untouched.
    for (int i = 0; i < N_IRQ_REGS; i++) axp_write(AXP_IRQ_EN0 + i, 0xFF);
    for (int i = 0; i < N_IRQ_REGS; i++) axp_write(AXP_IRQ_STA0 + i, 0xFF);
    return true;
}

void app_main(void)
{
    printf("\n#PROBE buttons — press BOOT, then *short*-press PWR.\n");
    printf("# Do not hold PWR: a long press is wired to the PMU's power-off.\n");

    int level[N_CANDIDATES];
    for (size_t i = 0; i < N_CANDIDATES; i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << CANDIDATES[i],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
        level[i] = gpio_get_level(CANDIDATES[i]);
    }

    printf("# watching gpio");
    for (size_t i = 0; i < N_CANDIDATES; i++) printf(" %d", CANDIDATES[i]);
    printf(" (idle:");
    for (size_t i = 0; i < N_CANDIDATES; i++) printf(" %d", level[i]);
    printf(")\n");

    bool pmu = axp_init();
    uint8_t irq[N_IRQ_REGS] = {0};

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20));

        for (size_t i = 0; i < N_CANDIDATES; i++) {
            int now = gpio_get_level(CANDIDATES[i]);
            if (now == level[i]) continue;
            level[i] = now;
            printf("GPIO%-2d %s\n", CANDIDATES[i], now ? "release" : "PRESS");
        }

        if (!pmu) continue;
        if (axp_read(AXP_IRQ_STA0, irq, N_IRQ_REGS) != ESP_OK) continue;
        if (!(irq[0] | irq[1] | irq[2])) continue;

        printf("PMU irq %02X %02X %02X\n", irq[0], irq[1], irq[2]);
        for (int i = 0; i < N_IRQ_REGS; i++) {
            if (irq[i]) axp_write(AXP_IRQ_STA0 + i, irq[i]);
        }
    }
}
