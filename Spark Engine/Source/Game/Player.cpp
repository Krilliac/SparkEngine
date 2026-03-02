#include "Player.h"
#include "Utils/Assert.h"
#include "..\Camera\SparkEngineCamera.h"
#include "..\Input\InputManager.h"
#include "..\Projectiles\WeaponStats.h"
#include "..\Projectiles\ProjectilePool.h"
#include "..\Utils\MathUtils.h"
#include "..\Game\Console.h"
#include "../Utils/ConsoleProcessManager.h"
#include "Model.h"  // Add Model class for weapon rendering
#include <algorithm>
#include <DirectXMath.h>
#include <cmath>
#include <iostream>

using namespace DirectX;
extern Console g_console;

// **FIXED: Rate-limited logging for Player to prevent console spam**
#define LOG_TO_CONSOLE_RATE_LIMITED(msg, type) \
    do { \
        static auto lastLogTime = std::chrono::steady_clock::now(); \
        static int logCounter = 0; \
        auto now = std::chrono::steady_clock::now(); \
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastLogTime).count(); \
        if (elapsed >= 5 || logCounter < 2) { \
            Spark::ConsoleProcessManager::GetInstance().Log(msg, type); \
            if (elapsed >= 5) { \
                lastLogTime = now; \
                logCounter = 0; \
            } else { \
                logCounter++; \
            } \
        } \
    } while(0)

// Use rate-limited logging for most messages, immediate for critical ones
#define LOG_TO_CONSOLE(msg, type) LOG_TO_CONSOLE_RATE_LIMITED(msg, type)
#define LOG_TO_CONSOLE_IMMEDIATE(msg, type) Spark::ConsoleProcessManager::GetInstance().Log(msg, type)

// Constructor
Player::Player()
    : m_currentWeapon(GetWeaponStats(WeaponType::PISTOL))
    , m_collisionSphere(GetPosition(), 0.5f)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Player constructed.", L"INFO");
    SetName("Player");
    m_currentAmmo = m_currentWeapon.MagazineSize;
    ASSERT_MSG(m_currentAmmo > 0, "Initial ammo must be positive");
}

// Initialization
HRESULT Player::Initialize(ID3D11Device* device,
    ID3D11DeviceContext* context,
    SparkEngineCamera* camera,
    InputManager* input)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Player::Initialize called.", L"OPERATION");
    ASSERT_NOT_NULL(device);
    ASSERT_NOT_NULL(context);
    ASSERT_NOT_NULL(camera);
    ASSERT_NOT_NULL(input);

    m_camera = camera;
    m_input = input;
    SetVisible(false);

    // Initialize weapon models
    m_pistolModel = std::make_unique<Model>();
    m_rifleModel = std::make_unique<Model>();
    m_shotgunModel = std::make_unique<Model>();
    m_rocketModel = std::make_unique<Model>();
    m_grenadeModel = std::make_unique<Model>();

    // Load weapon models from assets
    HRESULT hr = S_OK;
    hr = m_pistolModel->LoadObj(L"../Assets/Models/pistol.obj", device);
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Warning: Failed to load pistol model", L"WARNING");
    }
    
    hr = m_rifleModel->LoadObj(L"../Assets/Models/rifle.obj", device);
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Warning: Failed to load rifle model", L"WARNING");
    }

    // For weapons without specific models, we'll use pistol as fallback
    hr = m_shotgunModel->LoadObj(L"../Assets/Models/rifle.obj", device);
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Warning: Failed to load shotgun model (using rifle fallback)", L"WARNING");
    }
    
    hr = m_rocketModel->LoadObj(L"../Assets/Models/rifle.obj", device);
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Warning: Failed to load rocket launcher model (using rifle fallback)", L"WARNING");
    }
    
    hr = m_grenadeModel->LoadObj(L"../Assets/Models/rifle.obj", device);
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Warning: Failed to load grenade launcher model (using rifle fallback)", L"WARNING");
    }

    if (!m_projectilePool)
    {
        m_ownedProjectilePool = std::make_unique<ProjectilePool>(50);
        m_projectilePool = m_ownedProjectilePool.get();
        ASSERT_NOT_NULL(m_projectilePool);
        hr = m_projectilePool->Initialize(device, context);
        LOG_TO_CONSOLE_IMMEDIATE(L"Player projectile pool initialized. HR=0x" + std::to_wstring(hr), L"INFO");
        ASSERT_MSG(SUCCEEDED(hr), "ProjectilePool::Initialize failed");
        if (FAILED(hr)) return hr;
    }
    LOG_TO_CONSOLE_IMMEDIATE(L"Player initialization complete with weapon models.", L"INFO");
    return GameObject::Initialize(device, context);
}

