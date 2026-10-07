#pragma once
#include <stdint.h>

// ── Packet type discriminator ────────────────────────────────────────────────
// Every packet starts with a uint8_t type field.
// Receiver reads type first, then casts to the appropriate struct.

enum MK_MsgType : uint8_t {
    MSG_CONTROL  = 0x01,   // FB → Auto (Direct) | FB → Basis (Game)  — high frequency
    MSG_CONFIG   = 0x02,   // FB → Auto (Direct) | FB → Basis → Auto  — on change only
    MSG_FEEDBACK = 0x03,   // Auto → FB (Direct) | Basis → FB (Game)  — high frequency

    MSG_BEACON         = 0x10,   // FB/Auto → Broadcast — device announces itself
    MSG_ASSIGN         = 0x11,   // Basis → FB/Auto (Unicast) — slot assignment response
    MSG_CHANNEL_SWITCH = 0x12,   // Basis → Broadcast — all devices switch to new channel
                                 // At race end: base sends MSG_CHANNEL_SWITCH with channel=MK_ESPNOW_CHANNEL
                                 // so all devices return to channel 1 and persist it — next boot starts clean.
    MSG_MAPPING        = 0x13,   // Basis → FB/Auto (Unicast) — mapping process: show slot number/color
    MSG_IR_CONFIG      = 0x14,   // Basis → Auto (Unicast) — activates IR transmission with vehicle ID
                                 // Direct Mode: never sent → IR stays off

    MSG_HUNTER_TAG     = 0x15,   // Auto (Jäger) → Basis (Unicast) — Jäger meldet vermutliches Opfer
    MSG_HUNTER_STATE   = 0x16,   // Basis → Broadcast — offizieller neuer Jäger (autoritativ)
    MSG_HUNTER_SCORES  = 0x17,   // Basis → Broadcast (alle FBs) — Spielerliste + Jäger-Zeiten für Display

    MSG_OTA_REQUEST    = 0x18,   // FB → Auto (Direct) | Basis → Auto (Game) — Auto in den Update-Modus
    MSG_OTA_STATUS     = 0x19,   // Auto → FB / Basis (Unicast) — Antwort auf MSG_OTA_REQUEST
    MSG_SERVO_CAL      = 0x1A,   // FB → Auto (Direct) | FB → Basis → Auto — Lenkung kalibrieren
};

// ── Device types ─────────────────────────────────────────────────────────────
enum MK_DeviceType : uint8_t {
    DEVICE_FB  = 0x01,
    DEVICE_CAR = 0x02,
};

// ── Channel configuration ─────────────────────────────────────────────────────
// Channel lifecycle (Game Mode):
//
//   1. Boot / Registration:
//      All devices start on MK_ESPNOW_CHANNEL (1). FBs and cars beacon to
//      MK_BASE_MAC; base station responds with MK_Assign.
//
//   2. Race start:
//      After all devices are registered, the base station (RPi-triggered) scans
//      for the least congested channel, then broadcasts MSG_CHANNEL_SWITCH to all
//      paired peers. All devices switch and persist the channel. Race begins.
//
//   3. Race end:
//      Base station broadcasts MSG_CHANNEL_SWITCH with channel=MK_ESPNOW_CHANNEL.
//      All devices switch back to channel 1 and persist it. Next boot starts
//      clean on channel 1 — no rejoin attempt needed.
//
// Channel lifecycle (Direct Mode):
//   FB picks a random channel from MK_DIRECT_CHANNELS after pairing and sends
//   MSG_CHANNEL_SWITCH to the car. Both persist the channel; the FB also the
//   car MAC, the car the FB MAC. Reconnect is FB-driven, no beacon/assign:
//   the FB goes to the persisted channel with its real MAC and simply sends
//   MSG_CONTROL to the persisted car MAC; the car's MSG_FEEDBACK confirms the
//   link. The car keeps its pairing on link loss (restored from EEPROM after
//   its own reboot) and only falls back to MK_ESPNOW_CHANNEL after 30s of
//   silence (5s right after boot). FB gives up after 5s after boot (the user
//   can skip even that with any button), 30s after a runtime loss. Channel 1
//   is avoided on purpose: there the first FB wins, so every fallback may
//   change the FB↔car assignment.
//   No race-end reset needed — each new pairing picks a fresh random channel.

