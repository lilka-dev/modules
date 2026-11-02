# 003 LILKA MOD LORA

[Українська](README.uk.md) | **English** · [All modules](../../README.md)

| Low power (E22-900M22S) | High power (E22-900M30S) |
|---|---|
| <img src="images/lora_low_power.png" width="320"> | <img src="images/lora_high_power.png" width="320"> |

A sub-GHz LoRa / FSK radio for Lilka v2, built on an Ebyte module with a Semtech SX1262 inside
(850–930 MHz) and an SMA antenna connector.

| Revision | Module | Status |
|---|---|---|
| REV01 | E22-900M22S (up to 22 dBm) | Tested with [`main.cpp`](main.cpp) |
| REV02 low power | E22-900M22S | Not tested yet |
| REV02 high power | E22-900M30S (up to 30 dBm), with USB-C and an AMS1117 regulator on board | Not tested yet |

## Pinout (REV01)

| Header pin | Lilka GPIO | Signal | On the board |
|---|---|---|---|
| 1, 12 | GND | GND | |
| 2, 11 | 3V3 | VCC | |
| 3 | 44 | SCK | |
| 4 | 43 | MISO | 390 Ω series resistor |
| 5 | 48 | MOSI | 390 Ω series resistor |
| 6 | 47 | NSS | 10k pull-up |
| 7 | 21 | RXEN | 10k pull-down |
| 8 | 14 | TXEN | 10k pull-down |
| 9 | 13 | NRST | 10k pull-up |
| 10 | 12 | BUSY | |

## Notes for software

- `DIO1` is not connected, so read the IRQ status over SPI (`GetIrqStatus`) instead of waiting for
  an interrupt.
- The module has a TCXO. Call `SetDIO3AsTcxoCtrl(1.8 V)` before calibrating, otherwise you get
  `XOSC_START_ERR`.
- The RF switch is driven by the MCU: set `RXEN = 1, TXEN = 0` to receive and `RXEN = 0, TXEN = 1`
  to transmit. `DIO2` can drive TXEN through R7, but R7 is not fitted.
- When using RadioLib, pass `RADIOLIB_NC` for DIO1 and poll the IRQ status yourself.

## Example

[`main.cpp`](main.cpp) runs a self-test:
- SPI link, reset/BUSY, and buffer write/read-back
- TCXO calibration
- RX noise floor at 868.1 MHz
- a 0 dBm TX, checked for time-on-air

It then sends a beacon every 10 s (868.1 MHz, SF7, BW125) and prints every packet it receives.

```bash
pio run -e lora_sx1262 -t upload && pio device monitor
```

Result on a REV01 board: all checks passed, noise floor −108.5 dBm, time-on-air 52 ms.
