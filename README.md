# Dogfight

A browser dogfighting game flown by tilting an ESP32 board. The board is the
controller *and* the instrument panel: the IMU flies the plane, the two onboard
buttons fire the left and right hardpoints, and the AMOLED shows hull damage and
the selected weapons, which you switch by touch.

## Hardware

Waveshare **ESP32-S3-Touch-AMOLED-1.8, V2 revision**.

| | |
| --- | --- |
| MCU | ESP32-S3R8, 16MB flash, 8MB PSRAM |
| Display | 1.8" AMOLED 368x448, CO5300 over QSPI |
| Touch | CST820 (CST816 family), I2C 0x15, chip id 0xB7 |
| IMU | QMI8658, I2C 0x6B |
| Also on the bus | PCF85063 RTC, ES8311 codec, TCA9554 expander, AXP2101 PMU |
| I2C | SDA=15, SCL=14 |

V1 boards ship an SH8601 panel and FT3168 touch instead. The CST820 is what
identifies this one as V2, and the vendor BSP picks the right driver from it.

### Buttons, measured

`firmware/main/tools/button_discovery.c` watches every free pin as a pull-up
input and dumps the PMU's interrupt status, so the mapping below was read off a
real press rather than a pinout diagram:

| Button | Source | Press | Release |
| --- | --- | --- | --- |
| BOOT | GPIO0, active low | level 0 | level 1 |
| PWR | AXP2101 IRQ status reg 0x49 | 0x02 (key-down) | 0x09 (key-up + short-press) |

PWR reports its down and up edges separately, so it holds like a trigger rather
than only clicking. Its long press still belongs to the PMU's power-off, so the
game never asks you to hold it.

Reg 0x4A flaps between 0x08 and 0x10 continuously with no battery attached.
That is the fuel gauge talking to itself; the firmware ignores that register.

The probe drives nothing, ever. It only configures inputs. A single unverified
pin driven on the sibling board latched its panel into a striped mode that
survived reflashes, which is a day nobody needs twice.

## Split of responsibilities

```
device (ESP32-S3)                     host (python)            browser
  QMI8658 --+
  BOOT      +-- CSV lines --USB CDC--> serial reader --SSE--> game sim + canvas
  PWR       |                              ^                        |
  touch  ---+                              +------ HTTP POST -------+
  LVGL HUD  <---- "!hud ..." lines <-------+
```

Flight physics, weapons, damage and the enemy AI all live in the browser, so
they can be retuned without a reflash. The firmware owns sensors, buttons, and
painting whatever HUD state it is handed; it knows no game rules. A second
controller is therefore a host-side flag rather than a rewrite.

Inherited from `../magic-wand`, which proved the 500 Hz IMU path and the
orientation filter this game flies on.

## Run

```
./flash.sh                    # build + flash the controller
DOGFIGHT_PROBE=1 ./flash.sh   # build + flash the button-discovery probe
host/monitor.sh               # watch the raw serial stream
```
