/**
 * @file TFVehicleSystemTypes.h
 * @brief Shared vehicle declarations split from TFVehicleSystem.h (the
 *        umbrella includes this back, so no includer changes): the W3
 *        engine-side tuning constants, the TFVehicleInfo accessor snapshot and
 *        the W8 seat-driven turret-aim wire protocol (kTFVehMsg_TurretAim /
 *        TF_RepVehicleAim).
 */
#pragma once

#include "Core/TFTypes.h"

#include <cstdint>

namespace Terrafront
{

    // W3 vehicle tuning constants (engine-side limits; balance lives in vehicles.json).
    constexpr float kTFVehTerminalRangeM = 25.0f; ///< purchase reach from a terminal
    constexpr float kTFVehTerminalPromptM = 8.0f; ///< client prompt / menu-open reach
    constexpr float kTFVehEnterRangeM = 4.0f;     ///< enter reach from the hull
    constexpr float kTFVehExplodeRadiusM = 6.0f;  ///< destruction splash radius
    constexpr float kTFVehExplodeDamage = 400.0f; ///< destruction splash at center
    constexpr float kTFVehEjectDamageFrac = 0.5f; ///< of max health+shield on destruction
    constexpr float kTFVehDeployMaxSpeed = 1.0f;  ///< m/s — must be ~stopped to deploy
    constexpr uint16_t kTFVehKillXP = 300;

    // W8 turret aim: server pitch clamp in CAMERA convention (positive = down —
    // the sign TF_ClientInput.viewPitch and BuildViewRay use). Design range is
    // "[-10, +35] deg": 35 deg elevation (up) / 10 deg depression (down).
    constexpr float kTFTurretPitchMinRad = -0.6108652f; ///< 35 deg elevation (up)
    constexpr float kTFTurretPitchMaxRad = 0.1745329f;  ///< 10 deg depression (down)

    /// Shared snapshot of one vehicle — served from the authoritative records on
    /// server roles and from the replication mirror on pure clients (same
    /// pattern as TFRegionSystem's accessors).
    struct TFVehicleInfo
    {
        EntityId entity = 0;
        VehicleId vehId = VehicleId::None;
        FactionId faction = FactionId::None;
        float pos[3] = {0.0f, 0.0f, 0.0f};
        float yaw = 0.0f; ///< radians
        float hp = 0.0f;
        float maxHp = 0.0f;
        bool deployed = false;
        PlayerId seats[8] = {kInvalidPlayer, kInvalidPlayer, kInvalidPlayer, kInvalidPlayer,
                             kInvalidPlayer, kInvalidPlayer, kInvalidPlayer, kInvalidPlayer};
        uint8_t seatCount = 0;
    };

    // --- W8 seat-driven turret aim (reserved TFMsg block 0x5448-0x544B) ---------
    // Ids + packed structs live HERE, outside the frozen TFMsg enum, following
    // the TFRepProtocol/TFSocialProtocol precedent. 0x5449-0x544B stay free for
    // this lane. Channel map:
    //   0x5448 TurretAim S->C unreliable (10 Hz active / 1 Hz keepalive; sent
    //          reliable once per vehicle in the late-join burst) TF_RepVehicleAim
    constexpr uint16_t kTFVehMsg_TurretAim = 0x5448;

#pragma pack(push, 1)
    /// Gunner turret aim for one vehicle. yaw16 is the WORLD-space turret yaw,
    /// [-pi, pi) mapped onto [0, 65535]. pitchDeg is camera-convention degrees
    /// (positive = down) clamped to the kTFTurretPitch* range ([-35, +10]).
    struct TF_RepVehicleAim
    {
        uint32_t entityId;
        uint16_t yaw16;
        int8_t pitchDeg;
        uint8_t _pad;
    };
    static_assert(sizeof(TF_RepVehicleAim) == 8, "wire layout frozen");
#pragma pack(pop)

} // namespace Terrafront
