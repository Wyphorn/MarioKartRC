# Mario Kart RC

Mario-Kart-inspiriertes RC-Rennsystem für 8 Fahrzeuge im Außeneinsatz. Die Autos fahren mit ~30 km/h und erkennen Items, Kollisionen und Rundenzählung automatisch.

---

## Repo-Struktur

| Ordner | Inhalt |
|---|---|
| `fb-firmware/` | Fernbedienung – D1 Mini (ESP8266), PlatformIO |
| `car-firmware/` | Fahrzeug-Firmware – ESP32-C3 (noch nicht begonnen) |
| `receiver-test/` | Gegenstelle – D1 Mini empfängt ESP-NOW, Serial-Output |
| `shared/` | Gemeinsamer Code – `mk_protocol.h` (Datenstrukturen FB ↔ Auto ↔ Basis) |
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

**Direct Mode** – FB kommuniziert direkt mit dem zugeordneten Auto. Keine Basisstation nötig, ideal zum Einzelfahren und Testen.

**Game Mode** – FB kommuniziert mit der Basisstation. Diese übernimmt die Spiellogik (Items, Punkte, Kollisionen, Rundenzählung) und leitet Fahrbefehle ans Auto weiter.

---

## Kommunikationsprotokoll (`shared/mk_protocol.h`)

Alle ESP-NOW-Pakete beginnen mit einem `uint8_t type`-Feld als Discriminator.

| Struct | Richtung | Frequenz |
|---|---|---|
| `MK_ControlInput` | FB → Auto (Direct) / FB → Basis (Game) | ~50 Hz (20ms) |
| `MK_ConfigPacket` | FB → Auto / FB → Basis → Auto | Nur bei Änderung |
| `MK_GameFeedback` | Auto → FB (Direct) / Basis → FB (Game) | ~50 Hz |
| `MK_Beacon` | FB/Auto → Broadcast | Beim Pairing |
| `MK_Assign` | Basis → FB/Auto (Unicast) | Beim Pairing |
| `MK_ChannelSwitch` | Basis → Broadcast | Kanalwechsel / Rennende |
| `MK_Mapping` | Basis → FB/Auto (Unicast) | Vor Rennstart |

**MK_ControlInput** – Joystick-Steuerdaten:
- `throttle` – Vorwärts/Rückwärts, –100..100 (Standard: linker Stick Y-Achse)
- `steering` – Links/Rechts, –100..100 (Standard: rechter Stick X-Achse)
- `buttons` – Bit 0=Yellow, Bit 1=Green, Bit 2=Blue, Bit 3=Red
- `maxSpeed` – Fahrer-Präferenz 1–10

**MK_ConfigPacket** – Servo-Trim –10..10 (im Auto-EEPROM gespeichert; Basis leitet weiter ans Auto)

**MK_GameFeedback** – Spielzustand: Position, Runde, Item, Rumble-Befehl, Speed-Limit, Akku-Auto (0–5)

**MK_Mapping** – Slot-Zuweisung vor Rennstart: slot 1–8 = anzeigen, slot –1 = Mapping beendet

---

## Fernbedienung

### Hardware

| Komponente | Details |
|---|---|
| MCU | D1 Mini (ESP8266) |
| Joysticks | 2× PS2-Analog-Stick (5V VCC) |
| ADC | ADS1115, I2C 0x48 |
| Display | SSD1306 OLED 128×64, I2C 0x3C (Breadboard); geplant: 2.42" SSD1309 |
| Buttons | 4× Taster, SNES-Farblayout (Raute) |
| Status-LED | WS2812B RGB |
| Rumble | Coin-ERM-Vibrationsmotor 10mm (1034-Typ), 5V |
| Rumble-Treiber | 2N2222 NPN, 1kΩ Basis, 1N4001 Freilaufdiode |
| ADC-Teiler | 2× gleiche Widerstände (1k–100k) pro Joystick-Achse |
| Akku | LiPo 3700mAh + Battery Shield v1.2.0 |
| Power/Modus | 3-Position Switch (DPDT Center-Off) |
| Puffer | 220µF Elko auf 5V-Rail |

### Pinbelegung D1 Mini

