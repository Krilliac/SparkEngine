#include "Core/Platform.h"
#include "InteractiveObject.h"
#include "Player.h"
#include "Input/InputManager.h"
#include "Utils/Assert.h"
#include "Utils/Validate.h"
#include <algorithm>
#include <cmath>
#include <sstream>

using namespace DirectX;

namespace Spark
{

    // ============================================================================
    // InteractiveObject (Base)
    // ============================================================================

    InteractiveObject::InteractiveObject()
    {
        SetName("InteractiveObject");
    }

    InteractiveObject::InteractiveObject(InteractiveObjectType type) : m_objectType(type)
    {
        SetName("InteractiveObject");
    }

    HRESULT InteractiveObject::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
    {
        return GameObject::Initialize(device, context);
    }

    void InteractiveObject::Update(float deltaTime)
    {
        GameObject::Update(deltaTime);
    }

    void InteractiveObject::Render(const XMMATRIX& view, const XMMATRIX& proj)
    {
        // Could modify rendering for highlighted state
        GameObject::Render(view, proj);
    }

    void InteractiveObject::OnHit(GameObject* target) {}
    void InteractiveObject::OnHitWorld(const XMFLOAT3&, const XMFLOAT3&) {}

    bool InteractiveObject::Interact(Player* player)
    {
        SPARK_WARN_IF(Spark::LogCategory::Game, !player, "Interact called with null player");
        if (!m_isInteractable || !player)
            return false;
        if (m_interactionCallback)
        {
            m_interactionCallback(this, player);
        }
        return true;
    }

    bool InteractiveObject::IsPlayerInRange(const Player* player) const
    {
        if (!player)
            return false;
        return GetDistanceFrom(*player) <= m_interactionRange;
    }

    std::string InteractiveObject::GetInteractionPrompt() const
    {
        return "[E] Use";
    }

    // ============================================================================
    // DoorObject
    // ============================================================================

    DoorObject::DoorObject()
    {
        m_objectType = InteractiveObjectType::DOOR;
        SetName("Door");
        m_interactionRange = 3.0f;

        // Set up door state FSM — callbacks keep m_doorState in sync for external queries.
        m_doorFSM.AddState(DoorState::Closed, [this]() { m_doorState = DoorState::Closed; });
        m_doorFSM.AddState(
            DoorState::Opening, [this]() { m_doorState = DoorState::Opening; },
            [this](float dt)
            {
                m_openProgress += m_openSpeed * dt;
                if (m_openProgress > 1.0f)
                    m_openProgress = 1.0f;
            });
        m_doorFSM.AddState(DoorState::Open,
                           [this]()
                           {
                               m_doorState = DoorState::Open;
                               m_autoCloseTimer = 0.0f;
                           },
                           [this](float dt)
                           {
                               if (m_autoClose)
                                   m_autoCloseTimer += dt;
                           });
        m_doorFSM.AddState(
            DoorState::Closing, [this]() { m_doorState = DoorState::Closing; },
            [this](float dt)
            {
                m_openProgress -= m_openSpeed * dt;
                if (m_openProgress < 0.0f)
                    m_openProgress = 0.0f;
            });

        // Automatic transitions
        m_doorFSM.AddTransition(DoorState::Opening, DoorState::Open,
                                [this]() { return m_openProgress >= 1.0f; });
        m_doorFSM.AddTransition(DoorState::Closing, DoorState::Closed,
                                [this]() { return m_openProgress <= 0.0f; });
        m_doorFSM.AddTransition(DoorState::Open, DoorState::Closing,
                                [this]() { return m_autoClose && m_autoCloseTimer >= m_autoCloseTime; });

        m_doorFSM.Start(DoorState::Closed);
    }

