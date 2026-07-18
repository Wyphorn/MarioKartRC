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
//   MSG_CHANNEL_SWITCH to the car. Both persist the channel. On reconnect, FB
//   boots to the persisted channel and waits for the car's beacon there.
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
    uint8_t maxSpeed;     // Player preference 1-10 (game may override)
};

// ── FB → Auto / FB → Basis → Auto ───────────────────────────────────────────
// Sent only when a config value changes (not every loop).
// Car stores values in its own EEPROM and applies them immediately.
// Routing is transparent: car behaves identically whether packet
// arrives directly from FB (Direct Mode) or forwarded by base (Game Mode).
// Base MUST forward MK_ConfigPacket unicast to the paired car — it is not
// consumed by the base itself.

struct MK_ConfigPacket {
    uint8_t type = MSG_CONFIG;
    int8_t  trim;         // Servo trim -10..10, stored in car EEPROM
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

struct MK_Assign {
    uint8_t  type       = MSG_ASSIGN;
    uint8_t  slot;        // Assigned slot 1–8
    uint8_t  baseMac[6]; // Target MAC for all future TX — base station (Game) or FB (Direct)
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
