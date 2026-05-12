# Flashing the T-SIM7000G

The ESP32 (WROVER) on this board talks to the host through a CH9102F USB-serial
bridge. The board enumerates as `/dev/ttyACM0` on Linux (no extra driver needed
on modern kernels — CH9102 has been mainline since ~5.5). PlatformIO handles
the auto-reset into download mode via DTR/RTS automatically; **you do not need
to hold BOOT/RESET** in normal cases.

## One-time setup

```bash
# Toolchain
sudo apt update
sudo apt install -y python3-pip python3-venv pipx
pipx ensurepath
pipx install platformio

# Serial access
sudo usermod -aG dialout "$USER"
# log out + back in (or: newgrp dialout) for the group to take effect
```

## Verify the board

```bash
lsusb | grep -i 1a86
# expect: ID 1a86:55d4 QinHeng Electronics USB Single Serial (CH9102F)
ls -l /dev/ttyACM0
```

## Build & flash

```bash
cd firmware
pio run               # compile
pio run -t upload     # flash
```

To watch logs: `pio device monitor -b 115200` (exit with `Ctrl-T Ctrl-X`).

Or use Python directly (works around pio's terminal weirdness in some shells):

```bash
python3 - <<'PY'
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=1)
s.dtr = False; time.sleep(0.1); s.dtr = True   # trigger reset
while True:
    line = s.readline()
    if line: print(line.decode('utf-8','replace').rstrip(), flush=True)
PY
```

## If upload fails

1. Confirm the cable carries data (not charge-only). Some USB-C-to-USB-C
   cables ship from phone vendors as charge-only and silently fail here.
2. Hold **BOOT**, tap **RESET**, release **BOOT** → forces ROM bootloader.
   Then re-run `pio run -t upload`.
3. If `/dev/ttyACM0` disappears mid-upload: usually a brown-out caused by
   the modem powering up during the flash. Plug in a charged 18650 to
   absorb current spikes.

## Erase flash (full reset)

```bash
pio run -t erase
```

## Note on serial logging during deep sleep

When the firmware enters deep sleep, the ESP32 stops sourcing the CDC
interface — `/dev/ttyACM0` may briefly disappear or look frozen until
next wake. That's expected, not a crash.
