# LilyGo T-SIM7000G GPS Tracker

End-to-end GPS tracker built on the **LilyGo T-SIM7000G** (ESP32-WROVER + SIM7000G CAT-M1 / NB-IoT modem with GNSS, Li-Ion powered), reporting to a self-hosted Traccar dashboard over cellular.

> **Note:** the original `PLAN.md` targets the T-SIM7080G-S3. The board that
> ended up in this build is the older T-SIM7000G (ESP32-WROVER, no AXP PMU).
> The two share a similar carrier layout but have different MCU, modem chip,
> pin map, and AT command quirks. See `STATUS.md` for what changed.

## Hardware

| | |
|---|---|
| Board | LilyGo T-SIM7000G (ESP32-WROVER variant) |
| SoC | ESP32 (dual-core Tensilica LX6, 240 MHz, 8 MB PSRAM, 4 MB flash) |
| Modem | SIM7000G (CAT-M1 + NB-IoT + 2G fallback + GNSS) |
| Modem FW | `1529B10SIM7000G` — note: B10 has a documented HTTPS bug, see `STATUS.md` |
| USB bridge | CH9102F → `/dev/ttyACM0` |
| Power | USB-C + 18650 Li-Ion via JST |

## Architecture

```
[LilyGo T-SIM7000G]
   │ cellular (T-Mobile CAT-M1, APN fast.t-mobile.com)
   ▼
[bore.pub:<port>]                    ← public TCP tunnel, plain HTTP
   │
   ▼
[Pi: `bore local 5055 --to bore.pub` as systemd service]
   │ localhost
   ▼
[Traccar OsmAnd listener :5055]
   ▼
[Traccar UI :8082]                   ← Tailscale Funnel exposes this for browsing
```

## Why bore and not Tailscale Funnel for tracker ingress?

Funnel only does HTTPS. The SIM7000G's `1529B10` firmware has a documented
bug in its `+SH*` HTTPS application — `AT+SHCONN` returns "operation not
allowed" with no software workaround. Plain HTTP via `+HTTPACTION` (with
`+SAPBR` bearer) works fine. So:

- **Tracker → bore.pub:`<port>` → Pi:5055**  : plain HTTP (modem-compatible)
- **Browser → `traccar.<tailnet>.ts.net` → Pi:8082** : HTTPS via Tailscale Funnel (browser-compatible)

Two different ingress paths for two different clients.

## Layout

```
.
├── README.md           ← you are here
├── STATUS.md           ← what works / what doesn't, with evidence
├── PLAN.md             ← original (7080G-S3) plan, kept for reference
├── firmware/           ← PlatformIO project for the ESP32
│   ├── platformio.ini
│   └── src/
│       ├── main.cpp    ← tracker firmware
│       └── isrg_der.h  ← Let's Encrypt root CA (unused in current path, kept for future)
├── server/             ← Pi-side Traccar config
│   ├── docker-compose.yml
│   └── traccar.xml     ← with server-side GPS-jump filters enabled
├── pi-image/           ← cloud-init for the Pi
│   ├── user-data       ← installs Docker, Traccar, bore, watchdog
│   ├── network-config  ← Wi-Fi creds (placeholders — fill in your own)
│   └── meta-data
└── docs/
    ├── pinmap.md       ← T-SIM7000G pin map
    ├── flashing.md     ← uploading the firmware
    └── tmobile-apn.md  ← T-Mobile APN reference
```

## Quickstart

Detailed setup in each subdirectory. High-level:

1. **Pi:** flash an SD card with Ubuntu Server + `pi-image/user-data` cloud-init. Pi auto-installs Docker, Traccar, bore, and watchdog on first boot.
2. **Tunnel:** SSH into the Pi after boot. `sudo journalctl -u bore | grep "listening at"` shows the assigned public port (e.g. `bore.pub:51452`).
3. **Firmware:** edit `firmware/src/main.cpp` — set `TRACCAR_HOST` (use `dig +short bore.pub` for the IP, more reliable than DNS on the modem) and `TRACCAR_PORT` (the port from step 2). `pio run -t upload`.
4. **Verify:** open the Traccar UI; `tracker-01` should appear after the first 5-min cycle.

## Status

Tracker is running, posting every 5 minutes when stationary and every 30 seconds during motion (burst mode), with GPS quality filtering on the device and Traccar server-side filters for crazy-distance protection. See `STATUS.md` for the full breakdown.