// Per-frame update - **REMOVED PER-FRAME LOGGING**
void Player::Update(float dt)
{
    // **FIXED: No per-frame logging to prevent console spam**
    ASSERT_MSG(dt >= 0.0f && std::isfinite(dt), "Delta time must be non-negative and finite");
    if (!IsAlive()) return;

    HandleInput(dt);
    UpdateMovement(dt);
    UpdateCombat(dt);
    UpdatePhysics(dt);
    UpdateClassMechanics(dt);
    UpdateAnimation(dt);
    UpdateCollision();
    GameObject::Update(dt);
}

// No mesh rendering for first-person
void Player::Render(const XMMATRIX& view, const XMMATRIX& proj)
{
    // **ENHANCED: Now render weapon models in first-person view**
    RenderWeapon(view, proj);
}

// Render weapon model in first-person view
void Player::RenderWeapon(const XMMATRIX& view, const XMMATRIX& proj)
{
    if (!m_camera) return;
    
    // Get the appropriate weapon model based on current weapon
    Model* currentWeaponModel = nullptr;
    switch (m_currentWeapon.Type) {
        case WeaponType::PISTOL:
            currentWeaponModel = m_pistolModel.get();
            break;
        case WeaponType::RIFLE:
            currentWeaponModel = m_rifleModel.get();
            break;
        case WeaponType::SHOTGUN:
            currentWeaponModel = m_shotgunModel.get();
            break;
        case WeaponType::ROCKET_LAUNCHER:
            currentWeaponModel = m_rocketModel.get();
            break;
        case WeaponType::GRENADE_LAUNCHER:
            currentWeaponModel = m_grenadeModel.get();
            break;
        default:
            currentWeaponModel = m_pistolModel.get(); // Fallback
            break;
    }
    
    if (currentWeaponModel && m_context) {
        // Position weapon relative to camera for first-person view
        XMFLOAT3 cameraPos = m_camera->GetPosition();
        XMFLOAT3 cameraForward = m_camera->GetForward();
        
        // Calculate right vector from forward vector (cross product with world up)
        XMVECTOR forwardVec = XMLoadFloat3(&cameraForward);
        XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        XMVECTOR rightVec = XMVector3Normalize(XMVector3Cross(worldUp, forwardVec));
        XMFLOAT3 cameraRight;
        XMStoreFloat3(&cameraRight, rightVec);
        
        // Calculate weapon position (down and to the right from camera)
        XMFLOAT3 weaponOffset = {
            cameraRight.x * 0.3f - cameraForward.x * 0.1f,  // Right and slightly back
            -0.2f,                                            // Down from camera
            cameraRight.z * 0.3f - cameraForward.z * 0.1f   // Right and slightly back
        };
        
        XMFLOAT3 weaponPos = {
            cameraPos.x + weaponOffset.x,
            cameraPos.y + weaponOffset.y,
            cameraPos.z + weaponOffset.z
        };
        
        // Create weapon transformation matrix
        XMMATRIX weaponWorld = XMMatrixTranslation(weaponPos.x, weaponPos.y, weaponPos.z);
        
        // Apply weapon rotation to match camera orientation
        XMMATRIX cameraRotation = XMMatrixInverse(nullptr, view);
        weaponWorld = weaponWorld * cameraRotation;
        
        // Set up constant buffer for weapon rendering (simplified)
        // In a real implementation, you'd set shader constants here
        // For now, we'll just assume the shaders are already bound
        
        // Render the weapon model
        try {
            currentWeaponModel->Render(m_context);
        } catch (...) {
            // Handle any rendering errors gracefully
            static int errorCount = 0;
            if (++errorCount <= 3) { // Only log first few errors
                LOG_TO_CONSOLE_IMMEDIATE(L"Warning: Weapon model rendering error", L"WARNING");
            }
        }
    }
}

