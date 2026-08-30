# Zero-touch connectivity for ESP32 controllers

How a battery-powered ESP32-S3 controller gets from power-up to streaming into a
game host with nothing typed at a cable, and what went wrong on the way there.

Written to be lifted into other work: the shape here is not specific to a
dogfighting game. Anything with a handheld board and a host on the same wifi
faces the same four problems — find the network, find the host, survive the
network being bad, and say what is happening when it does not work.

## The shape

```
board                                   host
  scan  ──> join strongest known SSID
  ask   ──> UDP broadcast :41234 ──────> answers with its TCP port
  talk  ──> TCP :41235  (control, reliable: banners, events, HUD)
  send  ──> UDP :41236  (samples, lossy on purpose)
```

Two channels rather than one, because they want opposite things. Control traffic
must arrive; sample traffic must be *recent*. Putting 50 Hz samples on the TCP
socket meant one lost packet held up every sample behind it — head-of-line
blocking showed up as the stick freezing and then catching up in a lurch. Moving
samples to their own datagram took the median from 59 ms to 21 ms and p99 from
354 ms to 157 ms on the same network, same board, same minute.

Nothing is stored that can go stale in a way the board cannot recover from. Up
to four networks live in NVS; the host's address is remembered but always
re-checked, and the board falls back to asking when it stops answering. USB
remains attached underneath: whenever the socket breaks, output goes back to the
cable, so the board is never mute.

## Finding the network

Credentials are given once over USB and kept in NVS across reflashes:

```
printf '!wifi ssid=NETWORK pass=SECRET\n' > /dev/cu.usbmodem101
```

At power-up the board scans and joins the strongest SSID it recognises. Four
slots means a board carried between a bench and a venue needs nothing said to it
in either place.

**Scanning is how it chooses, not how it connects.** This distinction is the
whole reliability story. When the scan fails or matches nothing, the board falls
through to trying its stored networks directly, one per retry. A scan is an
optimisation for the case where several known networks are in range; treating it
as a precondition turns every scan failure into a board that never joins
anything.

## Finding the host

The board broadcasts a short ask; the host answers with the port to come back
on. The address that answered is remembered, so subsequent boots go straight
there and only fall back to broadcasting when it stops replying, alternating
between the two so a stale address can never trap it.

The remembered address earns its keep on networks that hand out more than one
subnet — common on guest wifi. Our board sat on `192.168.4.x` while the host was
on `192.168.5.x`, and a broadcast to `255.255.255.255` never crossed. Knowing the
host's address means also knowing the right directed broadcast (`192.168.5.255`)
to ask on.

## The four bugs, and what they had in common

Every one of these was invisible in the same way, which is why each cost a round
to find. **The board failed silently, so every distinct fault presented as the
same symptom: nothing happens.**

### 1. Blocking work on the wifi event task

A blocking `esp_wifi_scan_start` plus a 20-entry `wifi_ap_record_t` array
(several KB) inside the wifi event handler overflowed the `sys_evt` stack:

```
***ERROR*** A stack overflow in task sys_evt has been detected.
```

Fix: joining gets its own task with a real stack, signalled by a semaphore.
Event handlers only ever signal. Scan results are heap-allocated.

**General rule: event handlers signal, they do not work.** The system event task
has a small stack sized for dispatch, not for whatever you want to do about the
event.

### 2. A failed scan returned false without a word

A board that could not scan looked exactly like a board that had been told no
networks at all. Two completely different faults, one symptom. This cost the most
time of anything here, and it cost it *twice* — I first misdiagnosed it as the
NVS migration having dropped the credentials, and went looking in the wrong file.

Fix: report the actual `esp_err_t`, report an empty scan distinctly, and report
at boot what the board thinks it knows:

```
#net know 2 networks
#net remember host 192.168.5.12:41235
#net joining NETWORK at -49 dBm
#net ip=192.168.4.76
#net host 192.168.5.12:41235
```

The same status goes to the board's own panel — scanning / the network it joined
/ looking for game / in game — so a controller that will not connect can be
diagnosed where it is, without carrying it back to a laptop.

### 3. Liveness measured on the wrong channel

Host side. The reaper closed a controller's socket after three seconds without a
read. That was correct when samples rode TCP. Once samples moved to UDP the TCP
socket carried an occasional banner and nothing else, so a **healthy** board was
killed every few seconds; it reconnected, sent a banner, and was killed again.

Fix: liveness counts a word on *either* channel. The socket timeout became a
poll that checks a shared `last_heard`, not a verdict.

**General rule: when you split a connection into two channels, every timer that
was watching the old one needs re-pointing.** The timer keeps working — it just
starts measuring something that no longer means what it meant.

### 4. Reconnects that took 56 seconds

Two causes stacked. lwip's default `connect()` timeout is tens of seconds, so a
host that had gone away blocked the board for most of a minute. And after the
writer dropped the socket, the reader stayed parked in `recv()` with no timeout
and never noticed.

Fix: non-blocking `connect()` with a 1.5 s `select()`, and a 1 s `SO_RCVTIMEO` on
the read so it can check whether the link is still supposed to exist. 56 s → ~1 s.

## Wifi power save, which is not a bug but reads like one

Jitter of 82 ms median and 290 ms p90 with the radio otherwise idle. The cause is
modem power save parking the radio between beacons.

```c
esp_wifi_set_ps(WIFI_PS_NONE);
esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
esp_wifi_set_max_tx_power(80);
```

**These must be called after `esp_wifi_start()`.** Called before, they are
accepted and silently do nothing — the first attempt at this fix produced no
change at all, and ICMP (min 6 ms, avg 105 ms) is what proved the radio was still
parking. Afterwards: 8 / 29 / 75 ms.

## Two things that did not work, recorded so they are not tried again

**Lowering the sample rate did not fix stalls.** 200 Hz → 50 Hz with 40 ms
coalescing worked exactly as designed and changed nothing: 17 stalls in 20 s
before, 17 after. The stalls are latency in the RF path, not bandwidth. Rate
reduction is still worth having for airtime on a busy channel, but it is not a
fix for the symptom it looks like it should fix.

**One `send()` per sample in the sampling loop.** At 200 Hz this delivered 70 Hz,
because the loop blocked on the network. A queue plus a writer task at higher
priority decouples them; the sampler never waits on a socket.

## The lesson under all of them

Three of the four bugs were the same bug: **a failure path that returned without
saying anything.** The fix that mattered was not any individual patch, it was
making the board narrate — at boot, on its own screen, and on every failed
attempt, rate-limited to once per gap so the reason is not buried by retries.

After two failed guesses at a cause, the next move is to build the instrument,
not to try fix number three. Every hour spent guessing here would have been
saved by the boot report, which took ten minutes to write.

## Measured

| | before | after |
| --- | --- | --- |
| sample latency, median / p99 | 59 / 354 ms | 21 / 157 ms |
| stalls in 20 s | 17 | 8 |
| jitter, median / p90 | 82 / 290 ms | 29 / 75 ms |
| reconnect after host restart | 56 s | ~1 s |
| 45 s soak | rejoined every few seconds | 1 join, 0 reaps, 0 dropped |
