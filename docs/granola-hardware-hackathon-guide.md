<!-- Space: SE -->
<!-- Title: Granola Hardware Hackathon Guide -->

# Granola Hardware Hackathon Guide

This guide covers the [V2 Waveshare ESP32-S3-Touch-AMOLED-1.8 board](https://thepihut.com/products/esp32-s3-development-board-with-1-8-amoled-touch-display-368-x-448?srsltid=AfmBOopbRx89e5WSq9FbFp_HbBQ8J4lt9vdZ6O9nwaziFN3jinZO9vTK).

## 1. Know the hardware

The board includes:

- An ESP32-S3 dual-core processor.
- A 1.8-inch 368×448 AMOLED display.
- An 8 MB PSRAM.
- A 16 MB flash.
- A capacitive touch screen.
- An onboard microphone and speaker.
- A microSD card slot.
- A six-axis motion sensor.
- A real-time clock.
- A power-management chip.
- A 2.4 GHz Wi-Fi and Bluetooth LE connection.
- A USB-C port for power, flashing, logs, and debugging.

The V2 board uses:

- A CO5300 display controller.
- A CST820 touch controller.
- An ES8311 audio codec.
- A QMI8658 motion sensor.
- A PCF85063A real-time clock.
- An AXP2101 power-management chip.

See the [Waveshare hardware guide](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.8) for the full specification.

## 2. Prepare the equipment

You need:

- A Waveshare ESP32-S3-Touch-AMOLED-1.8 V2 board.
- A USB-C cable that supports data.
- A macOS, Linux, or Windows computer.
- An internet connection for the first build.
- An optional FAT-formatted microSD card.
- Optional jumper wires for GPIO experiments.

CAUTION: Use USB power during the hackathon.

CAUTION: Do not connect an unknown battery to the battery socket.

## 3. Install the development tools

We recommend ESP-IDF for this hackathon. It gives direct access to all board functions.

Use ESP-IDF v5.5.x or v6.0.x. Waveshare tests its current examples with both versions.

### macOS and Linux

Follow the [Espressif installation guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/get-started/linux-macos-setup.html).

For macOS with Homebrew, install the required tools:

```bash
brew install cmake ninja dfu-util ccache
```

Clone ESP-IDF:

```bash
mkdir -p ~/esp
cd ~/esp
git clone -b v5.5.5 --recursive <https://github.com/espressif/esp-idf.git>
cd esp-idf
./install.sh esp32s3
```

Activate ESP-IDF in each new terminal:

```bash
. ~/esp/esp-idf/export.sh
```

### Windows

Install the Espressif Installation Manager.

Select ESP-IDF v5.5.x or v6.0.x. Install the ESP32-S3 tools and USB drivers.

You can also install the ESP-IDF extension for Visual Studio Code.

See the [Waveshare ESP-IDF guide](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.8/ESP-IDF) for the Windows procedure.

## 4. Check the USB drivers

macOS usually needs no additional USB driver.

Windows 10 and later usually install the USB Serial/JTAG driver automatically.

Use the Espressif installer if the port does not appear on Windows.

Linux usually needs no serial driver. Install the Espressif `udev` rules if you lack USB permissions.

## 5. Get the official examples

Run:

```bash
git clone <https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8.git>
cd ESP32-S3-Touch-AMOLED-1.8
```

The repository contains examples for ESP-IDF and Arduino.

This guide uses ESP-IDF.

## 6. Find the USB port

Connect the board with the USB-C data cable.

On macOS, run:

```bash
ls /dev/cu.*
```

Look for a new port after you connect the board.

On Linux, run:

```bash
ls /dev/ttyACM*
```

On Windows, find the `COM` port in Device Manager.

Keep the port name for the next steps.

## 7. Run the first hardware check

Start with the serial board check:

```bash
cd examples/esp-idf/00_board_check
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

Replace `PORT` with the board port.

For example, a macOS command can look like this:

```bash
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

This example checks:

- The ESP32-S3 processor.
- The flash.
- The PSRAM.
- The board support package.
- The display.
- The I2C bus.
- The microSD interface.
- The audio interface.

Press `Ctrl+]` to close the serial monitor.

## 8. Test the display and touch screen

Run the interactive quick-start example:

```bash
cd ../00_bsp_quickstart
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

This example starts the AMOLED display and the touch screen.

It also provides:

- An LVGL user interface.
- A display brightness control.
- A heap and PSRAM display.
- A microSD card check.
- A microSD file-write test.

The example still works without a microSD card.

## 9. Start a project

Copy the official project template:

```bash
cd ../../..
cp -R examples/esp-idf/01_project_template my-project
cd my-project
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

The template contains the required board settings.

It also uses the official Waveshare board support package.

## 10. Add the board support package

A new ESP-IDF project can declare the package in `main/idf_component.yml`:

```yaml
dependencies:
  idf: ">=5.5,<6.1"
  waveshare/esp32_s3_touch_amoled_1_8: "^2.0.3"
```

Include the board API in your C or C++ code:

```cpp
#include "bsp/esp-bsp.h"
```

Useful board functions include:

```cpp
bsp_display_start();
bsp_display_lock();
bsp_display_brightness_set();
bsp_sdcard_mount();
bsp_audio_init();
bsp_audio_codec_speaker_init();
```

Use the board support package before you write low-level hardware drivers.

The package contains the correct pins and initialization steps for the V2 board.

The first build downloads managed components from the internet.

## 11. Choose a starting example

The official repository contains examples for the main board functions.

### Board and display

Use these examples:

- `00_board_check` for a basic board check.
- `00_bsp_quickstart` for the display and touch screen.
- `13_display_colorbar` for direct display output.
- `14_lvgl_demo_v9` for an LVGL user interface.

### Storage

Use `09_sdmmc` for microSD card access and FAT file operations.

### Audio

Use `12_i2s_codec` for the ES8311 audio codec and speaker output.

Start with a low speaker volume.

### Wi-Fi

Use `10_wifi_station` for a Wi-Fi connection.

Do not commit a real Wi-Fi password to a public repository.

### Motion sensor

Use `92_qmi8658_imu` for acceleration and gyroscope data.

### Real-time clock

Use `91_pcf85063_rtc` for time storage and time reads.

### Power management

Use `90_axp2101_pmu` for power and battery information.

### Other interfaces

Use these examples:

- `05_gpio_io` for GPIO input and output.
- `06_gpio_interrupt` for GPIO interrupts.
- `08_i2c_tools` for an I2C device scan.
- `03_nvs_counter` for persistent settings.
- `04_freertos_tasks` for tasks and queues.

## 12. Develop a user interface

The board uses a 368×448 portrait display.

Use LVGL for controls, text, images, lists, and animation.

Start with `14_lvgl_demo_v9` or `00_bsp_quickstart`.

Use the board lock while you change LVGL objects:

```cpp
bsp_display_lock(0);

// Change LVGL objects here.

bsp_display_unlock();
```

Keep display updates small. Large redraws use more processor time and memory.

Use PSRAM for large frame buffers or image data.

Use software rotation if your project needs a landscape display.

## 13. Use the touch screen

The V2 board uses a CST820 touch controller.

The board support package initializes the controller and applies the correct board settings.

Use LVGL input events for buttons, sliders, and gestures.

Test these touch cases:

- A short tap.
- A long press.
- A drag.
- A screen-edge touch.
- A rapid sequence of taps.

Make important actions clear. Require confirmation before a destructive action.

## 14. Use the microphone and speaker

The ES8311 codec controls the onboard microphone and speaker.

Use the official audio example before you change the sample rate or I2S settings.

Start with one direction:

- Capture microphone audio.
- Play speaker audio.

Add simultaneous capture and playback only if your project needs both.

Audio buffers can use much memory. Monitor the free heap while audio runs.

Start the speaker at a low volume. Increase the volume after you confirm correct output.

## 15. Use the microSD card (not provided)

Format the card as FAT before the event.

Insert the card before the board starts.

Use the board support package to mount it:

```cpp
bsp_sdcard_mount();
```

Write data to the mounted `/sdcard` directory.

Close each file after you write it. Flush important data before you remove power.

Do not format a card automatically after a mount failure. The card can contain important data.

## 16. Use Wi-Fi

The ESP32-S3 supports 2.4 GHz Wi-Fi.

It does not support a 5 GHz-only network.

Store test credentials outside the source when possible.

Do not put API keys in firmware that you plan to publish.

Use HTTPS for network requests that contain private information.

Add timeouts to all network operations. Keep the display and controls responsive during a network failure.

## 17. Use a fast development cycle

Use this cycle:

1. Change one small function.
2. Run `idf.py build`.
3. Run `idf.py -p PORT flash monitor`.
4. Check the serial log.
5. Test the hardware.
6. Commit the working state.

Keep the serial log open. Hardware failures often appear there before they appear on the display.

Add logs around each hardware initialization step.

## 18. Troubleshoot common problems

### The USB port does not appear

- Use a USB cable that supports data.
- Try a different USB port.
- Disconnect other ESP32 boards.
- Install the Espressif USB drivers on Windows.
- Check the system USB device list.

### Flashing waits for a connection

- Hold the `BOOT` button.
- Power-cycle the board.
- Release `BOOT` after the flash tool connects.
- Run the flash command again.

### The serial monitor shows unreadable text

- Close other programs that use the same port.
- Reset the board.
- Use the port that appeared after you connected the board.

### The display stays black

- Run `00_board_check`.
- Run `00_bsp_quickstart`.
- Confirm that PSRAM works.
- Check the serial log for a display error.
- Confirm that your project uses the V2 board support package.

### The touch screen does not work

- Run `00_bsp_quickstart`.
- Check the I2C devices with `08_i2c_tools`.
- Use the board support package.
- Do not replace the touch settings with generic ESP32 settings.

Some source identifiers use the CST816 driver family name. The physical V2 board uses the compatible CST820 controller.

### The first build fails during dependency download

- Check the internet connection.
- Activate the ESP-IDF environment.
- Run the build again.
- Delete only the project build directory if the cache has an error.

Do not copy a generated `managed_components` directory between projects.

### The board restarts during display or audio work

- Check the serial log for a crash or watchdog error.
- Check the free heap.
- Move large buffers to PSRAM.
- Reduce the display buffer size.
- Reduce the audio buffer size.
- Do not block a FreeRTOS task for a long time.

### Wi-Fi does not connect

- Use a 2.4 GHz network.
- Check the network name and password.
- Avoid captive-portal networks.
- Add a clear connection timeout.
- Show the connection state on the display.

## 19. Protect the project

Do not commit:

- Wi-Fi passwords.
- API keys.
- Private certificates.
- Device tokens.
- Personal recordings.
- Generated build files.

Add local secrets and build directories to `.gitignore`.

Keep a working firmware build before you make a large change.

Do not erase the full flash unless you need recovery. A full erase removes the current application and saved settings.

Waveshare provides factory recovery firmware in the official repository.

## 20. Final demonstration checklist

Before the demonstration:

- Charge or power the board.
- Build the project from a clean terminal.
- Flash the final firmware.
- Restart the board.
- Test the main interaction.
- Test the interaction without Wi-Fi.
- Check the serial log for repeated errors.
- Remove private credentials from the screen.
- Keep the USB cable and a known port available.
- Keep one known working firmware build.

## Reference links

- [Waveshare product documentation](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.8)
- [Official examples and board support](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8)
- [Waveshare ESP-IDF guide](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.8/ESP-IDF)
- [Waveshare firmware recovery guide](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.8/Firmware-Flashing)
- [Espressif ESP32-S3 setup guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/get-started/linux-macos-setup.html)
