# Mario Kart RC

Mario-Kart-inspiriertes RC-Rennsystem für 8 Fahrzeuge im Außeneinsatz. Die Autos fahren mit ~30 km/h; Items, Kollisionen und Rundenzählung werden automatisch erkannt.

Das Projekt ist auf Nachbau ausgelegt: Alle Bauteile haben eine feste Bestellnummer eines echten Herstellers mit öffentlichem Datenblatt und sind handlötbar (SOT-23, SOT-223, SMA, THT – kein QFN).

---

## Repo-Struktur

| Ordner | Inhalt |
|---|---|
| `fb-firmware/` | Fernbedienung – ESP32-C6-SuperMini, PlatformIO |
| `car-firmware/` | Fahrzeug – ESP32-C6 Mini, PlatformIO |
| `shared/` | Gemeinsamer Code: `mk_protocol.h` (Pakete FB ↔ Auto ↔ Basis), `mk_clock_guard.h` (Workaround für den C6-Uhrenfehler) |
| `kicad/projects/` | KiCad-Projekte je Platine: `kart` (Fahrzeug-Board), `controller` (Fernbedienung), `buttons` (Tasterplatine FB), `switch` (Schalter-Platinchen Kart), jeweils mit Gerber als ZIP |
| `kicad/libs/` | Eigene Symbole und Footprints (`zzzz_Own`, `RF_Module_Own`), von allen Projekten über ihre lokalen Bibliothekstabellen eingebunden |
| `kicad/archive/` | Alte Stände: FB-V1-Layout, erster FB-Schaltplan (D1 Mini, Mai 2026) |
| `docs/` | Weitere Dokumentation, u.a. `base-station.md` |

---

## Gesamtarchitektur

| Bereich | Technologie |
|---|---|
| Drahtlos | ESP-NOW |
| Kabelgebunden | CAN-Bus (STM32F103 + TJA1050) |
| Basisstation | Raspberry Pi 5 + USB-CAN-Adapter |
| Fahrzeugbasis | Carrera 2013 RC-Autos (umgebaut) |
| Fahrzeug-MCU | ESP32-C6 Mini |
| Fernbedienung | ESP32-C6-SuperMini (gleicher Chip wie im Auto) |

**Faustregel:** kabelgebunden = CAN, drahtlos = ESP-NOW.

---

## Betriebs-Modi

**Direct Mode** – Die Fernbedienung (FB) steuert ihr Auto direkt. Keine Basisstation nötig, ideal zum Einzelfahren und Testen.

**Game Mode** – Die FB spricht mit der Basisstation. Diese übernimmt die Spiellogik (Items, Punkte, Kollisionen, Rundenzählung) und leitet die Fahrbefehle ans Auto weiter.

Gewählt wird der Modus mit dem Schiebeschalter der FB: Mitte = Aus, Links = Game, Rechts = Direct.

---

## Kommunikationsprotokoll (`shared/mk_protocol.h`)

Alle ESP-NOW-Pakete beginnen mit einem `uint8_t type` als Unterscheidungsmerkmal.

| Struct | Richtung | Frequenz |
|---|---|---|
| `MK_ControlInput` | FB → Auto (Direct) / FB → Basis (Game) | ~50 Hz |
| `MK_ConfigPacket` | FB → Auto / FB → Basis → Auto | nur bei Änderung |
| `MK_GameFeedback` | Auto → FB (Direct) / Basis → FB (Game) | 5 Hz |
| `MK_Beacon` | FB/Auto → Broadcast | beim Pairing |
| `MK_Assign` | Basis → FB/Auto (Unicast) | beim Pairing |
| `MK_ChannelSwitch` | Basis/FB → Auto | Kanalwechsel / Rennende |
| `MK_Mapping` | Basis → FB/Auto (Unicast) | vor Rennstart |
| `MK_IrConfig` | Basis → Auto (Unicast) | nach Assign (nur Game Mode) |

- **`MK_ControlInput`** – `throttle` und `steering` (–100..100), `buttons` (Bitmaske Gelb/Grün/Blau/Rot), `maxSpeed` (1–10)
- **`MK_ConfigPacket`** – Servo-Trim –10..10; `save = 1` speichert im Auto-EEPROM, `save = 0` ist eine Live-Vorschau
- **`MK_GameFeedback`** – Position, Runde, Item, Rumble, Akkustand des Autos (0–5) und sein aktueller Servo-Trim
- **`MK_Beacon`** – enthält beim Auto die Charakter-ID (1–8)