// Damage & healing - enhanced with class system (shield, resistance, overshield)
void Player::TakeDamage(float dmg)
{
    LOG_TO_CONSOLE(L"Player::TakeDamage called. dmg=" + std::to_wstring(dmg), L"OPERATION");
    ASSERT_MSG(dmg >= 0.0f, "Damage must be non-negative");

    // Check god mode from console integration
    if (m_godModeEnabled) {
        LOG_TO_CONSOLE(L"Damage blocked by god mode", L"INFO");
        return;
    }

    if (!IsAlive()) return;

    // Apply class damage resistance
    float effectiveDmg = dmg * (1.0f - m_damageResistance);

    // Cloaked infiltrator takes bonus damage (de-cloaks on hit)
    if (m_cloakActive) {
        m_cloakActive = false;
        m_primaryAbility.Deactivate();
        effectiveDmg *= 1.5f; // 50% bonus damage while cloaked
    }

    // Overshield absorbs damage first (Heavy Assault)
    if (m_overshieldHP > 0.0f) {
        float shieldAbsorbed = std::min(effectiveDmg, m_overshieldHP);
        m_overshieldHP -= shieldAbsorbed;
        effectiveDmg -= shieldAbsorbed;
    }

    // Rechargeable shield absorbs next
    if (m_shield > 0.0f && effectiveDmg > 0.0f) {
        float shieldAbsorbed = std::min(effectiveDmg, m_shield);
        m_shield -= shieldAbsorbed;
        effectiveDmg -= shieldAbsorbed;
        m_shieldRechargeTimer = 0.0f; // Reset recharge delay
    }

    // Armor absorbs 50% of remaining damage
    if (effectiveDmg > 0.0f) {
        float armorAbsorbed = std::min(effectiveDmg * 0.5f, m_armor);
        m_armor -= armorAbsorbed;
        effectiveDmg -= armorAbsorbed;
    }

    // Remaining damage hits health
    m_health = std::max(0.0f, m_health - effectiveDmg);
    if (m_health <= 0.0f) SetActive(false);

    // Reset shield recharge timer on any damage
    m_shieldRechargeTimer = 0.0f;

    // Notify console of state change
    NotifyStateChange();

    LOG_TO_CONSOLE(L"Player took damage. Health=" + std::to_wstring(m_health) +
                   L" Shield=" + std::to_wstring(m_shield) +
                   L" Armor=" + std::to_wstring(m_armor), L"INFO");
}

void Player::Heal(float amt)
{
    LOG_TO_CONSOLE(L"Player::Heal called. amt=" + std::to_wstring(amt), L"OPERATION");
    ASSERT_MSG(amt >= 0.0f, "Heal amount must be non-negative");
    m_health = std::min(m_maxHealth, m_health + amt);
    LOG_TO_CONSOLE(L"Player healed. Health=" + std::to_wstring(m_health), L"INFO");
}

void Player::AddArmor(float amt)
{
    LOG_TO_CONSOLE(L"Player::AddArmor called. amt=" + std::to_wstring(amt), L"OPERATION");
    ASSERT_MSG(amt >= 0.0f, "Armor amount must be non-negative");
    m_armor = std::min(m_maxArmor, m_armor + amt);
    LOG_TO_CONSOLE(L"Player armor added. Armor=" + std::to_wstring(m_armor), L"INFO");
}

// Actions
void Player::Jump()
{
    LOG_TO_CONSOLE(L"Player::Jump called.", L"OPERATION");
    if (m_isGrounded && !m_isJumping && m_stamina > 20.0f)
    {
        m_velocity.y = m_jumpHeight;
        m_isJumping = true;
        m_isGrounded = false;
        m_stamina -= 20.0f;
        LOG_TO_CONSOLE(L"Player jumped.", L"INFO");
    }
}

void Player::StartReload()
{
    LOG_TO_CONSOLE(L"Player::StartReload called.", L"OPERATION");
    if (!m_isReloading && m_currentAmmo < m_currentWeapon.MagazineSize)
    {
        m_isReloading = true;
        m_reloadTimer = m_currentWeapon.ReloadTime;
        LOG_TO_CONSOLE(L"Player started reloading.", L"INFO");
    }
}

void Player::Fire()
{
    // **FIXED: Rate-limited firing logs to prevent spam when holding mouse button**
    static auto lastFireLog = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastFireLog).count();
    
    if (m_isReloading || m_fireTimer > 0.0f) return;
    
    // **ENHANCED: Check infinite ammo from console integration**
    if (!m_infiniteAmmoEnabled && m_currentAmmo <= 0) return;
    
    ASSERT_NOT_NULL(m_projectilePool);

    XMFLOAT3 pos = m_camera->GetPosition();
    XMFLOAT3 dir = CalculateFireDirection();

    ProjectileType type = ProjectileType::BULLET;
    switch (m_currentWeapon.Type)
    {
    case WeaponType::ROCKET_LAUNCHER:   type = ProjectileType::ROCKET;   break;
    case WeaponType::GRENADE_LAUNCHER:  type = ProjectileType::GRENADE;  break;
    default:                            type = ProjectileType::BULLET;   break;
    }

    m_projectilePool->FireProjectile(type, pos, dir, m_currentWeapon.MuzzleVelocity);

    // Only consume ammo if infinite ammo is disabled
    if (!m_infiniteAmmoEnabled) {
        --m_currentAmmo;
    }
    
    m_fireTimer = 60.0f / m_currentWeapon.FireRate;
    m_isFiring = true;
    
    // Notify console of state change
    NotifyStateChange();
    
    // Only log weapon firing every 2 seconds
    if (elapsed >= 2) {
        LOG_TO_CONSOLE(L"Player fired weapon. Ammo=" + std::to_wstring(m_currentAmmo), L"INFO");
        lastFireLog = now;
    }
}

