# T-SIM7080G-S3 Pin Map

Source: LilyGo's reference repo
[Xinyuan-LilyGO/LilyGO-T-SIM7080G-S3](https://github.com/Xinyuan-LilyGO/LilyGO-T-SIM7080G-S3) (`utilities.h`).

| Function | ESP32-S3 GPIO | Notes |
|---|---|---|
| Modem PWRKEY | 41 | Active low pulse to toggle modem power |
| Modem DTR | 7 | Wake from sleep |
| Modem RI | 33 | Ring indicator (interrupt-capable) |
| Modem UART RX (ESP RX) | 4 | From SIM7080G TX |
| Modem UART TX (ESP TX) | 5 | To SIM7080G RX |
| VBAT ADC | 4 / shared | **Verify rev** — some revs use 2; the LilyGo demo defines `BOARD_BAT_ADC_PIN` — match that exactly when you flash |
| Solar/V_in ADC | 2 | Through divider |
| Status LED | 12 | Active high |
| SD MISO/MOSI/SCLK/CS | 38 / 39 / 40 / 14 | Optional onboard µSD |

**Important:** revisions differ. Before relying on `BOARD_BAT_ADC_PIN`, open
`utilities.h` from the LilyGo repo for *your* board's silkscreen rev (R1.0 vs
R2.0) and copy the values. The current `firmware/src/main.cpp` uses defaults
that match most R2 boards.

## Antennas

- **GPS:** dedicated u.FL/IPEX pad near the GNSS chip — must be active GPS antenna.
- **LTE:** separate u.FL pad near the SIM7080G — passive 4G/LTE antenna.
- Don't swap them. Powering up the modem with no LTE antenna can damage the PA.

## Power

- USB-C: charges the 18650 *and* powers the rails. SIM7080G transmit peaks
  (~2 A burst) cannot be sustained on USB alone — keep the 18650 installed.
- Charge IC: TP4054 (or similar, ~500 mA charge).
- VBAT divider: 1:2 → `Vbat = 2 * Vadc`. ADC is 12-bit on a 3.3 V reference.