#define MK_ESPNOW_CHANNEL  1        // Fixed registration/setup channel — NEVER used as race channel.
                                    // Base station channel scanner MUST exclude MK_ESPNOW_CHANNEL.
                                    // Devices use channel != 1 to detect active race vs. setup phase.

#define MK_DIRECT_CHANNELS   {4, 6, 9, 11}  // Direct Mode operational channels (1 excluded = setup only)
#define MK_DIRECT_CHAN_COUNT  4              // Not fully non-overlapping, but distributes load

struct MK_ChannelSwitch {
    uint8_t type    = MSG_CHANNEL_SWITCH;
    uint8_t channel;  // Target channel 1–13
    // Cars: if channel == MK_ESPNOW_CHANNEL (race end), deactivate IR (irId = 0).
};

// ── Mapping process ───────────────────────────────────────────────────────────
// Base station sends MK_Mapping unicast to each registered FB and car.
// FB: displays slot number on screen so the operator can identify it.
// Car: looks up slot in MK_MAPPING_COLORS and shows that color on its WS2812B.
// Operator then assigns FB↔car pairs in the base station UI.
// When mapping is complete, base sends MK_Mapping with slot = -1 to all
// registered FBs and cars — devices return to their normal idle display.

struct MK_Mapping {
    uint8_t type = MSG_MAPPING;
    int8_t  slot;  // 1–8: show slot | -1: mapping beendet, Normalanzeige
};

// Car WS2812B color per slot — 8 well-distinguishable colors (RGB).
// Usage: led.setPixelColor(0, led.Color(r, g, b)) with values below.
static const uint8_t MK_MAPPING_COLORS[8][3] = {
    {255,   0,   0},  // 1 – Rot
    {  0, 255,   0},  // 2 – Grün
    {  0,   0, 255},  // 3 – Blau
    {255, 255,   0},  // 4 – Gelb
    {  0, 255, 255},  // 5 – Cyan
    {255,   0, 255},  // 6 – Magenta
    {255, 128,   0},  // 7 – Orange
    {255, 255, 255},  // 8 – Weiß
};

// ── IR configuration ─────────────────────────────────────────────────────────
// Sent by base station after Assign to enable IR transmission on the car.
// Direct Mode: never sent → IR stays off.
// At race end: base sends MSG_CHANNEL_SWITCH with channel=MK_ESPNOW_CHANNEL —
// cars MUST deactivate IR on receiving this packet (irId implicitly = 0).
// IR can also be explicitly deactivated by sending irId = 0.

struct MK_IrConfig {
    uint8_t type  = MSG_IR_CONFIG;
    uint8_t irId;  // 0 = deactivate, 1–8 = vehicle ID to transmit
};

// ── Base station MAC address ──────────────────────────────────────────────────
// Fixed logical MAC spoofed by the base station ESP32 gateway on boot.
// All devices learn this address via MK_Assign and use it as their TX target.
#define MK_BASE_MAC { 0xDE, 0xAD, 0xBE, 0xEF, 0xBA, 0x5E }

// ── FB → Auto / FB → Basis ───────────────────────────────────────────────────
// Sent every control loop iteration (~20ms).
// Values are post-calibration, post-deadzone, post-axis-mapping (-100..100).
// Axis mapping (swap) and trim are applied on the FB/car side — not transmitted.
// Default mapping: LY → throttle, RX → steering.
// Swapped mapping: RY → throttle, LX → steering (set in FB settings menu).
// Welcher ADS-Kanal welche Stick-Richtung ist, steht in fb-firmware (JS_*-Defines).
// Receiver MUST implement a timeout (~200ms): if no packet arrives, set throttle=0.
// The FB stops sending when it detects connection loss — the car must not keep
// the last known throttle active indefinitely.