### Pairing und Wiederverbinden (Direct Mode)

1. Erstes Pairing auf dem Setup-Kanal 1: Das Auto sendet Beacons, die FB antwortet mit `MK_Assign` und schickt es per `MK_ChannelSwitch` auf einen zufälligen Betriebskanal (4, 6, 9 oder 11).
2. Beide speichern Kanal und Gegenstelle im EEPROM.
3. **Wiederverbinden ist FB-gesteuert:** Nach einem Neustart oder Funkabriss meldet sich die FB mit ihrer echten MAC auf dem gespeicherten Kanal beim gespeicherten Auto und sendet einfach Steuerpakete. Das Feedback des Autos bestätigt die Verbindung – kein Beacon, kein erneutes Assign.
4. Rückfall auf Kanal 1 erst, wenn die Gegenstelle wirklich weg ist: Auto nach 30 s Funkstille (5 s direkt nach dem Einschalten), FB nach 30 s (15 s nach dem Einschalten). Kanal 1 wird bewusst vermieden, weil dort die erste FB gewinnt und die Zuordnung FB ↔ Auto wechseln könnte.

---

## Fernbedienung

### Hardware

| Komponente | Details |
|---|---|
| MCU | ESP32-C6-SuperMini (MakerGO) |
| Joysticks | 2× PS2-Analog-Stick, direkt an 3.3V (kein Spannungsteiler) |
| ADC | ADS1115 als Breakout-Modul, I2C 0x48 (ADDR → GND) |
| Display | 1.9" IPS 170×320, ST7789, SPI |
| Buttons | 4× Taster (SNES-Raute) + 2× Stick-Taster (verdrahtet, ohne Funktion) |
| Status-LED | WS2812B, 330 Ω Serienwiderstand |
| Rumble | 2× ERM-Vibrationsmotor 28×12 mm, parallel an Zellspannung |
| Rumble-Treiber | 1× MMBT2222A + 1 kΩ Basis, 1× SS14 Freilaufdiode |
| Akku | 1× 18650, ungeschützt – zum Laden herausnehmen (externes Ladegerät) |
| Verpolschutz | P-MOSFET DMG2301L-7 (Ersatz: DMG2301LK-7), direkt hinter dem Akku |
| Spannungsregler | LDO MCP1826T-3302E/DC, fest 3.3V, 1 A |
| Power/Modus | Schiebeschalter 2P3T: Pol A schaltet SHDN des LDO, Pol B den Modus-Pin |
| Akku-Messung | Teiler 100k/100k + 100 nF an GPIO0 |
| Puffer | 220 µF Elko an der Akkuschiene (Rumble-Anlaufstrom) |

Alle Verbraucher außer dem Rumble laufen an einer einzigen 3.3V-Schiene.

> **Hinweis für Nachbauer:** Auf FB-Board V1 ist die Freilaufdiode D2 im Footprint verdreht. Den Kathodenring entgegen dem Siebdruck auf Pad 2 (+BATT) setzen – das ist die Seite mit Durchgang zu Pin 1 von J6/J7.

### Pinbelegung ESP32-C6-SuperMini

| GPIO | Funktion |
|---|---|
| GPIO0 | ADC – Akku-Messung |
| GPIO1 | Modus-Pin (Game = GND, Direct = 3.3V) |
| GPIO2 | Button Rot |
| GPIO3 | Button Blau |
| GPIO4 | Button Grün |
| GPIO5 | Display MOSI |
| GPIO6 | WS2812B DATA |
| GPIO7 | Display SCK |
| GPIO14 | Display Backlight (PWM) |
| GPIO15 | Display CS |
| GPIO17 | Rumble PWM |
| GPIO18 | Display DC |
| GPIO19 | I2C SDA (ADS1115) |
| GPIO20 | I2C SCL (ADS1115) |
| GPIO21 / 22 | Stick-Taster R / L (inneres Pad, reserviert) |
| GPIO23 | Button Gelb (inneres Pad, einzelner Draht) |

