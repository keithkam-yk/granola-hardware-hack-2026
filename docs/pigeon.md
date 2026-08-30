# The pigeon, end to end

You are a pigeon over photorealistic London. The board is the controller: one
button beats your wings, the other drops one, and tilt banks and pitches.

This is the handover for how the pieces fit, what is already done, and the one
thing that is blocked.

```
board (ESP32-S3)                   host (python)              browser
  QMI8658 ─┐
  BOOT     ├─ CSV ─ UDP ─────────>  server.py  ── SSE ──>  pigeon/index.html
  PWR      │                            ^                    flight sim
  AMOLED  <┴─ "!hud ..." ─ TCP ─────────┘                    3D London
```

## Running it

```bash
python3 pigeon/server.py            # then open the address it prints
```

Needs a Google Maps Platform key with the **Map Tiles API** enabled and billing
on, in `pigeon/google-key.txt`. That file is gitignored and deliberately not in
the repo — without it the page says so on screen rather than loading a blank
city.

```bash
echo 'YOUR_KEY' > pigeon/google-key.txt
```

No board? It is fully playable on keyboard and mouse: `Space` beats the wings,
the mouse steers, click captures the pointer. A board takes over the moment it
appears.

## The three firmwares

They are complementary, not duplicates, and it is worth being clear about which
does what before picking one up:

| Project | Talks to | Draws | Use it for |
| --- | --- | --- | --- |
| `firmware/main` (imu_streamer) | USB + BLE | — | air-mouse and the motion demos |
| `firmware/demos/pigeon_preview` | nobody | the bird, FLAP, DEUCE | art and animation work |
| `firmware/demos/pigeon_controller` | **wifi** | HUD sent by the host | **flying the pigeon** |

So the preview draws the bird but talks to nobody, the streamer talks but only
down a cable, and the controller is the one that actually flies the game.

```bash
cd firmware/demos/pigeon_controller
idf.py build && idf.py -p /dev/cu.usbmodem101 flash
```

## pigeon_preview does not currently build

This is the one blocker, and it is not in anyone's own code. The vendored
Waveshare BSP logs a `uint32_t` through a `%X`:

```
esp32_s3_touch_amoled_1_8.c:511: ESP_LOGI(TAG, "Touch FT5x06 0x%02X found", ...)
cc1: some warnings being treated as errors
```

This IDF's log macros reject that under `-Werror`, so the build dies before
reaching `main.c`. Relaxing warnings globally with `-DCMAKE_C_FLAGS=-Wno-error`
does **not** fix it — it was tried.

What does fix it is scoping the exemption to that one component.
`demos/pigeon_controller/CMakeLists.txt` already carries this, and the same
three lines will unblock the preview:

```cmake
idf_component_get_property(bsp_lib waveshare__esp32_s3_touch_amoled_1_8 COMPONENT_LIB)
target_compile_options(${bsp_lib} PRIVATE -Wno-error=format)
```

Our own code keeps the stricter setting, which is the point of scoping it.

## Telling the board the wifi, once

Credentials live in NVS and survive reflashing. With nothing else holding the
port:

```bash
printf '!wifi ssid=NETWORK pass=SECRET\n' > /dev/cu.usbmodem101
```

Four networks are kept and the board joins the strongest it recognises, so a
board carried between a bench and a venue needs nothing said to it in either
place. It finds the host by broadcast rather than storing an address, so
nothing goes stale. `docs/connectivity.md` has the full write-up, including the
four reliability bugs behind it — all of which were the same bug wearing
different clothes: a failure path that returned without saying anything, so
every distinct fault presented as "nothing happens".

## The wire format

One CSV line per sample, 50 Hz:

```
seq,t_us,ax,ay,az,gx,gy,gz,btn_l,btn_r
```

`btn_l` is BOOT, `btn_r` is PWR. Both are **levels, not edges** — the game
derives the edges, so one press is one wingbeat however long a thumb stays
down. 1 g is 4096 raw.

Two channels, because control and samples want opposite things: control on TCP
and must arrive, samples on UDP and must be *recent*. Both were on TCP once and
a single lost packet held up every sample behind it, which felt like the stick
freezing and then lurching to catch up.

## Attitude: read this before touching the tilt code

Attitude is measured against a **stored reference pose**, not a fixed axis
frame. Deriving which device axis points up from how the panel is rotated was
wrong twice here — the second time inverted, which is what made roll snap
between +180 and -180.

Held in the flying position the IMU reads `ax = -0.88 g`, so screen-up is
`-ax`. That is only the default: pressing `C` in the browser replaces it with
wherever the board actually is, so how anyone chooses to hold it stops being
something we have to get right.

Working from a reference also kills the wrap. Both angles come out of `asin`
about that pose, so level reads zero and nothing crosses a discontinuity short
of 90 degrees, which no pigeon reaches.

**Press `C` while holding the board level before flying.**

## How it flies

The model is fitted to a real pigeon rather than dialled in by feel. Sink comes
from a glide polar — induced drag falling off as `1/v`, form drag climbing as
`v³` — matched to least sink of 2.5 m/s at 12 m/s. One energy balance drives
the rest: the polar takes `g·sink` out per second, the wings put `g·power`
back, and the difference is spent climbing or banked as speed.

A wingbeat is one button press, because on the board it is one button press.
Each beat injects power that bleeds away over half a second, so the bird flies
on your cadence directly:

| Presses/sec | Climb |
| --- | --- |
| 0 (gliding) | −2.8 m/s, at 57 km/h, a little over 5:1 |
| 1 | level |
| 2 | +2.5 m/s |
| 3 | +4.9 m/s |

Speed is also capped by height — 13 km/h at the deck, 42 at twenty metres, 94
above fifty-five. That is what makes landing and taking off things you *do*
rather than things needing special cases: diving at a roof scrubs the speed off
on the way in, which is the flare, and climbing away hands it back.

Collision is a raycast against the streamed tile meshes, so roofs and streets
are solid and you can perch on them. **Walls are not solid yet** — you will fly
through the side of a building.

## Known gaps

- **The board's panel is blank while flying.** `pigeon_controller` paints only
  what the host sends, and the host currently sends nothing. Folding the
  `pigeon_preview` art — the aviator, FLAP and DEUCE animations — into
  `pigeon_controller/main/hud.c` is the obvious next job, and would give the
  board its instrument panel back while it streams.
- **No wall collision**, only ground and rooftops.
- **`pigeon_preview` does not build** — see above; a three-line fix.
