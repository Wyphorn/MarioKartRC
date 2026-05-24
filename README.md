# Mario Kart RC

Mario-Kart-inspiriertes RC-Rennsystem für 8 Fahrzeuge im Außeneinsatz. Die Autos fahren mit ~30 km/h und erkennen Items, Kollisionen und Rundenzählung automatisch.

---

## Repo-Struktur

| Ordner | Inhalt |
|---|---|
| `fb-firmware/` | Fernbedienung – D1 Mini (ESP8266), PlatformIO |
| `car-firmware/` | Fahrzeug-Firmware – ESP32-C3, PlatformIO |
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
| Fahrzeug-MCU | ESP32-C3 SuperMini |
| Fernbedienung | D1 Mini (ESP8266) |

---

## Betriebs-Modi

**Direct Mode** – FB kommuniziert direkt mit dem zugeordneten Auto. Keine Basisstation nötig, ideal zum Einzelfahren und Testen.

**Game Mode** – FB kommuniziert mit der Basisstation. Diese übernimmt die Spiellogik (Items, Punkte, Kollisionen, Rundenzählung) und leitet Fahrbefehle ans Auto weiter.

---

## Kommunikationsprotokoll (`shared/mk_protocol.h`)

Alle ESP-NOW-Pakete beginnen mit einem `uint8_t type`-Feld als Discriminator.

| Struct | Richtung | Frequenz |
|---|---|---|
| `MK_ControlInput` | FB → Auto (Direct) / FB → Basis (Game) | ~50 Hz |
| `MK_ConfigPacket` | FB → Auto / FB → Basis → Auto | Nur bei Änderung |
| `MK_GameFeedback` | Auto → FB (Direct) / Basis → FB (Game) | 5 Hz |
| `MK_Beacon` | FB/Auto → Broadcast | Beim Pairing |
| `MK_Assign` | Basis → FB/Auto (Unicast) | Beim Pairing |
| `MK_ChannelSwitch` | Basis → Broadcast | Kanalwechsel / Rennende |
| `MK_Mapping` | Basis → FB/Auto (Unicast) | Vor Rennstart |
| `MK_IrConfig` | Basis → Auto (Unicast) | Nach Assign (Game Mode) |

**MK_ControlInput** – Joystick-Steuerdaten:
- `throttle` – Vorwärts/Rückwärts, –100..100
- `steering` – Links/Rechts, –100..100
- `buttons` – Bitmask: `MK_BTN_YELLOW` | `MK_BTN_GREEN` | `MK_BTN_BLUE` | `MK_BTN_RED`
- `maxSpeed` – Fahrer-Präferenz 1–10

**MK_ConfigPacket** – Servo-Trim –10..10 (im Auto-EEPROM gespeichert)

**MK_GameFeedback** – Spielzustand: Position, Runde, Item, Rumble-Befehl, Akku-Auto (0–5)

**MK_Beacon** – enthält `charId` (1–8) beim Auto, damit Basis/FB den Charakter kennt

**MK_IrConfig** – aktiviert IR-Aussendung mit Fahrzeug-ID 1–8; nur im Game Mode gesendet. Rennende (MSG_CHANNEL_SWITCH mit channel=1) deaktiviert IR implizit.

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
| Rumble | 2× Zylindrischer ERM-Vibrationsmotor 28×12mm, DC 3–6V, direkt an LiPo VCC |
| Rumble-Treiber | 2× 2N2222 NPN, 2× 1kΩ Basis, 2× 1N4001 Freilaufdiode |
| ADC-Teiler | 2× 4,7kΩ pro Joystick-Achse (5V→~2,5V am ADS) |
| Akku | LiPo 3700mAh + Battery Shield v1.2.0 |
| Power/Modus | 3-Position Switch (SPDT Center-Off): Mitte=Aus, Links=Game, Rechts=Direct |
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
| Blau (oben) | — (Combo) | Charakter-Sound |
| Rot (rechts) | Zurück / Abbrechen | TBD |
| Grün + Blau (3s) | Menü öffnen | — |

### WS2812B Status-LED