Tabu: GPIO8/9 (Strapping, Onboard-LED/BOOT), GPIO12/13 (USB zum Flashen und für den Serial-Monitor), GPIO16 (UART0 TX, wird vom Boot-ROM getrieben).

ADS1115-Kanäle: A0 = Links X, A1 = Links Y, A2 = Rechts Y, A3 = Rechts X (A2/A3 bewusst getauscht, damit sich die Leitungen nicht kreuzen).

### Tastenbelegung

```
        [Blau]
   [Grün]      [Rot]
        [Gelb]
```

| Taste | Im Menü | Beim Fahren |
|---|---|---|
| Grün | Bestätigen | – |
| Rot | Zurück / Abbrechen | Licht am Auto an/aus |
| Blau | – | Charakter-Sound |
| Gelb | – | – (frei) |
| Grün + Blau 3 s | – | Menü öffnen |
| Rot 10 s halten | – | Einstellungen zurücksetzen (Countdown ab 5 s) |
| Rot beim Einschalten | Einstellungen zurücksetzen | |

Standardbelegung der Sticks: links hoch/runter = Gas, rechts links/rechts = Lenkung (im Menü tauschbar).

### Status-LED

| Farbe | Bedeutung |
|---|---|
| Orange blinkend | sucht Auto bzw. Basis |
| Grün | verbunden |
| Orange | Akku unter 33 % |
| Rot blinkend | Akku unter 16 % |
| Pink | Menü offen |
| Gelb pulsierend / solid | Kalibrierung startet / läuft |

### Einstellungsmenü

| Menüpunkt | Gespeichert |
|---|---|
| Offset-Kalibrierung (Nullpunkt der Sticks) | FB |
| Min/Max-Kalibrierung (8 geführte Schritte) | FB |
| Servo-Trim (±10, Räder bewegen sich live mit, Grün speichert, Rot verwirft) | Auto |
| Max. Speed (1–10) | – |
| Sprache (DE/EN) | FB |
| Rumble an/aus | – |
| Joysticks tauschen | – |
| Debug (Achsen, Buttons, Spannung, Kanal, MAC) | – |
| Reset | – |

Vor der ersten Fahrt **Offset-Kalibrierung** machen – sonst liegt der Nullpunkt der Sticks daneben und das Auto kriecht los.

### Akku

- 0 % = 3.4V, 100 % = 4.2V. Unter ~3.35V fällt der LDO aus der Regelung und die Joystick-Werte wandern – deshalb gilt die Zelle schon bei 3.4V als leer.
- **Tiefentladeschutz:** Liegt die Zelle 30 s unter 3.3V, zeigt die FB „Akku leer – bitte ausschalten“ und geht in Deep Sleep. Nur Aus- und Einschalten holt sie zurück. Ein Restverbrauch von wenigen mA bleibt; bei längerer Lagerung die Zelle herausnehmen.
- Nach 60 s ohne Eingabe wird das Display gedimmt.

---

## Fahrzeug

### Hardware

| Komponente | Details |
|---|---|
| MCU | ESP32-C6 Mini |
| Motortreiber | BTS7960 H-Brücke (IBT-2), 43 A Peak |
| Antrieb | 380er Bürstenmotor, 7.4V, Stallstrom ~8 A |
| Lenkservo | Multiplex MS-12022 MG DIGI, PWM 1050–1950 µs (mechanischer Puffer) |
| Audio | DFPlayer Mini (UART) – Sounds aus einem Ordner pro Charakter |
| IMU | LSM6DS3, I2C 0x6A – Kollisionserkennung |
| LEDs | WS2812B-Kette: Status-LED + 8er-Kreis im Charakterkopf |
| Beleuchtung | Front weiß + Heck rot, Rückfahrlicht weiß |
| IR-Sender | 3× IR-LED 940 nm nach unten (Mitte, ±8 cm) – Typ noch offen |
| Charakter-ID | 4-fach DIP-Schalter + Widerstandsleiter an einem ADC-Pin |
| Akku | 2× 18650 in Serie (2S), ungeschützt – zum Laden herausnehmen |
| Stromversorgung | 7.4V direkt an den BTS7960, 5V über Buck AP63205WU (C6, DFPlayer, LEDs, Servo) |
| Akku-Messung | Teiler 100k/47k an GPIO4 |

### Pinbelegung ESP32-C6 Mini

