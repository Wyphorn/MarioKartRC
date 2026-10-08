#!/usr/bin/env python3
"""Erzeugt die Teilelisten (Markdown) für die Wiki-Seiten der Platinen.
Werte/Bauformen kommen aus der KiCad-Netzliste, Funktion und Bestellnummer
sind hier gepflegt. Prüft, dass jedes Bauteil genau einmal vorkommt — kommt
nach einer Schaltplanänderung ein Bauteil dazu, bricht das Skript ab und
nennt es; dann in ROWS eintragen.

Aufruf (Netzlisten vorher exportieren):
  for b in kart controller buttons switch; do
    kicad-cli sch export netlist --format kicadxml -o /tmp/nl_$b.xml kicad/projects/$b/$b.kicad_sch
  done
  python3 kicad/wiki_partlist.py /tmp
Ausgabe: Markdown-Tabellen auf stdout und <dir>/partlists.json —
von Hand in die Wiki-Seiten (Abschnitt „Teileliste“) übernehmen."""
import re, sys, xml.etree.ElementTree as ET

S = sys.argv[1]

def bauform(fp):
    fp = fp.split(':')[-1]
    rules = [
        (r'^[CR]_0805', '0805'), (r'^[CR]_1206', '1206'),
        (r'^CP_Elec_6\.3x5\.4', 'Elko SMD Ø6,3 × 5,4 mm'),
        (r'^CP_Elec_12\.5x13\.5', 'Elko SMD Ø12,5 × 13,5 mm'),
        (r'^SOT-23_', 'SOT-23'), (r'^TSOT-23-6', 'TSOT-23-6'),
        (r'^SOT-223_5', 'SOT-223-5'), (r'^SOIC-8', 'SOIC-8'),
        (r'^D_SMA', 'SMA'), (r'^LED_WS2812B', 'PLCC-4, 5 × 5 mm'),
        (r'^L_Bourns_SRN6045', 'SMD 6 × 6 mm'),
        (r'^TestPoint_Pad_D2', 'Lötpad Ø2 mm'),
        (r'^JST_XH_B(\d)B-XH-A_.*Vertical', r'JST-XH \1-polig, stehend'),
        (r'^JST_XH_S(\d)B-XH-A-1_.*Horizontal', r'JST-XH \1-polig, liegend'),
        (r'^IDC-Header_2x04', 'Wannenstecker 2×4, RM 2,54'),
        (r'^SolderWire-0\.75sqmm', 'Lötbohrungen für 0,75 mm²'),
        (r'^PinHeader_1x(\d+)_P2\.54mm', r'Stiftleiste 1×\1, RM 2,54'),
        (r'^PinHeader_2x(\d+)_P2\.54mm', r'Stiftleiste 2×\1, RM 2,54'),
        (r'^SW_DIP_SPSTx04_Piano', 'DIP-Schalter 4-fach, Piano'),
        (r'^SW_Slide_2P3T', 'Schiebeschalter 2P3T, 8 Pins'),
        (r'^ESP32-C6-SuperMini', 'Modul, 2 × Stiftleiste 1×10'),
        (r'^Pins_1x02_P5\.0mm', '2 Drahtstifte, RM 5,0'),
        (r'^SW_Tactile_12x12mm', 'Taster 12 × 12 mm, THT'),
    ]
    for pat, rep in rules:
        if re.search(pat, fp):
            return re.sub(r'^.*$', lambda m: re.sub(pat, rep, fp) if '\\' in rep else rep, fp) if False else (re.match(pat, fp).expand(rep) if '\\' in rep else rep)
    return fp

VAL = {'Beleuchtung': 'MMBT2222A', 'D_Schottky': 'SS14', 'C_Polarized': '2200 µF / 10 V',
       'Conn_01x05_Pin': '–', '22 µF': '22 µF', '4,7µF': '4,7 µF', '1K': '1k',
       'SW_DP3T': '2P3T', 'Characterwähler': '4-fach'}

