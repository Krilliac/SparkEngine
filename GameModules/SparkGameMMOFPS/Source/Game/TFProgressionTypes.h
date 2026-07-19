/**
 * @file TFProgressionTypes.h
 * @brief Progression declaration groups split out of TFProgressionSystem.h:
 *        canonical XP reason codes, TFUnlockResult, the rank cap, the
 *        TF_LoadoutExtChange wire message (reserved TFMsg 0x5488) and the
 *        suits.json TFSuitDef row. Included by TFProgressionSystem.h (the
 *        umbrella) — include that header instead unless only these types
 *        are needed.
 */
#pragma once

#include <cstdint>
#include <string>

namespace Terrafront
{

    // Canonical XP reason codes (TF_XPEvent.reasonCode / ServerAwardXP reason).
    // The field is an opaque uint8_t on the wire; other systems may extend it.
    enum : uint8_t
    {
        kXPReasonKill = 0,
        kXPReasonAssist = 1,
        kXPReasonRevive = 2,
        kXPReasonRepairTick = 3,
        kXPReasonCaptureFacility = 4,
        kXPReasonCaptureFort = 5,
        kXPReasonCaptureOutpost = 6,
        kXPReasonDefend = 7,
        kXPReasonFluxTick = 8, ///< amount==0; carries a wallet refresh
        kXPReasonSync = 9,     ///< amount==0; totals refresh (spawn/spend)
        kXPReasonUnlock = 10,  ///< amount==0; unlock granted (wallet refresh; HUD toast cue)
        // 11 is claimed by kXPReasonDirective (Game/TFDirectiveData.h); 12 by
        // kXPReasonMedal (Game/TFMedalSystem.h); 13 by kXPReasonAlert
        // (World/TFAlertSystem.h) — keep in sync.
    };

    /// Result of TFProgressionSystem::ServerTryUnlock (W6 progression expansion).
    enum class TFUnlockResult : uint8_t
    {
        Ok = 0,
        UnknownKey,
        AlreadyUnlocked,
        RankTooLow,
        PrereqLocked,
        InsufficientFlux,
        NotAuthority, ///< also: not initialized / invalid player
    };

    constexpr uint16_t kTFMaxRank = 30;

    // ---------------------------------------------------------------------------
    // Wire protocol — reserved TFMsg id block 0x5488-0x548B (loadout-depth
    // wave). TFNetProtocol.h (contended) only gains the LoadoutExtChange C->S
    // enum entry via the wave wiringNotes (TFServerSim::RouteClientMessage
    // dispatches it to ServerHandleLoadoutExtMsgRaw below); this constant keeps
    // this lane compiling standalone and MUST stay value-identical to that
    // enum entry (TFGrenadeSystem.h precedent). 0x5489-0x548A are grenade-lane
    // FX ids (flash/smoke) defined in Game/TFGrenadeSystem.h, same block;
    // 0x548B is free.
    // ---------------------------------------------------------------------------

    constexpr uint16_t kTFMsgLoadoutExtChange = 0x5488; // C->S TF_LoadoutExtChange (server validates)

#pragma pack(push, 1)

    /// C->S: grenade + suit loadout selection. The UI always sends its full
    /// current pick for both fields; an empty (all-zero) string means "class
    /// default" (frag_grenade / no suit), matching the primary/secondary/tool
    /// convention on TF_LoadoutChange.
    struct TF_LoadoutExtChange
    {
        char grenadeKey[24]; // weapons.json key; empty == frag_grenade default
        char suitKey[24];    // suits.json key; empty == no passive
    };
    static_assert(sizeof(TF_LoadoutExtChange) == 48, "wire layout frozen");

#pragma pack(pop)

    /// loadout-depth wave: one suits.json row (Assets/MMOFPS/Data/suits.json).
    /// Multipliers default to 1.0 (no effect) so a row that only sets one stat
    /// leaves the others untouched.
    struct TFSuitDef
    {
        std::string key, name, desc;
        float shieldMult = 1.0f;
        float regenDelayMult = 1.0f;
        float reserveMult = 1.0f;
        float healthMult = 1.0f;
    };

} // namespace Terrafront
