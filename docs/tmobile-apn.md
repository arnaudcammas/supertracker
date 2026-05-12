# T-Mobile APN Notes

## APNs to try (in order)

| APN | Use case | User | Pass |
|---|---|---|---|
| `fast.t-mobile.com` | Standard T-Mobile US consumer (postpaid + prepaid) | *(blank)* | *(blank)* |
| `iot.t-mobile.com` | T-Mobile IoT plans (e.g. NB-IoT/CAT-M dev kits) | *(blank)* | *(blank)* |
| `epc.tmobile.com` | Older / legacy plans | *(blank)* | *(blank)* |

If `fast.t-mobile.com` doesn't attach in 2 minutes, try the others.

## CAT-M / NB-IoT on T-Mobile US

- **CAT-M1 (LTE-M):** broadly deployed on T-Mobile US — primary target for the SIM7000G.
- **NB-IoT:** very limited US coverage; treat as fallback only.
- T-Mobile **doesn't** allow CAT-M devices on consumer voice plans in all
  cases — if a consumer SIM refuses to attach in CAT-M mode but works on a
  phone, the SIM may be locked to LTE Cat-1+ only. Workarounds:
  1. Move the SIM into a phone briefly to register it on the network, then
     back into the modem.
  2. Get a T-Mobile IoT data SIM (e.g. via Hologram, Soracom, or T-Mobile DevEdge).

## Forcing radio mode in firmware

```
AT+CNMP=38         ; LTE only
AT+CMNB=1          ; CAT-M
;  CMNB values: 1 = CAT-M, 2 = NB-IoT, 3 = both
```

If attach fails, try `AT+CMNB=3` to let the modem pick.

## Verifying attach

```
AT+CSQ              -> +CSQ: 15,99   (>=10 is fine, 99 = unknown)
AT+CEREG?           -> +CEREG: 0,1   or  0,5  (1 = home, 5 = roaming)
AT+COPS?            -> "T-Mobile" or "310260"
AT+CGNAPN           -> network-suggested APN (cross-check)
AT+CNACT=0,1        -> activate PDP, IP returned
```
