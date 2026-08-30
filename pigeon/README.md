# Pigeon

You are a pigeon over London. One button beats your wings, the other drops one.

Self-contained: its own web server, its own build, no shared files. The city is
Google's Photorealistic 3D Tiles, streamed live.

```
python3 pigeon/server.py          # then open the address it prints
```

## Layout

```
server.py     serves the page, gathers controllers, fans samples out as SSE
main.js       the game: flight model, board input, tiles, render
page.html     the shell the bundle is injected into
build.sh      esbuild main.js + three.js + the tiles renderer -> index.html
index.html    the built page, one file, committed
```

`index.html` is a single file with everything inlined — three.js and the 3D
tiles renderer included — so there is no asset directory to serve alongside it
and this folder can be copied anywhere.

```
./build.sh          # after editing main.js
```

Dependencies are pinned in `package.json` and fetched on demand rather than
committed; `build.sh` runs `npm install` the first time.

## The controller

| Input | Does |
| --- | --- |
| PWR (right) | one wingbeat |
| BOOT (left) | drop one |
| tilt left/right | bank, which turns |
| tilt forward/back | dive and climb |
| `C` on the keyboard | take the current pose as level |

Buttons are read as rising edges, so one press is one beat however long a thumb
stays down. Tilt is a *rate*: holding a bank keeps the turn coming.

Attitude is measured against a stored reference pose rather than a fixed axis
frame — see the comment in `main.js`, which records why. Held in the flying
position the IMU reads `ax = -0.88g`, so screen-up is `-ax`; that is only the
default, and `C` replaces it with wherever the board actually is.

A board takes over the moment it appears on `/stream`; without one the keyboard
and pointer fly it, so the view is always playable with no hardware. `Space`
beats the wings, the mouse steers, and clicking captures the pointer.

The controller half of `server.py` — discovery, the reliable control socket, the
lossy sample datagrams — is lifted from the dogfight host rather than rewritten:
four separate reliability bugs were already found and fixed in it, and none of
that is worth learning twice. See `docs/connectivity.md`.

## Flying it

One press a second holds height. Two climbs at about 2.5 m/s, three at about 5.
Stop and you glide at roughly five to one, sinking 2.8 m/s. Those numbers come
out of a glide polar fitted to a real pigeon rather than being dialled in, so
changing `beatImpulse` or `powerDecay` moves all of them together.

## The API key

`pigeon/google-key.txt` holds the Google Maps Platform key and is gitignored —
it is deliberately not in this repository. Create it with:

```
echo 'YOUR_KEY' > pigeon/google-key.txt
```

It needs the Map Tiles API enabled and billing on. Without it the page says so
on screen rather than failing silently.