ROWS = {
'kart': [
    ('U1', 'Mikrocontroller', 'Modul, siehe [[Nachbauen]]'),
    ('U6', '5-V-Schaltregler, 2 A', 'AP63205WU-7 (Diodes)'),
    ('L1', 'Spule des 5-V-Reglers', 'SRN6045-4R7Y (Bourns)'),
    ('C1', 'Eingang 5-V-Regler', ''),
    ('C2', 'Ausgang 5-V-Regler', ''),
    ('C3', 'Bootstrap des Reglers (BST–SW)', ''),
    ('C6', 'Stütze 5-V-Schiene', ''),
    ('C10', 'Stütze 5-V-Schiene', ''),
    ('C5', 'Puffer am Servo', 'Markentyp, ≥ 10 V'),
    ('C8', 'Puffer am Servo', ''),
    ('R16', 'Spannungsteiler Akku-Messung, oben', ''),
    ('R14', 'Spannungsteiler Akku-Messung, unten', ''),
    ('C7', 'Filter Akku-Messung', ''),
    ('SW1', 'Figurenwahl', ''),
    ('R9', 'Figurenwahl: Widerstand nach 3,3 V', ''),
    ('R8 R10 R13 R15', 'Figurenwahl: je einer pro DIP-Schalter', ''),
    ('C9', 'Filter Figurenwahl', ''),
    ('R11 R12', 'Pulldown an den Motor-PWM-Leitungen', ''),
    ('R17', 'Serienwiderstand LED-Daten', ''),
    ('Q1', 'Schalter IR-LEDs', ''),
    ('Q2', 'Schalter Front- und Hecklicht', ''),
    ('Q3', 'Schalter Rückfahrlicht', ''),
    ('R1 R2 R3', 'Basiswiderstände Q1–Q3', ''),
    ('R4 R5 R6', 'Basis-Pulldowns Q1–Q3', ''),
    ('U2', 'Soundmodul DFPlayer Mini (Steckplatz)', 'Modul, siehe [[Nachbauen]]'),
    ('R7', 'Serienwiderstand UART zum DFPlayer', ''),
    ('U3', 'Lagesensor LSM6DS3 (Steckplatz)', 'Modul, siehe [[Nachbauen]]'),
    ('J2', 'Servo', ''),
    ('J3', 'Flachbandkabel zum Schalter-Platinchen', ''),
    ('J4', 'Batteriespannung vom Schalter-Platinchen', '20-AWG-Litze'),
    ('J5', 'Lautsprecher', ''),
    ('J7', 'IR-LEDs', ''),
    ('J8', 'Front- und Hecklicht', ''),
    ('J9', 'Rückfahrlicht', ''),
    ('+Batt1 +5V1 +3V3 GND1', 'Messpunkte (kein Bauteil)', ''),
],
'controller': [
    ('U2', 'Mikrocontroller', 'Modul, siehe [[Nachbauen]]'),
    ('U3', '3,3-V-LDO', 'MCP1826T-3302E/DC (Microchip)'),
    ('C1', 'Eingang LDO', ''),
    ('C2', 'Ausgang LDO', ''),
    ('C5 C6 C8 C10', 'Stütze 3,3-V-Schiene', ''),
    ('C7 C9 C15', 'Stütze 3,3-V-Schiene', 'X7R'),
    ('Q1 Q3', 'Verpolschutz und Hauptschalter (gegeneinander)', 'DMG2301L-7 (Diodes)'),
    ('R1', 'Gate-Widerstand Q1/Q3', ''),
    ('SW2', 'Ein/Aus und Modus (Game/Direct)', ''),
    ('R4 R5', 'Spannungsteiler Akku-Messung', ''),
    ('C4', 'Filter Akku-Messung', ''),
    ('J8', 'ADS1115-Breakout (Steckplatz)', 'Modul, siehe [[Nachbauen]]'),
    ('R8 R9 R10 R11 R12 R13 R14 R15', 'Mittenteiler an den vier ADS-Eingängen (je 2)', ''),
    ('C11 C12 C13 C14', 'Filter an den vier ADS-Eingängen', ''),
    ('J2', 'linker Stick', ''),
    ('J3', 'rechter Stick', ''),
    ('J4', 'Display', ''),
    ('R16 R17', 'Serienwiderstände SPI-Takt und -Daten', ''),
    ('J1', 'Tasterplatine', ''),
    ('R18 R19 R20 R21', 'Pull-ups der vier Tasten', ''),
    ('D1', 'Status-LED', ''),
    ('R6', 'Serienwiderstand LED-Daten', ''),
    ('J9', 'Ausgang LED-Kette: reserviert für weitere WS2812B, z. B. indirekte Beleuchtung des Gehäuses', ''),
    ('Q2', 'Schalter Rumble-Motoren', ''),
    ('R3', 'Basiswiderstand Q2', ''),
    ('R7', 'Basis-Pulldown Q2', ''),
    ('D2', 'Freilaufdiode Rumble', ''),
    ('C3', 'Puffer für den Rumble-Anlaufstrom', 'Markentyp, ≥ 10 V'),
    ('J6 J7', 'Rumble-Motoren', ''),
    ('J5', 'Akku 18650', '20-AWG-Litze'),
],
'switch': [
    ('Q1 Q2', 'Verpolschutz und Hauptschalter (gegeneinander)', 'AO4407A (Alpha & Omega)'),
    ('R1', 'Gate-Widerstand Q1/Q2', ''),
    ('SW1', 'Hauptschalter', ''),
    ('D2', 'Trenndiode zum Puffer', ''),
    ('C3', 'Puffer gegen Kontaktabrisse (optional)', 'EEEFK1A222AQ (Panasonic)'),
    ('D1', 'Status-LED', ''),
    ('C1', 'Stütze 5 V an der LED', ''),
    ('C2', 'Stütze 5 V', ''),
    ('J5', 'Akku 2S', '20-AWG-Litze'),
    ('J2', 'B+ / B− zum BTS7960 (direkt gesteckt)', 'Kupferdraht'),
    ('J6', 'Signale zum BTS7960 (direkt gesteckt)', ''),
    ('J1', 'Flachbandkabel vom Kart-Board', ''),
    ('J3', 'Batteriespannung zum Kart-Board', '20-AWG-Litze'),
    ('J4', 'Figur (10× WS2812B)', ''),
    ('+Batt1 +5V1 GND1', 'Messpunkte (kein Bauteil)', ''),
],
'buttons': [
    ('SW1', 'Taste Rot', 'siehe [[Nachbauen]]'), ('SW2', 'Taste Gelb', 'siehe [[Nachbauen]]'),
    ('SW3', 'Taste Blau', 'siehe [[Nachbauen]]'), ('SW4', 'Taste Grün', 'siehe [[Nachbauen]]'),
    ('J1', 'Kabel zur Fernbedienung', ''),
],
}