#define MK_BTN_YELLOW  0x01
#define MK_BTN_GREEN   0x02
#define MK_BTN_BLUE    0x04
#define MK_BTN_RED     0x08

struct MK_ControlInput {
    uint8_t type     = MSG_CONTROL;
    int8_t  throttle;     // -100..100  (forward/backward)
    int8_t  steering;     // -100..100  (left/right)
    uint8_t buttons;      // Bitmask: MK_BTN_YELLOW | MK_BTN_GREEN | MK_BTN_BLUE | MK_BTN_RED
                          // Tastendruck-Ereignis, nicht Haltezustand: die FB setzt ein
                          // Bit erst beim LOSLASSEN einer einzeln gedrueckten Taste,
                          // für 3 Pakete. Mehrtasten-Griffe (Menue: Gruen+Blau) erscheinen
                          // nie. Empfaenger reagieren auf die steigende Flanke.
    uint8_t maxSpeed;     // Player preference 1-10 (game may override)
};

// ── FB → Auto / FB → Basis → Auto ───────────────────────────────────────────
// Sent only when a config value changes (not every loop).
// Car applies values immediately; stores them in its own EEPROM only when
// save=1. save=0 is a live preview (trim menu: wheels move while adjusting).
// The trim belongs to the car — the FB does not persist it, it learns the
// current value from MK_GameFeedback.trim.
// Routing is transparent: car behaves identically whether packet
// arrives directly from FB (Direct Mode) or forwarded by base (Game Mode).
// Base MUST forward MK_ConfigPacket unicast to the paired car — it is not
// consumed by the base itself.

struct MK_ConfigPacket {
    uint8_t type = MSG_CONFIG;
    int8_t  trim;         // Servo trim -10..10, stored in car EEPROM.
                          // Eine Stufe = MK_TRIM_STEP_US, verschiebt die kalibrierte Mitte.
    uint8_t save = 1;     // 1 = persist in car EEPROM, 0 = apply only (preview)
};
#define MK_TRIM_STEP_US  15   // vorher 5 µs — kaum sichtbar (2026-10-04)

// ── Lenkung kalibrieren (FB-Menü) ────────────────────────────────────────────
// Der Servo meldet seine Position nicht. Der Fahrer stellt deshalb mit dem
// Lenk-Stick nacheinander den linken Anschlag, den rechten Anschlag und
// geradeaus ein (FB: Stick gibt die Geschwindigkeit vor, nicht die Position)
// und bestätigt jeweils. Währenddessen schickt die FB SERVO_CAL_PREVIEW mit der
// gewünschten Pulsbreite; das Auto fährt den Servo direkt dorthin, solange
// Previews kommen (Timeout MK_SERVO_PREVIEW_MS, danach wieder normale Lenkung).
// Am Ende SERVO_CAL_SAVE mit allen drei Werten; das Auto prüft und speichert sie
// im EEPROM. Danach bildet es Stick -100..0..100 auf links..mitte..rechts ab,
// der Trim verschiebt die Mitte innerhalb der Anschläge.
// Game Mode: die Basis reicht das Paket wie MK_ConfigPacket unverändert ans Auto.
// Harte Grenzen unabhängig von allem: MK_SERVO_HARD_MIN/MAX.

#define MK_SERVO_HARD_MIN     900
#define MK_SERVO_HARD_MAX     2100
#define MK_SERVO_MIN_SPAN     300    // links↔rechts mindestens so weit auseinander
#define MK_SERVO_PREVIEW_MS   300

