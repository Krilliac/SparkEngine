/**
 * @file Player.h
 * @brief Player character controller with movement, combat, and health systems
 * @author Spark Engine Team
 * @date 2025
 * 
 * The Player class represents the player character in the game world, inheriting
 * from GameObject and providing comprehensive first-person character functionality
 * including movement, jumping, weapon handling, health/armor systems, and combat.
 */

#pragma once

// Subsystem headers
#include "PlayerTypes.h"

#include "Spark/SparkExport.h"
#include "Core/Platform.h"

#include "Game/GameObject.h"
#include "ClassSystem.h"
#include "GravitySystem.h"
#include "Utils/Assert.h"
#include "Camera/SparkEngineCamera.h"
#include "Input/InputManager.h"
#include "Projectiles/WeaponStats.h"
#include "Projectiles/ProjectilePool.h"
#include "Utils/MathUtils.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <mutex>
#include <functional>

using SparkEditor::WeaponType;

// Forward declarations
class GraphicsEngine;
namespace Spark
{
    class SimpleConsole;
    class Vehicle;
    class GravitySystem;
    class InteractionSystem;
} // namespace Spark

/**
 * @brief Player character controller class with full console integration
 * 
 * The Player class provides a complete first-person character controller with
 * movement physics, weapon systems, health management, combat mechanics, and
 * comprehensive console integration for real-time debugging and manipulation.
 * 
 * Features include:
 * - First-person movement (WASD, jump, crouch, run)
 * - Health and armor systems with damage/healing
 * - Weapon system with multiple weapon types and reloading
 * - Physics-based movement with gravity and collision
 * - Animation systems (head bobbing, footsteps)
 * - Real-time console integration for debugging
 * - Live state monitoring and modification
 * 
 * @note The Player does not own the camera, input manager, or projectile pool references
 * @warning Ensure SetProjectilePool() is called before using weapons
 */
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251) // class needs dll-interface
#pragma warning(disable : 4275) // non dll-interface class used as base
#endif

class SPARK_GAME_API Player : public GameObject
{
    using GameObject::Initialize; // Bring base 2-arg overload into scope

  public:
    /**
     * @brief Default constructor
     * 
     * Initializes player stats, movement parameters, and combat variables
     * to default values suitable for first-person gameplay.
     */
    Player();

    /**
     * @brief Default destructor
     * 
     * Uses default cleanup as all resources are managed automatically.
     */
    ~Player() override;