void Player::ChangeWeapon(WeaponType t)
{
    LOG_TO_CONSOLE(L"Player::ChangeWeapon called. type=" + std::to_wstring(static_cast<int>(t)), L"OPERATION");
    if (m_isReloading) return;
    m_currentWeapon = GetWeaponStats(t);
    m_currentAmmo = m_currentWeapon.MagazineSize;
    m_fireTimer = 0.0f;
    LOG_TO_CONSOLE(L"Player weapon changed. Ammo=" + std::to_wstring(m_currentAmmo), L"INFO");
}

// Hit callbacks
void Player::OnHit(GameObject* target)
{
    LOG_TO_CONSOLE(L"Player::OnHit called.", L"OPERATION");
    ASSERT_NOT_NULL(target);
    // Player was hit by target - take a default collision damage, not own weapon damage
    TakeDamage(10.0f);
    LOG_TO_CONSOLE(L"Player hit by target.", L"INFO");
}

void Player::OnHitWorld(const XMFLOAT3& hitPoint, const XMFLOAT3& normal)
{
    LOG_TO_CONSOLE(L"Player::OnHitWorld called.", L"OPERATION");
    ASSERT_MSG(std::isfinite(hitPoint.x) && std::isfinite(hitPoint.y) && std::isfinite(hitPoint.z),
        "Invalid hitPoint");
    // World collision - no damage from hitting world geometry
    LOG_TO_CONSOLE(L"Player hit world.", L"INFO");
}

// Input handling - enhanced with class abilities and loadout slots
void Player::HandleInput(float)
{
    if (!m_input) return;
    if (m_input->WasKeyPressed('R')) StartReload();
    if (m_input->IsMouseButtonDown(0)) Fire();
    if (m_input->WasKeyPressed(VK_SPACE)) Jump();

    m_isRunning = m_input->IsKeyDown(VK_LSHIFT);
    m_isCrouching = m_input->IsKeyDown(VK_LCONTROL);

    // Class loadout weapon slots (1-4)
    if (m_input->WasKeyPressed('1')) EquipPrimary();
    if (m_input->WasKeyPressed('2')) EquipSecondary();
    if (m_input->WasKeyPressed('3')) EquipSidearm();
    if (m_input->WasKeyPressed('4')) EquipTool();

    // Class abilities
    if (m_input->WasKeyPressed('F')) ActivatePrimaryAbility();
    if (m_input->WasKeyPressed('G')) ActivateSecondaryAbility();

    // Jetpack hold (Light Assault) - hold Space while in air
    if (m_playerClass == PlayerClass::LIGHT_ASSAULT && m_primaryAbility.isActive) {
        m_jetpackActive = m_input->IsKeyDown(VK_SPACE) && m_jetpackFuel > 0.0f;
    }

    // Toggle cloak off manually
    if (m_cloakActive && m_input->WasKeyPressed('F')) {
        m_cloakActive = false;
        m_primaryAbility.Deactivate();
    }
}

// Movement logic - enhanced with class-specific speed and ability modifiers
void Player::UpdateMovement(float dt)
{
    if (!m_camera || !m_input) return;

    // MAX lockdown prevents all movement
    if (m_lockedDown) return;

    float speed = m_speed;

    if (m_isRunning && m_stamina > 0.0f)
    {
        speed *= m_sprintMultiplier;
        m_stamina = std::max(0.0f, m_stamina - 30.0f * dt);
    }
    else if (m_isCrouching)
    {
        speed *= m_crouchMultiplier;
    }

    // Cloaked infiltrator moves slightly slower
    if (m_cloakActive) {
        speed *= 0.75f;
    }

    // Heavy Assault overshield active = slower
    if (m_overshieldHP > 0.0f && m_primaryAbility.isActive &&
        m_playerClass == PlayerClass::HEAVY_ASSAULT) {
        speed *= 0.8f;
    }

    speed *= dt;

    if (m_input->IsKeyDown('W')) m_camera->MoveForward(speed);
    if (m_input->IsKeyDown('S')) m_camera->MoveForward(-speed);
    if (m_input->IsKeyDown('A')) m_camera->MoveRight(-speed);
    if (m_input->IsKeyDown('D')) m_camera->MoveRight(speed);

    if (!m_isRunning)
        m_stamina = std::min(m_maxStamina, m_stamina + 50.0f * dt);

    SetPosition(m_camera->GetPosition());
}

// Combat timers - **REMOVED PER-FRAME LOGGING**
void Player::UpdateCombat(float dt)
{
    // **FIXED: No per-frame logging**
    if (m_fireTimer > 0.0f) { m_fireTimer -= dt; if (m_fireTimer < 0) m_fireTimer = 0; }
    if (m_isReloading)
    {
        m_reloadTimer -= dt;
        if (m_reloadTimer <= 0.0f)
        {
            m_currentAmmo = m_currentWeapon.MagazineSize;
            m_isReloading = false;
        }
    }
}