| Farbe | Bedeutung |
|---|---|
| Orange blinkend | Suche / Verbinde |
| Grün | Verbunden |
| Pink | Menü offen |
| Gelb pulsierend | Kalibrierung wird ausgelöst |
| Gelb solid | Kalibrierung läuft |
| Orange solid | Akku niedrig |
| Rot blinkend | Akku kritisch |

### Einstellungsmenü

**Grün + Blau gleichzeitig 3 Sekunden halten** öffnet das Menü.

| Menüpunkt | EEPROM |
|---|---|
| Offset-Kalibrierung (Nullpunkt) | ✅ |
| Min/Max-Kalibrierung (8 Schritte) | ✅ |
| Servo-Trim (±10 Stufen, ans Auto gesendet) | ✅ |
| Max. Speed (1–10) | — |
| Sprache (DE/EN) | ✅ |
| Rumble (An/Aus) | — |
| Joysticks tauschen (Session) | — |
| Debug (Live-Anzeige) | — |
| Reset (EEPROM löschen) | — |

### FB-Firmware bauen & flashen

```bash
cd fb-firmware
~/.platformio/penv/bin/pio run -t upload
~/.platformio/penv/bin/pio device monitor
```

---

## Fahrzeug

### Hardware

| Komponente | Details |
|---|---|
| MCU | ESP32-C3 SuperMini |
| Motortreiber | BTS7960 H-Bridge, 43A Peak |
| Audio | DFPlayer Mini (UART GPIO20/21) – Sounds aus Ordner pro Charakter |
| IMU | LSM6DS3 I2C 0x6A – Kollisionserkennung (WHO_AM_I=0x69) |
| LEDs | WS2812B – 8 LEDs Charakterkopf + 1 Status-LED |
| Lenkservo | Multiplex MS-12022 MG DIGI, PWM 1000–2000µs |
| IR-Sender | 3× IR-LED 940nm ±40°, nach unten (Mitte + ±8cm lateral) |
| Charakter-ID | 4-Pin DIP-Schalter + Widerstandsnetzwerk, 1 ADC-Pin |
| Batterie-ADC | 1 Pin – Spannungsüberwachung per Spannungsteiler |
| Wireless | ESP-NOW |

### Pinbelegung ESP32-C3 SuperMini

| GPIO | Funktion |
|---|---|
| GPIO0 | ADC – Charakter-ID (DIP-Schalter) |
| GPIO1 | ADC – Batterie-Monitor |
| GPIO3 | BTS7960 RPWM |
| GPIO4 | BTS7960 LPWM |
| GPIO6 | I2C SCL (LSM6DS3) |
| GPIO7 | I2C SDA (LSM6DS3) |
| GPIO8 | WS2812B DATA |
| GPIO9 | IR LED (via MMBT2222) |
| GPIO10 | Servo PWM |
| GPIO20 | UART1 RX ← DFPlayer TX |
| GPIO21 | UART1 TX → DFPlayer RX |

### Charakter-Identifikation (DIP-Schalter)

10kΩ Festwiderstand nach 3.3V, 4 Widerstände (3.3k / 10k / 20k / 51k) je per DIP-Schalter nach GND. ADC-Scale-Faktor 2.985f (ESP32-C3 bei ADC_11db max ~2.985V). Rosalina (3.3V) saturiert den ADC → wird via `raw > 3940` erkannt.

| Charakter | DIP [3.3k 10k 20k 51k] | Spannung | DFPlayer-Ordner |
|---|---|---|---|
| Mario    | 1110 | 0.597V | 01 |
| Luigi    | 1000 | 0.819V | 02 |
| Yoshi    | 0110 | 1.320V | 03 |
| Bowser   | 0100 | 1.650V | 04 |
| DK       | 0011 | 1.946V | 05 |
| Peach    | 0010 | 2.200V | 06 |
| Toad     | 0001 | 2.759V | 07 |
| Rosalina | 0000 | 3.300V | 08 |

### DFPlayer SD-Karten-Struktur

```
/01/   001.mp3, 002.mp3, …   ← Mario
/02/   001.mp3, …             ← Luigi
…
/08/   001.mp3, …             ← Rosalina
/09/   001.mp3, …             ← Game-Sounds (Stern, Banane, …)
```

