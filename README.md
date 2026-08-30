# ESP32 motion lab

This project turns the Waveshare ESP32-S3-Touch-AMOLED-1.8 V2 into a live
accelerometer/gyroscope source and a touch-clutched BLE air mouse. The firmware
reads its onboard QMI8658 and streams JSON over USB; `dashboard.py` shows a
live pointer playground, plots the six axes, and reports mouse connection,
calibration, and clutch state.

## Run the demo

With the board connected:

```bash
./scripts/setup_toolchain.sh
./scripts/build_flash.sh /dev/cu.usbmodem1101
python3 dashboard.py
```

The dashboard opens at <http://127.0.0.1:8765>. Its pointer playground works
over USB before Bluetooth is paired: hold the touchscreen, rotate the board,
and watch the pointer move. It uses only the Python standard library, so there
are no host Python packages to install.

## Use the air mouse

1. Leave the board still for the first two seconds after reset while the gyro
   bias calibrates. The dashboard changes from **Keep board still** to
   **Gyro calibrated**.
2. In the computer's Bluetooth settings, connect **QMI8658 Air Mouse**. USB
   stays connected for power, flashing, logs, live graphing, and settings.
3. Touch and hold the board's screen. After 120 ms, the dashboard shows
   **Clutch active** and wrist rotation moves the pointer.
4. Lift your finger to stop movement immediately. The next hold starts from the
   new wrist position, so there is no accumulated absolute pointer pose.

A short tap never produces movement, and touch is not assigned to clicking.
The pointer uses bias-corrected QMI8658 gyro X/Z angular rates with a 1.5 °/s
dead zone and exponential smoothing. It does not integrate linear acceleration.
Use the dashboard slider to change sensitivity from 0.05 to 0.80; the setting
is sent to the firmware over the existing USB serial connection.

## What the values mean

- Acceleration X/Y/Z is in metres per second squared. A stationary board still
  reads roughly `9.81 m/s²` along the axis aligned with gravity.
- Gyroscope X/Y/Z is angular velocity in degrees per second. A stationary board
  should sit near zero, with a small amount of sensor bias and noise.
- The gravity-tilt dot is useful for slow orientation changes. Fast movement also
  contributes linear acceleration, so it temporarily disturbs the tilt estimate.

The firmware configures a ±4 g accelerometer and ±256 dps gyroscope at a 250 Hz
sensor data rate, then sends samples and relative BLE mouse reports at 50 Hz.

## Useful commands

Run the host-side tests:

```bash
python3 -m unittest discover -s tests -v
```

Run without automatically opening a browser or use a different device:

```bash
python3 dashboard.py --no-open --device /dev/cu.usbmodem1101
```
