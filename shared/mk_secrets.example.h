#pragma once
// Vorlage für mk_secrets.h — kopieren nach shared/mk_secrets.h und anpassen.
// mk_secrets.h steht in .gitignore und wird nicht eingecheckt.
//
// WLAN, in das sich die Autos im Update-Modus einwählen (siehe OTA-Abschnitt in
// mk_protocol.h). Später strahlt die Basis (RPi5) dieses WLAN aus.
// Ohne mk_secrets.h baut die Firmware trotzdem, lehnt Updates aber ab
// (OTA_REJ_NOWIFI).

#define MK_WIFI_SSID  "Mario Kart"
#define MK_WIFI_PASS  "hier-das-passwort"
