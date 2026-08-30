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

### The stick, measured

The axis frame is not derivable. The IMU is a separate part soldered in whatever
orientation suited the board, and it owes the panel's rotation nothing. Worse,
the frame depends on how the board is *held*, and this one is flown flat like a
tray rather than upright like a yoke, so three careful derivations from the
display rotation were all wrong about the pose before they were wrong about the
axes.

So the pilot demonstrates it: level, full left, full right, nose down, nose up,
pressing BOOT at each. Every axis is defined as whatever moved between its two
extremes, which makes roll positive banking right and pitch positive nose up by
construction, leaving no sign to invert. The result lands in
`host/calibration.json` and every view reads it, so it is measured once.

Measured for this board and this grip, up is very nearly `-z`, roll runs along
`+x` and pitch along `+y`.

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
./run.sh                      # start the game host
```

Then open the address `run.sh` prints from any browser on the same wifi: a
laptop, a phone, the TV.

A board needs to be told the network once. With the host stopped so the port is
free, send it the credentials over its own USB link:

```
printf '!wifi ssid=NETWORK pass=SECRET\n' > /dev/cu.usbmodem101
```

They are written to NVS and survive reflashing. Nothing else needs configuring:
the board broadcasts for a host and the host answers with the port to come back
on, so no address is ever stored to go stale.

To work on the cable instead, `./run.sh --serial /dev/cu.usbmodem101`. A board
falls back to USB whenever its socket breaks, so it is never mute.

```
DOGFIGHT_PROBE=1 ./flash.sh   # the button-discovery probe
host/monitor.sh               # raw serial, no host in the way
```
