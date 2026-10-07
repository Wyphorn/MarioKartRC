#!/usr/bin/env python3
"""
Generate docs/fb-schematic.kicad_sch  -  Mario Kart RC Fernbedienung
All modules shown as labeled connector blocks (abstracted, no exact symbols needed).
Run from repo root:  python3 docs/generate_schematic.py
"""
import uuid, os

def uid(): return str(uuid.uuid4())

P = 2.54  # standard grid pitch (mm)

# ── Symbol registry (name → ordered pin-name list) ─────────────────────────
# Note: KiCad 10 does NOT accept colons in embedded lib_symbols names.
# Use underscore-only names (e.g. FB_D1Mini, not MOD:D1Mini).
SYMS = {}

def reg(name, pins):
    SYMS[name] = list(pins)

reg("FB_D1Mini",    ["RST","A0","D0","D1_SCL","D2_SDA","D3","D4","D5","D6","D7","D8","GND","3V3","5V"])
reg("FB_ADS1115",   ["VDD","GND","SDA","SCL","A0","A1","A2","A3"])
reg("FB_OLED",      ["VCC","GND","SCL","SDA"])
reg("FB_WS2812B",   ["VCC","GND","DIN"])
reg("FB_Joystick",  ["VCC","GND","VRx","VRy"])
reg("FB_BatShield", ["VBAT","5V","GND"])
reg("FB_SW3pos",    ["COM_A","L_A","R_A","COM_B","L_B","R_B"])
reg("FB_Motor",     ["+","-"])
reg("FB_Button",    ["SIG","GND"])
reg("FB_R",         ["1","2"])
reg("FB_NPN",       ["B","C","E"])
reg("FB_Diode",     ["A","K"])
reg("FB_Cap",       ["1","2"])

# ── Collectors for schematic elements ──────────────────────────────────────
placed_syms = []
net_labels  = {}   # key=(lx,ly) → (net, lx, ly)

def place(lib_id, ref, val, x, y, nets):
    """Place a symbol at (x,y) and register net labels at its pin endpoints."""
    n = len(nets)
    lines = [
        f'  (symbol (lib_id "{lib_id}") (at {x:.2f} {y:.2f} 0) (unit 1)',
        f'    (in_bom yes) (on_board yes) (dnp no)',
        f'    (uuid "{uid()}")',
        f'    (property "Reference" "{ref}" (at {x+3:.2f} {y-1.27:.2f} 0)',
        f'      (effects (font (size 1.27 1.27))))',
        f'    (property "Value" "{val}" (at {x+3:.2f} {y+(n-0.5)*P:.2f} 0)',
        f'      (effects (font (size 1.27 1.27))))',
        f'    (property "Footprint" "" (at {x:.2f} {y:.2f} 0)',
        f'      (effects (font (size 1.27 1.27)) (hide yes)))',
    ]
    for i in range(n):
        lines.append(f'    (pin "{i+1}" (uuid "{uid()}"))')
    lines.append('  )')
    placed_syms.append('\n'.join(lines))

    for i, net in enumerate(nets):
        if not net:
            continue
        lx = round(x - 3.81, 2)
        ly = round(y + i * P, 2)
        k  = (lx, ly)
        if k not in net_labels:
            net_labels[k] = (net, lx, ly)

# ── Build external symbol library (.kicad_sym) ──────────────────────────────
def build_sym_lib():
    """Generates fb-components.kicad_sym content. Symbol names have NO lib prefix here."""
    parts = [
        '(kicad_symbol_lib',
        '  (version 20220914)',
        '  (generator kicad_symbol_editor)',
    ]
    for name, pins in SYMS.items():
        n   = len(pins)
        top =  P / 2
        bot = -(n - 1) * P - P / 2
        ref_letter = ("C" if "Cap" in name else "Q" if "NPN" in name
                      else "D" if "Diode" in name else "R" if name == "FB_R"
                      else "SW" if "SW" in name or "Button" in name
                      else "MOT" if "Motor" in name else "J")
        parts.append(f'  (symbol "{name}"')
        parts.append(f'    (pin_names (offset 0.508)) (in_bom yes) (on_board yes)')
        parts.append(f'    (property "Reference" "{ref_letter}" (at 2.54 -1.27 0)')
        parts.append(f'      (effects (font (size 1.27 1.27))))')
        parts.append(f'    (property "Value" "{name}" (at 2.54 1.27 0)')
        parts.append(f'      (effects (font (size 1.27 1.27))))')
        parts.append(f'    (property "Footprint" "" (at 0 0 0)')
        parts.append(f'      (effects (font (size 1.27 1.27)) (hide yes)))')
        parts.append(f'    (property "Datasheet" "" (at 0 0 0)')
        parts.append(f'      (effects (font (size 1.27 1.27)) (hide yes)))')
        parts.append(f'    (symbol "{name}_0_1"')
        parts.append(f'      (rectangle (start -1.27 {top:.3f}) (end 1.27 {bot:.3f})')
        parts.append(f'        (stroke (width 0) (type default)) (fill (type background))))')
        parts.append(f'    (symbol "{name}_1_1"')
        for i, pn in enumerate(pins):
            py = -i * P
            parts.append(f'      (pin bidirectional line (at -3.81 {py:.3f} 0) (length 2.54)')
            parts.append(f'        (name "{pn}" (effects (font (size 1.016 1.016))))')
            parts.append(f'        (number "{i+1}" (effects (font (size 1.016 1.016)))))')
        parts.append('    ))')
    parts.append(')')
    return '\n'.join(parts)