| GPIO | Funktion |
|---|---|
| GPIO0 | I2C SCL (LSM6DS3) |
| GPIO1 | I2C SDA (LSM6DS3) |
| GPIO2 | ADC – Charakter-ID |
| GPIO3 | IR-LED (über MMBT2222) |
| GPIO4 | ADC – Akku-Messung |
| GPIO5 | BTS7960 RPWM (10k Pulldown) |
| GPIO6 | BTS7960 LPWM (10k Pulldown) |
| GPIO7 | Servo PWM |
| GPIO14 | Rückfahrlicht |
| GPIO15 | Front- + Rücklicht |
| GPIO18 | UART1 TX → DFPlayer RX |
| GPIO19 | UART1 RX ← DFPlayer TX |
| GPIO20 | WS2812B DATA |

BTS7960 R_EN/L_EN fest an 5V. GPIO8/9 bleiben frei (Strapping: dürfen beim Reset nicht auf LOW gezogen werden).

### Charakter-Identifikation (DIP-Schalter)

10 kΩ nach 3.3V, dazu vier Widerstände (4.7k / 10k / 20k / 51k), je per DIP-Schalter nach GND. Der kleinste Abstand zwischen zwei Charakteren beträgt 254 mV.

| Charakter | DIP [4.7k 10k 20k 51k] | Spannung | SD-Ordner |
|---|---|---|---|
| Mario    | 1110 | 0.713V | 01 |
| Luigi    | 1000 | 1.055V | 02 |
| Yoshi    | 0110 | 1.320V | 03 |
| Bowser   | 0100 | 1.650V | 04 |
| DK       | 0011 | 1.946V | 05 |
| Peach    | 0010 | 2.200V | 06 |
| Toad     | 0001 | 2.759V | 07 |
| Rosalina | 0000 | 3.300V | 08 |

Wird der DIP-Schalter umgestellt, während das Auto nicht verbunden ist, spielt es zur Kontrolle den Erkennungssound des neuen Charakters.

### SD-Karte des DFPlayers

```
/01/  001.mp3, 002.mp3, …   ← Mario
/02/  …                     ← Luigi
…
/08/  …                     ← Rosalina
/09/  …                     ← Spiel-Sounds (Stern, Banane, …)
```

Welche Tracks fröhlich bzw. traurig sind, steht in den Arrays `kJoy_*` / `kSad_*` am Anfang von `car-firmware/src/main.cpp`.

### LEDs

| Index | Funktion |
|---|---|
| 0 | Status-LED |
| 1–8 | Charakter-Kreis (Regenbogen) |

| Status-LED | Bedeutung |
|---|---|
| Orange blinkend | sucht / Verbindung unterbrochen |
| Grün | verbunden |
| Gelb | Akku unter 3.3V/Zelle |
| Rot blinkend | Akku unter 3.2V/Zelle – Abschaltung steht bevor |

### Akku

**Tiefentladeschutz:** Liegt der Akku 30 s unter 6.4V (3.2V/Zelle), stoppt das Auto, spielt einen traurigen Sound und geht in Deep Sleep. Jede Messung zählt, auch unter Last; eine erholte Messung beim Gaswegnehmen setzt den Zähler zurück. So löst es im Rennen erst aus, wenn der Akku wirklich leer ist.

### Kollisionserkennung

LSM6DS3 im ±16g-Bereich. Bei einem Stoß über der Schwelle vibriert die gekoppelte FB 2 s lang, danach 3 s Pause. Die Schwelle steht zum Testen auf 1.5g und soll nach dem Fahrtest wieder auf ~6g.

---

## Firmware bauen und flashen

```bash
cd fb-firmware      # bzw. car-firmware
pio run -t upload
pio device monitor
```

`upload_port`/`monitor_port` in `platformio.ini` sind auf die Seriennummern der eigenen Boards festgelegt – für andere Boards anpassen oder entfernen. Geflasht wird über den eingebauten USB-Port des C6 (USB-Serial/JTAG).

Beide Firmwares nutzen eine OTA-taugliche Partitionstabelle (`default.csv`). Sie lässt sich nur per USB ändern, deshalb ist sie ab dem ersten Flash gesetzt.

### ESP32-C6 rev v0.2: Uhr springt zurück

