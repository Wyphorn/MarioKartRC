# Mario Kart RC

Mario-Kart-inspiriertes RC-Rennsystem für 8 Fahrzeuge im Außeneinsatz. Die Autos fahren mit ~30 km/h und erkennen Items, Kollisionen und Rundenzählung automatisch.

---

## Repo-Struktur

| Ordner | Inhalt |
|---|---|
| `fb-firmware/` | Fernbedienung – D1 Mini (ESP8266), PlatformIO |
| `car-firmware/` | Fahrzeug-Firmware – ESP32-C3 (noch nicht begonnen) |
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

## Betriebs-Modi

Die Fernbedienung unterstützt zwei Modi, wählbar über den 3-Position-Schalter:

**Direct Mode** – FB kommuniziert direkt mit dem zugeordneten Auto. Kein Basisstation nötig, ideal zum Einzelfahren und Testen.

**Game Mode** – FB kommuniziert mit der Basisstation. Diese übernimmt die Spiellogik (Items, Punkte, Kollisionen, Rundenzählung) und leitet Fahrbefehle ans Auto weiter.

---

## Fernbedienung

### Hardware

| Komponente | Details |
|---|---|
| MCU | D1 Mini (ESP8266) |
| Joysticks | 2x PS2-Analog-Stick (5V VCC) |
| ADC | ADS1115, I2C 0x48 |
| Display | SSD1306 OLED 128×64, I2C 0x3C (Breadboard); geplant: 2.42" SSD1309 |
| Buttons | 4x Taster |
| Status-LED | WS2812B RGB |
| Rumble | Coin-ERM-Vibrationsmotor 10mm (1034-Typ), 5V |
| Rumble-Treiber | 2N2222 NPN, 1kΩ Basis, 1N4001 Freilaufdiode |
| ADC-Teiler | 2x 4,7kΩ pro Joystick-Achse (5V → ~2,5V am ADS) |
| Akku | LiPo + Battery Shield v1.2.0 |
| Power/Modus | 3-Position Switch (SPDT Center-Off) |
| Puffer | 220µF Elko auf 5V-Rail |

### Pinbelegung D1 Mini

| Pin | Funktion |
|---|---|
| D0 | Modus-Erkennung (Game=GND / Direct=3.3V) |
| D1 | SCL (I2C) |
| D2 | SDA (I2C) |
| D3 | Button 4 |
| D4 | WS2812B Datenleitung |
| D5 | Button 1 |
| D6 | Button 2 |
| D7 | Button 3 |
| D8 | Rumble PWM (via 2N2222) |
| 5V | Joystick VCC, Rumble-Motor |
| 3.3V | ADS1115, OLED, WS2812B |
| GND | Alle gemeinsam |

### Power/Modus-Schalter

```
LiPo(+) → Schalter-Mitte (gemeinsam)
           ├── Links  → Shield(+)  UND  D0 → GND    = Game Mode
           └── Rechts → Shield(+)  UND  D0 → 3.3V   = Direct Mode
           Mitte = Aus (Stromkreis offen)
```

### Joystick Spannungsteiler

PS2-Joysticks liefern bis 5V, ADS1115 verträgt max 3,6V.
Pro Achse: `Signal --[4,7kΩ]--+--[4,7kΩ]-- GND`, Mitte → ADS-Eingang.

### Rumble-Schaltung

```
5V -- Motor -- 1N4001(Kathode zu 5V) -- Kollektor(2N2222) -- Emitter -- GND
D8 -- 1kΩ -- Basis(2N2222)
```

Motor rot→5V | blau→Kollektor. 220µF Elko auf 5V-Rail gegen Spannungseinbrüche beim Anlaufen.

> **Hinweis:** D4 (GPIO2) ist UART1-TX auf dem ESP8266 – nicht für PWM verwenden. WS2812B-Datenpuls funktioniert trotzdem.
> **Hinweis:** D8 (GPIO15) hat internen Pull-Down → Transistor bleibt beim Booten sicher aus.

### WS2812B Status-LED

| Farbe | Bedeutung |
|---|---|
| 🟢 Grün | Direct Mode – verbunden mit Auto |
| 🟠 Orange pulsierend | Direct Mode – sucht Auto |
| 🔵 Blau | Game Mode – verbunden mit Basisstation |
| 🔵 Blau pulsierend | Game Mode – sucht Basisstation |
| 🟡 Gelb pulsierend | Kalibrierung ausgelöst (B2+B3 3s halten) |
| 🟡 Gelb solid | Kalibrierung läuft – FB gesperrt (mind. 3s) |

### Einstellungsmenü