// Physics & gravity - Enhanced with console settings
void Player::UpdatePhysics(float dt)
{
    // **ENHANCED: Apply console-controlled physics settings**
    if (!m_isGrounded && !m_noclipEnabled) {
        ApplyGravity(dt);
    }
    
    // Skip collision in noclip mode
    if (!m_noclipEnabled) {
        CheckGroundCollision();
    }

    XMFLOAT3 pos = GetPosition();
    pos.x += m_velocity.x * dt;
    pos.y += m_velocity.y * dt;
    pos.z += m_velocity.z * dt;
    SetPosition(pos);
    if (m_camera) m_camera->SetPosition(pos);

    // Apply console-controlled friction
    m_velocity.x *= m_frictionCoeff;
    m_velocity.z *= m_frictionCoeff;
}

// Animation (head bob & footsteps) - **REMOVED PER-FRAME LOGGING**
void Player::UpdateAnimation(float dt)
{
    // **FIXED: No per-frame logging**
    if (m_input && (m_input->IsKeyDown('W') || m_input->IsKeyDown('A') ||
        m_input->IsKeyDown('S') || m_input->IsKeyDown('D')))
    {
        m_bobTimer += dt * 8.0f;
        HandleFootsteps(dt);
    }
}

// Collision sphere update - **REMOVED PER-FRAME LOGGING**
void Player::UpdateCollision()
{
    // **FIXED: No per-frame logging**
    m_collisionSphere.Center = GetPosition();
}

// Apply gravity - **REMOVED PER-FRAME LOGGING**
void Player::ApplyGravity(float dt)
{
    // **ENHANCED: Use console-controlled gravity force**
    m_velocity.y += m_gravityForce * dt;
    if (m_velocity.y < -50.0f) m_velocity.y = -50.0f;
}

// Ground check - **REMOVED PER-FRAME LOGGING**
void Player::CheckGroundCollision()
{
    // **FIXED: No per-frame logging**
    XMFLOAT3 pos = GetPosition();
    if (pos.y <= 0.0f && m_velocity.y <= 0.0f)
    {
        pos.y = 0.0f; SetPosition(pos);
        m_velocity.y = 0.0f;
        m_isGrounded = true; m_isJumping = false;
        if (m_camera) m_camera->SetPosition(pos);
    }
}

// Footsteps - **REMOVED PER-FRAME LOGGING**
void Player::HandleFootsteps(float dt)
{
    // **FIXED: No per-frame logging**
    m_footstepTimer += dt;
    float interval = m_isRunning ? 0.3f : 0.6f;
    if (m_footstepTimer >= interval)
    {
        // footstep SFX
        m_footstepTimer = 0.0f;
    }
}

// Weapon defaults lookup
WeaponStats Player::GetWeaponStats(WeaponType type)
{
    return GetDefaultWeaponStats(type);
}

// Fire direction w/spread
XMFLOAT3 Player::CalculateFireDirection()
{
    if (!m_camera) return XMFLOAT3(0, 0, 1);
    XMFLOAT3 f = m_camera->GetForward();
    float spread = (1.0f - m_currentWeapon.Accuracy) * 0.1f;
    if (spread > 0.0f)
    {
        f.x += MathUtils::RandomFloat(-spread, spread);
        f.y += MathUtils::RandomFloat(-spread, spread);
        XMVECTOR v = XMVector3Normalize(XMLoadFloat3(&f));
        XMStoreFloat3(&f, v);
    }
    return f;
}

// ============================================================================
// CLASS SYSTEM IMPLEMENTATIONS
// ============================================================================

void Player::SetClass(PlayerClass classType, Spark::ClassSystem* classSystem)
{
    m_playerClass = classType;
    m_classSystem = classSystem;

    if (classSystem) {
        const auto& def = classSystem->GetClassDefinition(classType);
        ApplyClassStats(def);
        LOG_TO_CONSOLE_IMMEDIATE(
            L"Player class set to: " + std::wstring(def.name.begin(), def.name.end()),
            L"SUCCESS");
    }
}

