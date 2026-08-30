# Pigeon controller preview

This is a standalone ESP-IDF app for trying the pigeon controller art on the
ESP32-S3 Touch AMOLED 1.8. It does not share source files with the main firmware.

- Hold **BOOT** to flap.
- Tap **PWR** to drop a fly-by deuce.

Build and flash from this directory:

```sh
export IDF_TOOLS_PATH="$PWD/../../../.tools/espressif"
source ../../../.tools/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

The checked-in RGB565 asset is generated from the approved screen PNG with:

```sh
python ../../../assets/pigeon/tools/png_to_rgb565.py \
  ../../../assets/pigeon/runtime/controller_screen_448x368.png \
  ../../../assets/pigeon/runtime/controller_screen_448x368.rgb565
```