**B2 + B3 gleichzeitig 3 Sekunden halten** öffnet das Menü. Navigation per Joystick (hoch/runter), B1 bestätigen, B4 zurück.

| Menüpunkt | Funktion |
|---|---|
| Offset-Kalibrierung | Nullpunkt setzen (Joysticks loslassen, läuft automatisch) |
| Min/Max-Kalibrierung | 8 Schritte geführt: jeden Stick in jede Richtung durchdrücken, B1 bestätigen |
| Servo-Trim | ±10 Stufen, EEPROM-persistent |
| Max. Speed | 1–10 Stufen (10 = 100 %), temporär |
| Sprache | Deutsch / English |

Beim ersten Start wird automatisch eine Offset-Kalibrierung durchgeführt (Joysticks in Ruhestellung lassen). Ausgabe: –100 … +100 pro Achse, Dead Zone ±5.

### OLED-Display

```
LX: -12  LY:   3
RX:   0  RY:  -5
B1:0 B2:0 B3:0 B4:0
────────────────────
[Rückmeldungen vom   ]
[Auto/Basisstation   ]
[erscheinen hier     ]
[(4 Zeilen, scrollend)]
```

### FB-Firmware bauen & flashen

```bash
cd fb-firmware

# Port-Berechtigung (einmalig pro Session bis uucp-Gruppe gesetzt)
sudo chmod 666 /dev/ttyUSB0

~/.platformio/penv/bin/pio run -t upload
~/.platformio/penv/bin/pio device monitor
```

> Dauerhaft ohne chmod: `sudo usermod -aG uucp $USER` (danach ausloggen)

---

## Fahrzeug (ESP32-C3)

Noch nicht begonnen. Geplante Komponenten:

| Komponente | Details |
|---|---|
| MCU | ESP32-C3 |
| Sound | MAX98357A (I2S) |
| IMU | LSM6DSO (I2C, Kollisionserkennung) |
| Wireless | ESP-NOW |

---

## Projektstatus

| Schritt | Status |
|---|---|
| Architektur & Komponentenwahl | ✅ |
| Breadboard-Aufbau FB | ✅ |
| FB-Firmware: Joysticks, Buttons, Rumble, OLED | ✅ |
| FB-Firmware: WS2812B Status-LED | ✅ |
| FB-Firmware: Joystick-Kalibrierung + EEPROM | ✅ |
| FB-Firmware: Einstellungsmenü (Kalibrierung, Trim, Speed, Sprache) | ✅ |
| FB-Firmware: Peripherie-Guards (bootet ohne Hardware) | ✅ |
| FB-Firmware: Einstellungsmenü-Bug (STATE_READY→MENU) | ✅ |
| Display wechseln: 2.42" SSD1309 128×64 I2C | 🚚 bestellt |
| Race-Display: Sprite, Position, Runde, Item | ⏳ |
| 3-Position Modus-Schalter (Hardware) | ⏳ |
| ESP-NOW implementieren (FB) | ⏳ |
| Daten-Struct FB ↔ Auto definieren | ⏳ |
| Button-Belegung Zusatzfunktionen festlegen | ⏳ |
| Pairing-Prozess definieren | ⏳ |
| Receiver-Test Gegenstelle (zweiter D1 Mini) | ⏳ |
| OLED: Live-Rückmeldung vom Fahrzeug | ⏳ |
| Gehäuse anpassen (Schalter, LED, Buttons) | ⏳ |
| Fahrzeug-Firmware (ESP32-C3) | ⏳ |
| IMU Kollisionserkennung | ⏳ |
| Basisstation-Software (Raspberry Pi) | ⏳ |
| Finales PCB in KiCad | ⏳ |

---

## Verworfene Optionen

| Option | Grund |
|---|---|
| UWB für Positionierung | Zu komplex |
| DFPlayer Mini | Sound kommt direkt vom Auto via MAX98357A |
| MOSFET IRFZ44N/RFP30N06LE für Rumble | Nicht Logic-Level / zu groß |
| Piezo-Buzzer in FB | Kein Sound in der FB geplant |
| ESP32 für Fernbedienung | D1 Mini aus Lager, ausreichend |
| D4 für Rumble | UART1-Interferenz auf GPIO2 |
| D8 für Button 4 | GPIO15 Pull-Down, INPUT_PULLUP funktioniert nicht |
| 3.3V für Rumble-Motor | Zu schwach, jetzt 5V |
| Farb-TFT (ILI9341/ST7789) für Race-Display | Hardware-SPI-Pins durch Buttons belegt; stattdessen größeres OLED gleicher Auflösung |