void Player::ApplyClassStats(const Spark::ClassDefinition& classDef)
{
    // Apply base stats
    m_maxHealth = classDef.baseHealth;
    m_health = m_maxHealth;
    m_maxArmor = classDef.baseArmor + 100.0f; // Armor is pickup-based, set cap
    m_armor = classDef.baseArmor;
    m_maxShield = classDef.baseShield;
    m_shield = m_maxShield;
    m_shieldRechargeRate = classDef.shieldRechargeRate;
    m_shieldRechargeDelay = classDef.shieldRechargeDelay;
    m_speed = classDef.baseSpeed;
    m_jumpHeight = classDef.baseJumpHeight;
    m_maxStamina = classDef.baseStamina;
    m_stamina = m_maxStamina;
    m_sprintMultiplier = classDef.sprintMultiplier;
    m_crouchMultiplier = classDef.crouchMultiplier;
    m_adsSpeedMultiplier = classDef.adsSpeedMultiplier;
    m_damageResistance = classDef.damageResistance;
    m_explosiveResistance = classDef.explosiveResistance;

    // Apply loadout
    m_loadout = classDef.loadout;
    m_allowedWeapons = classDef.allowedWeapons;

    // Equip primary weapon
    m_activeLoadoutSlot = 0;
    ChangeWeapon(m_loadout.primary.weapon);

    // Setup abilities
    m_primaryAbility = classDef.primaryAbility;
    m_primaryAbility.Reset();
    m_secondaryAbility = classDef.secondaryAbility;
    m_secondaryAbility.Reset();

    // Reset class-specific state
    m_jetpackActive = false;
    m_jetpackFuel = m_jetpackMaxFuel;
    m_cloakActive = false;
    m_overshieldHP = 0.0f;
    m_lockedDown = false;
    m_energy = m_maxEnergy;
}

bool Player::ActivatePrimaryAbility()
{
    if (m_primaryAbility.Activate(m_energy)) {
        m_energy -= m_primaryAbility.energyCost;

        // Class-specific activation logic
        switch (m_primaryAbility.type) {
        case ClassAbility::JETPACK:
            m_jetpackActive = true;
            m_jetpackFuel = m_jetpackMaxFuel;
            break;
        case ClassAbility::CLOAK:
            m_cloakActive = true;
            break;
        case ClassAbility::OVERSHIELD:
            m_overshieldHP = m_overshieldMaxHP;
            break;
        case ClassAbility::LOCKDOWN:
            m_lockedDown = true;
            break;
        default:
            break;
        }

        LOG_TO_CONSOLE(L"Primary ability activated: " +
            std::wstring(Spark::ClassSystem::GetAbilityName(m_primaryAbility.type),
                Spark::ClassSystem::GetAbilityName(m_primaryAbility.type) +
                strlen(Spark::ClassSystem::GetAbilityName(m_primaryAbility.type))),
            L"INFO");
        return true;
    }
    return false;
}

bool Player::ActivateSecondaryAbility()
{
    if (m_secondaryAbility.Activate(m_energy)) {
        m_energy -= m_secondaryAbility.energyCost;

        // Class-specific secondary activation
        switch (m_secondaryAbility.type) {
        case ClassAbility::EMERGENCY_REPAIR:
            // MAX self-repair: restore 50% health
            Heal(m_maxHealth * 0.5f);
            break;
        case ClassAbility::HEAL_AURA:
            // Medic heal aura: heal self (allies would be handled by game logic)
            Heal(30.0f);
            break;
        default:
            break;
        }

        LOG_TO_CONSOLE(L"Secondary ability activated", L"INFO");
        return true;
    }
    return false;
}

void Player::DeactivatePrimaryAbility()
{
    if (m_primaryAbility.isActive) {
        m_primaryAbility.Deactivate();
        m_jetpackActive = false;
        m_cloakActive = false;
        m_lockedDown = false;
    }
}

bool Player::CanEquipWeapon(WeaponType type) const
{
    if (m_allowedWeapons.empty()) return true; // No restrictions
    for (auto w : m_allowedWeapons) {
        if (w == type) return true;
    }
    return false;
}

void Player::EquipPrimary()
{
    m_activeLoadoutSlot = 0;
    ChangeWeapon(m_loadout.primary.weapon);
}

void Player::EquipSecondary()
{
    m_activeLoadoutSlot = 1;
    ChangeWeapon(m_loadout.secondary.weapon);
}

void Player::EquipSidearm()
{
    m_activeLoadoutSlot = 2;
    ChangeWeapon(m_loadout.sidearm.weapon);
}

void Player::EquipTool()
{
    if (m_loadout.tool.weapon != WeaponType::PISTOL || m_activeLoadoutSlot == 3) {
        m_activeLoadoutSlot = 3;
        ChangeWeapon(m_loadout.tool.weapon);
    }
}

void Player::UpdateClassMechanics(float dt)
{
    // Shield recharge (PlanetSide 2 style)
    m_shieldRechargeTimer += dt;
    if (m_shieldRechargeTimer >= m_shieldRechargeDelay && m_shield < m_maxShield) {
        m_shield = std::min(m_maxShield, m_shield + m_shieldRechargeRate * dt);
    }

    // Energy regeneration
    m_energy = std::min(m_maxEnergy, m_energy + m_energyRegenRate * dt);

    // Update ability cooldowns
    m_primaryAbility.Update(dt);
    m_secondaryAbility.Update(dt);

    // Class-specific passive: Combat Medic health regen
    if (m_playerClass == PlayerClass::COMBAT_MEDIC && m_health < m_maxHealth) {
        m_health = std::min(m_maxHealth, m_health + 2.0f * dt);
    }

    // Class-specific updates
    switch (m_playerClass) {
    case PlayerClass::LIGHT_ASSAULT:
        UpdateJetpack(dt);
        break;
    case PlayerClass::INFILTRATOR:
        UpdateCloak(dt);
        break;
    case PlayerClass::HEAVY_ASSAULT:
        UpdateOvershield(dt);
        break;
    case PlayerClass::MAX_SUIT:
        // Lockdown: deactivate when ability ends
        if (m_lockedDown && !m_primaryAbility.isActive) {
            m_lockedDown = false;
        }
        break;
    default:
        break;
    }
}

