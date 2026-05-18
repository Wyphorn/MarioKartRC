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
};

// ── Device types ─────────────────────────────────────────────────────────────
enum MK_DeviceType : uint8_t {
    DEVICE_FB  = 0x01,
    DEVICE_CAR = 0x02,
};

// ── Channel configuration ─────────────────────────────────────────────────────
// All devices boot on MK_ESPNOW_CHANNEL for registration (beacon/assign flow).
// After all devices are registered, the base station (RPi-triggered) scans for
// the least congested channel and broadcasts MSG_CHANNEL_SWITCH to all peers.
// Race data transmission begins after the channel switch, on the optimal channel.
#define MK_ESPNOW_CHANNEL  1   // Fixed registration channel — all devices start here

struct MK_ChannelSwitch {
    uint8_t type    = MSG_CHANNEL_SWITCH;
    uint8_t channel;  // New channel 1–13, selected by base station after scan
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

struct MK_ControlInput {
    uint8_t type     = MSG_CONTROL;
    int8_t  throttle;     // -100..100  (forward/backward)
    int8_t  steering;     // -100..100  (left/right)
    uint8_t buttons;      // Bit 0=Yellow, Bit 1=Green, Bit 2=Blue, Bit 3=Red
    uint8_t maxSpeed;     // Player preference 1-10 (game may override)
};

// ── FB → Auto / FB → Basis → Auto ───────────────────────────────────────────
// Sent only when a config value changes (not every loop).
// Car stores values in its own EEPROM and applies them immediately.
// Routing is transparent: car behaves identically whether packet
// arrives directly from FB (Direct Mode) or forwarded by base (Game Mode).

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
    uint8_t rumble;       // Rumble command (0=off, 1=short, 2=long, TBD)
    uint8_t speedLimit;   // Game-imposed speed limit 1-10 (10 = no limit)
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

struct MK_Beacon {
    uint8_t  type       = MSG_BEACON;
    uint8_t  deviceType;  // MK_DeviceType: DEVICE_FB or DEVICE_CAR
};

struct MK_Assign {
    uint8_t  type       = MSG_ASSIGN;
    uint8_t  slot;        // Assigned slot 1–8
    uint8_t  baseMac[6]; // Target MAC for all future TX — base station (Game) or FB (Direct)
};
