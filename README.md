# ESP32 motion lab

This project turns the Waveshare ESP32-S3-Touch-AMOLED-1.8 V2 into a live
accelerometer/gyroscope source and an always-on BLE air mouse. The firmware
reads its onboard QMI8658 and streams JSON over USB; `dashboard.py` shows a
live pointer playground, plots the six axes, and reports mouse connection,
calibration, and physical PWR/BOOT button state.

## Run the demo

With the board connected:

```bash
./scripts/setup_toolchain.sh
./scripts/build_flash.sh /dev/cu.usbmodem1101
python3 dashboard.py
```

The dashboard opens at <http://127.0.0.1:8765>. Its pointer playground works
over USB before Bluetooth is paired: rotate the board and watch the pointer
move. It uses only the Python standard library, so there
are no host Python packages to install.

Open <http://127.0.0.1:8765/flight> for the low-poly flight demo. Rotate the
controller to bank and pitch; the controls spring smoothly toward neutral.
Hold **BOOT** to boost and tap **PWR** to airbrake. The simulator sends its live
altitude back over USB so the board's on-screen altitude indicator stays in sync.
On startup, the board guides a nine-pose orientation calibration: center, pitch
forward/back, roll right/left, center yaw, yaw right, center again, and yaw left.
Press **BOOT** to capture each pose. Flight then consumes normalized roll,
pitch, and relative yaw directly. Because the QMI8658 has no magnetometer, yaw
is relative to the captured center and can slowly drift over time.

Open <http://127.0.0.1:8765/cat> for the cozy pixel-art cat demo. Rotation
always aims the pointer; hold **BOOT** to turn on the laser so Marmalade chases
it, and tap **PWR** to center the pointer.

Open <http://127.0.0.1:8765/hockey> for a single-player air-hockey control
study. Rotation always steers the cyan paddle; hold **BOOT** for a short-range
magnetic pull and tap **PWR** to reset the puck. The computer controls the pink paddle.

Open <http://127.0.0.1:8765/doodle> for the motion-controlled QuickDraw demo.
Rotate the controller at any time to aim the brush. Hold **BOOT** while rotating
to paint; releasing ends the stroke and asks the local model to guess the
doodle. Tap **PWR** to clear the canvas. Install
`doodle_demo/requirements.txt` and run the dashboard with
`doodle_demo/.venv/bin/python dashboard.py` to enable prediction.

## Use the air mouse

1. Leave the board still for the first two seconds after reset while the gyro
   bias calibrates. The dashboard changes from **Keep board still** to
   **Gyro calibrated**.
2. In the computer's Bluetooth settings, connect **QMI8658 Air Mouse**. USB
   stays connected for power, flashing, logs, live graphing, and settings.
3. Rotate the controller. Motion is always active once calibration completes;
   no touch or button hold is required.
4. Use physical **BOOT** as the primary demo action and short-press **PWR** as
   the secondary action. Their states are included in the USB event stream.

The touchscreen and physical buttons are not assigned to desktop clicking. The
pointer uses bias-corrected QMI8658 gyro
X/Z angular rates with a 1.5 °/s dead zone and exponential smoothing. It does
not integrate linear acceleration. Use the dashboard slider to change
sensitivity from 0.05 to 0.80; the setting is sent to the firmware over the
existing USB serial connection.

## What the values mean

- Acceleration X/Y/Z is in metres per second squared. A stationary board still
  reads roughly `9.81 m/s²` along the axis aligned with gravity.
- Gyroscope X/Y/Z is angular velocity in degrees per second. A stationary board
  should sit near zero, with a small amount of sensor bias and noise.
- The gravity-tilt dot is useful for slow orientation changes. Fast movement also
  contributes linear acceleration, so it temporarily disturbs the tilt estimate.

The firmware configures a ±4 g accelerometer and ±256 dps gyroscope at a 250 Hz
sensor data rate, then sends samples and relative BLE mouse reports at 50 Hz.
The CO5300 QSPI display is registered through LVGL's SPI display path with a
single internal DMA buffer; LVGL waits for the real transfer-complete callback
before reusing that buffer, preventing stale blocks and solid-color stripes.

## Useful commands

Run the host-side tests:

```bash
python3 -m unittest discover -s tests -v
```

Run without automatically opening a browser or use a different device:

```bash
python3 dashboard.py --no-open --device /dev/cu.usbmodem1101
```

## The pigeon

Fly a pigeon over photorealistic London, with the board as the controller.

```bash
python3 pigeon/server.py            # then open the address it prints
```

Playable on keyboard and mouse with no hardware; a board takes over the moment
it appears. `docs/pigeon.md` covers the firmware split, the wire format, and
the one thing currently blocked.
