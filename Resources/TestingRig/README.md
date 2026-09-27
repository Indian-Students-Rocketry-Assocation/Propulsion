# Testing Rig

Static-fire thrust stand built by the Electronics team. A load cell under the motor is read by an Arduino and radioed to a ground station, so nobody has to be near the motor to log data.

```
load cell → HX711 → Arduino (TransmitterCode) → NRF24L01 ))) NRF24L01 → Arduino (ReceiverCode) → USB serial → T0NN.csv
```

## Wiring

| | Transmitter | Receiver |
|---|---|---|
| Board | Arduino (ATmega328P) | Arduino Uno / Nano |
| HX711 | DOUT D5, SCK D4 | — |
| NRF24L01 | CE D9, CSN D10, SPI | CE D9, CSN D10, SPI |

NRF24L01 at **3.3 V only**, with a 10 µF + 0.1 µF capacitor across its VCC and GND.

The two sides must match: channel 108, 250 kbps, address `THRST`, and the same `TelemetryPacket` struct.

Libraries: `HX711`, `RF24` (TMRh20), `SPI`, `EEPROM`.

## Using it

1. Power the transmitter with the load cell **unloaded**. It settles for 30 s, then tares. Press `T` to tare early.
2. It samples at 10 Hz, with a 5-sample moving average.
3. The receiver prints CSV over serial at 115200 baud: `Time_ms,Raw_ADC,Force_N`. Save it as `T0NN.csv`.

Transmitter serial commands:

| Command | Does |
|---|---|
| `T` | re-tare |
| `C <value>` | set the calibration factor |
| `W` / `L` | write / load calibration and tare to/from EEPROM |
| `S` | print status |

The built-in calibration factor is `24374`. A value saved to EEPROM overrides it.