def build_lib_symbols():
    parts = ['  (lib_symbols']
    for name, pins in SYMS.items():
        n   = len(pins)
        top =  P / 2
        bot = -(n - 1) * P - P / 2
        parts.append(f'    (symbol "{name}"')
        parts.append(f'      (pin_names (offset 0.508)) (in_bom yes) (on_board yes)')
        parts.append(f'      (symbol "{name}_0_1"')
        parts.append(f'        (rectangle (start -1.27 {top:.3f}) (end 1.27 {bot:.3f})')
        parts.append(f'          (stroke (width 0) (type default)) (fill (type background))))')
        parts.append(f'      (symbol "{name}_1_1"')
        for i, pn in enumerate(pins):
            py = -i * P
            parts.append(f'        (pin bidirectional line (at -3.81 {py:.3f} 0) (length 2.54)')
            parts.append(f'          (name "{pn}" (effects (font (size 1.016 1.016))))')
            parts.append(f'          (number "{i+1}" (effects (font (size 1.016 1.016)))))')
        parts.append('      ))')
    parts.append('  )')
    return '\n'.join(parts)

# ── Build net labels section ────────────────────────────────────────────────
def build_labels():
    lines = []
    for net, lx, ly in net_labels.values():
        lines.append(
            f'  (label "{net}" (at {lx:.2f} {ly:.2f} 180)\n'
            f'    (effects (font (size 1.27 1.27)) (justify right))\n'
            f'    (uuid "{uid()}"))'
        )
    return '\n'.join(lines)

# ═══════════════════════════════════════════════════════════════════════════
# Component placement
# Layout: D1 Mini left-center, peripherals right, passives bottom
# ═══════════════════════════════════════════════════════════════════════════

# D1 Mini
place("FB_D1Mini", "U1", "D1_Mini", 50, 50, [
    "nRST", "A0_BAT", "D0_MODE", "SCL", "SDA",
    "D3_BTN4", "D4_WS", "D5_BTN1", "D6_BTN2", "D7_BTN3",
    "D8_RUM", "GND", "VCC_3V3", "VCC_5V",
])

# ADS1115 (I2C 0x48, ADDR pin → GND)
place("FB_ADS1115", "U2", "ADS1115_0x48", 120, 20, [
    "VCC_3V3", "GND", "SDA", "SCL",
    "LX_DIV", "LY_DIV", "RX_DIV", "RY_DIV",
])

# SSD1306 OLED (I2C 0x3C)
place("FB_OLED", "U3", "SSD1306_0x3C", 120, 60, [
    "VCC_3V3", "GND", "SCL", "SDA",
])

# WS2812B status LED
place("FB_WS2812B", "LED1", "WS2812B", 120, 78, [
    "VCC_3V3", "GND", "D4_WS",
])

# Buttons B1–B4  (SIG → GPIO, other side → GND)
for i, (net, lbl) in enumerate([
    ("D5_BTN1", "BTN1"), ("D6_BTN2", "BTN2"),
    ("D7_BTN3", "BTN3"), ("D3_BTN4", "BTN4"),
]):
    place("FB_Button", f"SW{i+1}", lbl, 120, 95 + i * 8, [net, "GND"])

# Left Joystick (5V VCC, outputs divided down by R network)
place("FB_Joystick", "JOY1", "Joystick_Left", 30, 130, [
    "VCC_5V", "GND", "LX_RAW", "LY_RAW",
])

# Right Joystick
place("FB_Joystick", "JOY2", "Joystick_Right", 30, 152, [
    "VCC_5V", "GND", "RX_RAW", "RY_RAW",
])

# Voltage dividers: 4.7k + 4.7k per axis (5V → ~2.5V for ADS1115)
# R_xA: RAW_SIGNAL → DIV_NET
# R_xB: DIV_NET → GND
for i, (ax, raw, div) in enumerate([
    ("LX", "LX_RAW", "LX_DIV"),
    ("LY", "LY_RAW", "LY_DIV"),
    ("RX", "RX_RAW", "RX_DIV"),
    ("RY", "RY_RAW", "RY_DIV"),
]):
    bx = 80 + i * 18
    place("FB_R", f"R_{ax}A", "4k7", bx, 140, [raw, div])
    place("FB_R", f"R_{ax}B", "4k7", bx, 148, [div, "GND"])

