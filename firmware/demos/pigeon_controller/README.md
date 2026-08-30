# Pigeon controller firmware

Turns the board into the wireless controller for `pigeon/`: it streams the IMU
and both buttons to the game host over wifi, and paints whatever HUD state the
host sends back.

```
. $HOME/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.usbmodem101 flash
```

Then run the game host and open the address it prints:

```
python3 pigeon/server.py
```

## Telling it the wifi, once

Credentials live in NVS and survive reflashing. With nothing else holding the
port:

```
printf '!wifi ssid=NETWORK pass=SECRET\n' > /dev/cu.usbmodem101
```

Up to four networks are kept, and the board joins the strongest it recognises.
Nothing else needs configuring: it finds the host by broadcast, so no address
is ever stored to go stale.

## What it sends

One CSV line per sample, at 50 Hz:

```
seq,t_us,ax,ay,az,gx,gy,gz,btn_l,btn_r
```

Buttons are levels, not edges — the game derives edges, so one press is one
wingbeat however long a thumb stays down. `btn_l` is BOOT, `btn_r` is PWR.

Two channels, because control and samples want opposite things. Control is TCP
and must arrive; samples are UDP and must be *recent*. Putting 50 Hz samples on
the TCP socket meant one lost packet held up every sample behind it, which
showed up as the stick freezing and then catching up in a lurch. See
`docs/connectivity.md` for that and the four reliability bugs behind it.

USB stays attached underneath: whenever the socket breaks, output goes back to
the cable, so the board is never mute and `cat /dev/cu.usbmodem101` always shows
what it is doing.

## Note on the BSP

`CMakeLists.txt` scopes `-Wno-error=format` to the vendored Waveshare component.
That component logs a `uint32_t` through a `%X`, which this IDF's log macros
reject under `-Werror`, and it is the reason `demos/pigeon_preview` does not
build as-is. Our own code keeps the stricter setting.
