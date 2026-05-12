# Status — what actually works

The original `PLAN.md` targeted Tailscale Funnel for HTTPS ingress. That path
failed because of a confirmed bug in the SIM7000G modem firmware `1529B10`:
`AT+SHCONN` returns `+CME ERROR: operation not allowed`, blocking HTTPS via
the modem's `+SH*` application. The bug is documented in [botletics/SIM7000-LTE-Shield#329](https://github.com/botletics/SIM7000-LTE-Shield/issues/329)
and supposedly fixed in firmware `1529B11` — flashing the newer firmware
requires soldering a USB lead to the modem chip's flash pads (not the
board's USB-C port, which goes to the ESP32).

## What works instead

Plain HTTP from the modem through a [bore](https://github.com/ekzhang/bore)
TCP tunnel:

```
LilyGo T-SIM7000G
  → cellular (T-Mobile CAT-M1 via fast.t-mobile.com)
  → bore.pub:<port>            (public TCP tunnel, no TLS)
  → bore client (on Pi)
  → localhost:5055             (Traccar OsmAnd port)
```

The bore client runs as a systemd service on the Pi with `--port <fixed>`
so the public port persists across restarts. A watchdog cron (every 2 min)
probes the public side and restarts bore if the proxy goes stale (a known
bore.pub failure mode).

## Firmware notes

- `+HTTPACTION=0` (plain HTTP GET) works on B10 — only `+SH*` HTTPS is broken.
- Requires `+SAPBR` bearer activated alongside `+CNACT`. Bearer-1 IP is what
  `+HTTPACTION` actually uses.
- GPS antenna LDO must be powered explicitly via `AT+CGPIO=0,48,1,1` before
  `AT+CGNSPWR=1`; without it the active antenna gets no bias and no fix
  ever comes through.
- Battery ADC on GPIO 35 reads junk when USB-C is connected (LilyGo
  hardware quirk, documented in their `ReadBattery.ino`). Real readings
  only appear in cycles where the modem's TX or ADC sampling happens
  without USB current dominating the rail.

## Firmware-side filters

- Skips fixes with HDOP > 5 or satellites < 4 (server never sees obvious
  garbage).
- Sends `hdop` and `sat` so Traccar can apply server-side filters too.
- Burst mode: while moving (speed > 3 km/h or distance > 30 m between
  fixes), samples every 30 s indefinitely until you stop or GPS goes bad
  for three consecutive samples.

## Server-side filters (Pi)

`traccar.xml` enables: `filter.invalid`, `filter.zero`, `filter.duplicate`,
`filter.future`, `filter.maxSpeed=150 km/h`, `filter.distance=10000 m`,
`filter.skipLimit=600 s`. Catches anything that the firmware filter misses.

## Known limitations

1. **bore.pub port collisions**: if the Pi restarts and someone else grabs
   your bore port in the brief window, you'd need to reflash with the new
   port. Probability is low (random 16-bit port) but non-zero. Bulletproof
   fix is self-hosting bore on a free VPS.
2. **No encryption**: the cellular → bore → Pi path is plain HTTP. Anyone
   who knows the public endpoint can spoof posts with your device ID.
   Fine for personal use, not for anything sensitive.
3. **Modem firmware bug** is the root cause — see top of this file.