enum MK_ServoCalCmd : uint8_t {
    SERVO_CAL_PREVIEW = 1,   // us[0] anfahren (Live-Vorschau)
    SERVO_CAL_SAVE    = 2,   // us[0..2] = links, mitte, rechts speichern
    SERVO_CAL_CANCEL  = 3,   // Vorschau sofort beenden
};

struct MK_ServoCal {
    uint8_t  type = MSG_SERVO_CAL;
    uint8_t  cmd;            // MK_ServoCalCmd
    uint16_t us[3];          // Pulsbreiten in µs
};

// ── Auto → FB / Basis → FB ───────────────────────────────────────────────────
// Game state sent back to the FB for display and haptic feedback.
// In Direct Mode: sent by car. In Game Mode: sent by base station.

struct MK_GameFeedback {
    uint8_t type      = MSG_FEEDBACK;
    uint8_t position;     // Race position 1-8
    uint8_t lap;          // Current lap
    uint8_t lapTotal;     // Total laps
    uint8_t item;         // Active item/booster (0 = none, TBD)
    uint8_t rumble;       // 0=off, 1=on — Basis steuert Dauer über Paketanzahl
    // uint8_t hitByItem; // TODO: item that hit this player (banana, shell, …) — triggers rumble + display hint
    uint8_t carBat;       // 0=leer … 5=voll — vorquantisiert vom Fahrzeug (kein Display-Jitter durch LiPo-Rauschen)
    int8_t  trim;         // aktueller Servo-Trim des Autos (-10..10). Game Mode: Basis reicht den Wert des Autos durch
};

// ── Device registration ───────────────────────────────────────────────────────
// Pairing works identically in both modes — the car does not need to know the mode.
//
// Game Mode:
//   Car sends MK_Beacon (unicast to MK_BASE_MAC) → Base station (DEADBEEF:BA5E) responds with MK_Assign.
//   Car sends all future packets to baseMac (= real base station MAC).
//
// Direct Mode:
//   FB temporarily spoofs MAC to DEADBEEF:BA5E on boot.
//   Car sends MK_Beacon (unicast to MK_BASE_MAC) → FB (acting as base) responds with MK_Assign,
//   baseMac set to FB's real MAC.
//   FB then restores its real MAC.
//   Car sends all future packets to baseMac (= FB's real MAC) — direct link established.
//   FB knows car's real MAC from the beacon sender address.
//
// On replacement hardware: device re-registers automatically, host reassigns slot.
//
// ── Reconnect after connection loss ──────────────────────────────────────────
// Error handling is identical for FB and car:
//
//   1. Control-timeout (~200ms): stop motor / ignore inputs immediately.
//   2. Re-beacon on CURRENT channel — do NOT fall back to MK_ESPNOW_CHANNEL
//      autonomously. The car does not know the mode; falling back would break
//      Game Mode reconnect where the base is not on channel 1 during a race.
//   3. Both FB and car persist the operational channel in EEPROM after every
//      MSG_CHANNEL_SWITCH. On reboot, load the saved channel and beacon there.
//   4. If no pairing response within 5s on the saved channel: clear EEPROM,
//      reboot to MK_ESPNOW_CHANNEL → clean re-registration.
//
// At race end the base sends MSG_CHANNEL_SWITCH with channel=MK_ESPNOW_CHANNEL,
// so all devices land on channel 1 and persist it — the next boot is always
// a clean start with no rejoin attempt.

struct MK_Beacon {
    uint8_t  type       = MSG_BEACON;
    uint8_t  deviceType;  // MK_DeviceType: DEVICE_FB or DEVICE_CAR
    uint8_t  charId;      // 1–8 (car only, matches DFPlayer folder); 0 if not applicable
};

// ── Spieltyp ──────────────────────────────────────────────────────────────────
// Basis legt den Spieltyp VOR dem Pairing fest (erster Bildschirm der Basis-UI)
// und sendet ihn direkt in MK_Assign mit. Die FB wählt darüber in Game Mode
// zwischen STATE_RACE_DISPLAY und STATE_HUNTER_DISPLAY (o.ä.).
// Direct Mode: irrelevant/ignoriert, da keine Basis-Spiellogik vorhanden.

