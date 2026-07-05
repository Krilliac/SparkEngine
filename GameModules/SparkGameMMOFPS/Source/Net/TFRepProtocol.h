/**
 * @file TFRepProtocol.h
 * @brief Private wire structs for the TF replication channel (owned by the
 *        server-sim/replication agent — NOT part of the frozen TFMsg enum).
 *
 * TFMsg (TFNetProtocol.h) carries game events/commands. Entity STATE flows
 * through EntityReplicator delta packets, which need their own transport
 * message ids. Those ids live here, in the 0x54F0 block, deliberately outside
 * the frozen TFMsg enum so the contract file stays untouched.
 *
 * Channel map (all S->C):
 *   0x54F0 RepCreate  reliable    TF_RepPawnCreate      (full pawn state on spawn / late join)
 *   0x54F1 RepUpdate  unreliable  TF_RepUpdateHeader + N EntityReplicator update packets
 *   0x54F2 RepDestroy reliable    TF_RepDestroy
 *   0x54F3 MoveState  unreliable  TF_MoveState          (owner-only; prediction reconcile + input ack)
 */
#pragma once

#include "Core/TFTypes.h"
#include <cstdint>
#include <cmath>

namespace Terrafront {

// Message ids (cast to Spark::Net::MessageType at the call site).
constexpr uint16_t kTFRepMsg_Create    = 0x54F0;
constexpr uint16_t kTFRepMsg_Update    = 0x54F1;
constexpr uint16_t kTFRepMsg_Destroy   = 0x54F2;
constexpr uint16_t kTFRepMsg_MoveState = 0x54F3;

// ---------------------------------------------------------------------------
// Quantized replication value types (used as ReplicatedField<T> payloads).
// ---------------------------------------------------------------------------

/// World position quantized to centimeters (12 bytes instead of 12 — same size
/// as floats but integral compare == exact dirty detection, and cm precision
/// caps replication noise from float jitter).
struct QuantPos {
    int32_t x = 0, y = 0, z = 0;
    bool operator==(const QuantPos&) const = default;

    static QuantPos From(const float p[3]) {
        return { static_cast<int32_t>(std::lround(p[0] * 100.0f)),
                 static_cast<int32_t>(std::lround(p[1] * 100.0f)),
                 static_cast<int32_t>(std::lround(p[2] * 100.0f)) };
    }
    void To(float out[3]) const {
        out[0] = x * 0.01f; out[1] = y * 0.01f; out[2] = z * 0.01f;
    }
};
static_assert(sizeof(QuantPos) == 12);

/// View angles quantized to 1/10000 rad (int16 covers ±3.2767 rad > ±pi).
struct QuantAim {
    int16_t yaw = 0, pitch = 0;
    bool operator==(const QuantAim&) const = default;

    static constexpr float kScale = 10000.0f;

    static float WrapPi(float a) {
        while (a >  3.14159265f) a -= 6.28318531f;
        while (a < -3.14159265f) a += 6.28318531f;
        return a;
    }
    static QuantAim From(float yawRad, float pitchRad) {
        return { static_cast<int16_t>(std::lround(WrapPi(yawRad) * kScale)),
                 static_cast<int16_t>(std::lround(WrapPi(pitchRad) * kScale)) };
    }
    float Yaw() const   { return yaw / kScale; }
    float Pitch() const { return pitch / kScale; }
};
static_assert(sizeof(QuantAim) == 4);

// ---------------------------------------------------------------------------
// Wire structs (packed PODs, layout frozen by static_assert like TFNetProtocol)
// ---------------------------------------------------------------------------

#pragma pack(push, 1)

/// Full pawn state, sent reliably when a pawn spawns or a client late-joins.
struct TF_RepPawnCreate {
    uint32_t entityId;
    uint32_t ownerPlayer;
    uint8_t  faction;        // FactionId
    uint8_t  classId;        // ClassId
    uint8_t  alive;          // 0/1
    uint8_t  _pad;
    float    posX, posY, posZ;
    float    yaw, pitch;     // radians
    float    health, shield;
};
static_assert(sizeof(TF_RepPawnCreate) == 40, "wire layout frozen");

struct TF_RepDestroy {
    uint32_t entityId;
};
static_assert(sizeof(TF_RepDestroy) == 4, "wire layout frozen");

/// Header preceding N EntityReplicator update packets in one RepUpdate message.
struct TF_RepUpdateHeader {
    float    serverTime;     // TFServerSim::ServerTime() at send, seconds
    uint16_t entityCount;
};
static_assert(sizeof(TF_RepUpdateHeader) == 6, "wire layout frozen");

/// Authoritative movement state for the OWNING client only (20 Hz).
/// This is the input-ack + prediction-reconcile channel:
/// lastAckedSeq == TF_ClientInput.seq of the last input the server consumed.
/// TFClientNet feeds this into Spark::Net::ClientPrediction::Reconcile().
struct TF_MoveState {
    uint32_t lastAckedSeq;
    float    posX, posY, posZ;   // feet position (pawn Transform truth)
    float    velX, velY, velZ;
    float    yaw, pitch;         // radians
    uint8_t  grounded;           // 0/1
    uint8_t  _pad[3];
};
static_assert(sizeof(TF_MoveState) == 40, "wire layout frozen");

#pragma pack(pop)

} // namespace Terrafront
