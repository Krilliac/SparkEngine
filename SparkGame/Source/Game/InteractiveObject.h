/**
 * @file InteractiveObject.h
 * @brief Interactive object system for Spark Engine
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a system for interactive world objects including doors, class change
 * terminals, switches, elevators, jump pads, teleporters, pickups, and
 * destructible objects. Each interactive object supports proximity detection,
 * use-key activation, and visual/audio feedback.
 */

#pragma once
#include "Core/Platform.h"

#include "Game/GameObject.h"
#include "Enums/GameSystemEnums.h"
#include "Projectiles/WeaponStats.h"
#include "ClassSystem.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <vector>
#include <memory>
#include <string>
#include <functional>

using SparkEditor::InteractiveObjectType;
using SparkEditor::PlayerClass;
using SparkEditor::WeaponType;

class Player;

namespace Spark
{

    /**
 * @brief Base interactive object in the game world
 *
 * Provides proximity-based interaction with the player.
 * When the player is within interaction range and presses the use key,
 * the object's Interact() method is called.
 */
    class InteractiveObject : public GameObject
    {
      public:
        InteractiveObject();
        explicit InteractiveObject(InteractiveObjectType type);
        ~InteractiveObject() override = default;

        HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context) override;
        void Update(float deltaTime) override;
        void Render(const XMMATRIX& view, const XMMATRIX& proj) override;

        void OnHit(GameObject* target) override;
        void OnHitWorld(const XMFLOAT3& hitPoint, const XMFLOAT3& normal) override;

        /**
     * @brief Interact with this object (called when player presses use key)
     * @param player The interacting player
     * @return true if interaction succeeded
     */
        virtual bool Interact(Player* player);

        /**
     * @brief Check if the player is within interaction range
     */
        bool IsPlayerInRange(const Player* player) const;

        /**
     * @brief Get the interaction prompt text for HUD display
     */
        virtual std::string GetInteractionPrompt() const;

        // === Properties ===

        InteractiveObjectType GetObjectType() const { return m_objectType; }
        float GetInteractionRange() const { return m_interactionRange; }
        void SetInteractionRange(float range) { m_interactionRange = range; }
        bool IsInteractable() const { return m_isInteractable; }
        void SetInteractable(bool v) { m_isInteractable = v; }
        bool IsHighlighted() const { return m_isHighlighted; }
        void SetHighlighted(bool v) { m_isHighlighted = v; }

        /**
     * @brief Set callback for when interaction occurs
     */
        void SetInteractionCallback(std::function<void(InteractiveObject*, Player*)> callback)
        {
            m_interactionCallback = callback;
        }

