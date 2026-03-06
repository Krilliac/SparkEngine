/**
 * @file VehicleSystem.h
 * @brief Ground and aerial vehicle system for Spark Engine
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a comprehensive vehicle system with ground vehicles (buggy, tank, APC,
 * motorcycle, truck) and aerial vehicles (helicopter, jet, dropship, drone).
 * Each vehicle type has unique handling, physics, and weapon characteristics.
 * Vehicles support multiple seats with driver, gunner, and passenger roles.
 */

#pragma once
#include "../Core/Platform.h"

#include "GameObject.h"
#include "Enums/GameSystemEnums.h"
#include "Projectiles/WeaponStats.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <vector>
#include <memory>
#include <string>
#include <functional>

using namespace DirectX;
using SparkEditor::VehicleType;
using SparkEditor::VehicleSeatRole;

class Player;
class InputManager;

namespace Spark {

/**
 * @brief Vehicle seat definition
 */
struct VehicleSeat {
    VehicleSeatRole role = VehicleSeatRole::PASSENGER;
    XMFLOAT3 localOffset = {0, 0, 0};    ///< Seat position relative to vehicle center
    float yawMin = -180.0f;               ///< Turret/look yaw limits (degrees)
    float yawMax = 180.0f;
    float pitchMin = -45.0f;
    float pitchMax = 45.0f;
    WeaponType mountedWeapon = WeaponType::PISTOL; ///< Weapon for gunner seats
    float weaponFireRate = 5.0f;          ///< Rounds per second
    float weaponDamage = 25.0f;
    Player* occupant = nullptr;           ///< Currently seated player (not owned)
    bool isOccupied = false;

    VehicleSeat() = default;
    VehicleSeat(VehicleSeatRole r, const XMFLOAT3& off)
        : role(r), localOffset(off) {}
};

/**
 * @brief Vehicle handling parameters
 */
struct VehicleHandling {
    float maxSpeed = 20.0f;               ///< Maximum forward speed
    float reverseSpeed = 8.0f;            ///< Maximum reverse speed
    float acceleration = 15.0f;           ///< Acceleration rate
    float brakeForce = 25.0f;             ///< Braking deceleration
    float turnRate = 2.0f;                ///< Turning speed (radians/sec)
    float friction = 0.95f;               ///< Ground friction coefficient
    float mass = 1000.0f;                 ///< Vehicle mass in kg
    float suspensionStiffness = 50.0f;    ///< Suspension spring force
    float suspensionDamping = 10.0f;      ///< Suspension damping

    // Aerial-specific
    float liftForce = 0.0f;              ///< Vertical thrust (aerial only)
    float maxAltitude = 100.0f;           ///< Maximum flight altitude
    float pitchRate = 1.5f;               ///< Pitch rotation speed
    float rollRate = 2.0f;                ///< Roll rotation speed
    float hoverStability = 0.8f;          ///< Auto-stabilization strength
};

/**
 * @brief Vehicle definition with all parameters
 */
struct VehicleDefinition {
    VehicleType type = VehicleType::BUGGY;
    std::string name;
    std::string description;

    // Stats
    float maxHealth = 500.0f;
    float maxArmor = 100.0f;
    float explosionDamage = 200.0f;       ///< Damage dealt on destruction
    float explosionRadius = 10.0f;        ///< Explosion radius on destruction

    // Handling
    VehicleHandling handling;

    // Seats
    std::vector<VehicleSeat> seats;

    // Visual
    XMFLOAT3 dimensions = {2, 1.5f, 4};  ///< Bounding box dimensions
    float meshScale = 1.0f;

    bool isAerial = false;                ///< Ground vs aerial vehicle
};

/**
 * @brief Active vehicle instance in the game world
 *
 * Extends GameObject with vehicle-specific physics, seat management,
 * and weapon systems. Supports both ground and aerial movement modes.
 */
class Vehicle : public GameObject {
public:
    Vehicle();
    explicit Vehicle(const VehicleDefinition& def);
    ~Vehicle() override = default;

    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context) override;
    void Update(float deltaTime) override;
    void Render(const XMMATRIX& view, const XMMATRIX& proj) override;

    void OnHit(GameObject* target) override;
    void OnHitWorld(const XMFLOAT3& hitPoint, const XMFLOAT3& normal) override;

    // === Vehicle Control ===

    /**
     * @brief Apply throttle input (-1 to 1, negative = reverse)
     */
    void SetThrottle(float throttle);

    /**
     * @brief Apply steering input (-1 to 1, negative = left)
     */
    void SetSteering(float steering);

    /**
     * @brief Apply brake
     */
    void SetBrake(bool braking);

    /**
     * @brief Apply vertical thrust (aerial vehicles)
     */
    void SetVerticalThrust(float thrust);

    /**
     * @brief Fire the weapon for a specific seat
     */
    void FireWeapon(int seatIndex);

    // === Seat Management ===

    /**
     * @brief Enter a specific seat
     * @return true if successfully entered
     */
    bool EnterSeat(Player* player, int seatIndex = -1);

    /**
     * @brief Exit a specific seat
     * @return true if successfully exited
     */
    bool ExitSeat(Player* player);

