# Mario Kart RC

Mario-Kart-inspiriertes RC-Rennsystem für 8 Fahrzeuge im Außeneinsatz. Die Autos fahren mit ~30 km/h und erkennen Items, Kollisionen und Rundenzählung automatisch.

---

## Repo-Struktur

| Ordner | Inhalt |
|---|---|
| `fb-firmware/` | Fernbedienung – D1 Mini (ESP8266), PlatformIO |
| `car-firmware/` | Fahrzeug-Firmware – ESP32-C3 (kommt später) |
| `receiver-test/` | Gegenstelle – D1 Mini empfängt ESP-NOW, Serial-Output |
| `docs/` | Schaltpläne, KiCad-Dateien |

---

## Gesamtarchitektur

| Bereich | Technologie |
|---|---|
| Wireless | ESP-NOW |
| Kabelgebunden | CAN-Bus (STM32F103 + TJA1050) |
| Basisstation | Raspberry Pi 5 + USB-CAN-Adapter |
| Fahrzeugbasis | Carrera 2013 RC-Autos (umgebaut) |
| Fahrzeug-MCU | ESP32-C3 |
| Fernbedienung | D1 Mini (ESP8266) |

---

## Fernbedienung

### Hardware

| Komponente | Details |
|---|---|
| MCU | D1 Mini (ESP8266) |
| Joysticks | 2x PS2-Analog-Stick (5V VCC) |
| ADC | ADS1115, I2C 0x48 |
| Display | SSD1306 OLED 128×64, I2C 0x3C |
| Buttons | 4x Taster |
| Status-LED | WS2812B RGB-LED |
| Rumble | Coin-ERM-Vibrationsmotor 10mm (1034-Typ), 5V |
| Rumble-Treiber | 2N2222 NPN, 1kΩ Basis, 1N4001 Freilaufdiode |
| ADC-Teiler | 2x 4,7kΩ pro Joystick-Achse (5V → ~2,5V am ADS) |
| Akku | LiPo + LiPo-Ladeshield |

### Pinbelegung D1 Mini

| Pin | Funktion |
|---|---|
| D1 | SCL (I2C) |
| D2 | SDA (I2C) |
| D3 | Button 4 |
| D4 | WS2812B Datenleitung |
| D5 | Button 1 |
| D6 | Button 2 |
| D7 | Button 3 |
| D8 | Rumble (PWM via 2N2222) |
| 5V | Joystick VCC, Rumble-Motor |
| 3.3V | ADS1115, OLED, WS2812B |
| GND | Alle gemeinsam |

### Joystick Spannungsteiler

PS2-Joysticks liefern bis 5V, ADS1115 verträgt max 3,6V.  
Pro Achse: `Signal --[4,7kΩ]--+--[4,7kΩ]-- GND`, Mitte → ADS-Eingang (~2,5V max).

### Rumble-Schaltung

```
5V -- Motor -- 1N4001 (Kathode zu 5V) -- Kollektor (2N2222) -- Emitter -- GND
D8 -- 1kΩ -- Basis (2N2222)
```

Motor-rot → 5V | Motor-blau → Kollektor. 3,3V reicht nicht für ausreichend Vibration.

> **Hinweis:** D4 (GPIO2) ist auf dem ESP8266 gleichzeitig UART1-TX und Boot-Strapping-Pin. Nicht für PWM verwenden – das erzeugt Interferenzen auf Serial. Für den WS2812B-Datenpuls ist GPIO2 unproblematisch.  
> **Hinweis:** D8 (GPIO15) hat einen internen Pull-Down und muss beim Booten LOW sein – ideal für den Rumble-Transistor, der so beim Start ausbleibt.

### WS2812B Status-LED

| Farbe | Bedeutung |
|---|---|
| 🟢 Grün | Betriebsbereit |
| 🟡 Gelb pulsierend | Kalibrierung wird ausgelöst (3s) |
| 🟡 Gelb solid | Kalibrierung läuft (FB gesperrt) |
| weitere | werden mit dem Spielsystem definiert |

Kalibrierung auslösen: **B1 + B2 gleichzeitig 3 Sekunden gehalten**.

### Joystick-Kalibrierung

Beim ersten Start kalibriert die FB automatisch (Joysticks müssen losgelassen sein).  
Die Mittelpunkt-Werte werden im EEPROM gespeichert und überleben Neustarts.  
Ausgabe gemappte Werte: –100 … +100 pro Achse.

### OLED-Display

```
LX: -12  LY:   3
RX:   0  RY:  -5
B1:0 B2:0 B3:0 B4:0
────────────────────
[Empfangene Serial-  ]
[Daten erscheinen    ]
[hier (4 Zeilen,     ]
[scrollend)          ]
```

Der untere Bereich ist für Rückmeldungen vom Fahrzeug via ESP-NOW vorgesehen (aktuell: Serial-Eingabe zum Testen).

### FB-Firmware bauen & flashen

```bash
cd fb-firmware
# Port-Berechtigung (einmalig pro Session, bis uucp-Gruppe gesetzt ist)
sudo chmod 666 /dev/ttyUSB0

~/.platformio/penv/bin/pio run -t upload
~/.platformio/penv/bin/pio device monitor
```

> Dauerhaft ohne chmod: `sudo usermod -aG uucp $USER` (einmal ausloggen danach)

---

## Projektstatus

| Schritt | Status |
|---|---|
| Architektur & Komponentenwahl | ✅ |
| Breadboard-Aufbau FB | ✅ |
| FB-Firmware: Joysticks, Buttons, Rumble, OLED | ✅ |
| FB-Firmware: WS2812B Status-LED + Kalibrierung | ✅ |
| FB-Firmware: EEPROM-Persistenz Kalibrierung | ✅ |
| ESP-NOW senden | ⏳ |
| Daten-Struct FB ↔ Fahrzeug definieren | ⏳ |
| Gegenstelle Receiver-Test (zweiter D1 Mini) | ⏳ |
| OLED: Live-Status vom Fahrzeug | ⏳ |
| Fahrzeug-Firmware (ESP32-C3) | ⏳ |
| IMU L/R-Kollisionserkennung | ⏳ |
| Finales PCB in KiCad | ⏳ |

---

## Verworfene Optionen

| Option | Grund |
|---|---|
| UWB für Positionierung | Zu komplex |
| DFPlayer Mini | Sound kommt direkt vom Auto via MAX98357A |
| MOSFET IRFZ44N/RFP30N06LE für Rumble | Nicht Logic-Level / zu groß |
| Piezo-Buzzer in FB | Kein Sound in der FB geplant |
| ESP32 für Fernbedienung | D1 Mini aus Lager, ausreichend für FB |