Track-Nummern in den `kJoy_*` / `kSad_*` Arrays am Anfang von `car-firmware/src/main.cpp` pflegen. Blauer Knopf → zufälliger Joy-Sound des aktuellen Charakters.

### LED-Layout

| Index | Funktion |
|---|---|
| 0–7 | Charakter-Kreis (8 LEDs im Charakterkopf) |
| 8 | Status-LED |

| Status-LED | Bedeutung |
|---|---|
| Orange blinkend | Suche / Koppeln |
| Grün | Verbunden |
| Gelb | Akku < 33% |
| Rot blinkend | Akku < 16% |

### Kollisionserkennung

IMU LSM6DS3, ±16g-Range. Threshold: 6g (Tuning nach Fahrtest). Bei Auslösung: 2s Rumble im gekoppelten FB, 3s Cooldown.

### Fahrzeug-Firmware bauen & flashen

```bash
cd car-firmware
pio run -t upload
pio device monitor
```

---

## Streckeninfrastruktur & IR-System

| Komponente | Funktion |
|---|---|
| TSOP38238 | IR-Empfänger, 38kHz, ±45° – in Toren und Bodenplatten |
| STM32F103 + TJA1050 | CAN-Controller + Transceiver in Tor-Nodes |
| RPi5 + USB-CAN | Basisstation / Spielserver |

**Kommunikationsweg:** Auto fährt über Bodenplatte → TSOP38238 erkennt IR-ID → STM32 → CAN → RPi5 → Spiellogik

---

## PCB-Aufteilung FB (geplant)

| Board | Inhalt |
|---|---|
| Main-Board | D1 Mini, ADS1115, Rumble-Schaltung, alle JST-Ausgänge |
| Button-Board | 4× 12×12×5mm Taster, JST zum Main-Board |
| Switch-Board | 3-Position-Schalter, JST zum Main-Board |

Schaltplan: `docs/fb-schematic.kicad_sch`

---

## Projektstatus

| Schritt | Status |
|---|---|
| Architektur & Komponentenwahl | ✅ |
| Kommunikationsprotokoll (`mk_protocol.h`) | ✅ |
| FB-Firmware: vollständig (Joysticks, Display, Menü, ESP-NOW, Rumble) | ✅ |
| KiCad-Schaltplan FB | ✅ |
| Auto-Peripherie POC (alle Komponenten getestet) | ✅ |
| Auto-Firmware: vollständig (ESP-NOW, Motor, Servo, LED, IMU, Sound) | ✅ |
| Direct Mode End-to-End (FB ↔ Auto) | ✅ |
| Display wechseln: 2.42" SSD1309 | 🚚 bestellt |
| Multiplex MS-12022 Servo einbauen | ⏳ |
| Batterie-ADC verdrahten | ⏳ |
| IR-LED einbauen + 38kHz-Modulation | ⏳ |
| Sound-Arrays befüllen (alle Charaktere) | ⏳ |
| PCB-Layout FB | ⏳ |
| Race-Display: Sprite, Position, Runde, Item | ⏳ |
| Gehäuse anpassen (Fusion 360) | ⏳ |
| Basisstation-Software (Raspberry Pi) | ⏳ |
| Game Mode End-to-End | ⏳ |

---

## Verworfene Optionen

| Option | Grund |
|---|---|
| UWB für Positionierung | Zu komplex |
| MAX98357A (I2S) für Auto-Sound | DFPlayer Mini gewählt – einfacher, vorhanden |
| DRV8833 für Motortreiber | Max. 1.5A vs. 8A Stallstrom des Motors |
| MOSFET IRFZ44N/RFP30N06LE für Rumble | Nicht Logic-Level / zu groß |
| Piezo-Buzzer in FB | Kein Sound in der FB |
| ESP32 für Fernbedienung | D1 Mini aus Lager, ausreichend |
| D4 für Rumble | UART1-Interferenz auf GPIO2 |
| 3.3V für Rumble-Motor | Zu schwach |
| Farb-TFT für Race-Display | SPI-Pins belegt; stattdessen größeres OLED |
| VSMA1094750X02 / TSAL6200 als IR-Sender | Falscher Abstrahlwinkel / existiert nicht |
