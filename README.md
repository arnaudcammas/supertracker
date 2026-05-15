# supertracker

A bike-tracker that should not exist. An ESP32 glued to a cellular modem,
yelling its GPS coordinates at the open internet, which a Raspberry Pi
in my house then catches and feeds to a self-hosted dashboard.

It runs on solar. It sleeps when nobody's riding. It survives power loss.
It does HTTP because the modem's HTTPS stack is haunted.

## What it does

```
   🚲 [LilyGo T-SIM7000G]
       │  cellular (T-Mobile, plain HTTP, HMAC-signed)
       ▼
   ☁️  bore.pub:<port>                ← free public TCP tunnel
       │
       ▼
   🥧 Raspberry Pi at home
       ├─ bore   (catches the tunnel)
       ├─ hmac-proxy.py  (checks the signature, drops impostors)
       └─ Traccar  (the actual dashboard)
       │
       ▼
   💻 Browser → traccar.<your-tailnet>.ts.net  (Tailscale Funnel, HTTPS)
```

Two ingress paths because the tracker speaks HTTP and browsers speak
HTTPS, and forcing either one to switch was more pain than running two
tunnels.

## Why HTTP and not HTTPS for the tracker?

Because the SIM7000G's `1529B10` firmware has a known bug in its HTTPS
stack — `AT+SHCONN` returns "operation not allowed" with no workaround
short of reflashing the modem itself (lol no). Plain HTTP via
`+HTTPACTION` works fine. So instead of fighting the modem, we sign
every request with HMAC-SHA256 and let the Pi verify it. Same end
result, fewer tears.

## Hardware

| | |
|---|---|
| Board | LilyGo T-SIM7000G — yes the *old* one, not the 7080G-S3 |
| Brain | ESP32-WROVER (240 MHz, 8 MB PSRAM, 4 MB flash) |
| Radio | SIM7000G — CAT-M1 / NB-IoT / 2G fallback, plus GNSS |
| USB   | CH9102F → `/dev/ttyACM0` |
| Power | 18650 Li-Ion via JST + USB-C + a tiny solar panel |

> The original `PLAN.md` was for the T-SIM7080G-S3. The board that
> showed up in the mail was the T-SIM7000G. Same family, different
> pinout, different AT commands, different bugs. `STATUS.md` has the
> autopsy.

## Repo layout

```
.
├── README.md        ← hi
├── STATUS.md        ← what works, what doesn't, with receipts
├── PLAN.md          ← original plan (wrong board, kept for history)
├── firmware/        ← PlatformIO project for the ESP32
│   └── src/
│       ├── main.cpp
│       ├── secrets.example.h   ← copy → secrets.h, fill in
│       └── secrets.h           ← gitignored, your real values live here
├── server/          ← Traccar + docker-compose for the Pi
├── pi-image/        ← cloud-init: hand the Pi an SD card and walk away
└── docs/            ← pinmap, flashing notes, T-Mobile APN cheatsheet
```

## Quickstart

You will need: the LilyGo board, a Raspberry Pi, a SIM with data, a 
shared secret, and roughly one afternoon.

**1. Bake the Pi.** Flash an SD card with Ubuntu Server + `pi-image/user-data`.
Boot it once. Cloud-init installs Docker, Traccar, bore, the HMAC proxy,
and the watchdog. Go make a sandwich.

**2. Pick a secret.**

```bash
openssl rand -hex 32
```

Same string goes in two places: the Pi (so it can verify) and the
firmware (so it can sign).

**3. Tell the Pi the secret:**

```bash
sudo mkdir -p /etc/systemd/system/hmac-proxy.service.d
sudo tee /etc/systemd/system/hmac-proxy.service.d/secret.conf > /dev/null <<EOF
[Service]
Environment="HMAC_SECRET=$YOUR_SECRET"
EOF
sudo systemctl daemon-reload && sudo systemctl restart hmac-proxy
```

**4. Find the bore port:**

```bash
sudo journalctl -u bore | grep "listening at"
# bore.pub:51452 → that's the port
```

**5. Tell the firmware the secret:**

```bash
cp firmware/src/secrets.example.h firmware/src/secrets.h
```

Edit `secrets.h`:
- `TRACCAR_HOST` — run `dig +short bore.pub`, paste the IP. (DNS on the
  modem is flaky; an IP literal Just Works.)
- `HMAC_SECRET` — paste the same string from step 2.

Then in `firmware/src/main.cpp`, set `TRACCAR_PORT` to whatever bore
gave you in step 4.

> `secrets.h` is `.gitignored`. Don't try to outsmart it — the whole
> point of HMAC is undone if the secret leaks.

**6. Flash:**

```bash
cd firmware && pio run -t upload
```

**7. Wait one cycle.** Traccar UI should show `tracker-01` within a
minute or so. If something's wrong, `journalctl -u hmac-proxy` on the
Pi will tell you (`403` = bad signature, usually a typo in the secret).

## OTA updates (no USB needed)

After the first USB flash, set `WIFI_SSID`, `WIFI_PASS`, `OTA_URL`, and
`OTA_TOKEN` in `main.cpp` and you can ship new firmware over your home
WiFi. Bump `FW_VERSION` in `main.cpp`, then:

```bash
./release.sh 2
```

That builds, copies the binary to the Pi's OTA directory, and bumps the
manifest. Next time the tracker wakes up in WiFi range, it'll grab the
new image and reboot into it. Out on the road? OTA quietly skips and
cellular carries on.

The OTA server (`server/ota-server.py`) only listens on the Pi's LAN —
**deliberately not** exposed through bore. Anyone on your LAN holding
the `X-OTA-Token` can push firmware. Keep it secret, keep it safe.

## Status

It's posting. Every 5 minutes when parked, every 30 seconds when moving
(burst mode), with GPS quality filtering on the device and sanity
filters on Traccar so it doesn't draw a 200-mile teleport every time
the GPS hiccups.

Also: if cellular drops, fixes get buffered to flash and flushed when
the connection comes back. Boot counter is persisted too, so the HMAC
replay guard survives a power loss without the device locking itself
out.

Full warts-and-all writeup in `STATUS.md`.