    /**
     * @brief Initialize the player with required engine systems
     * 
     * Sets up the player character with DirectX resources and engine system
     * references. Configures the player for gameplay with camera and input.
     * 
     * @param device DirectX 11 device for resource creation
     * @param context DirectX 11 device context for rendering
     * @param camera Pointer to the first-person camera system
     * @param input Pointer to the input manager for controls
     * @return HRESULT indicating success or failure of initialization
     * @note The camera and input pointers are stored but not owned by Player
     */
    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context, SparkEngineCamera* camera,
                       InputManager* input);

    /**
     * @brief Update player logic for the current frame
     * 
     * Processes input, updates movement physics, handles combat timing,
     * updates animations, and manages player state for the current frame.
     * 
     * @param dt Delta time since last frame in seconds
     */
    void Update(float dt) override;

    /**
     * @brief Render the player (typically no visual representation in first-person)
     * 
     * In first-person mode, this typically doesn't render anything visible.
     * May be used for debugging visualization or third-person mode.
     * 
     * @param view Camera view transformation matrix
     * @param proj Camera projection transformation matrix
     */
    void Render(const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj) override;

    /**
     * @brief Render the weapon model in first-person view
     * 
     * Renders the current weapon model positioned relative to the camera
     * for first-person weapon display.
     * 
     * @param view Camera view transformation matrix
     * @param proj Camera projection transformation matrix
     */
    void RenderWeapon(const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj);

    /**
     * @brief Apply damage to the player
     * 
     * Reduces player health, accounting for armor protection. Can result
     * in player death if health reaches zero.
     * 
     * @param dmg Amount of damage to apply (positive value)
     */
    void TakeDamage(float dmg);

    /**
     * @brief Restore player health
     * 
     * Increases player health up to the maximum health limit.
     * 
     * @param amt Amount of health to restore (positive value)
     */
    void Heal(float amt);

    /**
     * @brief Increase player armor
     * 
     * Adds armor points up to the maximum armor limit.
     * 
     * @param amt Amount of armor to add (positive value)
     */
    void AddArmor(float amt);

    /**
     * @brief Execute a jump if the player is grounded
     * 
     * Applies upward velocity for jumping. Only works if the player
     * is currently on the ground.
     */
    void Jump();

    /**
     * @brief Begin reloading the current weapon
     * 
     * Starts the reload process for the current weapon if not already
     * reloading and if the weapon can be reloaded.
     */
    void StartReload();

    /**
     * @brief Fire the current weapon
     * 
     * Attempts to fire the current weapon if ammunition is available,
     * the weapon is not on cooldown, and not currently reloading.
     */
    void Fire();

    /**
     * @brief Switch to a different weapon type
     * 
     * Changes the current weapon to the specified type and resets
     * weapon-specific timers and ammunition.
     * 
     * @param t New weapon type to switch to
     */
    void ChangeWeapon(WeaponType t);

    /**
     * @brief Set the projectile pool for weapon projectiles
     * 
     * Assigns the projectile pool that will be used when firing weapons.
     * This must be set before the player can use weapons.
     * 
     * @param pool Pointer to the projectile object pool
     * @warning Must not be null - assertion will fail if null is passed
     */
    void SetProjectilePool(ProjectilePool* pool)
    {
        ASSERT_NOT_NULL(pool);
        m_projectilePool = pool;
    }

    /** @brief Get the player's projectile pool (owned or external) */
    ProjectilePool* GetProjectilePool() const { return m_projectilePool; }

    /**
     * @brief Get current player health
     * @return Current health value
     */
    float GetHealth() const { return m_health; }

    /**
     * @brief Get maximum player health
     * @return Maximum health value
     */
    float GetMaxHealth() const { return m_maxHealth; }

    /**
     * @brief Get current player armor
     * @return Current armor value
     */
    float GetArmor() const { return m_armor; }

    /**
     * @brief Get maximum player armor
     * @return Maximum armor value
     */
    float GetMaxArmor() const { return m_maxArmor; }

    /**
     * @brief Get current player stamina
     * @return Current stamina value
     */
    float GetStamina() const { return m_stamina; }

    /**
     * @brief Get current player velocity
     * @return Current velocity vector
     */
    DirectX::XMFLOAT3 GetVelocity() const { return m_velocity; }

    /**
     * @brief Get current weapon type
     * @return Current weapon type
     */
    WeaponType GetCurrentWeaponType() const { return m_currentWeapon.Type; }

    /**
     * @brief Get current ammunition count
     * @return Current ammunition in magazine
     */
    int GetCurrentAmmo() const { return m_currentAmmo; }

    /**
     * @brief Check if the player is alive
     * @return true if health is above zero, false if dead
     */
    bool IsAlive() const { return m_health > 0.0f; }

    /**
     * @brief Check if the player is on the ground
     * @return true if grounded, false if in air
     */
    bool IsGrounded() const { return m_isGrounded; }

    /**
     * @brief Check if the player is currently reloading
     * @return true if reloading, false otherwise
     */
    bool IsReloading() const { return m_isReloading; }

    /**
     * @brief Set the running state of the player
     * @param v true to enable running, false for normal walking speed
     */
    void SetRunning(bool v) { m_isRunning = v; }

    /**
     * @brief Set the crouching state of the player
     * @param v true to crouch, false to stand normally
     */
    void SetCrouching(bool v) { m_isCrouching = v; }

    // ============================================================================
    // CLASS SYSTEM INTEGRATION
    // ============================================================================

    /**
     * @brief Set the player's class and apply class stats/loadout
     * @param classType The class to switch to
     * @param classSystem Pointer to the class system for definitions
     */
    void SetClass(PlayerClass classType, Spark::ClassSystem* classSystem);

    /**
     * @brief Get the player's current class type
     * @return Current PlayerClass
     */
    PlayerClass GetClass() const { return m_playerClass; }

    /**
     * @brief Get the player's current class name
     * @return String name of current class
     */
    const char* GetClassName() const { return Spark::ClassSystem::GetClassName(m_playerClass); }

    /**
     * @brief Get the rechargeable shield value
     * @return Current shield points
     */
    float GetShield() const { return m_shield; }

    /**
     * @brief Get maximum shield value
     * @return Maximum shield points
     */
    float GetMaxShield() const { return m_maxShield; }

    /**
     * @brief Get the ability energy pool
     * @return Current energy
     */
    float GetEnergy() const { return m_energy; }

    /**
     * @brief Get maximum energy
     * @return Maximum energy points
     */
    float GetMaxEnergy() const { return m_maxEnergy; }

    /**
     * @brief Activate primary class ability (F key)
     * @return true if ability activated
     */
    bool ActivatePrimaryAbility();

    /**
     * @brief Activate secondary class ability (G key)
     * @return true if ability activated
     */
    bool ActivateSecondaryAbility();

    /**
     * @brief Deactivate primary ability manually
     */
    void DeactivatePrimaryAbility();

    /**
     * @brief Check if primary ability is currently active
     */
    bool IsPrimaryAbilityActive() const { return m_primaryAbility.isActive; }

    /**
     * @brief Check if secondary ability is currently active
     */
    bool IsSecondaryAbilityActive() const { return m_secondaryAbility.isActive; }

    /**
     * @brief Get primary ability state for HUD display
     */
    const Spark::AbilityState& GetPrimaryAbilityState() const { return m_primaryAbility; }

    /**
     * @brief Get secondary ability state for HUD display
     */
    const Spark::AbilityState& GetSecondaryAbilityState() const { return m_secondaryAbility; }

    /**
     * @brief Check if a weapon switch is valid for the current class
     */
    bool CanEquipWeapon(WeaponType type) const;

    /**
     * @brief Switch to primary loadout weapon
     */
    void EquipPrimary();

    /**
     * @brief Switch to secondary loadout weapon
     */
    void EquipSecondary();

    /**
     * @brief Switch to sidearm
     */
    void EquipSidearm();

    /**
     * @brief Switch to class tool
     */
    void EquipTool();

    /**
     * @brief Get the active loadout slot index (0=primary, 1=secondary, 2=sidearm, 3=tool)
     */
    int GetActiveLoadoutSlot() const { return m_activeLoadoutSlot; }

    // ============================================================================
    // VEHICLE SYSTEM INTEGRATION
    // ============================================================================

    /**
     * @brief Enter a vehicle
     * @param vehicle The vehicle to enter
     * @param seatIndex Preferred seat (-1 for first available)
     * @return true if successfully entered
     */
    bool EnterVehicle(Spark::Vehicle* vehicle, int seatIndex = -1);

    /**
     * @brief Exit the current vehicle
     * @return true if successfully exited
     */
    bool ExitVehicle();

    /**
     * @brief Check if the player is currently in a vehicle
     */
    bool IsInVehicle() const { return m_currentVehicle != nullptr; }

    /**
     * @brief Get the current vehicle (null if not in one)
     */
    Spark::Vehicle* GetCurrentVehicle() const { return m_currentVehicle; }

    /**
     * @brief Get the seat index in the current vehicle
     */
    int GetVehicleSeatIndex() const { return m_vehicleSeatIndex; }

    // ============================================================================
    // GRAVITY ZONE SUPPORT
    // ============================================================================

    /**
     * @brief Set the gravity system for zone-based gravity
     */
    void SetGravitySystem(Spark::GravitySystem* gravSystem) { m_gravitySystem = gravSystem; }

    /**
     * @brief Get the current gravity state
     */
    const Spark::GravityState& GetGravityState() const { return m_gravityState; }

    /**
     * @brief Check if player is in a gravity zone
     */
    bool IsInGravityZone() const { return m_gravityState.inZone; }

    /**
     * @brief Set external velocity (used by jump pads, explosions, etc.)
     */
    void SetVelocity(const DirectX::XMFLOAT3& vel) { m_velocity = vel; }

    // ============================================================================
    // INTERACTION SYSTEM
    // ============================================================================

    /**
     * @brief Set the interaction system for use-key interactions
     */
    void SetInteractionSystem(Spark::InteractionSystem* interactSystem) { m_interactionSystem = interactSystem; }

    /**
     * @brief Set the graphics engine for weapon rendering shader setup
     * @param graphics Graphics engine pointer (not owned)
     */
    void SetGraphicsEngine(GraphicsEngine* graphics) { m_graphics = graphics; }

    // ============================================================================
    // CONSOLE INTEGRATION METHODS - Full Cross-Code Hooking
    // ============================================================================

    /**
     * @brief Set player health directly (console integration)
     * @param health New health value (will be clamped to valid range)
     */
    void Console_SetHealth(float health);

    /**
     * @brief Set player armor directly (console integration)
     * @param armor New armor value (will be clamped to valid range)
     */
    void Console_SetArmor(float armor);

    /**
     * @brief Set player maximum health (console integration)
     * @param maxHealth New maximum health value
     */
    void Console_SetMaxHealth(float maxHealth);

    /**
     * @brief Set player movement speed (console integration)
     * @param speed New movement speed multiplier
     */
    void Console_SetSpeed(float speed);

    /**
     * @brief Set player jump height (console integration)
     * @param height New jump height value
     */
    void Console_SetJumpHeight(float height);

    /**
     * @brief Set player position directly (console integration)
     * @param x New X coordinate
     * @param y New Y coordinate
     * @param z New Z coordinate
     */
    void Console_SetPosition(float x, float y, float z);

    /**
     * @brief Enable/disable god mode (console integration)
     * @param enabled true to enable god mode, false to disable
     */
    void Console_SetGodMode(bool enabled);

    /**
     * @brief Enable/disable noclip mode (console integration)
     * @param enabled true to enable noclip, false to disable
     */
    void Console_SetNoclip(bool enabled);

    /**
     * @brief Enable/disable infinite ammo (console integration)
     * @param enabled true to enable infinite ammo, false to disable
     */
    void Console_SetInfiniteAmmo(bool enabled);

    /**
     * @brief Give ammunition to current weapon (console integration)
     * @param amount Amount of ammunition to add
     */
    void Console_GiveAmmo(int amount);

    /**
     * @brief Force weapon change (console integration)
     * @param weaponType Weapon type to switch to
     */
    void Console_ChangeWeapon(WeaponType weaponType);

    // PlayerState struct is defined in PlayerTypes.h (included at the top of this file)

    /**
     * @brief Get complete player state (console integration)
     * @return PlayerState structure with all current values
     */
    PlayerState Console_GetState() const;

    /**
     * @brief Register console state change callback
     * @param callback Function to call when state changes
     */
    void Console_RegisterStateCallback(std::function<void(const PlayerState&)> callback);

    /**
     * @brief Apply physics settings from console
     * @param gravity Gravity force to apply
     * @param friction Friction coefficient for movement
     */
    void Console_ApplyPhysicsSettings(float gravity, float friction);

    /**
     * @brief Collision callback when player hits another game object
     * 
     * Handles interactions when the player collides with other objects
     * in the game world (items, enemies, etc.).
     * 
     * @param target Pointer to the game object that was hit
     */
    void OnHit(GameObject* target) override;

    /**
     * @brief Collision callback when player hits world geometry
     * 
     * Handles collisions with static world geometry like walls, floors,
     * and other environmental objects.
     * 
     * @param hitPoint World position where the collision occurred
     * @param normal Surface normal at the collision point
     */
    void OnHitWorld(const DirectX::XMFLOAT3& hitPoint, const DirectX::XMFLOAT3& normal) override;

  private:
    // Stats
    float m_health{100}, m_maxHealth{100};   ///< Current and maximum health
    float m_armor{0}, m_maxArmor{100};       ///< Current and maximum armor
    float m_stamina{100}, m_maxStamina{100}; ///< Current and maximum stamina
    float m_speed{5}, m_jumpHeight{3};       ///< Movement speed and jump height

    // Class system
    PlayerClass m_playerClass{PlayerClass::SCOUT}; ///< Current class
    Spark::ClassSystem* m_classSystem{nullptr};    ///< Reference to class system
    float m_shield{0}, m_maxShield{50};            ///< Rechargeable shield
    float m_shieldRechargeRate{10.0f};
    float m_shieldRechargeDelay{6.0f};
    float m_shieldRechargeTimer{0.0f};     ///< Timer since last damage
    float m_energy{100}, m_maxEnergy{100}; ///< Ability energy pool
    float m_energyRegenRate{10.0f};        ///< Energy regen per second
    Spark::AbilityState m_primaryAbility;
    Spark::AbilityState m_secondaryAbility;
    float m_damageResistance{0.0f};
    float m_explosiveResistance{0.0f};
    float m_sprintMultiplier{2.0f};
    float m_crouchMultiplier{0.5f};
    float m_adsSpeedMultiplier{0.5f};
    Spark::ClassLoadout m_loadout;
    int m_activeLoadoutSlot{0}; ///< 0=primary, 1=secondary, 2=sidearm, 3=tool
    std::vector<WeaponType> m_allowedWeapons;
    bool m_jetpackActive{false};
    float m_jetpackFuel{100.0f};
    float m_jetpackMaxFuel{100.0f};
    float m_jetpackThrust{8.0f};
    bool m_cloakActive{false};
    float m_energyShieldHP{0.0f};
    float m_energyShieldMaxHP{200.0f};
    bool m_lockedDown{false}; ///< Titan lockdown state

    // Movement
    DirectX::XMFLOAT3 m_velocity{};              ///< Current velocity vector
    bool m_isGrounded{true}, m_isRunning{false}, ///< Movement state flags
        m_isCrouching{false}, m_isJumping{false};

    // Combat
    WeaponStats m_currentWeapon;                  ///< Current weapon configuration
    int m_currentAmmo{0};                         ///< Current ammunition count
    float m_fireTimer{0}, m_reloadTimer{0};       ///< Weapon timing
    bool m_isReloading{false}, m_isFiring{false}; ///< Combat state

    // Weapon models for rendering
    std::unique_ptr<class Model> m_pistolModel;  ///< Pistol weapon model
    std::unique_ptr<class Model> m_rifleModel;   ///< Rifle weapon model
    std::unique_ptr<class Model> m_shotgunModel; ///< Shotgun weapon model
    std::unique_ptr<class Model> m_rocketModel;  ///< Rocket launcher model
    std::unique_ptr<class Model> m_grenadeModel; ///< Grenade launcher model

    // Console integration state
    bool m_godModeEnabled{false};      ///< God mode prevents damage
    bool m_noclipEnabled{false};       ///< Noclip disables collision
    bool m_infiniteAmmoEnabled{false}; ///< Infinite ammo prevents consumption
    float m_gravityForce{-20.0f};      ///< Gravity force applied
    float m_frictionCoeff{0.9f};       ///< Movement friction coefficient

    // Console callback system
    std::function<void(const PlayerState&)> m_stateCallback;
    mutable std::recursive_mutex m_stateMutex; ///< Thread safety for state access

    // External references (not owned)
    SparkEngineCamera* m_camera{nullptr};                  ///< Reference to camera system
    InputManager* m_input{nullptr};                        ///< Reference to input manager
    GraphicsEngine* m_graphics{nullptr};                   ///< Reference to graphics engine
    ProjectilePool* m_projectilePool{nullptr};             ///< Reference to projectile pool
    std::unique_ptr<ProjectilePool> m_ownedProjectilePool; ///< Owned pool if created by Player

    // Vehicle system
    Spark::Vehicle* m_currentVehicle{nullptr}; ///< Vehicle currently occupied (not owned)
    int m_vehicleSeatIndex{-1};                ///< Seat index in current vehicle

    // Gravity zone system
    Spark::GravitySystem* m_gravitySystem{nullptr}; ///< Gravity system reference (not owned)
    Spark::GravityState m_gravityState;             ///< Current gravity state with transitions

    // Interaction system
    Spark::InteractionSystem* m_interactionSystem{nullptr}; ///< Interaction system (not owned)

    // Collision & animation
    BoundingSphere m_collisionSphere;        ///< Collision bounds for player
    float m_bobTimer{0}, m_footstepTimer{0}; ///< Animation timers

    /**
     * @brief Process input for movement and actions
     * @param dt Delta time for frame-rate independent input
     */
    void HandleInput(float dt);

    /**
     * @brief Update movement based on input and physics
     * @param dt Delta time for frame-rate independent movement
     */
    void UpdateMovement(float dt);

    /**
     * @brief Update weapon timers and combat state
     * @param dt Delta time for frame-rate independent timing
     */
    void UpdateCombat(float dt);

    /**
     * @brief Update physics simulation (gravity, velocity, etc.)
     * @param dt Delta time for frame-rate independent physics
     */
    void UpdatePhysics(float dt);

    /**
     * @brief Update animation timers and effects
     * @param dt Delta time for frame-rate independent animation
     */
    void UpdateAnimation(float dt);

    /**
     * @brief Check and resolve collisions with world geometry
     */
    void UpdateCollision();

    /**
     * @brief Apply gravity to player velocity
     * @param dt Delta time for frame-rate independent gravity
     */
    void ApplyGravity(float dt);

    /**
     * @brief Check if player is touching the ground
     */
    void CheckGroundCollision();

    /**
     * @brief Handle footstep sound timing
     * @param dt Delta time for frame-rate independent audio
     */
    void HandleFootsteps(float dt);

    /**
     * @brief Update class-specific mechanics (shield, energy, abilities)
     * @param dt Delta time
     */
    void UpdateClassMechanics(float dt);

    /**
     * @brief Handle jetpack mechanics for Scout
     * @param dt Delta time
     */
    void UpdateJetpack(float dt);

    /**
     * @brief Handle cloak mechanics for Recon
     * @param dt Delta time
     */
    void UpdateCloak(float dt);

    /**
     * @brief Handle energy shield mechanics for Vanguard
     * @param dt Delta time
     */
    void UpdateEnergyShield(float dt);

    /**
     * @brief Apply class definition stats to player
     */
    void ApplyClassStats(const Spark::ClassDefinition& classDef);

    /**
     * @brief Get weapon statistics for a specific weapon type
     * @param type Weapon type to get stats for
     * @return WeaponStats structure with weapon parameters
     */
    WeaponStats GetWeaponStats(WeaponType type);

    /**
     * @brief Calculate the direction vector for projectile firing
     * @return Normalized direction vector from camera forward
     */
    DirectX::XMFLOAT3 CalculateFireDirection();

    /**
     * @brief Notify console of state change
     */
    void NotifyStateChange();

    /**
     * @brief Thread-safe state access helper
     * @return Current player state with thread safety
     */
    PlayerState GetStateThreadSafe() const;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif
