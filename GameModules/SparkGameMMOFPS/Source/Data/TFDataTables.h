/**
 * @file TFDataTables.h
 * @brief JSON data-table loaders (weapons/vehicles/classes/regions/factions).
 *
 * FROZEN CONTRACT: every system consumes these structs/accessors; the structs
 * mirror JSON files under Assets/MMOFPS/Data field-for-field. The loader is
 * owned by the data-world agent; extend private helpers freely, do not change the
 * public surface without a DESIGN.md update.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"
#include <string>
#include <vector>
#include <array>
#include <optional>

namespace Terrafront {

// ---------------------------------------------------------------------------

struct FactionDef {
    FactionId  id = FactionId::None;
    std::string tag, name, blurb;
    float color[3]     = {0.5f, 0.5f, 0.5f};
    float colorSec[3]  = {0.5f, 0.5f, 0.5f};
    // trait multipliers applied to weapon base stats at lookup time
    float rofMult = 1.0f, damageMult = 1.0f, reloadMult = 1.0f;
    float projGravityMult = 1.0f;
    float shieldRegenDelaySec = 6.0f;
    // Faction structure/pawn/vehicle tint material (W1 placeholder scheme);
    // see Game/TFVisualUtils.h FactionStructureMaterial for the fallback.
    std::string structureMaterial = "Assets/Materials/MMOFPS/Structure_Concrete.json";
};

struct WeaponDef {
    WeaponId    id = kInvalidWeapon;
    std::string key, name;
    FactionId   faction = FactionId::None;       // None == common pool ("ALL")
    std::string slot;                            // rifle/carbine/lmg/sniper/pistol/shotgun/launcher/melee/tool/...
    std::string kind;                            // hitscan/projectile/melee/beam
    float damage = 0, headshotMult = 1, rofRpm = 600;
    int   magSize = 0, reserve = 0;
    float reloadSec = 0, adsSec = 0;
    bool  reloadPerShell = false;
    float spreadHipDeg = 0, spreadAdsDeg = 0, recoilVert = 0, recoilHoriz = 0;
    float falloffStartM = 999, falloffEndM = 999, minDamage = 0;
    float projSpeed = 0;                         // 0 == hitscan
    float gravity = 1.0f;                        // gravity factor for projectiles
    int   pellets = 1;
    float rangeM = 0;                            // beam/melee reach
    float splashRadiusM = 0, splashDamage = 0, vsVehicleMult = 1.0f;
    bool  healsInfantry = false, healsVehicles = false, canRevive = false;
    std::string model, audioFire, audioReload;
};

struct ClassAbilityDef {
    std::string key, name, desc;
    float durationSec = 0, cooldownSec = 0, regenPerSec = 0;
    bool  toggle = false;
};

struct ClassDef {
    ClassId     id = ClassId::COUNT;
    std::string name, role;
    float health = 500, shield = 500;
    float sprintSpeed = 7.2f, runSpeed = 5.2f;
    ClassAbilityDef ability;
    std::vector<std::string> primarySlots;       // allowed primary weapon slots
    std::string secondarySlot, toolKey;
    int  grenades = 0;
    bool rocketLauncher = false;                 // Bulwark
    int  fluxCost = 0;                           // Colossus
    bool noRegen = false;
    std::string mesh;                            // per-class pawn OBJ (empty == presentation.pawnMesh)
};

struct VehicleSeatDef {
    std::string role;                            // driver/gunner/passenger
    std::string weaponKey;                       // empty == unarmed seat
};

struct VehicleDef {
    VehicleId   id = VehicleId::None;
    std::string name, role;
    bool  enabled = true;
    float health = 1000;
    int   fluxCost = 0;
    float topSpeed = 10, accel = 5, turnRate = 1.5f;
    std::vector<VehicleSeatDef> seats;
    // Aegis mobile spawn
    bool  hasDeploySpawn = false;
    float deployRadiusM = 0, deployRespawnSec = 0;
    std::string model, audioEngine, explodeAudio;
    // Optional separate turret mesh rendered as a hull-parented child at
    // turretPivot (hull-local meters). Empty == no turret child.
    std::string turretMesh;
    float turretPivot[3] = {0.0f, 0.0f, 0.0f};
};

struct RegionDef {
    RegionId    id = kInvalidRegion;
    std::string key, name;
    std::string tier;                            // skyanchor/outpost/fort/facility
    FactionId   homeFaction = FactionId::None;   // skyanchors only
    int   hexQ = 0, hexR = 0;                    // axial map coords
    float centerX = 0, centerZ = 0;
    float captureSec = 60;
    int   fluxPerTick = 0;
    std::vector<std::array<float, 2>> capturePoints;  // world XZ
    std::vector<std::array<float, 2>> spawns;         // world XZ
    std::optional<std::array<float, 2>> vehicleTerminal;
    std::vector<RegionId> neighbors;             // built from "conduits"
};

struct ContinentDef {
    std::string name;
    float sizeM = 4096;
    std::string scene;
    float fluxTickSec = 60;
    std::vector<RegionDef> regions;
    std::vector<FactionId> initialOwner;         // indexed by RegionId; None == neutral
};

// ---------------------------------------------------------------------------
// World presentation (Assets/MMOFPS/Data/presentation.json). Every field's
// in-class default equals the magic number it replaces in TFWorldSetup.cpp,
// so an unloaded/missing table renders byte-identically to before this table
// existed (see TFWorldSetup::Pres()).
// ---------------------------------------------------------------------------

struct SkyboxDef {
    // Face order matches TFWorldSetup::DrawSkybox's mesh winding: +X,-X,+Y,-Y,+Z,-Z.
    std::string faceTex[6] = {
        "Textures/MMOFPS/sky/space_px.png", "Textures/MMOFPS/sky/space_nx.png",
        "Textures/MMOFPS/sky/space_py.png", "Textures/MMOFPS/sky/space_ny.png",
        "Textures/MMOFPS/sky/space_pz.png", "Textures/MMOFPS/sky/space_nz.png",
    };
    float scale = 2900.0f;             // box half-extent-ish (uniform XMMatrixScaling)
    float tint[4] = {1.8f, 1.8f, 1.8f, 1.0f}; // over-1.0 so it reads full-bright
};

struct TerrainVisualDef {
    std::string texture = "Textures/MMOFPS/terrain/sand_color.png";
    float uvTiles = 96.0f;             // texture repeats across the map
};

struct AmbientAudioDef {
    // No "Assets/" prefix (mirrors weapon/vehicle model path convention);
    // consumers prepend "Assets/" for the wide LoadSound path and use the
    // bare path as the sound-cache key, matching pre-existing behavior.
    std::string path = "Audio/MMOFPS/ambient/wind_loop.wav";
    float volume = 0.35f;
};

struct ViewmodelDef {
    // Recenter the weapon OBJ (~2.7 units long on local X, centered near
    // (0.89,0.10,0)) before scale/rotate/place. See TFWorldSetup::RenderWorld.
    float recenter[3]  = {-0.89f, -0.10f, 0.0f};
    float scale        = 0.26f;
    float rotationYRad = -1.57079633f; // -XM_PIDIV2: local +X (barrel) -> view +Z (forward)
    float place[3]     = {0.28f, -0.26f, 0.9f};
    float gunmetal[4]  = {0.60f, 0.62f, 0.68f, 1.0f}; // untextured fallback tint
};

struct MuzzleFxDef {
    // Muzzle point offset from the fire origin (view-space right/up/forward).
    float muzzleForwardM = 1.2f;
    float muzzleRightM   = 0.30f;
    float muzzleUpM      = -0.22f;     // negative == below the eye line
    // Tracer: thin bright streak from the muzzle forward.
    float tracerLenM     = 60.0f;
    float tracerThickM   = 0.05f;
    float tracerLifeSec  = 0.08f;
    float tracerColor[4] = {6.0f, 5.0f, 2.0f, 1.0f};
    // Muzzle flash: bright puff at the gun tip.
    float flashScale[3]  = {0.22f, 0.22f, 0.30f};
    float flashLifeSec   = 0.05f;
    float flashColor[4]  = {8.0f, 6.5f, 2.5f, 1.0f};
};

struct WorldPresentationDef {
    SkyboxDef        skybox;
    TerrainVisualDef terrain;
    AmbientAudioDef  ambient;
    ViewmodelDef     viewmodel;
    MuzzleFxDef      muzzleFx;
    // Humanoid pawn placeholder mesh (no "Assets/" prefix; see AmbientAudioDef).
    std::string      pawnMesh = "Models/MMOFPS/characters/soldier.obj";
};

struct DeployableVisualDef {
    DeployableKind id = DeployableKind::COUNT;
    std::string    model;              // no "Assets/" prefix
    float          scale[3] = {1.0f, 1.0f, 1.0f};
};

// ---------------------------------------------------------------------------

class TFDataTables {
  public:
    TFDataTables();
    ~TFDataTables();

    bool Initialize(TFGameContext& ctx, TFEventBus& events);
    void Update(float deltaTime);
    void FixedUpdate(float fixedDeltaTime);
    void Shutdown();
    void RenderDebugUI();

    /// Reload all tables from disk (tf_reload_data). Fires EvDataReloaded on success.
    bool ReloadAll();

    // --- typed accessors (nullptr == unknown id) ---
    const FactionDef* GetFaction(FactionId f) const;
    const WeaponDef*  GetWeapon(WeaponId id) const;
    const WeaponDef*  GetWeaponByKey(const std::string& key) const;
    const ClassDef*   GetClass(ClassId id) const;
    const ClassDef*   GetClassByName(const std::string& name) const;
    const VehicleDef* GetVehicle(VehicleId id) const;
    const RegionDef*  GetRegion(RegionId id) const;
    const ContinentDef& GetContinent() const { return m_continent; }
    const WorldPresentationDef& GetPresentation() const { return m_presentation; }
    const DeployableVisualDef* GetDeployableVisual(DeployableKind kind) const;

    const std::vector<WeaponDef>&  AllWeapons()  const { return m_weapons; }
    const std::vector<ClassDef>&   AllClasses()  const { return m_classes; }
    const std::vector<VehicleDef>& AllVehicles() const { return m_vehicles; }
    const std::vector<DeployableVisualDef>& AllDeployableVisuals() const { return m_deployableVisuals; }

    /// Weapon stats with faction traits applied (rof/damage/reload/gravity).
    WeaponDef ResolveWeapon(WeaponId id, FactionId f) const;

    bool IsLoaded() const { return m_loaded; }

  private:
    bool LoadAllInternal(std::string& outError);

    TFGameContext* m_ctx{nullptr};
    TFEventBus*    m_events{nullptr};
    bool           m_initialized{false};
    bool           m_loaded{false};

    std::vector<FactionDef> m_factions;
    std::vector<WeaponDef>  m_weapons;
    std::vector<ClassDef>   m_classes;
    std::vector<VehicleDef> m_vehicles;
    ContinentDef            m_continent;
    WorldPresentationDef    m_presentation;
    std::vector<DeployableVisualDef> m_deployableVisuals;
};

} // namespace Terrafront