    void DoorObject::Update(float deltaTime)
    {
        m_doorFSM.Tick(deltaTime);

        // Apply door movement based on style
        if (m_doorStyle == DoorStyle::Sliding)
        {
            XMFLOAT3 pos = m_closedPosition;
            pos.x += m_slideDirection.x * m_slideDistance * m_openProgress;
            pos.y += m_slideDirection.y * m_slideDistance * m_openProgress;
            pos.z += m_slideDirection.z * m_slideDistance * m_openProgress;
            SetPosition(pos);
        }
        else
        {
            // Rotating door
            XMFLOAT3 rot = GetRotation();
            rot.y = m_closedRotationY + (m_rotationAngle * 3.14159f / 180.0f) * m_openProgress;
            SetRotation(rot);
        }

        InteractiveObject::Update(deltaTime);
    }

    bool DoorObject::Interact(Player* player)
    {
        if (m_isLocked)
            return false;
        Toggle();
        return InteractiveObject::Interact(player);
    }

    std::string DoorObject::GetInteractionPrompt() const
    {
        if (m_isLocked)
            return "[E] Locked";
        if (IsOpen() || m_doorState == DoorState::Opening)
            return "[E] Close Door";
        return "[E] Open Door";
    }

    void DoorObject::Open()
    {
        if (m_doorState == DoorState::Closed || m_doorState == DoorState::Closing)
        {
            if (m_openProgress == 0.0f)
            {
                m_closedPosition = GetPosition();
                m_closedRotationY = GetRotation().y;
            }
            m_doorFSM.TransitionTo(DoorState::Opening);
        }
    }

    void DoorObject::Close()
    {
        if (m_doorState == DoorState::Open || m_doorState == DoorState::Opening)
        {
            m_doorFSM.TransitionTo(DoorState::Closing);
        }
    }

    void DoorObject::Toggle()
    {
        if (m_doorState == DoorState::Closed || m_doorState == DoorState::Closing)
        {
            Open();
        }
        else
        {
            Close();
        }
    }

    void DoorObject::CreateMesh()
    {
        if (m_mesh)
        {
            m_mesh->CreateCube(1.0f);
            SetScale({0.2f, 3.0f, 2.0f}); // Thin, tall door
        }
    }

    // ============================================================================
    // ClassTerminal
    // ============================================================================

    ClassTerminal::ClassTerminal()
    {
        m_objectType = InteractiveObjectType::TERMINAL;
        SetName("Class Terminal");
        m_interactionRange = 2.5f;
    }

    bool ClassTerminal::Interact(Player* player)
    {
        if (!player || !m_classSystem)
            return false;

        // Cycle to next class
        m_currentClassIndex = (m_currentClassIndex + 1) % static_cast<int>(PlayerClass::COUNT);
        PlayerClass newClass = static_cast<PlayerClass>(m_currentClassIndex);
        player->SetClass(newClass, m_classSystem);

        return InteractiveObject::Interact(player);
    }

    std::string ClassTerminal::GetInteractionPrompt() const
    {
        int nextClass = (m_currentClassIndex + 1) % static_cast<int>(PlayerClass::COUNT);
        const char* className = ClassSystem::GetClassName(static_cast<PlayerClass>(nextClass));
        return std::string("[E] Change Class -> ") + className;
    }

    void ClassTerminal::CreateMesh()
    {
        if (m_mesh)
        {
            m_mesh->CreateCube(1.0f);
            SetScale({1.0f, 1.5f, 0.5f}); // Terminal-shaped box
        }
    }

    // ============================================================================
    // SwitchObject
    // ============================================================================

    SwitchObject::SwitchObject()
    {
        m_objectType = InteractiveObjectType::SWITCH;
        SetName("Switch");
        m_interactionRange = 2.0f;
    }

    void SwitchObject::Update(float deltaTime)
    {
        InteractiveObject::Update(deltaTime);
    }

    bool SwitchObject::Interact(Player* player)
    {
        Toggle();
        if (m_toggleCallback)
        {
            m_toggleCallback(m_isOn);
        }
        return InteractiveObject::Interact(player);
    }

    std::string SwitchObject::GetInteractionPrompt() const
    {
        return m_isOn ? "[E] Turn Off" : "[E] Turn On";
    }

    void SwitchObject::CreateMesh()
    {
        if (m_mesh)
        {
            m_mesh->CreateCube(1.0f);
            SetScale({0.3f, 0.5f, 0.2f}); // Small switch box
        }
    }

