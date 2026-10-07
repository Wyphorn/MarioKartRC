#!/usr/bin/env bash
# Exportiert Bilder und Stücklisten (docs/bom/) fürs Wiki aus den KiCad-Projekten nach docs/images/<board>/.
#
#   kicad/export_images.sh            alle Platinen (aktueller Stand + V1 aus der Git-Historie)
#   kicad/export_images.sh kart       nur eine Platine
#
# Je Platine:
#   schematic.svg / schematic.pdf     Schaltplan
#   layer_top.png / layer_bottom.png  Kupfer + Bestückungsdruck, Unterseite gespiegelt
#   3d_top.jpg / 3d_bottom.jpg / 3d_iso.jpg
#
# Braucht kicad-cli (KiCad 10), rsvg-convert. Die 3D-Renderings dauern je ~10 s.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/docs/images
# Letzter Commit mit Kart- und FB-Platine V1 (vor dem V2-Umbau ab 2026-09-30)
V1_COMMIT=6059d1e

export_board() {   # <name> <projektordner> <basisname>
    local name=$1 dir=$2 base=$3 dst=$OUT/$1
    mkdir -p "$dst"
    echo "== $name"
    ( cd "$dir"
      kicad-cli sch export svg -o "$dst" "$base.kicad_sch" >/dev/null
      mv "$dst/$base.svg" "$dst/schematic.svg"
      kicad-cli sch export pdf -o "$dst/schematic.pdf" "$base.kicad_sch" >/dev/null
      if [[ $name != *_v1 ]]; then
          mkdir -p "$ROOT/docs/bom"
          kicad-cli sch export bom --exclude-dnp \
              --fields 'Reference,Value,Footprint,${QUANTITY},MPN,Manufacturer' \
              --labels 'Referenz,Wert,Footprint,Anzahl,Bestellnummer,Hersteller' \
              --group-by 'Value,Footprint' -o "$ROOT/docs/bom/$name.csv" "$base.kicad_sch" >/dev/null
      fi

      local tmp; tmp=$(mktemp -d)
      kicad-cli pcb export svg --mode-single --page-size-mode 2 --exclude-drawing-sheet \
          -l F.Cu,F.Silkscreen,Edge.Cuts -o "$tmp/top.svg" "$base.kicad_pcb" >/dev/null
      kicad-cli pcb export svg --mode-single --page-size-mode 2 --exclude-drawing-sheet --mirror \
          -l B.Cu,B.Silkscreen,Edge.Cuts -o "$tmp/bottom.svg" "$base.kicad_pcb" >/dev/null
      rsvg-convert -w 1400 -b white "$tmp/top.svg"    -o "$dst/layer_top.png"
      rsvg-convert -w 1400 -b white "$tmp/bottom.svg" -o "$dst/layer_bottom.png"
      rm -rf "$tmp"

      local r=(--width 1600 --height 1200 --quality high --background opaque)
      kicad-cli pcb render "${r[@]}" --side top    -o "$dst/3d_top.jpg"    "$base.kicad_pcb" >/dev/null
      kicad-cli pcb render "${r[@]}" --side bottom -o "$dst/3d_bottom.jpg" "$base.kicad_pcb" >/dev/null
      kicad-cli pcb render "${r[@]}" --perspective --rotate '-40,0,30' --zoom 0.75 \
          -o "$dst/3d_iso.jpg" "$base.kicad_pcb" >/dev/null
    )
}

export_v1() {      # <name> <basisname im alten Pfad docs/kart/>
    local tmp; tmp=$(mktemp -d)
    for e in kicad_pro kicad_sch kicad_pcb; do
        git -C "$ROOT" show "$V1_COMMIT:docs/kart/$2.$e" > "$tmp/$2.$e"
    done
    cp "$ROOT/kicad/projects/kart/fp-lib-table" "$ROOT/kicad/projects/kart/sym-lib-table" "$tmp/"
    sed -i "s|\${KIPRJMOD}/../../libs|$ROOT/kicad/libs|" "$tmp/fp-lib-table" "$tmp/sym-lib-table"
    export_board "$1" "$tmp" "$2"
    rm -rf "$tmp"
}

sel=("$@")
has() { [[ ${#sel[@]} -eq 0 ]] || [[ " ${sel[*]} " == *" $1 "* ]]; }

for b in kart controller buttons switch; do
    has "$b" && export_board "$b" "$ROOT/kicad/projects/$b" "$b"
done
has kart_v1       && export_v1 kart_v1 kart
has controller_v1 && export_v1 controller_v1 controller
echo "fertig: $OUT"
