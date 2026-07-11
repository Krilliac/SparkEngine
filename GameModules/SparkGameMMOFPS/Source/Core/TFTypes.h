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

namespace Spark
{
    class IEngineContext;
}

namespace Terrafront
{

    // ---------------------------------------------------------------------------
    // Identity
    // ---------------------------------------------------------------------------

    enum class FactionId : uint8_t
    {
        None = 0,
        MRA = 1, // Meridian Accord   (crimson/gunmetal, high RoF ballistics)
        AUC = 2, // Aurum Combine     (cobalt/gold, hard-hitting slow shots)
        HLX = 3, // Helix Covenant    (violet/teal, energy weapons, no drop)
        COUNT
    };

    enum class ClassId : uint8_t
    {
        Ghost = 0,  // recon / sniper, reduced minimap signature
        Striker,    // jump-jet light assault
        Medtech,    // heal / revive
        Fabricator, // repair / ammo / turret
        Bulwark,    // overshield heavy
        Colossus,   // flux-purchased exosuit (not selectable in spawn UI; via terminal)
        COUNT
    };

    enum class VehicleId : uint8_t
    {
        None = 0,
        Drifter, // fast quad
        Aegis,   // armored transport, deployable mobile spawn
        Ravager, // light tank
        Vulture, // VTOL gunship (W4 stretch)
        COUNT
    };

    enum class DeployableKind : uint8_t
    {
        FabTurret = 0,
        FabAmmoPack,
        MedBeacon,
        COUNT
    };

    using PlayerId = uint32_t; // == network client id
    using EntityId = uint32_t; // engine EntityID
    using RegionId = uint16_t; // index into regions.json
    using WeaponId = uint16_t; // index into weapons.json
    using SquadId = uint16_t;

    constexpr PlayerId kInvalidPlayer = 0xFFFFFFFFu;
    constexpr RegionId kInvalidRegion = 0xFFFFu;
    constexpr WeaponId kInvalidWeapon = 0xFFFFu;

    // ---------------------------------------------------------------------------
    // Tuning constants (gameplay defaults live in Assets/MMOFPS/Data/*.json;
    // these are engine-side hard limits, not balance numbers)
    // ---------------------------------------------------------------------------

    constexpr uint32_t kMaxPlayers = 64;
    constexpr uint32_t kMaxRegions = 64;
    constexpr uint32_t kMaxCapturePoints = 3; // per region
    constexpr uint32_t kMaxSquadSize = 6;
    constexpr float kServerTickHz = 60.0f;
    constexpr float kReplicationHz = 20.0f;
    constexpr float kLagCompWindowSec = 0.25f;
    constexpr uint32_t kFluxWalletCap = 750;

    // ---------------------------------------------------------------------------
    // Net role
    // ---------------------------------------------------------------------------

    enum class NetRole : uint8_t
    {
        Standalone = 0,  // no networking booted yet (menu / local test)
        ListenHost,      // in-process server + local player
        DedicatedServer, // headless authoritative server
        Client           // connected to a remote host
    };

    // ---------------------------------------------------------------------------
    // Shared game context — one instance owned by TerrafrontModule, passed to
    // every system's Initialize(). Systems talk to each other through this;
    // never through globals.
    // ---------------------------------------------------------------------------

    class TFDataTables;
    class TFWorldSetup;
    class TFRegionSystem;
    class TFTravelSystem; // continents lane (additive): World/TFTravelSystem.h
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
    class TFDirectiveSystem;
    class TFSquadSystem;
    class TFOutfitSystem; // outfits lane (additive): Game/TFOutfitSystem.h
    class TFHUD;
    class TFMapScreen;
    class TFSpawnScreen;
    class TFScoreboard;
    // W5 onboarding (Task 4): account/character systems live under Source/Account
    // and Source/Persistence; forward-declared here only as opaque pointer targets
    // so TFServerSim/TFClientNet can reach them through the context without this
    // FROZEN header depending on their headers. Full boot wiring (members,
    // construction, Main.cpp publish order) is Task 6 — these three pointers are
    // added now, additive-only, so Task 4's net handlers compile and call the
    // real Task 2/3 systems instead of stubs.
    class TFDatabase;
    class TFAccountSystem;
    class TFCharacterSystem;
    // W5 onboarding (Task 6, additive): the client login/char-select/enter-world
    // ImGui state machine (Source/UI/TFLoginFlow.h). Forward-declared only, same
    // reasoning as the three pointers above — this FROZEN header never includes
    // TFLoginFlow.h. See DESIGN.md "W5 — Onboarding" for the full contract note.
    class TFLoginFlow;
    // chat-social lane (additive): forward decls for the context pointers below.
    class TFSocialSystem;
    class TFChatWindow;
    class TFSocialPanel;
    // class-abilities lane (additive): Game/TFAbilitySystem.h.
    class TFAbilitySystem;
    // grenades lane (additive): Game/TFGrenadeSystem.h.
    class TFGrenadeSystem;

