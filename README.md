# Dogfight

A browser dogfighting game flown by tilting an ESP32 board. The board is the
controller *and* the instrument panel: the IMU flies the plane, the two onboard
buttons fire the left and right hardpoints, and the AMOLED shows hull damage and
the selected weapons, which you switch by touch.

```
board (ESP32-S3)                      host (python)            browser
  IMU  -----+
  BOOT      +--- CSV lines --wifi---> game host --SSE--> game view on a TV
  PWR       |                             ^                     |
  touch  ---+                             +----- HTTP POST -----+
  AMOLED HUD <----- "!hud ..." lines <----+
```

Flight physics, weapons, damage and the enemy AI live in the browser, so they
can be retuned without a reflash. The firmware owns sensors, buttons and
painting whatever HUD state it is handed; it knows no game rules. A second
controller is therefore a matter of plugging one in.

## The controller

### Controls

| Input | Does | Where it comes from |
| --- | --- | --- |
| Tilt left/right | roll, which banks and turns | IMU, `+x` |
| Tilt forward/back | pitch, which climbs and dives | IMU, `+y` |
| **BOOT** button | fires the left hardpoint | GPIO0, active low |
| **PWR** button | fires the right hardpoint | AXP2101 IRQ register `0x49` |
| Both buttons together | recalibrate the stick | — |
| Touch a weapon card | switch that side's weapon | CST820 panel, via LVGL |

Shots are discrete: one per press, on the rising edge. Nothing fires by holding.

Neither button mapping was read off a pinout. `firmware/main/tools/button_discovery.c`
watches every free pin as a pull-up input and dumps the PMU's interrupt status,
so both were measured from real presses. The probe drives nothing, ever — a
single unverified pin driven on a sibling board latched its panel into a striped
mode that survived reflashes.

### The HUD

`firmware/main/hud.c` draws the panel at 448x368, landscape, and knows nothing
about the game. It renders the state the host sends and reports what you touch.

- **Hull** as a bar and a percentage across the top.
- **Three damage blocks** laid out like the aircraft they stand for: left wing,
  fuselage, right wing. Wing damage costs turn rate on that side, so the shape
  is the point — "the left one is red" needs no translating mid-turn. Colour
  runs green, amber, red.
- **Two weapon cards**, each labelled with the button that fires it, showing the
  weapon and its ammo. Tapping one switches that side's weapon and sends
  `!sel side=L` upstream.

The panel is rotated in software because the CO5300 has no hardware rotation,
and the rotation must be set under the LVGL mutex — the port's task is already
running by the time `bsp_display_start` returns, and setting it unlocked leaves
the screen blank.

### The stick, measured rather than derived

The axis frame is not derivable, and three careful attempts to get it from the
panel's rotation were all wrong. The IMU is a separate part soldered in whatever
orientation suited the board and owes the display nothing; worse, the frame
depends on how the board is *held*, and this one is flown flat like a tray
rather than upright like a yoke.

So the pilot demonstrates it: level, full left, full right, nose down, nose up,
pressing BOOT at each. Every axis is defined as whatever moved between its two
extremes, which makes roll positive banking right and pitch positive nose up by
construction, leaving no sign to invert. The result lands in
`host/calibration.json` and every view reads it from `GET /calibration`, so it
is measured once for the board rather than once per page.

Measured for this board and this grip: up is `-z`, roll runs along `+x`, pitch
along `+y`.

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

## Wire protocol

Text, one message per line, both directions, so the transport can change without
anything above it noticing. It already has: USB first, then wifi.

Samples travel by UDP to the port beside the socket's; everything else, in both
directions, goes over the socket.

Up, at 50 Hz:

```
#DOGFIGHT v3 imu=QMI8658 rate_hz=50 acc_scale_g=... gyr_scale_dps=... link_dropped=0
#cols seq,t_us,ax,ay,az,gx,gy,gz,btn_l,btn_r
1284,25680000,-31,142,-4090,-2,1,-4,0,0
!sel side=L
```

Down, whenever the panel should change. Every field is optional and unknown keys
are ignored, so the host can add readouts before the firmware has heard of them:

```
!hud hp=87 wl=40 wr=95 spd=310 gl=CANNON al=120 gr=MISSILE ar=4
```

## Run

```
./flash.sh          # build + flash the controller
./run.sh            # start the game host
```

Open the address `run.sh` prints from any browser on the same wifi:

| Address | |
| --- | --- |
| `/` | the controller: calibration and a live check of stick and triggers |
| `/2d` | one shared screen, both planes |
| `/3d` | split screen, third-person over London |

### Connecting

A board is told a network once and then never again. With the host stopped so
the port is free:

```
printf '!wifi ssid=NETWORK pass=SECRET\n' > /dev/cu.usbmodem101
```

Credentials go to NVS and survive reflashing; they never pass through the host
or a build. Up to four networks are kept, so a board carried between the bench
and the venue needs nothing said to it in either place: at power-up it scans,
joins whichever known network is actually in the room, and prefers the strongest
when several are.

Finding the game is the same story. The board asks by broadcast and the host
answers with the port to come back on; the address that answered is remembered,
so from then on it goes straight there and only falls back to asking when that
address stops replying. Where a network hands out more than one subnet a
broadcast cannot cross, the remembered address also gives the board the right
subnet to ask on. To seed that by hand:

```
printf '!host ip=192.168.1.20\n' > /dev/cu.usbmodem101
```

None of this is silent. The panel shows where the board has got to — scanning,
the network it joined, looking for game, in game — and the same story goes up
the link, so a controller that will not connect says why instead of being a
black box:

```
#net know 2 networks
#net remember host 192.168.5.12:41235
#net joining NETWORK at -49 dBm
#net ip=192.168.4.76
#net host 192.168.5.12:41235
```

To work on the cable instead, `./run.sh --serial /dev/cu.usbmodem101`. A board
falls back to USB whenever its socket breaks, so it is never mute.

```
DOGFIGHT_PROBE=1 ./flash.sh   # the button-discovery probe
host/monitor.sh               # raw serial, no host in the way
```

## What the link actually does

Measured, so that expectations match the hardware rather than the diagram.

| | |
| --- | --- |
| Sample rate delivered | 50 Hz, no drops |
| Typical gap | ~20 ms |
| Stalls | 150-350 ms, a few times a minute |
| Reconnect after a host restart | ~1 s |

The tail is the network, not the code: an ESP32 is 2.4 GHz only, and on a guest
access point that also routes between subnets and a laptop on 5 GHz, ICMP alone
runs a 7 ms floor against a 200 ms average. Two things that did not fix it,
recorded so nobody tries them twice: halving the sample rate, and quartering the
number of transmissions. Both worked as designed and neither moved the stalls,
because they are latency and not bandwidth.

- **Samples by datagram instead of over the socket.** TCP holds everything
  behind a lost packet until it has been resent, and a stale sample was
  worthless anyway, so waiting for it cost the fresh ones for nothing. Halved
  both the p99 gap and the number of stalls. Weapon changes still go over the
  socket, because those must arrive.

Two other things that did:

- **Wifi modem power save off.** It parks the radio between beacons and turned a
  steady stream into bursts. It only takes effect after the driver is started.
- **Painting on a display clock rather than on packet arrival.** A stall then
  reads as a slightly slower needle instead of a freeze. The game views run a
  continuous simulation for the same reason: the stick is an input, not a
  position, so the aircraft keeps flying through a gap.

## Inherited

From `../magic-wand`, which proved the 500 Hz IMU path and the orientation
filter this game flies on.
