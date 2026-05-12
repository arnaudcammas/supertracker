# Flashing the T-SIM7080G-S3

The ESP32-S3 has native USB. The board appears as `/dev/ttyACM0` directly —
no CP210x/CH340 driver, no `esptool` flasher chip. PlatformIO handles the
download-mode handshake automatically; **you do not need to hold BOOT/RESET**
in normal cases.

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
lsusb | grep Espressif
# expect: ID 303a:1001 Espressif USB JTAG/serial debug unit
ls -l /dev/ttyACM0
```

## Build & flash

```bash
cd ~/Desktop/lilygo-tracker/firmware
pio run
pio run -t upload
pio device monitor -b 115200
```

Exit monitor: `Ctrl-T` then `Ctrl-X`.

## If upload fails

1. Hold **BOOT**, tap **RESET**, release **BOOT** → forces ROM bootloader.
2. Re-run `pio run -t upload`.
3. If `/dev/ttyACM0` disappears mid-upload: cable issue (some USB-C cables
   are charge-only). Try a different cable.

## Erase flash (full reset)

```bash
pio run -t erase
```

## Note on USB CDC + serial monitor

With `ARDUINO_USB_CDC_ON_BOOT=1` (set in `platformio.ini`), `Serial.print`
goes to the same USB CDC device. So `/dev/ttyACM0` is both the upload port
and the log port. When the firmware enters deep sleep, the CDC device
disappears from the host until next wake — that's expected, not a crash.