    // ============================================================================
    // ElevatorObject
    // ============================================================================

    ElevatorObject::ElevatorObject()
    {
        m_objectType = InteractiveObjectType::ELEVATOR;
        SetName("Elevator");
        m_interactionRange = 3.0f;
    }

    void ElevatorObject::Update(float deltaTime)
    {
        switch (m_state)
        {
        case ElevatorState::MovingUp:
            m_moveProgress += m_moveSpeed * deltaTime / GetDistanceFrom(m_topPosition);
            if (m_moveProgress >= 1.0f)
            {
                m_moveProgress = 1.0f;
                m_state = ElevatorState::AtTop;
                m_waitTimer = 0.0f;
            }
            break;

        case ElevatorState::MovingDown:
            m_moveProgress -= m_moveSpeed * deltaTime / GetDistanceFrom(m_bottomPosition);
            if (m_moveProgress <= 0.0f)
            {
                m_moveProgress = 0.0f;
                m_state = ElevatorState::AtBottom;
                m_waitTimer = 0.0f;
            }
            break;

        case ElevatorState::AtTop:
            if (m_autoReturn)
            {
                m_waitTimer += deltaTime;
                if (m_waitTimer >= m_waitTime)
                {
                    m_state = ElevatorState::MovingDown;
                }
            }
            break;

        case ElevatorState::AtBottom:
            break;
        }

        // Interpolate position
        XMFLOAT3 pos = {m_bottomPosition.x + (m_topPosition.x - m_bottomPosition.x) * m_moveProgress,
                        m_bottomPosition.y + (m_topPosition.y - m_bottomPosition.y) * m_moveProgress,
                        m_bottomPosition.z + (m_topPosition.z - m_bottomPosition.z) * m_moveProgress};
        SetPosition(pos);

        // Move riding player
        if (m_ridingPlayer && (m_state == ElevatorState::MovingUp || m_state == ElevatorState::MovingDown))
        {
            XMFLOAT3 playerPos = pos;
            playerPos.y += 1.5f; // Stand on top
            m_ridingPlayer->SetPosition(playerPos);
        }

        InteractiveObject::Update(deltaTime);
    }

    bool ElevatorObject::Interact(Player* player)
    {
        if (m_state == ElevatorState::AtBottom)
        {
            m_state = ElevatorState::MovingUp;
            m_ridingPlayer = player;
        }
        else if (m_state == ElevatorState::AtTop)
        {
            m_state = ElevatorState::MovingDown;
            m_ridingPlayer = player;
        }
        return InteractiveObject::Interact(player);
    }

    std::string ElevatorObject::GetInteractionPrompt() const
    {
        if (m_state == ElevatorState::AtBottom)
            return "[E] Go Up";
        if (m_state == ElevatorState::AtTop)
            return "[E] Go Down";
        return ""; // Moving, can't interact
    }

    void ElevatorObject::CreateMesh()
    {
        if (m_mesh)
        {
            m_mesh->CreateCube(1.0f);
            SetScale({3.0f, 0.3f, 3.0f}); // Flat platform
        }
    }

    // ============================================================================
    // JumpPadObject
    // ============================================================================

    JumpPadObject::JumpPadObject()
    {
        m_objectType = InteractiveObjectType::JUMP_PAD;
        SetName("Jump Pad");
        m_interactionRange = 1.5f; // Auto-trigger on proximity
    }

    void JumpPadObject::Update(float deltaTime)
    {
        if (m_cooldownTimer > 0.0f)
        {
            m_cooldownTimer -= deltaTime;
        }

        // Floating animation
        m_animTimer += deltaTime * 2.0f;

        InteractiveObject::Update(deltaTime);
    }

