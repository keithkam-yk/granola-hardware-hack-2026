# Pigeon controller preview

This is a standalone ESP-IDF app for trying the pigeon controller art on the
ESP32-S3 Touch AMOLED 1.8. It does not share source files with the main firmware.

- Hold **BOOT** to play the 400 ms four-frame wing-flap loop.
- Tap **PWR** to play the 700 ms four-frame fly-by deuce loop.

The action widgets use predecoded ARGB8888 frame bundles generated from the
versioned runtime GIFs. At startup, the firmware composes nearest-neighbour
full-screen RGB565 frames in PSRAM so playback fills the 448x368 display without
the cost of scaling and alpha blending every refresh. The original GIF and
static PNG assets remain available as design sources and fallbacks.

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