    struct TFGameContext
    {
        Spark::IEngineContext* engine = nullptr;
        NetRole role = NetRole::Standalone;
        PlayerId localPlayer = kInvalidPlayer;
        FactionId localFaction = FactionId::None;

        TFDataTables* data = nullptr;
        TFWorldSetup* world = nullptr;
        TFRegionSystem* regions = nullptr;
        TFReplication* replication = nullptr;
        TFServerSim* serverSim = nullptr; // null on pure clients
        TFClientNet* clientNet = nullptr; // null on dedicated servers
        TFPlayerSystem* players = nullptr;
        TFWeaponSystem* weapons = nullptr;
        TFDamageSystem* damage = nullptr;
        TFVehicleSystem* vehicles = nullptr;
        TFColossusSystem* colossus = nullptr;
        TFDeployableSystem* deployables = nullptr;
        TFProgressionSystem* progression = nullptr;
        TFDirectiveSystem* directives = nullptr;
        TFSquadSystem* squads = nullptr;
        TFOutfitSystem* outfits = nullptr; // outfits lane (Game/TFOutfitSystem.h)
        TFHUD* hud = nullptr;
        TFMapScreen* map = nullptr;
        TFSpawnScreen* spawnUI = nullptr;
        TFScoreboard* scoreboard = nullptr;
        // continents lane (additive): sanctuary/warpgate travel. Published for
        // OTHER lanes (e.g. redeploy/sanctuary queries); TFTravelSystem itself
        // deliberately never reads this pointer.
        TFTravelSystem* travel = nullptr;

        // class-abilities lane (additive): ability activation/cooldowns + effects.
        TFAbilitySystem* abilities = nullptr;
        // grenades lane (additive): throw validation/simulation + HUD count.
        TFGrenadeSystem* grenades = nullptr;

        // W5 onboarding (Task 4, additive): null until Task 6 constructs + publishes
        // them in Main.cpp boot order. Net handlers guard every use with `if
        // (m_ctx->account)` / `if (m_ctx->characters)` so the module still builds
        // and runs pre-Task-6 with onboarding messages accepted-but-inert.
        TFDatabase* db = nullptr;
        TFAccountSystem* account = nullptr;
        TFCharacterSystem* characters = nullptr;

        // W5 onboarding (Task 6, additive): the client login/char-select/
        // enter-world UI. Constructed + published in Main.cpp boot order (after
        // clientNet); null on dedicated servers (no local player, no UI).
        TFLoginFlow* loginFlow = nullptr;

        // chat-social lane (additive): null until Main.cpp constructs them.
        // `chatWindow` is load-bearing — TFHUD yields its built-in chat when set.
        TFSocialSystem* social = nullptr;
        TFChatWindow* chatWindow = nullptr;
        TFSocialPanel* socialPanel = nullptr;

        // W5 onboarding (Task 6, additive): true once TF_WorldWelcome has been
        // received for the local session (i.e. TFCharacterSystem::EnterWorld
        // succeeded server-side and the gated WorldWelcome round-tripped back).
        // Client-side systems (TFSpawnScreen, HUD/map/scoreboard in Main.cpp
        // OnImGui) gate on InWorld() instead of just HasLocalPlayer() so the
        // pre-onboarding UI never shows before login completes.
        bool inWorld = false;

        bool IsAuthority() const
        {
            return role == NetRole::ListenHost || role == NetRole::DedicatedServer || role == NetRole::Standalone;
        }
        bool HasLocalPlayer() const { return role != NetRole::DedicatedServer; }
        bool InWorld() const { return inWorld; }
    };

    // ---------------------------------------------------------------------------
    // Faction display helpers
    // ---------------------------------------------------------------------------

    inline const char* FactionName(FactionId f)
    {
        switch (f)
        {
        case FactionId::MRA:
            return "Meridian Accord";
        case FactionId::AUC:
            return "Aurum Combine";
        case FactionId::HLX:
            return "Helix Covenant";
        default:
            return "Unaligned";
        }
    }

    inline const char* FactionTag(FactionId f)
    {
        switch (f)
        {
        case FactionId::MRA:
            return "MRA";
        case FactionId::AUC:
            return "AUC";
        case FactionId::HLX:
            return "HLX";
        default:
            return "---";
        }
    }

    // RGBA 0-1 faction colors (UI + team tinting)
    inline void FactionColor(FactionId f, float out[4])
    {
        switch (f)
        {
        case FactionId::MRA:
            out[0] = 0.78f;
            out[1] = 0.12f;
            out[2] = 0.15f;
            break; // crimson
        case FactionId::AUC:
            out[0] = 0.16f;
            out[1] = 0.38f;
            out[2] = 0.85f;
            break; // cobalt
        case FactionId::HLX:
            out[0] = 0.55f;
            out[1] = 0.20f;
            out[2] = 0.80f;
            break; // violet
        default:
            out[0] = 0.55f;
            out[1] = 0.55f;
            out[2] = 0.55f;
            break;
        }
        out[3] = 1.0f;
    }

} // namespace Terrafront