      protected:
        InteractiveObjectType m_objectType = InteractiveObjectType::SWITCH;
        float m_interactionRange = 3.0f;
        bool m_isInteractable = true;
        bool m_isHighlighted = false;
        std::function<void(InteractiveObject*, Player*)> m_interactionCallback;
    };

    /**
 * @brief Door object that can be opened and closed
 *
 * Supports sliding and rotating doors with configurable open/close
 * speed and auto-close timer.
 */
    class DoorObject : public InteractiveObject
    {
      public:
        enum class DoorState
        {
            Closed,
            Opening,
            Open,
            Closing
        };
        enum class DoorStyle
        {
            Sliding,
            Rotating
        };

        DoorObject();
        ~DoorObject() override = default;

        void Update(float deltaTime) override;
        bool Interact(Player* player) override;
        std::string GetInteractionPrompt() const override;

        // === Door Properties ===

        DoorState GetDoorState() const { return m_doorState; }
        bool IsOpen() const { return m_doorState == DoorState::Open; }
        bool IsClosed() const { return m_doorState == DoorState::Closed; }

        void Open();
        void Close();
        void Toggle();
        void SetLocked(bool locked) { m_isLocked = locked; }
        bool IsLocked() const { return m_isLocked; }

        void SetOpenSpeed(float speed) { m_openSpeed = speed; }
        void SetAutoCloseTime(float time) { m_autoCloseTime = time; }
        void SetAutoClose(bool enabled) { m_autoClose = enabled; }

        void SetSlideDirection(const XMFLOAT3& dir) { m_slideDirection = dir; }
        void SetSlideDistance(float dist) { m_slideDistance = dist; }
        void SetDoorStyle(DoorStyle style) { m_doorStyle = style; }
        void SetRotationAngle(float angle) { m_rotationAngle = angle; }

      protected:
        void CreateMesh() override;

      private:
        DoorState m_doorState = DoorState::Closed;
        DoorStyle m_doorStyle = DoorStyle::Sliding;
        bool m_isLocked = false;
        bool m_autoClose = true;
        float m_openSpeed = 3.0f;
        float m_autoCloseTime = 5.0f;
        float m_autoCloseTimer = 0.0f;
        float m_openProgress = 0.0f; ///< 0 = closed, 1 = fully open

        // Sliding door params
        XMFLOAT3 m_slideDirection = {1, 0, 0};
        float m_slideDistance = 2.0f;
        XMFLOAT3 m_closedPosition = {0, 0, 0};

        // Rotating door params
        float m_rotationAngle = 90.0f; ///< Degrees to rotate when open
        float m_closedRotationY = 0.0f;
    };

    /**
 * @brief Class change terminal
 *
 * Allows the player to switch between available classes.
 * Cycles through classes on each interaction.
 */
    class ClassTerminal : public InteractiveObject
    {
      public:
        ClassTerminal();
        ~ClassTerminal() override = default;

        bool Interact(Player* player) override;
        std::string GetInteractionPrompt() const override;

        void SetClassSystem(ClassSystem* classSystem) { m_classSystem = classSystem; }

      protected:
        void CreateMesh() override;

      private:
        ClassSystem* m_classSystem = nullptr;
        int m_currentClassIndex = 0;
    };

    /**
 * @brief Toggle switch object
 *
 * Toggles between on/off states. Can trigger callbacks for
 * lighting, traps, bridges, or other mechanisms.
 */
    class SwitchObject : public InteractiveObject
    {
      public:
        SwitchObject();
        ~SwitchObject() override = default;

        void Update(float deltaTime) override;
        bool Interact(Player* player) override;
        std::string GetInteractionPrompt() const override;

        bool IsOn() const { return m_isOn; }
        void SetOn(bool on) { m_isOn = on; }
        void Toggle() { m_isOn = !m_isOn; }

        void SetToggleCallback(std::function<void(bool)> callback) { m_toggleCallback = callback; }

      protected:
        void CreateMesh() override;

      private:
        bool m_isOn = false;
        std::function<void(bool)> m_toggleCallback;
    };

    /**
 * @brief Elevator/lift platform
 *
 * Moves between two positions, carrying the player.
 */
    class ElevatorObject : public InteractiveObject
    {
      public:
        enum class ElevatorState
        {
            AtBottom,
            MovingUp,
            AtTop,
            MovingDown
        };

        ElevatorObject();
        ~ElevatorObject() override = default;

        void Update(float deltaTime) override;
        bool Interact(Player* player) override;
        std::string GetInteractionPrompt() const override;

        void SetBottomPosition(const XMFLOAT3& pos) { m_bottomPosition = pos; }
        void SetTopPosition(const XMFLOAT3& pos) { m_topPosition = pos; }
        void SetSpeed(float speed) { m_moveSpeed = speed; }
        void SetAutoReturn(bool enabled) { m_autoReturn = enabled; }
        void SetWaitTime(float time) { m_waitTime = time; }

        ElevatorState GetState() const { return m_state; }

      protected:
        void CreateMesh() override;

      private:
        ElevatorState m_state = ElevatorState::AtBottom;
        XMFLOAT3 m_bottomPosition = {0, 0, 0};
        XMFLOAT3 m_topPosition = {0, 10, 0};
        float m_moveSpeed = 5.0f;
        float m_moveProgress = 0.0f; ///< 0 = bottom, 1 = top
        bool m_autoReturn = true;
        float m_waitTime = 3.0f;
        float m_waitTimer = 0.0f;
        Player* m_ridingPlayer = nullptr;
    };

    /**
 * @brief Jump pad that launches the player upward
 */
    class JumpPadObject : public InteractiveObject
    {
      public:
        JumpPadObject();
        ~JumpPadObject() override = default;

        void Update(float deltaTime) override;
        bool Interact(Player* player) override;
        std::string GetInteractionPrompt() const override;

        void SetLaunchForce(float force) { m_launchForce = force; }
        void SetLaunchDirection(const XMFLOAT3& dir) { m_launchDirection = dir; }
        void SetCooldown(float cooldown) { m_cooldown = cooldown; }

      protected:
        void CreateMesh() override;

      private:
        float m_launchForce = 15.0f;
        XMFLOAT3 m_launchDirection = {0, 1, 0};
        float m_cooldown = 1.0f;
        float m_cooldownTimer = 0.0f;
        float m_animTimer = 0.0f;
    };

    /**
 * @brief Teleporter that moves the player to a destination
 */
    class TeleporterObject : public InteractiveObject
    {
      public:
        TeleporterObject();
        ~TeleporterObject() override = default;

        bool Interact(Player* player) override;
        std::string GetInteractionPrompt() const override;

        void SetDestination(const XMFLOAT3& dest) { m_destination = dest; }
        XMFLOAT3 GetDestination() const { return m_destination; }

        void SetLinkedTeleporter(TeleporterObject* other) { m_linkedTeleporter = other; }

      protected:
        void CreateMesh() override;

      private:
        XMFLOAT3 m_destination = {0, 0, 0};
        TeleporterObject* m_linkedTeleporter = nullptr;
        float m_cooldown = 2.0f;
        float m_cooldownTimer = 0.0f;
    };

    /**
 * @brief Pickup item (health, armor, ammo, weapons, shields)
 */
    class PickupObject : public InteractiveObject
    {
      public:
        PickupObject();
        explicit PickupObject(InteractiveObjectType pickupType);
        ~PickupObject() override = default;

        void Update(float deltaTime) override;
        bool Interact(Player* player) override;
        std::string GetInteractionPrompt() const override;

        void SetAmount(float amount) { m_amount = amount; }
        float GetAmount() const { return m_amount; }
        void SetWeaponType(WeaponType type) { m_weaponType = type; }
        void SetRespawnTime(float time) { m_respawnTime = time; }
        bool IsAvailable() const { return m_isAvailable; }

      protected:
        void CreateMesh() override;

      private:
        float m_amount = 25.0f;
        WeaponType m_weaponType = WeaponType::PISTOL;
        float m_respawnTime = 30.0f;
        float m_respawnTimer = 0.0f;
        bool m_isAvailable = true;
        float m_bobTimer = 0.0f;  ///< Floating animation
        float m_spinTimer = 0.0f; ///< Rotation animation
    };

    /**
 * @brief Destructible object (barrel, crate) that can be broken
 */
    class DestructibleObject : public InteractiveObject
    {
      public:
        DestructibleObject();
        ~DestructibleObject() override = default;

        void Update(float deltaTime) override;
        void OnHit(GameObject* target) override;
        bool Interact(Player* player) override;
        std::string GetInteractionPrompt() const override;

        void SetHealth(float health)
        {
            m_objectHealth = health;
            m_maxObjectHealth = health;
        }
        float GetObjectHealth() const { return m_objectHealth; }
        bool IsBroken() const { return m_objectHealth <= 0.0f; }

        void SetExplosionDamage(float damage) { m_explosionDamage = damage; }
        void SetExplosionRadius(float radius) { m_explosionRadius = radius; }
        void SetContainedPickup(InteractiveObjectType type) { m_containedPickup = type; }

      protected:
        void CreateMesh() override;

      private:
        float m_objectHealth = 50.0f;
        float m_maxObjectHealth = 50.0f;
        float m_explosionDamage = 50.0f;
        float m_explosionRadius = 5.0f;
        InteractiveObjectType m_containedPickup = InteractiveObjectType::HEALTH_PICKUP;
        bool m_hasExploded = false;

        void Explode();
    };

    /**
 * @brief Interactive object manager
 *
 * Manages all interactive objects in the scene, handles proximity
 * detection, and coordinates player interactions.
 */
    class InteractionSystem
    {
      public:
        InteractionSystem();
        ~InteractionSystem() = default;

        bool Initialize();
        void Update(float deltaTime, Player* player);

        /**
     * @brief Try to interact with the nearest object
     * @return true if interaction occurred
     */
        bool TryInteract(Player* player);

        /**
     * @brief Add an interactive object to the system
     */
        void AddObject(std::unique_ptr<InteractiveObject> obj);

        /**
     * @brief Remove an object by pointer
     */
        void RemoveObject(InteractiveObject* obj);

        /**
     * @brief Get all interactive objects
     */
        const std::vector<std::unique_ptr<InteractiveObject>>& GetObjects() const { return m_objects; }

        /**
     * @brief Get the currently highlighted object (nearest interactable)
     */
        InteractiveObject* GetHighlightedObject() const { return m_highlightedObject; }

        /**
     * @brief Get interaction prompt for HUD
     */
        std::string GetCurrentPrompt() const;

        /**
     * @brief Spawn a door
     */
        DoorObject* SpawnDoor(const XMFLOAT3& position, const XMFLOAT3& scale, ID3D11Device* device,
                              ID3D11DeviceContext* context);

        /**
     * @brief Spawn a class terminal
     */
        ClassTerminal* SpawnClassTerminal(const XMFLOAT3& position, ClassSystem* classSystem, ID3D11Device* device,
                                          ID3D11DeviceContext* context);

        /**
     * @brief Spawn a pickup
     */
        PickupObject* SpawnPickup(InteractiveObjectType type, const XMFLOAT3& position, float amount,
                                  ID3D11Device* device, ID3D11DeviceContext* context);

        /**
     * @brief Spawn a jump pad
     */
        JumpPadObject* SpawnJumpPad(const XMFLOAT3& position, float force, ID3D11Device* device,
                                    ID3D11DeviceContext* context);

        /**
     * @brief Spawn a teleporter pair
     */
        void SpawnTeleporterPair(const XMFLOAT3& posA, const XMFLOAT3& posB, ID3D11Device* device,
                                 ID3D11DeviceContext* context);

        /**
     * @brief Spawn an elevator
     */
        ElevatorObject* SpawnElevator(const XMFLOAT3& bottom, const XMFLOAT3& top, ID3D11Device* device,
                                      ID3D11DeviceContext* context);

        /**
     * @brief Spawn a destructible object
     */
        DestructibleObject* SpawnDestructible(const XMFLOAT3& position, float health, ID3D11Device* device,
                                              ID3D11DeviceContext* context);

        // Console integration
        std::string Console_ListObjects() const;
        size_t GetObjectCount() const { return m_objects.size(); }

      private:
        std::vector<std::unique_ptr<InteractiveObject>> m_objects;
        InteractiveObject* m_highlightedObject = nullptr;

        static constexpr int MAX_INTERACTIVE_OBJECTS = 256;
        static constexpr float HIGHLIGHT_CHECK_INTERVAL = 0.1f;
        float m_highlightCheckTimer = 0.0f;
    };

} // namespace Spark