enum MK_GameType : uint8_t {
    GAME_RACE   = 0x01,   // klassisches Rennen (Runde/Position/Item)
    GAME_HUNTER = 0x02,   // Jagd
};

struct MK_Assign {
    uint8_t  type       = MSG_ASSIGN;
    uint8_t  slot;        // Assigned slot 1–8
    uint8_t  baseMac[6]; // Target MAC for all future TX — base station (Game) or FB (Direct)
    uint8_t  gameType;    // MK_GameType — siehe oben
};

// ── Spielmodus "Jagd" (Hunter) ────────────────────────────────────────────────
// Ein Jäger-Auto zeigt WS2812B-Regenbogen + Star-Sound. Rammt der Jäger ein
// anderes Auto, wird das gerammte Auto der neue Jäger.
//
// Erkennung "OB" gerammt wurde: bestehende IMU-Kollisionserkennung (main.cpp,
// checkCollision(), 10Hz) — unverändert, kein neuer Sensor nötig.
//
// Erkennung "WEN" der Jäger gerammt hat (kein Tor/Bodenplatte in diesem Modus,
// daher keine Positionsdaten verfügbar):
//   1. Jedes NICHT-Jäger-Auto sendet bei eigener IMU-Kollision 1s lang einen
//      BLE-Advertising-Beacon (eigene Slot-ID im Payload). BLE nutzt feste
//      Kanäle 37/38/39, unabhängig vom aktuellen ESP-NOW/WiFi-Kanal des Autos
//      (S3-1/2/3 auf CH1/6/11) — Coexistence mit WiFi ist Standard auf dem C6.
//   2. Der Jäger scannt nach eigener IMU-Kollision für 1s nach diesen Beacons
//      und nimmt den Slot mit der höchsten empfangenen RSSI als Opfer an
//      (relativer Vergleich zwischen den empfangenen Beacons, keine absolute
//      Distanzschätzung — deutlich robuster gegen Ground-Bounce/Multipath).
//   3. Jäger meldet das Ergebnis per MK_HunterTag an die Basis.
//   4. Basis bleibt Single Source of Truth: bestätigt den Wechsel per
//      MK_HunterState-Broadcast an alle Autos. Der alte Jäger schaltet sein
//      Signal ERST bei Empfang von MK_HunterState ab (nicht schon beim Senden
//      von MK_HunterTag) — ESP-NOW garantiert keine Zustellung, sonst könnte
//      kurzzeitig niemand Jäger sein, falls MK_HunterTag verloren geht.
//
// Mehrdeutigkeit bei parallelen Unfällen (z.B. zwei Autos crashen gleichzeitig
// an unterschiedlichen Stellen der Strecke): wird durch das BLE-Beacon-Fenster
// weitgehend gefiltert, da ein Auto weit entfernt vom Jäger kein/kaum
// empfangbares Signal liefert. Der Jäger wird NIE zurückgestuft, nur weil
// mehrere Kandidaten kurzzeitig Beacons senden — er hat erfolgreich gerammt,
// das zählt.
//
// Wertung: Basis führt pro Spieler eine kumulative Jäger-Zeit (Sekunden).
// Sieger am Spielende = wer insgesamt am KÜRZESTEN Jäger war.
//
// FB-Display in diesem Modus: keine Runde/Position, stattdessen Spielerliste
// mit Jäger-Kennzeichnung + invertierter eigener Zeile. Für "Live-Feeling"
// zählt die FB die Sekunden des laut letztem Update aktiven Jägers lokal
// zwischen zwei MK_HunterScores-Updates einfach weiter hoch (reine
// Display-Interpolation, auch wenn serverseitig der Jäger evtl. schon
// gewechselt hat) — Basis-Daten selbst bleiben davon unberührt und
// korrigieren die Anzeige beim nächsten Update automatisch.

