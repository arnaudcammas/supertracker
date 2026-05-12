# Build Plan

## Phase 0 — Confirm

- [x] Identify board: LilyGo T-SIM7080G-S3 (ESP32-S3 + SIM7080G + Li-Ion)
- [x] Confirmed enumerates as `/dev/ttyACM0` (native USB, no CP210x/CH340 needed)
- [ ] Confirm T-Mobile SIM type (consumer vs IoT) → APN
- [ ] Confirm VPS hostname for Traccar
- [ ] Confirm reporting cadence

## Phase 1 — Toolchain

```bash
sudo apt install -y python3-pip python3-venv pipx
pipx ensurepath
pipx install platformio
sudo usermod -aG dialout "$USER"   # then log out / back in
```

Sanity:
```bash
pio --version
ls -l /dev/ttyACM0
```

## Phase 2 — Firmware

1. `cd ~/Desktop/lilygo-tracker/firmware`
2. `pio run` (downloads esp32-s3 platform + libs)
3. Edit `src/main.cpp`:
   - `APN`              — `fast.t-mobile.com` (consumer) or `iot.t-mobile.com` (IoT)
   - `TRACCAR_HOST`     — VPS hostname
   - `TRACCAR_PORT`     — 5055 (OsmAnd)
   - `DEVICE_ID`        — anything stable, e.g. `tracker-01`
   - `SLEEP_SECONDS`    — default 300
4. `pio run -t upload`
5. `pio device monitor -b 115200`

Expected boot log:
```
[boot] modem on
[modem] CSQ ok / CREG=1 / network: T-Mobile
[gprs] connect ok ip=...
[gnss] fix lat=.. lon=.. sats=..
[http] GET /?id=...&lat=... → 200
[sleep] 300s
```

## Phase 3 — Server (VPS)

```bash
scp -r server/ user@vps:/opt/traccar/
ssh user@vps
cd /opt/traccar
docker compose up -d
docker compose logs -f traccar
```

- Open `https://<vps>:8082` → log in `admin/admin` → change password.
- **Add device** with the same `DEVICE_ID` you set in firmware.
- Open OsmAnd port: ensure firewall allows 5055/tcp.

## Phase 4 — Bring-up

1. Insert T-Mobile SIM (with antenna attached — never power up SIM7080G without LTE antenna).
2. Insert charged 18650.
3. USB plug for serial monitor only (not required for run).
4. Watch monitor through one full cycle: attach → fix → POST → sleep.
5. Verify position appears in Traccar map.

## Phase 5 — Tuning

- Add motion-aware cadence: keep modem off when stationary >10 min, wake every 30 min just to ping.
- Battery: cut-off at 3.4 V → 1 hr sleep; <3.2 V → hibernate forever (only USB wakes).
- OTA: optional, low priority — only matters if device is far away.

## Risks / known gotchas

- **CAT-M coverage:** T-Mobile US runs CAT-M1 broadly but not everywhere. SIM7080G can fall back to NB-IoT, but T-Mobile NB-IoT is limited. If attach fails, force CAT-M only and check tower coverage.
- **Battery brownout during modem TX:** SIM7080G can pull 2 A peaks. Don't run on USB-only without a battery — the cap on the board is not enough; modem will reset mid-attach. Always have the 18650 in.
- **Antenna polarity:** GNSS and LTE antennas use different IPEX pads — don't swap.
- **APN auth:** T-Mobile typically needs no user/pass. Leave blank.
- **Traccar OsmAnd HTTP vs HTTPS:** OsmAnd port is plain HTTP by default. Either terminate TLS at Caddy/Nginx in front, or accept plain HTTP from the device (data is just lat/lon; consider tradeoff).
