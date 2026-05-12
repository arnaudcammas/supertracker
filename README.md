# LilyGo T-SIM7080G-S3 GPS Tracker

End-to-end GPS tracker built on the LilyGo T-SIM7080G-S3 (ESP32-S3 + SIM7080G CAT-M/NB-IoT modem with GNSS, Li-Ion powered), reporting to a self-hosted Traccar dashboard over cellular.

## Hardware

| Item | Value |
|---|---|
| Board | LilyGo T-SIM7080G-S3 |
| MCU | ESP32-S3 (native USB, appears as `/dev/ttyACM0`) |
| Modem | SIM7080G (CAT-M1 + NB-IoT + GNSS) |
| Battery | Li-Ion 18650 (on-board holder + charger) |
| SIM | T-Mobile US |

## Stack

- **Firmware:** PlatformIO + Arduino-ESP32 + TinyGSM (`firmware/`)
- **Server:** Traccar in Docker on VPS (`server/`)
- **Protocol:** OsmAnd HTTP (Traccar built-in, port 5055) — simplest from modem AT/HTTP

## High-level flow

```
[T-SIM7080G-S3]
  │  power on modem → attach CAT-M (T-Mobile) → open data context
  │  poll GNSS until fix
  │  read VBAT (ADC + divider)
  │  HTTP GET https://<vps>:5055/?id=<dev-id>&lat=..&lon=..&timestamp=..&speed=..&batt=..
  │  deep sleep N minutes
  ▼
[VPS: Traccar (Docker)]
  ├─ :5055   OsmAnd ingest
  └─ :8082   Web UI / API
```

## Status

- [x] Hardware identified, USB enumerates as ESP32-S3 on `/dev/ttyACM0`
- [ ] PlatformIO toolchain installed
- [ ] Firmware scaffolded
- [ ] Pin map verified against LilyGo reference repo
- [ ] First flash + serial bring-up
- [ ] T-Mobile CAT-M attach verified
- [ ] GNSS fix verified
- [ ] Traccar receiving positions
- [ ] Deep sleep + battery profile tuned

## Key decisions

- **OsmAnd over MQTT:** SIM7080G's HTTP AT stack is reliable; OsmAnd protocol is one HTTP GET per fix; no broker to run. We can swap to MQTT later if we need bidirectional control.
- **Deep sleep, not light sleep:** ESP32-S3 native USB stays attached in light sleep and burns ~mA; deep sleep + RTC wake is what makes Li-Ion endurance reasonable.
- **APN:** T-Mobile consumer SIM → `fast.t-mobile.com`. T-Mobile IoT SIM → `iot.t-mobile.com`. We'll try `fast.t-mobile.com` first.

## Folder map

```
lilygo-tracker/
├── README.md            ← this file
├── PLAN.md              ← step-by-step build plan + checklist
├── firmware/            ← PlatformIO project
│   ├── platformio.ini
│   └── src/main.cpp
├── server/              ← VPS-side Traccar deployment
│   ├── docker-compose.yml
│   └── traccar.xml
└── docs/
    ├── pinmap.md        ← T-SIM7080G-S3 pin reference
    ├── tmobile-apn.md   ← APN notes + CAT-M caveats
    └── flashing.md      ← how to flash this board
```

## Open questions (need from user)

1. VPS hostname / IP for Traccar.
2. Whether the T-Mobile SIM is a consumer plan or an IoT plan (decides APN).
3. Reporting interval — default 5 min; faster while moving, slower when stationary?
