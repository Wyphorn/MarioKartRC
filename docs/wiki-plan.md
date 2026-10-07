# Wiki-Plan – Arbeitsliste

Steuerdatei für den Aufbau des öffentlichen GitHub-Wikis. **Wird nach jedem Schritt aktualisiert.**
Ablauf: Claude erledigt einen Schritt → meldet sich → gemeinsame Durchsicht → GO für den nächsten.

Status: ✅ fertig · 🔄 in Arbeit / in Durchsicht · ⏳ offen · ⏸ wartet auf User

| # | Schritt | Status |
|---|---|---|
| 0 | Plan erstellen und vorstellen | ✅ |
| 1 | Repo vorbereiten (aufräumen, Lizenz, Prüfung vor Veröffentlichung) | ✅ |
| 2 | Repo öffentlich schalten, Wiki aktivieren, Wiki-Gerüst (Home, Sidebar, Footer) | ✅ |
| 3 | Bilder aus KiCad exportieren (Schaltpläne, Platinen, 3D-Renderings) | ⏳ |
| 4 | Seite: Projektüberblick und Systemarchitektur (inkl. Protokoll) | ⏳ |
| 5 | Seiten: Hardware Fernbedienung | ⏳ |
| 6 | Seiten: Hardware Kart (Kart-Board + Schalter-Platinchen) | ⏳ |
| 7 | Seite: Pannen und Lehren (geflickte Platine, falsche Diode, Durchgeher, …) | ⏳ |
| 8 | Seite: Software Fernbedienung | ⏳ |
| 9 | Seite: Software Kart (inkl. OTA, DFPlayer/SD-Karte) | ⏳ |
| 10 | Seite: Strecke, IR-System, Basisstation (Planungsstand) | ⏳ |
| 11 | Seite: Nachbauen (Stückliste, Bestellen, Flashen, Akku-Regeln) | ⏳ |
| 12 | Fotos und Video einbinden | ⏸ User liefert |
| 13 | README kürzen und aufs Wiki verweisen | ⏳ |
| 14 | Optional: Blog / „Was letzte Woche geschah“ | ⏳ später |

---

## Grundsatzentscheidungen

- **Ort:** GitHub-Wiki (Repo `MarioKartRC.wiki.git`, lokal geklont nach `../MarioKartRC.wiki/`).
- **Bilder** liegen im Hauptrepo unter `docs/images/` und werden im Wiki per
  `https://raw.githubusercontent.com/Wyphorn/MarioKartRC/main/docs/images/...` eingebunden —
  so sind sie versioniert, und das Wiki-Repo bleibt klein.
- **Sprache:** Deutsch. (Englisch ggf. später.) **Leser duzen**, Autor schreibt in Ich-Form.
- **Wiki lokal:** `~/Dokumente/MarioKartRC.wiki/` (eigenes Git-Repo, Identität = noreply wie Hauptrepo).
- **Quelle der Inhalte:** `CLAUDE.md`, `README.md`, `shared/mk_protocol.h`, Firmware, KiCad-Dateien.
  Das Wiki *erklärt*, es kopiert nicht jede Zahl — Datenblattwerte nur, wo sie eine Entscheidung begründen.
- **Ton:** erzählend, ehrlich. Auf der Startseite und im Footer: Projekt entsteht mit KI (Claude),
  Einstieg ohne KiCad-Vorkenntnisse.
- **Versionen:** Jede Hardware-Seite trennt klar V1 (gebaut, mit Macken) und V2 (bestellt 2026-10-07).

## Geplante Wiki-Seiten

```
Home                         – kurz: was, warum, Status, KI-Hinweis
├─ Systemüberblick           – Architektur, Modi, Funkprotokoll
├─ Hardware
│  ├─ Fernbedienung          – Stromversorgung, Verpolschutz, Rumble, Pins, V1→V2
│  ├─ Tasterplatine
│  ├─ Kart-Board             – C6, Buck, Motor, Servo, IMU, Charakter-ID
│  └─ Schalter-Platinchen    – Back-to-back-FETs, Puffer, Status-LED
├─ Software
│  ├─ Firmware Fernbedienung – Menü, Kalibrierung, Display, Akku
│  └─ Firmware Kart          – Fahren, Kollision, Sound, Licht, OTA
├─ Strecke & IR-System       – Planung
├─ Pannen & Lehren
├─ Nachbauen
└─ (Blog)
```