struct MK_HunterTag {
    uint8_t type          = MSG_HUNTER_TAG;
    uint8_t newHunterSlot; // 1–8: per BLE-RSSI ermitteltes Opfer
};

struct MK_HunterState {
    uint8_t type       = MSG_HUNTER_STATE;
    uint8_t hunterSlot; // 1–8: aktueller, offizieller Jäger
};

struct MK_HunterScores {
    uint8_t  type = MSG_HUNTER_SCORES;
    uint8_t  hunterSlot;        // 1–8: aktueller offizieller Jäger
    uint16_t secondsPerSlot[8]; // kumulierte Jäger-Sekunden, Index = Slot-1; Sieger = niedrigster Wert
};

// ── Firmware-Update über WLAN (OTA) ───────────────────────────────────────────
// Das Auto hat keine Tasten und steckt zum Flashen im Chassis. Deshalb schickt
// es sein Partner in den Update-Modus: im Direct Mode die FB (Menüpunkt
// „Firmware-Update → Auto"), im Game Mode später die Basis.
//
//   1. Partner → Auto: MK_OtaRequest (confirm = MK_OTA_CONFIRM). Das Auto nimmt
//      ihn NUR von seinem gepairten Partner an (Absender == baseMac) — eine
//      fremde FB kann kein fremdes Auto umschalten. Der Partner sendet 3×,
//      einzelne Pakete gehen verloren.
//   2. Auto → Partner: MK_OtaStatus. OTA_ACCEPTED mit Hostname, sonst Grund der
//      Ablehnung. Kommt keine Antwort (1.5 s), gilt das Auto als nicht erreichbar.
//   3. Auto: Motor aus, Servo Mitte, Licht/IR aus. Nur die Status-LED zeigt
//      den Modus: lila blinkend = meldet sich am WLAN an, lila dauerhaft = im
//      WLAN und bereit, rot blinkend = Abbruch, Neustart folgt.
//      ESP-NOW aus, Anmeldung am WLAN (SSID/Passwort in mk_secrets.h), dann
//      ArduinoOTA unter <hostname>.local, Port 3232, ohne Passwort. Den Schutz
//      gibt das Zeitfenster: Firmware wird nur im Update-Modus angenommen.
//   4. Abbruch mit Neustart: kein WLAN nach MK_OTA_WIFI_MS, keine Firmware nach
//      MK_OTA_WAIT_MS, Fehler beim Empfang. Nach erfolgreichem Update ebenfalls
//      Neustart. Ein abgebrochener Download ändert nichts: umgeschaltet wird
//      erst nach geprüfter Prüfsumme, sonst startet die alte Firmware.
//   5. Nach dem Neustart holt das Auto sein Pairing aus dem EEPROM. Der Partner
//      sendet während des ganzen Updates weiter neutrale MSG_CONTROL auf dem
//      Betriebskanal — so hört das Auto ihn innerhalb seiner 5 s nach dem Boot
//      und bleibt ihm zugeordnet. Der Partner wartet bis MK_OTA_PARTNER_MS.
//
// Im Update-Modus gibt der WLAN-Access-Point den Funkkanal vor, ESP-NOW ruht.
// Später strahlt die Basis (RPi5) das WLAN aus.
//
// PC: pio run -e car_ota -t upload --upload-port <hostname>.local
//     (Kart-Board V2: -e car_v2_ota)

#define MK_OTA_CONFIRM     0xA5      // Schutz gegen versehentliches Auslösen
#define MK_OTA_WIFI_MS     20000     // Auto: WLAN-Anmeldung, sonst Neustart
#define MK_OTA_WAIT_MS     90000     // Auto: auf Firmware warten (1.5 min), sonst Neustart.
                                     // Ein Upload dauert ~15 s (2026-10-04, vorher 5 min)
