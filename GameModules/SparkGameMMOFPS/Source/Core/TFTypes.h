/**
 * @file TFTypes.h
 * @brief TERRAFRONT core types, ids, constants, and the shared game context.
 *
 * FROZEN CONTRACT (see DESIGN.md). Systems may extend their own headers freely,
 * but changes to this file require a design-doc update.
 */
#pragma once

#include <cstdint>
#include <string>

namespace Spark { class IEngineContext; }

namespace Terrafront {

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

enum class FactionId : uint8_t {
    None = 0,
    MRA  = 1,   // Meridian Accord   (crimson/gunmetal, high RoF ballistics)
    AUC  = 2,   // Aurum Combine     (cobalt/gold, hard-hitting slow shots)
    HLX  = 3,   // Helix Covenant    (violet/teal, energy weapons, no drop)
    COUNT
};

enum class ClassId : uint8_t {
    Ghost = 0,      // recon / sniper, reduced minimap signature
    Striker,        // jump-jet light assault
    Medtech,        // heal / revive
    Fabricator,     // repair / ammo / turret
    Bulwark,        // overshield heavy
    Colossus,       // flux-purchased exosuit (not selectable in spawn UI; via terminal)
    COUNT
};

enum class VehicleId : uint8_t {
    None = 0,
    Drifter,        // fast quad
    Aegis,          // armored transport, deployable mobile spawn
    Ravager,        // light tank
    Vulture,        // VTOL gunship (W4 stretch)
    COUNT
};

enum class DeployableKind : uint8_t {
    FabTurret = 0,
    FabAmmoPack,
    MedBeacon,
    COUNT
};

using PlayerId   = uint32_t;   // == network client id
using EntityId   = uint32_t;   // engine EntityID
using RegionId   = uint16_t;   // index into regions.json
using WeaponId   = uint16_t;   // index into weapons.json
using SquadId    = uint16_t;

constexpr PlayerId kInvalidPlayer = 0xFFFFFFFFu;
constexpr RegionId kInvalidRegion = 0xFFFFu;
constexpr WeaponId kInvalidWeapon = 0xFFFFu;

// ---------------------------------------------------------------------------
// Tuning constants (gameplay defaults live in Assets/MMOFPS/Data/*.json;
// these are engine-side hard limits, not balance numbers)
// ---------------------------------------------------------------------------

constexpr uint32_t kMaxPlayers        = 64;
constexpr uint32_t kMaxRegions        = 64;
constexpr uint32_t kMaxCapturePoints  = 3;    // per region
constexpr uint32_t kMaxSquadSize      = 6;
constexpr float    kServerTickHz      = 60.0f;
constexpr float    kReplicationHz     = 20.0f;
constexpr float    kLagCompWindowSec  = 0.25f;
constexpr uint32_t kFluxWalletCap     = 750;

// ---------------------------------------------------------------------------
// Net role
// ---------------------------------------------------------------------------

enum class NetRole : uint8_t {
    Standalone = 0,   // no networking booted yet (menu / local test)
    ListenHost,       // in-process server + local player
    DedicatedServer,  // headless authoritative server
    Client            // connected to a remote host
};

// ---------------------------------------------------------------------------
// Shared game context — one instance owned by TerrafrontModule, passed to
// every system's Initialize(). Systems talk to each other through this;
// never through globals.
// ---------------------------------------------------------------------------

class TFDataTables;
class TFWorldSetup;
class TFRegionSystem;
class TFReplication;
class TFServerSim;
class TFClientNet;
class TFPlayerSystem;
class TFWeaponSystem;
class TFDamageSystem;
class TFVehicleSystem;
class TFColossusSystem;
class TFDeployableSystem;
class TFProgressionSystem;
class TFSquadSystem;
class TFHUD;
class TFMapScreen;
class TFSpawnScreen;
class TFScoreboard;

struct TFGameContext {
    Spark::IEngineContext* engine = nullptr;
    NetRole                role   = NetRole::Standalone;
    PlayerId               localPlayer = kInvalidPlayer;
    FactionId              localFaction = FactionId::None;

    TFDataTables*        data        = nullptr;
    TFWorldSetup*        world       = nullptr;
    TFRegionSystem*      regions     = nullptr;
    TFReplication*       replication = nullptr;
    TFServerSim*         serverSim   = nullptr;   // null on pure clients
    TFClientNet*         clientNet   = nullptr;   // null on dedicated servers
    TFPlayerSystem*      players     = nullptr;
    TFWeaponSystem*      weapons     = nullptr;
    TFDamageSystem*      damage      = nullptr;
    TFVehicleSystem*     vehicles    = nullptr;
    TFColossusSystem*    colossus    = nullptr;
    TFDeployableSystem*  deployables = nullptr;
    TFProgressionSystem* progression = nullptr;
    TFSquadSystem*       squads      = nullptr;
    TFHUD*               hud         = nullptr;
    TFMapScreen*         map         = nullptr;
    TFSpawnScreen*       spawnUI     = nullptr;
    TFScoreboard*        scoreboard  = nullptr;

    bool IsAuthority() const {
        return role == NetRole::ListenHost || role == NetRole::DedicatedServer
            || role == NetRole::Standalone;
    }
    bool HasLocalPlayer() const {
        return role != NetRole::DedicatedServer;
    }
};

// ---------------------------------------------------------------------------
// Faction display helpers
// ---------------------------------------------------------------------------

inline const char* FactionName(FactionId f) {
    switch (f) {
        case FactionId::MRA: return "Meridian Accord";
        case FactionId::AUC: return "Aurum Combine";
        case FactionId::HLX: return "Helix Covenant";
        default:             return "Unaligned";
    }
}

inline const char* FactionTag(FactionId f) {
    switch (f) {
        case FactionId::MRA: return "MRA";
        case FactionId::AUC: return "AUC";
        case FactionId::HLX: return "HLX";
        default:             return "---";
    }
}

// RGBA 0-1 faction colors (UI + team tinting)
inline void FactionColor(FactionId f, float out[4]) {
    switch (f) {
        case FactionId::MRA: out[0]=0.78f; out[1]=0.12f; out[2]=0.15f; break; // crimson
        case FactionId::AUC: out[0]=0.16f; out[1]=0.38f; out[2]=0.85f; break; // cobalt
        case FactionId::HLX: out[0]=0.55f; out[1]=0.20f; out[2]=0.80f; break; // violet
        default:             out[0]=0.55f; out[1]=0.55f; out[2]=0.55f; break;
    }
    out[3] = 1.0f;
}

} // namespace Terrafront
