#pragma once
#include <stdint.h>

// ── Packet type discriminator ────────────────────────────────────────────────
// Every packet starts with a uint8_t type field.
// Receiver reads type first, then casts to the appropriate struct.

enum MK_MsgType : uint8_t {
    MSG_CONTROL  = 0x01,   // FB → Auto (Direct) | FB → Basis (Game)  — high frequency
    MSG_CONFIG   = 0x02,   // FB → Auto (Direct) | FB → Basis → Auto  — on change only
    MSG_FEEDBACK = 0x03,   // Auto → FB (Direct) | Basis → FB (Game)  — high frequency
};

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
    uint8_t buttons;      // Bit 0=B1, Bit 1=B2, Bit 2=B3, Bit 3=B4
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
