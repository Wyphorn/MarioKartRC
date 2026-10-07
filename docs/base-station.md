# Basisstation – Architektur

## Überblick

Die Basisstation besteht aus einem Raspberry Pi 5 und vier ESP32-S3-Modulen, alle
per USB verbunden. Drei S3s sind aktiv, einer ist Standby.

```
                    RPi 5
                   /  |  \ \
            USB   /   |   \ \
                 /    |    \ \
              S3-1  S3-2  S3-3  S3-4(Standby)
              CH1   CH6   CH11
```

## ESP32-S3 als Relay

Jeder aktive S3:
- Spooft seine MAC auf `MK_BASE_MAC` (DEADBEEF:BA5E) beim Start
- Operiert auf einem dedizierten, nicht-überlappenden Kanal
- Verwaltet die ihm zugewiesenen FB+Auto-Paare
- Empfängt `MK_ControlInput` von der FB, wendet Spieleffekte an, leitet
  das modifizierte Paket ans Auto weiter
- Empfängt Spielzustands-Updates vom RPi per USB Serial

### Kanalverteilung

| S3   | Kanal | Nicht-überlappend |
|------|-------|-------------------|
| S3-1 | 1     | ✓                 |
| S3-2 | 6     | ✓                 |
| S3-3 | 11    | ✓                 |

Kanalwahl CH1/6/11 entspricht den drei nicht-überlappenden 2.4GHz-Kanälen.
Bis zu 8 Fahrzeuge werden gleichmäßig auf die drei S3s aufgeteilt (~2–3 pro S3).

### Input-Modifikation (Spieleffekte)

Der S3 sitzt als transparenter Mittelsmann zwischen FB und Auto.
FB- und Auto-Firmware bleiben unverändert.

```
FB  →  MK_ControlInput (original)  →  S3  →  MK_ControlInput (modifiziert)  →  Auto
```

Mögliche Modifikationen (per `S3_GameState` vom RPi konfiguriert):
- `speedFactor`: additiver Throttle-Offset (-100..100), für Bonus/Malus
- `invertSteering`: Lenkung invertieren (Bananenschale, etc.)
- `maxSpeedOverride`: maximale Geschwindigkeit überschreiben (1–10, 0 = Spieler-Wert)

## Failover

Der RPi erkennt den Ausfall eines S3 **sofort** über den USB-Disconnect-Event –
kein Heartbeat-Timeout nötig.

Failover-Ablauf:
1. RPi erkennt USB-Disconnect von S3-x
2. RPi sendet Konfiguration des ausgefallenen S3 an S3-4 (Standby):
   - Kanal
   - Zugewiesene Fahrzeuge (MAC, Slot, Spielzustand)
3. S3-4 spooft dieselbe MAC (`MK_BASE_MAC`) auf demselben Kanal
4. Autos und FBs bemerken nichts – gleiche MAC, gleicher Kanal

Hinweis: `S3_Heartbeat` (alle 500ms) ergänzt den USB-Disconnect für den Fall
dass ein S3 zwar verbunden aber eingefroren ist (Firmware-Hang ohne Reboot).

## USB Serial Protokoll (RPi ↔ S3)

Binäres Protokoll über USB CDC. Jede Nachricht beginnt mit einem `uint8_t`-Typ.
Framing: Länge wird durch den bekannten Struct-Typ impliziert (kein Length-Byte).

Siehe `shared/mk_protocol.h` für Struct-Definitionen (`S3_*`).

### RPi → S3

| Typ           | Wann                                          |
|---------------|-----------------------------------------------|
| `S3_ASSIGN`   | Nach Pairing eines FB+Auto-Paares             |
| `S3_GAMESTATE`| Bei Spielereignis (Item, Kollision, Runde...) |

### S3 → RPi

| Typ           | Wann                          |
|---------------|-------------------------------|
| `S3_STATUS`   | Jedes Mal wenn Auto-Feedback eintrifft (5Hz) |
| `S3_HEARTBEAT`| Alle 500ms                    |
