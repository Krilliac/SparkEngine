/**
 * @file TFFireFxProtocol.h
 * @brief Wire struct + message id for remote-fire fx: per-shot muzzle flash +
 *        distant gunfire audio on PURE clients (W8 gap: only listen hosts
 *        heard remote fire, via the in-process ServerHandleFire hook).
 *
 * OWNERSHIP: remote-fire-events lane (W9), alongside the broadcast section of
 * Game/TFWeaponServer.cpp and the client fx seam in Game/TFAudioAmbience.
 *
 * Ids live in the lane's RESERVED 0x54F4-0x54F7 block, deliberately OUTSIDE
 * the frozen TFMsg enum (TFNetProtocol.h) — the exact precedent set by
 * TFRepProtocol.h's 0x54F0 block and TFSocialProtocol.h's 0x5440 block. They
 * ride NetworkManager's handler registry cast to Spark::Net::MessageType at
 * the call site.
 *
 * Channel map:
 *   0x54F4 RemoteFireFx S->C unreliable TF_RemoteFireFx (validated fire, <=250 m, never to the shooter)
 *   0x54F5-0x54F7 reserved (unused)
 *
 * Server-authoritative: sent only from TFWeaponSystem::ServerHandleFire AFTER
 * full fire validation (loadout/unlock/RoF/ammo), rate-capped per shooter
 * (kTFRemoteFireFxMinIntervalSec) and range-gated per recipient
 * (kTFRemoteFireFxRangeM). Purely presentational on the client: flash quad +
 * distant-fire tail + combat heat — a dropped packet costs nothing, hence
 * unreliable.
 *
 * Packed POD with a static_assert frozen-layout guard, exactly like
 * TFNetProtocol.h / TFRepProtocol.h. Budget <= 16 bytes:
 *  - muzzle pos quantized to 0.25 m steps in int16 (covers ±8191 m; the whole
 *    shared world is [0,4096]^2 m — see World/TFSanctuaryZone.h). 0.25 m error
 *    is invisible at the >0 m distances the fx plays at.
 *  - direction as int8 yaw/pitch (~1.4 deg / ~0.7 deg steps), same
 *    yaw/pitch -> vector convention as TFWeaponSystem::BuildViewRay
 *    (dir = {cos(p)*sin(y), -sin(p), cos(p)*cos(y)}).
 */
#pragma once

#include "Core/TFTypes.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Terrafront
{

    // Message id (remote-fire-events lane reserved block 0x54F4-0x54F7).
    constexpr uint16_t kTFFxMsg_RemoteFire = 0x54F4; // S->C TF_RemoteFireFx

    /// Recipient range gate: clients farther than this from the muzzle get
    /// nothing (matches TFWeaponSystem's kRemoteFireMaxM audio cutoff tier).
    constexpr float kTFRemoteFireFxRangeM = 250.0f;

    /// Per-shooter send rate cap (~10/s). Full-auto weapons fire faster than
    /// this; the fx layer is impressionistic, not a shot counter.
    constexpr double kTFRemoteFireFxMinIntervalSec = 0.1;

    /// Muzzle position quantization: world meters * 4 -> int16 (0.25 m steps).
    constexpr float kTFFireFxPosScale = 4.0f;

    namespace FireFxDetail
    {
        constexpr float kPi = 3.14159265f;
        constexpr float kHalfPi = 1.57079633f;

        inline int16_t QuantCoord(float v)
        {
            return static_cast<int16_t>(std::lround(std::clamp(v * kTFFireFxPosScale, -32767.0f, 32767.0f)));
        }
    } // namespace FireFxDetail

#pragma pack(push, 1)

    /// S->C: one validated remote shot (muzzle flash + distant-fire audio cue).
    struct TF_RemoteFireFx
    {
        uint32_t shooterEntity; // shooter pawn ECS/network id (0x54F0 rep id space)
        uint16_t weaponId;      // WeaponId (weapons.json index)
        int16_t posQX;          // muzzle world pos * kTFFireFxPosScale (0.25 m steps)
        int16_t posQY;
        int16_t posQZ;
        int8_t yawQ;   // atan2(dir.x, dir.z) * 127/pi
        int8_t pitchQ; // asin(-dir.y) * 127/(pi/2)  (BuildViewRay: dir.y == -sin(pitch))
        uint8_t _pad[2];

        static TF_RemoteFireFx From(EntityId shooterPawn, WeaponId weapon, const float muzzle[3],
                                    const float dirUnit[3])
        {
            using namespace FireFxDetail;
            TF_RemoteFireFx fx{};
            fx.shooterEntity = shooterPawn;
            fx.weaponId = weapon;
            fx.posQX = QuantCoord(muzzle[0]);
            fx.posQY = QuantCoord(muzzle[1]);
            fx.posQZ = QuantCoord(muzzle[2]);
            const float yaw = std::atan2(dirUnit[0], dirUnit[2]);
            const float pitch = std::asin(std::clamp(-dirUnit[1], -1.0f, 1.0f));
            fx.yawQ = static_cast<int8_t>(std::lround(yaw * (127.0f / kPi)));
            fx.pitchQ = static_cast<int8_t>(std::lround(pitch * (127.0f / kHalfPi)));
            return fx;
        }

        void DecodePos(float out[3]) const
        {
            out[0] = posQX / kTFFireFxPosScale;
            out[1] = posQY / kTFFireFxPosScale;
            out[2] = posQZ / kTFFireFxPosScale;
        }

        /// Unit direction, TFWeaponSystem::BuildViewRay convention.
        void DecodeDir(float out[3]) const
        {
            using namespace FireFxDetail;
            const float yaw = yawQ * (kPi / 127.0f);
            const float pitch = pitchQ * (kHalfPi / 127.0f);
            const float cp = std::cos(pitch);
            out[0] = cp * std::sin(yaw);
            out[1] = -std::sin(pitch);
            out[2] = cp * std::cos(yaw);
        }
    };
    static_assert(sizeof(TF_RemoteFireFx) == 16, "wire layout frozen (<= 16 byte budget)");

#pragma pack(pop)

} // namespace Terrafront