    bool JumpPadObject::Interact(Player* player)
    {
        if (!player || m_cooldownTimer > 0.0f)
            return false;

        // Launch the player
        XMFLOAT3 velocity = player->GetVelocity();
        velocity.x += m_launchDirection.x * m_launchForce;
        velocity.y += m_launchDirection.y * m_launchForce;
        velocity.z += m_launchDirection.z * m_launchForce;

        // We need to set player velocity - using position manipulation
        // The player's ApplyGravity will handle the arc
        player->Console_SetPosition(player->GetPosition().x + m_launchDirection.x * 0.1f,
                                    player->GetPosition().y + 0.5f,
                                    player->GetPosition().z + m_launchDirection.z * 0.1f);

        m_cooldownTimer = m_cooldown;
        return InteractiveObject::Interact(player);
    }

    std::string JumpPadObject::GetInteractionPrompt() const
    {
        return ""; // Auto-trigger, no prompt needed
    }

    void JumpPadObject::CreateMesh()
    {
        if (m_mesh)
        {
            m_mesh->CreateCube(1.0f);
            SetScale({2.0f, 0.2f, 2.0f}); // Flat pad
        }
    }

    // ============================================================================
    // TeleporterObject
    // ============================================================================

    TeleporterObject::TeleporterObject()
    {
        m_objectType = InteractiveObjectType::TELEPORTER;
        SetName("Teleporter");
        m_interactionRange = 2.0f;
    }

    bool TeleporterObject::Interact(Player* player)
    {
        if (!player)
            return false;
        if (m_cooldownTimer > 0.0f)
            return false;

        XMFLOAT3 dest = m_destination;
        if (m_linkedTeleporter)
        {
            dest = m_linkedTeleporter->GetPosition();
            dest.y += 1.0f; // Slightly above destination pad
        }

        player->Console_SetPosition(dest.x, dest.y, dest.z);
        m_cooldownTimer = m_cooldown;

        return InteractiveObject::Interact(player);
    }

    std::string TeleporterObject::GetInteractionPrompt() const
    {
        if (m_cooldownTimer > 0.0f)
            return "[E] Recharging...";
        return "[E] Teleport";
    }

    void TeleporterObject::CreateMesh()
    {
        if (m_mesh)
        {
            m_mesh->CreateSphere(1.0f, 12, 12);
            SetScale({1.5f, 0.3f, 1.5f}); // Flat disc
        }
    }

    // ============================================================================
    // PickupObject
    // ============================================================================

    PickupObject::PickupObject()
    {
        m_objectType = InteractiveObjectType::HEALTH_PICKUP;
        SetName("Pickup");
        m_interactionRange = 2.0f;
    }

    PickupObject::PickupObject(InteractiveObjectType pickupType)
    {
        m_objectType = pickupType;
        SetName("Pickup");
        m_interactionRange = 2.0f;

        // Set defaults based on type
        switch (pickupType)
        {
        case InteractiveObjectType::HEALTH_PICKUP:
            m_amount = 25.0f;
            SetName("Health Pack");
            break;
        case InteractiveObjectType::ARMOR_PICKUP:
            m_amount = 25.0f;
            SetName("Armor Pack");
            break;
        case InteractiveObjectType::AMMO_PICKUP:
            m_amount = 30.0f;
            SetName("Ammo Box");
            break;
        case InteractiveObjectType::SHIELD_PICKUP:
            m_amount = 50.0f;
            SetName("Shield Cell");
            break;
        default:
            break;
        }
    }

    void PickupObject::Update(float deltaTime)
    {
        // Respawn timer
        if (!m_isAvailable)
        {
            m_respawnTimer -= deltaTime;
            if (m_respawnTimer <= 0.0f)
            {
                m_isAvailable = true;
                SetVisible(true);
            }
        }

        // Floating/spinning animation
        if (m_isAvailable)
        {
            m_bobTimer += deltaTime * 2.0f;
            m_spinTimer += deltaTime * 1.5f;

            XMFLOAT3 pos = GetPosition();
            pos.y += std::sin(m_bobTimer) * 0.003f; // Gentle bob
            SetPosition(pos);

            XMFLOAT3 rot = GetRotation();
            rot.y = m_spinTimer;
            SetRotation(rot);
        }

        InteractiveObject::Update(deltaTime);
    }