def fmt_val(v):
    v = VAL.get(v, v)
    v = re.sub(r'(\d)\.(\d)', r'\1,\2', v)
    v = re.sub(r'(\d)(µF|nF|uF|µH)', r'\1 \2', v).replace('uF', 'µF')
    v = v.replace('MΩ', 'M').replace('Ω', '')
    return v

out = {}
for board, rows in ROWS.items():
    r = ET.parse(f'{S}/nl_{board}.xml').getroot()
    comps = {c.get('ref'): (c.findtext('value'), c.findtext('footprint') or '') for c in r.iter('comp')}
    seen = []
    lines = ['| Referenz | Funktion | Wert | Bauform | Anzahl | Bestellnummer / Hinweis |', '|---|---|---|---|---|---|']
    for refs, func, mpn in rows:
        rl = refs.split()
        for x in rl:
            assert x in comps, (board, x)
        vl = [fmt_val(comps[x][0]) for x in rl]; bfs = {bauform(comps[x][1]) for x in rl}
        assert len(bfs) == 1, (board, refs, bfs)
        val = vl[0] if len(set(vl)) == 1 else ' / '.join(vl); bf = bfs.pop()
        if func.startswith('Messpunkte') or rl[0].startswith('J') or board == 'buttons': val = '–'
        lines.append(f'| {", ".join(rl)} | {func} | {val} | {bf} | {len(rl)} | {mpn} |')
        seen += rl
    missing = set(comps) - set(seen); dup = {x for x in seen if seen.count(x) > 1}
    assert not missing and not dup, (board, missing, dup)
    out[board] = '\n'.join(lines)
    print(f'== {board}: {len(comps)} Bauteile\n' + out[board] + '\n')
import json; json.dump(out, open(f'{S}/partlists.json', 'w'), ensure_ascii=False)