#define MK_OTA_PARTNER_MS  150000    // FB/Basis: auf die Rückkehr des Autos warten (2.5 min):
                                     // WLAN 20 s + Warten 90 s + Upload/Neustart + Reserve

enum MK_OtaState : uint8_t {
    OTA_ACCEPTED   = 1,   // Auto wechselt ins WLAN, hostname gültig
    OTA_REJ_BAT    = 2,   // abgelehnt: Akku zu leer für ein sicheres Update
    OTA_REJ_NOWIFI = 3,   // abgelehnt: Firmware ohne WLAN-Zugangsdaten gebaut
};

struct MK_OtaRequest {
    uint8_t type    = MSG_OTA_REQUEST;
    uint8_t confirm = MK_OTA_CONFIRM;
};

struct MK_OtaStatus {
    uint8_t type  = MSG_OTA_STATUS;
    uint8_t state;          // MK_OtaState
    char    hostname[24];   // z.B. "kart-mario-b924", nullterminiert; nur bei OTA_ACCEPTED
};

// ── Base station architecture (multi-S3) ─────────────────────────────────────
// The Game Mode base station consists of:
//   3× ESP32-S3 (active relays) + 1× ESP32-S3 (hot standby), all USB-connected to RPi5.
//
// Each active S3:
//   • Spoofs MAC to MK_BASE_MAC on boot
//   • Operates on a dedicated non-overlapping channel (S3-1: CH1, S3-2: CH6, S3-3: CH11)
//   • Relays traffic between FB and car — receives MK_ControlInput from FB,
//     applies game effects (speedFactor, invertSteering, maxSpeedOverride),
//     forwards modified packet to car. FB and car firmware are unaware of this.
//   • Receives game state updates from RPi via USB Serial (S3_GameState)
//
// Failover:
//   RPi detects failure via USB disconnect (immediate — no heartbeat timeout needed).
//   RPi sends channel + vehicle assignments + game states to standby S3.
//   Standby spoofs same MAC on same channel → cars/FBs notice nothing, no re-pairing.
//   S3_Heartbeat (500ms) additionally catches firmware hangs (connected but frozen).
//
// See docs/base-station.md for full architecture.

// ── USB Serial protocol (RPi ↔ S3) ───────────────────────────────────────────
// Binary protocol over USB CDC. Every message starts with a uint8_t type.
// Framing: struct size is implicit from the known type — no length byte needed.

enum S3_MsgType : uint8_t {
    S3_ASSIGN     = 0x01,  // RPi → S3: assign a FB+car pair to this S3
    S3_GAMESTATE  = 0x02,  // RPi → S3: update game effects for one car
    S3_STATUS     = 0x03,  // S3 → RPi: relay car feedback (battery, rumble)
    S3_HEARTBEAT  = 0x04,  // S3 → RPi: liveness ping every 500ms
};

struct S3_Assign {
    uint8_t type    = S3_ASSIGN;
    uint8_t slot;           // 1–8
    uint8_t carMac[6];
    uint8_t fbMac[6];
    uint8_t channel;        // Operational channel for this pair
};

struct S3_GameState {
    uint8_t type    = S3_GAMESTATE;
    uint8_t slot;               // 1–8: which car
    int8_t  speedFactor;        // Additive throttle offset -100..100 (malus/bonus)
    uint8_t invertSteering;     // 1 = invert steering, 0 = normal
    uint8_t maxSpeedOverride;   // 1–10 overrides player maxSpeed; 0 = use player value
};

struct S3_Status {
    uint8_t type    = S3_STATUS;
    uint8_t slot;
    uint8_t carBat;   // Relayed from MK_GameFeedback.carBat
    uint8_t rumble;   // Relayed from MK_GameFeedback.rumble
};

struct S3_Heartbeat {
    uint8_t type = S3_HEARTBEAT;
};