    bool PickupObject::Interact(Player* player)
    {
        if (!player || !m_isAvailable)
            return false;

        bool consumed = false;

        switch (m_objectType)
        {
        case InteractiveObjectType::HEALTH_PICKUP:
            if (player->GetHealth() < player->GetMaxHealth())
            {
                player->Heal(m_amount);
                consumed = true;
            }
            break;

        case InteractiveObjectType::ARMOR_PICKUP:
            if (player->GetArmor() < player->GetMaxArmor())
            {
                player->AddArmor(m_amount);
                consumed = true;
            }
            break;

        case InteractiveObjectType::AMMO_PICKUP:
            player->Console_GiveAmmo(static_cast<int>(m_amount));
            consumed = true;
            break;

        case InteractiveObjectType::WEAPON_PICKUP:
            player->ChangeWeapon(m_weaponType);
            consumed = true;
            break;

        case InteractiveObjectType::SHIELD_PICKUP:
            // Shield pickup restores shield
            consumed = true;
            break;

        default:
            break;
        }

        if (consumed)
        {
            m_isAvailable = false;
            m_respawnTimer = m_respawnTime;
            SetVisible(false);
        }

        return consumed ? InteractiveObject::Interact(player) : false;
    }

    std::string PickupObject::GetInteractionPrompt() const
    {
        if (!m_isAvailable)
            return "";

        switch (m_objectType)
        {
        case InteractiveObjectType::HEALTH_PICKUP:
            return "[E] Health +" + std::to_string((int)m_amount);
        case InteractiveObjectType::ARMOR_PICKUP:
            return "[E] Armor +" + std::to_string((int)m_amount);
        case InteractiveObjectType::AMMO_PICKUP:
            return "[E] Ammo +" + std::to_string((int)m_amount);
        case InteractiveObjectType::WEAPON_PICKUP:
            return "[E] Pick Up Weapon";
        case InteractiveObjectType::SHIELD_PICKUP:
            return "[E] Shield +" + std::to_string((int)m_amount);
        default:
            return "[E] Pick Up";
        }
    }

    void PickupObject::CreateMesh()
    {
        if (m_mesh)
        {
            switch (m_objectType)
            {
            case InteractiveObjectType::HEALTH_PICKUP:
                m_mesh->CreateCube(1.0f);
                SetScale({0.4f, 0.4f, 0.4f});
                break;
            case InteractiveObjectType::ARMOR_PICKUP:
                m_mesh->CreateCube(1.0f);
                SetScale({0.5f, 0.3f, 0.5f});
                break;
            case InteractiveObjectType::AMMO_PICKUP:
                m_mesh->CreateCube(1.0f);
                SetScale({0.3f, 0.2f, 0.5f});
                break;
            default:
                m_mesh->CreateSphere(0.3f, 8, 8);
                break;
            }
        }
    }

    // ============================================================================
    // DestructibleObject
    // ============================================================================

    DestructibleObject::DestructibleObject()
    {
        m_objectType = InteractiveObjectType::DESTRUCTIBLE;
        SetName("Destructible");
        m_interactionRange = 1.5f;
    }

    void DestructibleObject::Update(float deltaTime)
    {
        InteractiveObject::Update(deltaTime);
    }

    void DestructibleObject::OnHit(GameObject* target)
    {
        // Take damage from projectiles
        m_objectHealth -= 25.0f; // Default hit damage
        if (m_objectHealth <= 0.0f && !m_hasExploded)
        {
            Explode();
        }
    }

    bool DestructibleObject::Interact(Player* player)
    {
        // Melee hit
        m_objectHealth -= 15.0f;
        if (m_objectHealth <= 0.0f && !m_hasExploded)
        {
            Explode();
        }
        return InteractiveObject::Interact(player);
    }

    std::string DestructibleObject::GetInteractionPrompt() const
    {
        if (IsBroken())
            return "";
        return "[E] Break";
    }

    void DestructibleObject::Explode()
    {
        m_hasExploded = true;
        SetActive(false);
        SetVisible(false);
        // Explosion damage and pickup spawning would be handled by the game
    }