# Battery Shield v1.2.0
place("FB_BatShield", "BAT1", "BatteryShield_v1.2", 30, 178, [
    "VBAT", "VCC_5V", "GND",
])

# 100k: VBAT → A0 for battery voltage measurement
place("FB_R", "R_BAT", "100k", 55, 185, ["VBAT", "A0_BAT"])

# 220µF electrolytic on 5V rail (Rumble inrush buffer)
place("FB_Cap", "C_BULK", "220uF", 75, 185, ["VCC_5V", "GND"])

# 3-position DPDT power/mode switch
# Pole A (power): COM=VBAT, L=VCC_5V (Game), R=VCC_5V (Direct) – both sides power shield
# Pole B (mode):  COM=D0_MODE, L=GND (Game, D0 LOW), R=VCC_3V3 (Direct, D0 HIGH)
place("FB_SW3pos", "SW_PWR", "Switch_3pos_DPDT", 30, 105, [
    "VBAT",    "VCC_5V",  "VCC_5V",
    "D0_MODE", "GND",     "VCC_3V3",
])

# ── Rumble Motor 1 (linke Spalte) ───────────────────────────────────────────
# D8_RUM → 1k → Q_B; Q_C → Motor(-); Motor(+) = VBAT; Flyback: A=Q_C, K=VBAT
# Motors powered directly from VBAT to avoid 5V boost impedance issues
place("FB_R",     "R_B1",  "1k",            160, 145, ["D8_RUM", "Q1B"])
place("FB_NPN",   "Q1",    "2N2222",         175, 145, ["Q1B",    "Q1C",  "GND"])
place("FB_Diode", "D_F1",  "1N4001_flyback", 175, 130, ["Q1C",    "VBAT"])
place("FB_Motor", "MOT1",  "Rumble_Motor_1", 175, 118, ["VBAT",   "Q1C"])
place("FB_Cap",   "C_M1",  "100nF",          192, 118, ["VBAT",   "Q1C"])

# ── Rumble Motor 2 (rechte Spalte, 50mm versetzt) ───────────────────────────
place("FB_R",     "R_B2",  "1k",            210, 145, ["D8_RUM", "Q2B"])
place("FB_NPN",   "Q2",    "2N2222",         225, 145, ["Q2B",    "Q2C",  "GND"])
place("FB_Diode", "D_F2",  "1N4001_flyback", 225, 130, ["Q2C",    "VBAT"])
place("FB_Motor", "MOT2",  "Rumble_Motor_2", 225, 118, ["VBAT",   "Q2C"])
place("FB_Cap",   "C_M2",  "100nF",          242, 118, ["VBAT",   "Q2C"])

# ═══════════════════════════════════════════════════════════════════════════
# Assemble and write schematic
# ═══════════════════════════════════════════════════════════════════════════

schematic = f"""\
(kicad_sch
  (version 20260306)
  (generator "fb_schematic_gen")
  (generator_version "1.0")
  (uuid "{uid()}")
  (paper "A2")

{build_lib_symbols()}

{chr(10).join(placed_syms)}

{build_labels()}

  (sheet_instances
    (path "/"
      (page "1")))
)"""

docs = os.path.dirname(os.path.abspath(__file__))

# Schematic
sch_path = os.path.join(docs, "fb-schematic.kicad_sch")
with open(sch_path, "w") as f:
    f.write(schematic)
print(f"Written: {sch_path}")

# External symbol library
sym_path = os.path.join(docs, "fb-components.kicad_sym")
with open(sym_path, "w") as f:
    f.write(build_sym_lib())
print(f"Written: {sym_path}")

# Project sym_lib_table (KiCad reads this when .kicad_pro exists in same dir)
lib_table = """\
(sym_lib_table
  (lib (name "FB_components") (type "KiCad") (uri "${KIPRJMOD}/fb-components.kicad_sym") (options "") (descr "Mario Kart RC FB-Firmware Symbole"))
)"""
table_path = os.path.join(docs, "sym-lib-table")
with open(table_path, "w") as f:
    f.write(lib_table)
print(f"Written: {table_path}")

# Minimal KiCad project file (required for sym-lib-table to be picked up)
pro = '{\n  "meta": {\n    "filename": "fb-schematic.kicad_pro",\n    "version": 1\n  }\n}\n'
pro_path = os.path.join(docs, "fb-schematic.kicad_pro")
with open(pro_path, "w") as f:
    f.write(pro)
print(f"Written: {pro_path}")
