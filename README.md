# Mario Kart RC

Mario-Kart-inspiriertes RC-Rennsystem für 8 Fahrzeuge im Außeneinsatz. Die Autos fahren mit ~30 km/h und erkennen Items, Kollisionen und Rundenzählung automatisch.

## Repo-Struktur

| Ordner | Inhalt |
|---|---|
| `fb-firmware/` | Fernbedienung PoC – D1 Mini (ESP8266), PlatformIO |
| `car-firmware/` | Fahrzeug-Firmware – ESP32-C3 (kommt später) |
| `receiver-test/` | Gegenstelle – D1 Mini empfängt ESP-NOW, Serial-Output |
| `docs/` | Schaltpläne, KiCad-Dateien |

## Gesamtarchitektur

- **Wireless:** ESP-NOW
- **Kabelgebunden:** CAN-Bus (STM32F103 + TJA1050)
- **Basisstation:** Raspberry Pi 5 + USB-CAN-Adapter
- **Fahrzeugbasis:** Carrera 2013 RC-Autos (umgebaut)
- **Fahrzeug-MCU:** ESP32-C3
- **PoC-Plattform:** D1 Mini (ESP8266)

## Fernbedienung PoC

### Hardware

| Komponente | Details |
|---|---|
| MCU | D1 Mini (ESP8266) |
| Joysticks | 2x PS2-Analog-Stick (5V VCC) |
| ADC | ADS1115, I2C 0x48 |
| Display | SSD1306 OLED, I2C 0x3C |
| Buttons | 4x Taster |
| Rumble | Coin-ERM-Vibrationsmotor 10mm (1034-Typ) |
| Rumble-Treiber | 2N2222 NPN, 1kΩ Basis, 1N4001 Freilaufdiode |
| ADC-Teiler | 2x 4,7kΩ pro Joystick-Achse (5V → 2,5V) |
| Akku | LiPo + LiPo-Ladeshield |
| Gehäuse | PS4-Stil, Resin-Druck |

### Pinbelegung D1 Mini

| Pin | Funktion |
|---|---|
| D1 | SCL (I2C) |
| D2 | SDA (I2C) |
| D4 | Rumble PWM |
| D5 | Button 1 |
| D6 | Button 2 |
| D7 | Button 3 |
| D8 | Button 4 |
| 5V | Joystick VCC |
| 3.3V | ADS1115, OLED, Rumble-Schaltung |
| GND | Alle gemeinsam |

### Joystick Spannungsteiler
PS2-Joysticks liefern 5V, ADS1115 verträgt max 3,6V → 2x 4,7kΩ pro Achse ergibt 2,5V.

### Rumble-Schaltung
2N2222 NPN als Schalter, PWM über D4, 1N4001 Freilaufdiode gegen Spannungsspitzen.

## Status

| Schritt | Status |
|---|---|
| Architektur & Komponentenwahl | ✅ |
| Breadboard-Aufbau FB | 🔧 in Arbeit |
| FB-Firmware (Joysticks, Buttons, Rumble, OLED, ESP-NOW) | 🔧 in Arbeit |
| Gegenstelle Receiver-Test | ⏳ |
| Daten-Struct FB ↔ Auto definieren | ⏳ |
| OLED Statusanzeige (Rückmeldung vom Auto) | ⏳ |
| IMU L/R-Kollisionserkennung im Auto | ⏳ |
| Finales PCB in KiCad | ⏳ |