    void DestructibleObject::CreateMesh()
    {
        if (m_mesh)
        {
            m_mesh->CreateCube(1.0f);
            SetScale({0.8f, 0.8f, 0.8f}); // Barrel/crate size
        }
    }

    // ============================================================================
    // InteractionSystem
    // ============================================================================

    InteractionSystem::InteractionSystem() {}

    bool InteractionSystem::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Game);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Initializing InteractionSystem");
        m_objects.clear();
        m_highlightedObject = nullptr;
        return true;
    }

    void InteractionSystem::Update(float deltaTime, Player* player)
    {
        // Update all objects
        for (auto& obj : m_objects)
        {
            if (obj && obj->IsActive())
            {
                obj->Update(deltaTime);
            }
        }

        // Check for highlighted object (nearest interactable in range)
        m_highlightCheckTimer += deltaTime;
        if (m_highlightCheckTimer >= HIGHLIGHT_CHECK_INTERVAL && player)
        {
            m_highlightCheckTimer = 0.0f;

            // Reset highlights
            if (m_highlightedObject)
            {
                m_highlightedObject->SetHighlighted(false);
                m_highlightedObject = nullptr;
            }

            float nearestDist = 999999.0f;
            for (auto& obj : m_objects)
            {
                if (!obj || !obj->IsActive() || !obj->IsInteractable())
                    continue;
                if (!obj->IsPlayerInRange(player))
                    continue;

                float dist = obj->GetDistanceFrom(*player);
                if (dist < nearestDist)
                {
                    nearestDist = dist;
                    m_highlightedObject = obj.get();
                }
            }

            if (m_highlightedObject)
            {
                m_highlightedObject->SetHighlighted(true);
            }
        }

        // Auto-trigger jump pads and proximity pickups
        if (player)
        {
            for (auto& obj : m_objects)
            {
                if (!obj || !obj->IsActive())
                    continue;

                auto type = obj->GetObjectType();
                if (type == InteractiveObjectType::JUMP_PAD)
                {
                    if (obj->IsPlayerInRange(player))
                    {
                        obj->Interact(player);
                    }
                }
            }
        }

        // Clean up destroyed objects
        m_objects.erase(std::remove_if(m_objects.begin(), m_objects.end(),
                                       [](const std::unique_ptr<InteractiveObject>& obj) {
                                           return !obj || (!obj->IsActive() &&
                                                           obj->GetObjectType() == InteractiveObjectType::DESTRUCTIBLE);
                                       }),
                        m_objects.end());
    }

    bool InteractionSystem::TryInteract(Player* player)
    {
        if (!m_highlightedObject || !player)
            return false;
        return m_highlightedObject->Interact(player);
    }

    void InteractionSystem::AddObject(std::unique_ptr<InteractiveObject> obj)
    {
        if ((int)m_objects.size() < MAX_INTERACTIVE_OBJECTS)
        {
            m_objects.push_back(std::move(obj));
        }
    }

    void InteractionSystem::RemoveObject(InteractiveObject* obj)
    {
        m_objects.erase(std::remove_if(m_objects.begin(), m_objects.end(),
                                       [obj](const std::unique_ptr<InteractiveObject>& o) { return o.get() == obj; }),
                        m_objects.end());

        if (m_highlightedObject == obj)
        {
            m_highlightedObject = nullptr;
        }
    }

    std::string InteractionSystem::GetCurrentPrompt() const
    {
        if (m_highlightedObject)
        {
            return m_highlightedObject->GetInteractionPrompt();
        }
        return "";
    }

    DoorObject* InteractionSystem::SpawnDoor(const XMFLOAT3& position, const XMFLOAT3& scale, ID3D11Device* device,
                                             ID3D11DeviceContext* context)
    {
        auto door = std::make_unique<DoorObject>();
        door->Initialize(device, context);
        door->SetPosition(position);
        door->SetScale(scale);
        DoorObject* ptr = door.get();
        AddObject(std::move(door));
        return ptr;
    }

    ClassTerminal* InteractionSystem::SpawnClassTerminal(const XMFLOAT3& position, ClassSystem* classSystem,
                                                         ID3D11Device* device, ID3D11DeviceContext* context)
    {
        auto terminal = std::make_unique<ClassTerminal>();
        terminal->Initialize(device, context);
        terminal->SetPosition(position);
        terminal->SetClassSystem(classSystem);
        ClassTerminal* ptr = terminal.get();
        AddObject(std::move(terminal));
        return ptr;
    }

    PickupObject* InteractionSystem::SpawnPickup(InteractiveObjectType type, const XMFLOAT3& position, float amount,
                                                 ID3D11Device* device, ID3D11DeviceContext* context)
    {
        auto pickup = std::make_unique<PickupObject>(type);
        pickup->Initialize(device, context);
        pickup->SetPosition(position);
        pickup->SetAmount(amount);
        PickupObject* ptr = pickup.get();
        AddObject(std::move(pickup));
        return ptr;
    }

    JumpPadObject* InteractionSystem::SpawnJumpPad(const XMFLOAT3& position, float force, ID3D11Device* device,
                                                   ID3D11DeviceContext* context)
    {
        auto pad = std::make_unique<JumpPadObject>();
        pad->Initialize(device, context);
        pad->SetPosition(position);
        pad->SetLaunchForce(force);
        JumpPadObject* ptr = pad.get();
        AddObject(std::move(pad));
        return ptr;
    }

    void InteractionSystem::SpawnTeleporterPair(const XMFLOAT3& posA, const XMFLOAT3& posB, ID3D11Device* device,
                                                ID3D11DeviceContext* context)
    {
        auto teleA = std::make_unique<TeleporterObject>();
        teleA->Initialize(device, context);
        teleA->SetPosition(posA);

        auto teleB = std::make_unique<TeleporterObject>();
        teleB->Initialize(device, context);
        teleB->SetPosition(posB);

        // Link them
        teleA->SetDestination(posB);
        teleB->SetDestination(posA);
        teleA->SetLinkedTeleporter(teleB.get());
        teleB->SetLinkedTeleporter(teleA.get());

        AddObject(std::move(teleA));
        AddObject(std::move(teleB));
    }

    ElevatorObject* InteractionSystem::SpawnElevator(const XMFLOAT3& bottom, const XMFLOAT3& top, ID3D11Device* device,
                                                     ID3D11DeviceContext* context)
    {
        auto elevator = std::make_unique<ElevatorObject>();
        elevator->Initialize(device, context);
        elevator->SetPosition(bottom);
        elevator->SetBottomPosition(bottom);
        elevator->SetTopPosition(top);
        ElevatorObject* ptr = elevator.get();
        AddObject(std::move(elevator));
        return ptr;
    }

    DestructibleObject* InteractionSystem::SpawnDestructible(const XMFLOAT3& position, float health,
                                                             ID3D11Device* device, ID3D11DeviceContext* context)
    {
        auto obj = std::make_unique<DestructibleObject>();
        obj->Initialize(device, context);
        obj->SetPosition(position);
        obj->SetHealth(health);
        DestructibleObject* ptr = obj.get();
        AddObject(std::move(obj));
        return ptr;
    }

    std::string InteractionSystem::Console_ListObjects() const
    {
        std::stringstream ss;
        ss << "Interactive Objects (" << m_objects.size() << "/" << MAX_INTERACTIVE_OBJECTS << "):\n";

        const char* typeNames[] = {"Door", "Terminal", "Switch", "Elevator"};

        for (size_t i = 0; i < m_objects.size(); ++i)
        {
            const auto& obj = m_objects[i];
            if (!obj)
                continue;
            ss << "  [" << i << "] " << obj->GetName() << " Pos:(" << obj->GetPosition().x << ","
               << obj->GetPosition().y << "," << obj->GetPosition().z << ")"
               << (obj->IsActive() ? " [ACTIVE]" : " [INACTIVE]") << "\n";
        }
        return ss.str();
    }

} // namespace Spark