| Pin | Funktion |
|---|---|
| D0 | Modus-Erkennung (Game=GND / Direct=3.3V) |
| D1 | SCL (I2C) |
| D2 | SDA (I2C) |
| D3 | Button Red (TBD PCB) |
| D4 | WS2812B Datenleitung |
| D5 | Button Yellow (TBD PCB) |
| D6 | Button Green (TBD PCB) |
| D7 | Button Blue (TBD PCB) |
| D8 | Rumble PWM (via 2N2222) |
| 5V | Joystick VCC, Rumble-Motor |
| 3.3V | ADS1115, OLED, WS2812B |
| GND | Alle gemeinsam |

> Die endgültige Farb-Pin-Zuordnung wird beim PCB-Layout festgelegt, wenn die Leiterbahnen vom Button-Board zum Main-Board definiert werden.

### Button-Layout (SNES-Raute)

```
        [Blau]
   [Grün]      [Rot]
        [Gelb]
```

| Farbe | Funktion im Menü | Funktion im Spiel |
|---|---|---|
| Gelb (unten) | Bestätigen | TBD |
| Grün (links) | — (Combo) | TBD |
| Blau (oben) | — (Combo) | TBD |
| Rot (rechts) | Zurück / Abbrechen | TBD |
| Grün + Blau (3s) | Menü öffnen | — |

### Power/Modus-Schalter

```
LiPo(+) → Schalter-Mitte (gemeinsam)
           ├── Links  → Shield(+)  UND  D0 → GND    = Game Mode
           └── Rechts → Shield(+)  UND  D0 → 3.3V   = Direct Mode
           Mitte = Aus (Stromkreis offen)
```

### Joystick Spannungsteiler

PS2-Joysticks liefern bis 5V, ADS1115 verträgt max 3,6V (GAIN_ONE: ±4,096V Referenz).

Pro Achse: `Signal --[R]--+--[R]-- GND`, Mitte → ADS-Eingang.

Beide Widerstände müssen gleich sein, der absolute Wert ist egal (1kΩ–100kΩ):

```
V_out = 5V × R/(R+R) = 5V × 0,5 = 2,5V  →  ADS-Rohwert ≈ 19989
```

### Joystick-Achsenzuordnung

Standard: linker Stick Y-Achse = Throttle, rechter Stick X-Achse = Steering.

Im Menü umschaltbar: „Joysticks tauschen" → rechter Stick Y = Throttle, linker Stick X = Steering. Nur für die laufende Session, nicht im EEPROM gespeichert.

### Rumble-Schaltung

```
5V -- Motor -- 1N4001(Kathode zu 5V) -- Kollektor(2N2222) -- Emitter -- GND
D8 -- 1kΩ -- Basis(2N2222)
```

Motor rot→5V | blau→Kollektor. 220µF Elko auf 5V-Rail gegen Spannungseinbrüche beim Anlaufen.

> D8 (GPIO15) hat internen Pull-Down → Transistor bleibt beim Booten sicher aus.
> D4 (GPIO2) ist UART1-TX auf dem ESP8266 – nicht für PWM verwenden. WS2812B-Datenpuls funktioniert trotzdem.

### WS2812B Status-LED

| Farbe | Bedeutung |
|---|---|
| Orange pulsierend | Suche / Verbinde (beide Modi) |
| Grün | Verbunden / Bereit |
| Pink | Menü offen |
| Gelb pulsierend | Kalibrierung wird ausgelöst (Grün+Blau 3s halten) |
| Gelb solid | Kalibrierung läuft – FB gesperrt (mind. 3s) |
| Orange solid | Akku niedrig |
| Rot blinkend | Akku kritisch |

### Einstellungsmenü

**Grün + Blau gleichzeitig 3 Sekunden halten** öffnet das Menü.  
Navigation per Joystick (hoch/runter), Gelb = bestätigen, Rot = zurück.