    /**
     * @brief Get the first available seat
     * @return Seat index or -1 if full
     */
    int GetFirstAvailableSeat() const;

    /**
     * @brief Get the driver's seat index
     */
    int GetDriverSeatIndex() const;

    /**
     * @brief Get the exit position for a player leaving a seat
     */
    XMFLOAT3 GetExitPosition(int seatIndex) const;

    /**
     * @brief Check if vehicle has any occupants
     */
    bool IsOccupied() const;

    /**
     * @brief Check if a specific player is in this vehicle
     */
    bool ContainsPlayer(const Player* player) const;

    /**
     * @brief Get the seat index for a player
     */
    int GetPlayerSeatIndex(const Player* player) const;

    // === Damage ===

    /**
     * @brief Apply damage to the vehicle
     */
    void TakeDamage(float damage);

    /**
     * @brief Repair the vehicle
     */
    void Repair(float amount);

    // === State ===

    float GetHealth() const { return m_health; }
    float GetMaxHealth() const { return m_definition.maxHealth; }
    float GetArmor() const { return m_armor; }
    float GetSpeed() const { return m_currentSpeed; }
    float GetMaxSpeed() const { return m_definition.handling.maxSpeed; }
    bool IsDestroyed() const { return m_health <= 0.0f; }
    bool IsAerial() const { return m_definition.isAerial; }
    VehicleType GetVehicleType() const { return m_definition.type; }
    const std::string& GetVehicleName() const { return m_definition.name; }
    const VehicleDefinition& GetDefinition() const { return m_definition; }
    const std::vector<VehicleSeat>& GetSeats() const { return m_seats; }
    XMFLOAT3 GetVelocity() const { return m_velocity; }
    float GetAltitude() const { return GetPosition().y; }

    /**
     * @brief Set destruction callback
     */
    void SetDestructionCallback(std::function<void(Vehicle*)> callback) {
        m_destructionCallback = callback;
    }

protected:
    void CreateMesh() override;

private:
    VehicleDefinition m_definition;
    std::vector<VehicleSeat> m_seats;

    // State
    float m_health = 500.0f;
    float m_armor = 100.0f;
    float m_currentSpeed = 0.0f;
    XMFLOAT3 m_velocity = {0, 0, 0};
    float m_throttle = 0.0f;
    float m_steering = 0.0f;
    bool m_braking = false;
    float m_verticalThrust = 0.0f;

    // Aerial state
    float m_pitch = 0.0f;
    float m_roll = 0.0f;

    // Timers
    float m_weaponCooldown = 0.0f;
    float m_engineSoundTimer = 0.0f;

    // Callbacks
    std::function<void(Vehicle*)> m_destructionCallback;

    // Internal updates
    void UpdateGroundVehicle(float dt);
    void UpdateAerialVehicle(float dt);
    void UpdateWeapons(float dt);
    void ApplyGravity(float dt);
    void CheckGroundCollision();
    void Explode();
};

/**
 * @brief Vehicle system manager
 *
 * Manages vehicle definitions, spawning, and global vehicle state.
 */
class VehicleSystem {
public:
    VehicleSystem();
    ~VehicleSystem() = default;

    /**
     * @brief Initialize vehicle definitions
     */
    bool Initialize();

    /**
     * @brief Update all active vehicles
     */
    void Update(float deltaTime);

    /**
     * @brief Get a vehicle definition by type
     */
    const VehicleDefinition& GetDefinition(VehicleType type) const;

    /**
     * @brief Spawn a vehicle at a position
     * @return Pointer to spawned vehicle (owned by the system)
     */
    Vehicle* SpawnVehicle(VehicleType type, const XMFLOAT3& position,
                          ID3D11Device* device, ID3D11DeviceContext* context);

    /**
     * @brief Remove a destroyed vehicle
     */
    void RemoveVehicle(Vehicle* vehicle);

    /**
     * @brief Get all active vehicles
     */
    const std::vector<std::unique_ptr<Vehicle>>& GetVehicles() const { return m_vehicles; }

    /**
     * @brief Find nearest vehicle to a position within range
     */
    Vehicle* FindNearestVehicle(const XMFLOAT3& position, float maxRange = 5.0f) const;

    /**
     * @brief Get vehicle count
     */
    size_t GetVehicleCount() const { return m_vehicles.size(); }

    // Console integration
    std::string Console_ListVehicles() const;
    bool Console_SpawnVehicle(const std::string& type, float x, float y, float z,
                              ID3D11Device* device, ID3D11DeviceContext* context);

private:
    void InitVehicleDefinitions();
    void InitBuggy();
    void InitTank();
    void InitAPC();
    void InitMotorcycle();
    void InitTruck();
    void InitHelicopter();
    void InitJet();
    void InitDropship();
    void InitDrone();

    std::unordered_map<int, VehicleDefinition> m_definitions;
    std::vector<std::unique_ptr<Vehicle>> m_vehicles;
    VehicleDefinition m_defaultDefinition;

    static constexpr int MAX_VEHICLES = 32;
};

} // namespace Spark