void Player::UpdateJetpack(float dt)
{
    if (m_jetpackActive && m_jetpackFuel > 0.0f) {
        // Apply upward thrust
        m_velocity.y = m_jetpackThrust;
        m_isGrounded = false;
        m_jetpackFuel -= 25.0f * dt; // Fuel consumption
        if (m_jetpackFuel <= 0.0f) {
            m_jetpackFuel = 0.0f;
            m_jetpackActive = false;
        }
    } else if (!m_jetpackActive) {
        // Recharge fuel when not using jetpack
        m_jetpackFuel = std::min(m_jetpackMaxFuel, m_jetpackFuel + 15.0f * dt);
    }
}

void Player::UpdateCloak(float dt)
{
    // Firing decloaks the infiltrator
    if (m_cloakActive && m_isFiring) {
        m_cloakActive = false;
        m_primaryAbility.Deactivate();
    }

    // Cloak deactivates when ability duration runs out
    if (!m_primaryAbility.isActive && m_cloakActive) {
        m_cloakActive = false;
    }
}

void Player::UpdateOvershield(float dt)
{
    // Overshield decays slowly
    if (m_overshieldHP > 0.0f && m_primaryAbility.isActive) {
        m_overshieldHP -= 5.0f * dt; // Slow decay
        if (m_overshieldHP <= 0.0f) {
            m_overshieldHP = 0.0f;
        }
    }

    // Overshield disappears when ability ends
    if (!m_primaryAbility.isActive) {
        m_overshieldHP = 0.0f;
    }
}

// ============================================================================
// CONSOLE INTEGRATION IMPLEMENTATIONS - Full Cross-Code Hooking
// ============================================================================

void Player::Console_SetHealth(float health)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_health = std::max(0.0f, std::min(m_maxHealth, health));
    if (m_health <= 0.0f) {
        SetActive(false);
    } else if (!IsActive()) {
        SetActive(true); // Resurrect if health is restored
    }
    NotifyStateChange();
    LOG_TO_CONSOLE_IMMEDIATE(L"Player health set to " + std::to_wstring(m_health) + L" via console", L"SUCCESS");
}

void Player::Console_SetArmor(float armor)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_armor = std::max(0.0f, std::min(m_maxArmor, armor));
    NotifyStateChange();
    LOG_TO_CONSOLE_IMMEDIATE(L"Player armor set to " + std::to_wstring(m_armor) + L" via console", L"SUCCESS");
}

void Player::Console_SetMaxHealth(float maxHealth)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (maxHealth > 0.0f && maxHealth <= 9999.0f) {
        m_maxHealth = maxHealth;
        // Ensure current health doesn't exceed new maximum
        if (m_health > m_maxHealth) {
            m_health = m_maxHealth;
        }
        NotifyStateChange();
        LOG_TO_CONSOLE_IMMEDIATE(L"Player max health set to " + std::to_wstring(m_maxHealth) + L" via console", L"SUCCESS");
    } else {
        LOG_TO_CONSOLE_IMMEDIATE(L"Invalid max health value. Must be between 1 and 9999", L"ERROR");
    }
}

void Player::Console_SetSpeed(float speed)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (speed > 0.0f && speed <= 100.0f) {
        m_speed = speed;
        NotifyStateChange();
        LOG_TO_CONSOLE_IMMEDIATE(L"Player speed set to " + std::to_wstring(m_speed) + L" via console", L"SUCCESS");
    } else {
        LOG_TO_CONSOLE_IMMEDIATE(L"Invalid speed value. Must be between 0.1 and 100", L"ERROR");
    }
}

void Player::Console_SetJumpHeight(float height)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (height > 0.0f && height <= 50.0f) {
        m_jumpHeight = height;
        NotifyStateChange();
        LOG_TO_CONSOLE_IMMEDIATE(L"Player jump height set to " + std::to_wstring(m_jumpHeight) + L" via console", L"SUCCESS");
    } else {
        LOG_TO_CONSOLE_IMMEDIATE(L"Invalid jump height. Must be between 0.1 and 50", L"ERROR");
    }
}

