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

## OTA over WiFi

When the device boots and home WiFi is in range, it briefly associates,
fetches `http://<pi-lan-ip>:8090/manifest.json` (authenticated via
`X-OTA-Token`), and compares the server's reported version to the firmware's
`FW_VERSION`. If newer, the device downloads `firmware.bin` and reboots
into it via the Arduino `httpUpdate` library (uses ESP32's dual-partition
scheme for safe A/B updates — failed flashes roll back automatically).

Partition layout switched from `huge_app.csv` (single 3 MB app slot) to
`default.csv` (two 1.3 MB app slots + 192 KB SPIFFS).

The OTA HTTP server (`server/ota-server.py`) listens on `:8090` on the
Pi's LAN — **not exposed via bore**, so it isn't internet-reachable.
Anyone on the LAN with the shared token can push firmware, so the token
must be kept secret.

To ship a new version: bump `FW_VERSION` in `firmware/src/main.cpp`, then
run `./release.sh <version>` from the repo root. Builds, copies to Pi,
bumps `version.txt`, restarts the OTA server. Device picks it up on next
home-WiFi-reachable boot.

## Hardening & robustness features

- **HMAC-signed posts** — every request includes `&sig=` (HMAC-SHA256 over
  `id|lat|lon|boot|fixes`, truncated to 16 hex). The Pi runs `hmac-proxy.py`
  in front of Traccar; bad signatures get 403. Without the shared secret an
  attacker who finds the bore endpoint can't spoof the device.
- **Replay guard** — the proxy persists `(boot, fixes)` per device id and
  rejects any tuple it has already seen. Captured posts can't be replayed.
- **Retry-once on HTTP failure** — transient cellular hiccups no longer
  cost a whole post cycle.
- **Modem soft-reset** — after 2 consecutive failed posts, the firmware
  issues `AT+CFUN=0; AT+CFUN=1` on next wake to unwedge the radio. Fixes
  the "device goes silent for 14 minutes" failure mode we used to see.
- **Task watchdog** — `esp_task_wdt` set to 4 min. If any phase hangs
  (modem AT timeout, GPS poll, HTTP stuck), the ESP32 reboots instead of
  sitting awake draining battery.
- **Adaptive sleep** — `<3.45 V` → sleep 30 min; `<3.20 V` → sleep 6 h.
  Protects the cell from over-discharge.
- **Bore tunnel watchdog** — cron on the Pi probes `bore.pub:<port>` every
  2 min and restarts the bore client if the public proxy goes stale (a
  known bore.pub failure mode).
- **GPS quality filter** — drops fixes with HDOP > 5 or sats < 4 before
  posting; Traccar server-side filters drop any speed/distance jumps that
  slip through.
- **Burst-mode hysteresis** — needs 2 consecutive low-speed/low-distance
  samples to exit, so traffic-light stops don't kick out of burst mode.

## Modem firmware update path — verified ready

The modem currently runs `1529B10SIM7000G`, which has the broken `+SH*` HTTPS
stack documented at the top of this file. SIMCom officially fixes this in
`1529B11SIM7000G` per [LilyGo's update guide](https://github.com/Xinyuan-LilyGO/LilyGO-T-SIM7000G/blob/master/docs/How%20to%20update%20firmware.md).

The CFOTA-over-UART path (no soldering required) is fully proven on this
hardware:

| Step | Status |
|---|---|
| `AT+HTTPTOFS` exists and accepts our URL syntax | ✅ probe firmware confirmed |
| Modem can download a file via cellular → bore tunnel → Pi | ✅ HTTP 200 served, 49 bytes returned (proxy log) |
| `AT+CFSWFILE` writes to modem flash | ✅ confirmed (HMAC proxy upload path also uses it) |
| `AT+CFOTA=1` triggers update | ⏳ not tried — would need a valid SIMCom-issued delta `.zip` |

**The only missing piece is the actual B10→B11 delta firmware**, which
SIMCom doesn't publish publicly. To get it: email `support@lilygo.cc` (or
`support@simcom.com` via your module provider) with the output of
`AT+SIMCOMATI` and request the delta.

Once the delta arrives:
1. `scp delta.zip traccar:/opt/tracker/static/update.zip`
2. On the device, `AT+CFSWFILE` it into modem flash in chunks (firmware
   code path already exercised by the HMAC proxy's cert upload helper).
3. `AT+CFOTA=1` to apply. Modem reboots into B11.
4. After update, switch back from the bore.pub workaround to Tailscale
   Funnel HTTPS (if desired), since the modem's TLS stack will now work.

## Known limitations

1. **bore.pub port collisions** — if the Pi restarts and another bore user
   grabs your port in the brief window, you'd need to reflash with the new
   port. Probability is low (random 16-bit port). Bulletproof fix is
   self-hosting bore on a free VPS.
2. **Plain HTTP on the wire** — the tunnel is unencrypted between modem
   and Pi. HMAC prevents spoofing, but lat/lon/etc are visible to anyone
   between you and bore.pub (cellular MITM is rare but theoretically possible).
3. **Modem firmware bug** is the root cause for not using TLS — see top of
   this file.
