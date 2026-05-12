# T-SIM7000G Pin Map

Source: official LilyGo / SIMCom docs for the
[LilyGo SIM7000G ESP32 version](https://github.com/Xinyuan-LilyGO/LilyGo-Modem-Series/blob/main/docs/en/esp32/sim7000-esp32/REAMDE.MD)
(also matches the older [Xinyuan-LilyGO/LilyGO-T-SIM7000G](https://github.com/Xinyuan-LilyGO/LilyGO-T-SIM7000G) historical README, version `20200415`).

| Function | ESP32 GPIO | Notes |
|---|---|---|
| Modem UART RX (ESP receives) | 26 | from SIM7000G TX |
| Modem UART TX (ESP sends) | 27 | to SIM7000G RX |
| Modem PWRKEY | 4 | active-low pulse to toggle modem power |
| Modem DTR | 25 | hold LOW to keep modem awake |
| Modem RESET | 5 | active-high reset (unused in our firmware) |
| BOARD_POWERON (modem rail enable + LED) | 12 | **must be HIGH** for modem to receive power |
| Battery ADC | 35 | 1:2 divider → multiply by 2 for VBAT |
| Solar ADC | 36 | 1:2 divider |
| SD MISO / MOSI / SCK / CS | 2 / 15 / 14 / 13 | unused in this build |
| (modem-internal GPIO) GPS antenna LDO | 48 | toggle via `AT+CGPIO=0,48,1,1` — must be enabled for GPS fix |

## Hardware quirks worth remembering

### Battery ADC reads garbage when USB-C is connected

Per LilyGo's official `ReadBattery.ino` example:
> *"When connected to USB, the battery voltage data read is not the real battery voltage… please disconnect the USB-C."*

GPIO 35 reads ~140 mV (noise floor) under USB power because the modem and
ADC reference rails fight. Real readings (3.3–4.2 V) only appear when the
device is running on the JST battery alone.

### `BOARD_POWERON` (GPIO 12) is the modem power switch

If you don't drive GPIO 12 HIGH at boot, the modem stays off. This pin
also drives the on-board red LED, so confusingly it's sometimes labelled
"LED" in older code samples — but it's actually the modem rail enable.

### GPS antenna needs explicit power-up via AT command

`AT+CGNSPWR=1` enables the GPS engine but **does not** power the active
antenna's LDO. Without `AT+CGPIO=0,48,1,1` the external antenna gets no
bias and the modem will sit forever in "searching" with zero satellites
visible. TinyGSM's `enableGPS()` wrapper does not issue the LDO command —
we send it directly.

### Slide switch behavior

The slide switch only controls battery power flow. When USB is connected
the switch position doesn't matter (board runs on USB regardless). When
USB is disconnected the switch must be ON for the battery to feed the
rails. A common "device dies when I unplug USB" symptom is the switch
being left OFF.

### Red LED diagnostic

The red LED near the slide switch is informative:
- **Solid ON** = battery connected and charging from USB
- **Blinking** = USB connected but **no battery detected** (JST loose, battery
  reverse-polarized, or BMS in cutoff)
- **OFF** = no charging activity (running on battery only, or no power)

## Antennas

- **GPS:** dedicated u.FL/IPEX pad near the GNSS chip — must be an active
  (powered) GPS antenna. Passive won't work; the antenna LDO above feeds it.
- **LTE:** separate u.FL pad near the SIM7000G — passive 4G/LTE antenna.
- **Don't swap them.** Powering up the modem with no LTE antenna can damage
  the PA over time.

## Power

- USB-C charges the 18650 *and* powers the rails. SIM7000G transmit peaks
  (~2 A burst) can drop the rail on USB-only — keep the 18650 installed.
- Charge IC: CN3065 (~500 mA solar/USB charge management).
- VBAT divider: 1:2 → `Vbat = 2 × Vadc`. ADC is 12-bit on a 3.3 V reference.