void Player::Console_SetPosition(float x, float y, float z)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    DirectX::XMFLOAT3 newPos = { x, y, z };
    SetPosition(newPos);
    
    // Update camera position to match
    if (m_camera) {
        m_camera->SetPosition(newPos);
    }
    
    // Reset velocity on teleport
    m_velocity = { 0.0f, 0.0f, 0.0f };
    
    NotifyStateChange();
    LOG_TO_CONSOLE_IMMEDIATE(L"Player teleported to (" + std::to_wstring(x) + L", " + 
                            std::to_wstring(y) + L", " + std::to_wstring(z) + L") via console", L"SUCCESS");
}

void Player::Console_SetGodMode(bool enabled)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_godModeEnabled = enabled;
    NotifyStateChange();
    LOG_TO_CONSOLE_IMMEDIATE(L"Player god mode " + std::wstring(enabled ? L"enabled" : L"disabled") + L" via console", L"SUCCESS");
}

void Player::Console_SetNoclip(bool enabled)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_noclipEnabled = enabled;
    NotifyStateChange();
    LOG_TO_CONSOLE_IMMEDIATE(L"Player noclip " + std::wstring(enabled ? L"enabled" : L"disabled") + L" via console", L"SUCCESS");
}

void Player::Console_SetInfiniteAmmo(bool enabled)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_infiniteAmmoEnabled = enabled;
    NotifyStateChange();
    LOG_TO_CONSOLE_IMMEDIATE(L"Player infinite ammo " + std::wstring(enabled ? L"enabled" : L"disabled") + L" via console", L"SUCCESS");
}

void Player::Console_GiveAmmo(int amount)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (amount > 0 && amount <= 9999) {
        m_currentAmmo = std::min(m_currentWeapon.MagazineSize, m_currentAmmo + amount);
        NotifyStateChange();
        LOG_TO_CONSOLE_IMMEDIATE(L"Player given " + std::to_wstring(amount) + L" ammo. Current: " + 
                                std::to_wstring(m_currentAmmo) + L" via console", L"SUCCESS");
    } else {
        LOG_TO_CONSOLE_IMMEDIATE(L"Invalid ammo amount. Must be between 1 and 9999", L"ERROR");
    }
}

void Player::Console_ChangeWeapon(WeaponType weaponType)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    ChangeWeapon(weaponType);
    NotifyStateChange();
    LOG_TO_CONSOLE_IMMEDIATE(L"Player weapon changed via console", L"SUCCESS");
}

Player::PlayerState Player::Console_GetState() const
{
    return GetStateThreadSafe();
}

void Player::Console_RegisterStateCallback(std::function<void(const PlayerState&)> callback)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_stateCallback = callback;
    LOG_TO_CONSOLE_IMMEDIATE(L"Player state callback registered", L"INFO");
}

void Player::Console_ApplyPhysicsSettings(float gravity, float friction)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (gravity >= -100.0f && gravity <= 100.0f) {
        m_gravityForce = gravity;
    }
    if (friction >= 0.0f && friction <= 1.0f) {
        m_frictionCoeff = friction;
    }
    NotifyStateChange();
    LOG_TO_CONSOLE_IMMEDIATE(L"Player physics settings updated - Gravity: " + std::to_wstring(m_gravityForce) + 
                            L", Friction: " + std::to_wstring(m_frictionCoeff) + L" via console", L"SUCCESS");
}

void Player::NotifyStateChange()
{
    if (m_stateCallback) {
        m_stateCallback(GetStateThreadSafe());
    }
}

Player::PlayerState Player::GetStateThreadSafe() const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    PlayerState state;
    state.health = m_health;
    state.maxHealth = m_maxHealth;
    state.armor = m_armor;
    state.maxArmor = m_maxArmor;
    state.stamina = m_stamina;
    state.maxStamina = m_maxStamina;
    state.shield = m_shield;
    state.maxShield = m_maxShield;
    state.energy = m_energy;
    state.maxEnergy = m_maxEnergy;
    state.position = GetPosition();
    state.velocity = m_velocity;
    state.currentWeapon = m_currentWeapon.Type;
    state.currentAmmo = m_currentAmmo;
    state.maxAmmo = m_currentWeapon.MagazineSize;
    state.isAlive = IsAlive();
    state.isGrounded = m_isGrounded;
    state.isReloading = m_isReloading;
    state.isRunning = m_isRunning;
    state.isCrouching = m_isCrouching;
    state.godMode = m_godModeEnabled;
    state.noclip = m_noclipEnabled;
    state.infiniteAmmo = m_infiniteAmmoEnabled;
    state.fireTimer = m_fireTimer;
    state.reloadTimer = m_reloadTimer;
    state.speed = m_speed;
    state.jumpHeight = m_jumpHeight;
    state.playerClass = m_playerClass;
    state.activeLoadoutSlot = m_activeLoadoutSlot;
    state.primaryAbilityActive = m_primaryAbility.isActive;
    state.secondaryAbilityActive = m_secondaryAbility.isActive;
    return state;
}