| Menüpunkt | Funktion | EEPROM |
|---|---|---|
| Offset-Kalibrierung | Nullpunkt setzen (Joysticks loslassen) | ✅ |
| Min/Max-Kalibrierung | 8 Schritte: jeden Stick in jede Richtung durchdrücken | ✅ |
| Servo-Trim | ±10 Stufen (wird ans Auto gesendet, dort gespeichert) | ✅ |
| Max. Speed | 1–10 Stufen (10 = 100 %) | — |
| Sprache | Deutsch / English | ✅ |
| Rumble | An/Aus | — |
| Joysticks tauschen | Standard/Getauscht (nur Session) | — |
| Reset | Alle Werte zurücksetzen | — |

Beim ersten Start läuft automatisch eine Offset-Kalibrierung (Joysticks loslassen).  
**Rot beim Einschalten gedrückt halten** → sofortiger EEPROM-Reset.  
**Rot 10 Sekunden halten** (im Betrieb) → EEPROM-Reset mit Countdown ab 5s.

### Joystick-Kalibrierung

Ausgabe: –100…+100 pro Achse. Positive und negative Richtung werden separat skaliert (asymmetrisches Mapping), damit auch ungleichmäßige Joysticks den vollen Bereich erreichen. Dead Zone ±6.

### OLED-Display (Normalbetrieb)

```
LX: -12  LY:   3
RX:   0  RY:  -5
Y:0 G:0 B:0 R:0   [Akku]
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

## PCB-Aufteilung (geplant)

| Board | Inhalt |
|---|---|
| Main-Board | D1 Mini, ADS1115, Rumble-Schaltung (2N2222, Dioden, R, C), alle JST-Ausgänge |
| Button-Board | 4× 12×12×5mm Taster, JST zum Main-Board |
| Switch-Board | 3-Position-Schalter, JST zum Main-Board |

Display und Joysticks kommen als fertige Module mit JST-Kabel (kein eigenes PCB).  
Verbindungen zwischen Boards: JST-XH 2.54mm.

Schaltplan: `docs/fb-schematic.kicad_sch` (KiCad 10, generiert via `docs/generate_schematic.py`)

---

## Gehäuse

- PS5-Controller-Ober/Unterschale als Basis
- Redesign in Fusion 360 (Löcher schließen, neue für Display/Sticks/Buttons/Schalter/LED/USB-C)
- TPU-gedruckte Buttonmatten in SNES-Farben (Gelb/Grün/Blau/Rot) über den Tastern

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
| FB-Firmware: Einstellungsmenü | ✅ |
| FB-Firmware: Peripherie-Guards | ✅ |
| FB-Firmware: Button-Farbnamen (SNES-Layout) | ✅ |
| FB-Firmware: Joystick-Tausch (Session-Toggle) | ✅ |
| FB-Firmware: Loop-Takt 50 Hz (millis + ADS 860 SPS) | ✅ |
| Kommunikationsprotokoll (`mk_protocol.h`) | ✅ |
| FB-Firmware: ESP-NOW vollständig (alle 7 Msg-Typen) | ✅ |
| FB-Firmware: Pairing, Kanal-Persistenz, Reconnect | ✅ |
| FB-Firmware: Feedback-Empfang (Rumble, Speed-Limit, Akku-Auto) | ✅ |
| FB-Firmware: Mapping-Anzeige (Slot-Nummer) | ✅ |
| KiCad-Schaltplan FB | ✅ |
| Display wechseln: 2.42" SSD1309 128×64 I2C | 🚚 bestellt |
| PCB-Layout (Main / Button / Switch Board) | ⏳ |
| Race-Display: Sprite, Position, Runde, Item | ⏳ |
| Receiver-Test Gegenstelle (zweiter D1 Mini) | ⏳ |
| Gehäuse anpassen (Fusion 360) | ⏳ |
| Fahrzeug-Firmware (ESP32-C3) | ⏳ |
| IMU Kollisionserkennung | ⏳ |
| Basisstation-Software (Raspberry Pi) | ⏳ |

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
| D8 für Button | GPIO15 Pull-Down, INPUT_PULLUP funktioniert nicht zuverlässig |
| 3.3V für Rumble-Motor | Zu schwach, jetzt 5V |
| Farb-TFT (ILI9341/ST7789) für Race-Display | Hardware-SPI-Pins durch Buttons belegt; stattdessen größeres OLED gleicher Auflösung |
| 10kΩ ADC-Teiler | Zu wenige im Lager (beliebiger gleicher Wert funktioniert) |
