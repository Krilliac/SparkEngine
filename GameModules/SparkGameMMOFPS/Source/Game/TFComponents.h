/**
 * @file TFComponents.h
 * @brief TERRAFRONT ECS components (DESIGN.md §3).
 *
 * Plain-old-data components attached to pawns/vehicles/regions/deployables.
 * Engine-agnostic on purpose: no entt/engine includes so any TF header can
 * pull this in cheaply. Replicated state is mapped to wire fields by
 * TFReplication; everything else stays server-local.
 */
#pragma once

#include "Core/TFTypes.h"

namespace Terrafront {

// --------------------------------------------------------------------------
// Pawn identity & stats
// --------------------------------------------------------------------------

/// Which faction an entity fights for (pawns, vehicles, deployables).
struct TFFactionComp {
    FactionId faction = FactionId::None;
};

/// Infantry class of a pawn.
struct TFClassComp {
    ClassId cls = ClassId::Ghost;
};

/// Marks an entity as a player pawn and remembers its controlling player.
/// (NetworkIdentity.ownerClientID mirrors this for replicated entities;
/// TFPawnComp also covers the in-process host player who has no client id.)
struct TFPawnComp {
    PlayerId owner = kInvalidPlayer;
};

/// Energy shield layered on top of HealthComponent (DESIGN §4: 500 + 500).
/// TFDamageSystem depletes `cur` first, resets `sinceDamage` to 0 on every
/// hit, and regenerates at `regenRate` once `sinceDamage` >= `regenDelay`.
struct TFShieldComp {
    float cur         = 0.0f;
    float max         = 0.0f;
    float regenDelay  = 6.0f;   ///< seconds without damage before regen starts
    float regenRate   = 80.0f;  ///< shield points per second
    float sinceDamage = 999.0f; ///< seconds since last damage (damage system owns)
};

/// State ids for TFWeaponHeldComp::state.
enum class TFWeaponState : uint8_t { Ready = 0, Firing, Reloading };

/// The weapon a pawn currently holds.
struct TFWeaponHeldComp {
    WeaponId weaponId = kInvalidWeapon;
    int32_t  ammoMag  = 0;
    int32_t  ammoPool = 0;
    uint8_t  state    = 0;      ///< TFWeaponState
};

/// Authoritative movement state the server sim integrates each tick and
/// TFReplication mirrors to clients (velocity/view for PawnInfo + prediction).
struct TFPawnMoveComp {
    float vel[3]  = {0.0f, 0.0f, 0.0f};
    float yaw     = 0.0f;   ///< radians (camera convention, see TFMovementModel.h)
    float pitch   = 0.0f;   ///< radians
    bool  grounded = true;
};

// --------------------------------------------------------------------------
// Vehicles (W3 systems consume these; defined here per DESIGN §3)
// --------------------------------------------------------------------------

struct TFVehicleComp {
    VehicleId vehId = VehicleId::None;
    PlayerId  seats[8] = { kInvalidPlayer, kInvalidPlayer, kInvalidPlayer, kInvalidPlayer,
                           kInvalidPlayer, kInvalidPlayer, kInvalidPlayer, kInvalidPlayer };
    float     fuel = 0.0f;
};

/// Attached to a pawn while seated in a vehicle.
struct TFSeatComp {
    EntityId vehicle = 0;
    uint8_t  seatIdx = 0;
};

/// Aegis mobile-spawn deploy toggle.
struct TFAegisDeployComp {
    bool active = false;
};

// --------------------------------------------------------------------------
// Territory (W2 systems consume these; defined here per DESIGN §3)
// --------------------------------------------------------------------------

struct TFRegionComp {
    RegionId regionId = kInvalidRegion;
};

struct TFCapturePointComp {
    RegionId  regionId = kInvalidRegion;
    uint8_t   idx      = 0;
    float     progress = 0.0f;          ///< 0..1 toward `owner`
    FactionId owner    = FactionId::None;
};

// --------------------------------------------------------------------------
// Deployables (W3)
// --------------------------------------------------------------------------

struct TFDeployableComp {
    DeployableKind kind  = DeployableKind::FabTurret;
    PlayerId       owner = kInvalidPlayer;
    float          life  = 0.0f;        ///< remaining lifetime seconds
};

} // namespace Terrafront