Beim ESP32-C6 in Revision v0.2 fallen im Zeitgeber gelegentlich einzelne Bits aus, sobald Funk und ein I2C-Master gleichzeitig laufen – `millis()` springt dann um Sekunden bis Minuten zurück ([esp-idf #19036](https://github.com/espressif/esp-idf/issues/19036)). Die Firmware umgeht das mit `shared/mk_clock_guard.h`:

- `nowMs()` zählt über den FreeRTOS-Tick, der auf einem nicht betroffenen Zähler läuft. Im eigenen Code **immer `nowMs()` statt `millis()` verwenden.**
- Ein Guard-Task erkennt Rücksprünge und setzt die Systemuhr zurück auf Stand (Log-Zeile `[CLOCK]`).
- Der WLAN-Stromsparmodus ist abgeschaltet.

Die Revision zeigt `esptool.py chip_id` an.

---

## Streckeninfrastruktur und IR-System

| Komponente | Funktion |
|---|---|
| TSOP38238 | IR-Empfänger, 38 kHz, ±45° – in Toren und Bodenplatten |
| STM32F103 + TJA1050 | CAN-Controller + Transceiver in den Tor-Nodes |
| RPi 5 + USB-CAN | Basisstation / Spielserver |

**Weg eines Ereignisses:** Auto fährt über eine Bodenplatte → TSOP38238 erkennt die IR-ID → STM32 → CAN → RPi 5 → Spiellogik.

Pro Tor sitzen drei Empfänger (Mitte und ±8 cm) passend zu den drei IR-LEDs am Auto. Bei 30 km/h bleibt ein Erkennungsfenster von ~5 ms.

---

## Projektstatus

| Schritt | Status |
|---|---|
| Architektur, Komponentenwahl, Protokoll | ✅ |
| Auto-Firmware (ESP-NOW, Motor, Servo, LEDs, IMU, Sound, Akku-Schutz) | ✅ |
| FB-Firmware auf ESP32-C6 portiert (Display, Menü, ESP-NOW, Rumble, Akku-Schutz) | ✅ |
| Direct Mode Ende-zu-Ende (Pairing, Reconnect, Trim, Sound, Licht) | ✅ |
| KiCad: Kart-Board und FB-Board | ✅ |
| FB-Board V1 gefertigt und aufgebaut | ✅ |
| Race-Display (Charakter, Position, Runde, Item) | ⏳ |
| IR-LED auswählen, einbauen, 38-kHz-Modulation | ⏳ |
| Verpolschutz + Hauptschalter am Akkuhalter des Autos | ⏳ |
| FB-Board V2 (Diode gedreht, Basis-Pulldown, 0.5-mm²-Anschlüsse) | ⏳ |
| Gehäuse anpassen (Fusion 360) | ⏳ |
| Basisstation-Software (Raspberry Pi) | ⏳ |
| Game Mode Ende-zu-Ende | ⏳ |
| OTA-Update | ⏳ zurückgestellt bis nach den ersten Fahrten |

---

## Verworfene Optionen

| Option | Grund |
|---|---|
| UWB für Positionierung | zu komplex |
| MAX98357A (I2S) für den Sound im Auto | DFPlayer Mini ist einfacher |
| DRV8833 als Motortreiber | max. 1.5 A gegen 8 A Stallstrom |
| IRFZ44N / RFP30N06LE für den Rumble | nicht Logic-Level bzw. zu groß |
| Piezo-Buzzer in der FB | kein Sound in der FB |
| D1 Mini (ESP8266) für die FB | ersetzt durch ESP32-C6: ein Chip-Typ für Auto und FB, Hardware-SPI fürs Display, einheitliches OTA |
| OLED (SSD1306/SSD1309) in der FB | ungünstiges Seitenverhältnis; stattdessen 1.9" IPS |
| Battery Shield / Buck-Boost für die FB | LDO genügt, die Schiene muss nur unter 3.6V bleiben; Buck-Boost-ICs waren QFN oder zu schwach |
| AMS1117 / LD1117 als FB-Regler | 1.1–1.3V Dropout – an einer 18650 unbrauchbar |
| 2 Zellen in der FB | konstruktiv ausgeschlossen |
| VSMA1094750X02 als IR-Sender | ±75° passt nicht zu den ±45° des TSOP38238 |
