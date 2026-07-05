/**
 * @file TFDataTables.h
 * @brief JSON data-table loaders (weapons/vehicles/classes/regions/factions).
 *
 * FROZEN CONTRACT: every system consumes these structs/accessors; the structs
 * mirror Assets/MMOFPS/Data/*.json field-for-field. The .cpp (loader) is owned
 * by the data-world agent; extend private helpers freely, do not change the
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

    const std::vector<WeaponDef>&  AllWeapons()  const { return m_weapons; }
    const std::vector<ClassDef>&   AllClasses()  const { return m_classes; }
    const std::vector<VehicleDef>& AllVehicles() const { return m_vehicles; }

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
};

} // namespace Terrafront