## Entscheidungen des Users (2026-10-07)

1. **Lizenz:** niemand soll damit Geld verdienen, Nachbau erlaubt → **CC BY-NC-SA 4.0** für alles (`LICENSE`).
2. PDFs/DOCX und alles Vorläufige → `.gitignore`. Dateisystem darf frei umsortiert werden.
3. **CLAUDE.md wird nicht veröffentlicht** (interne Gedanken) → aus Git entfernt, in `.gitignore`.
4. Commit-E-Mail: alle Commits nutzen `wyphorn@users.noreply.github.com`, nichts zu tun.
5. Fan-Projekt-Hinweis „nicht Nintendo“ → ja (steht schon in `LICENSE`, kommt auf die Startseite).
6. Wiki-Erstanlage: GitHub legt das Wiki-Repo erst nach der ersten Seite über die Weboberfläche an —
   Claude gibt den Link, User klickt. Sprache: Deutsch.

## Schritt 1 – erledigt

- Sicherheitskopie: `~/Dokumente/MarioKartRC_backup_2026-10-07`
- KiCad neu geordnet: `kicad/projects/{kart,controller,buttons,switch}`, `kicad/libs/`, `kicad/archive/`.
  Jedes Projekt hat eigene `sym-lib-table`/`fp-lib-table` → `${KIPRJMOD}/../../libs/…`.
  `einschalter` → `switch`, Titel „Mario Kart PCB Switch“ Rev. 1. Alter Tasterentwurf `docs/kart/buttons/` verworfen.
  ERC/DRC vor/nach Umzug identisch (Kart: `RF_Module_Own` wird jetzt sogar gefunden).
- AO4407A-Footprint: kaputter 3D-Pfad (`C16072.3dshapes`) → KiCad-Standardmodell SOIC-8.
- Unbenutzte Footprint-Entwürfe `SOT-223_5MC_MCH-L/-M` entfernt.
- Firmware `car`, `car_v2`, `fb`, `fb_v2` baut.

- Nach Durchsicht: AO4407A-3D-Modell gedreht (`rotate 0 0 -90`, per Render geprüft: Beinchen über Pads, Pin-1-Punkt auf Pad 1).
- CLAUDE.md per `git filter-branch` aus der gesamten Historie entfernt, force-gepusht (30 Commits, neue Hashes).
- Globale KiCad-Symboltabelle bereinigt (`zzzz_Own`, `RF_Module_Own`, `zz_own` raus; nur noch KiCad-Standard).

## Merkposten für spätere Schritte

- **Startseite** später ggf. kürzen (User-Eindruck: etwas viel Info). Sidebar-Einträge „(folgt)“ beim Anlegen jeder Seite in echte Links umwandeln.

- **Sounds (Schritt 11):** Die MP3/WAV-Dateien stammen aus dem Internet und werden **nicht** im Repo gehostet.
  Liegen seit 2026-10-07 lokal in `sounds/` (SD-Layout 01–09, 46 MP3 + 2 WAV, `.gitignore`). `09/old_002.mp3` ist ein Überbleibsel.
  User sucht die Quelle, das Wiki verlinkt nur. Dazu eine **Mapping-Tabelle**: Quelldatei → Dateiname auf der SD-Karte →
  Ordner (pro Charakter). Bezug: Memory „SD-Ordner umbenennen“ und
  README-Abschnitt „SD-Karte des DFPlayers“.

## Log

- 2026-10-07: Plan erstellt. Vorab geprüft: Repo privat, Wiki aus, 28 Commits, keine Zugangsdaten
  in der Historie (`mk_secrets.h` nie committet), `kicad-cli` 10.0.6 vorhanden.
- 2026-10-07: Schritt 1 umgesetzt und mit User durchgesehen, Historie bereinigt, gepusht. Wartet auf GO für Schritt 2.
- 2026-10-07: Repo öffentlich, Wiki aktiviert, Beschreibung + Topics gesetzt. Gerüst (Home, _Sidebar, _Footer) als Entwurf im Scratchpad; Push sobald User die erste Wiki-Seite angelegt hat.
- 2026-10-07: Schritt 2 fertig. Wiki-Gerüst gepusht (Home, _Sidebar, _Footer). User: Startseite evtl. zu viel Info, vorerst so lassen. Leser duzen.
