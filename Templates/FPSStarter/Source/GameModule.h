/**
 * @file GameModule.h
 * @brief FPSStarter — FPS game module
 *
 * First-person shooter template with weapon system, AI enemies, health,
 * and a basic HUD. Extend this to build your own FPS game.
 */

#pragma once

#include <Spark/SparkSDK.h>

#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
// Player state
// ============================================================================

struct PlayerState
{
    float health = 100.0f;
    float maxHealth = 100.0f;
    float armor = 0.0f;
    uint32_t currentWeapon = 0;
    uint32_t ammo = 30;
    uint32_t reserveAmmo = 90;
    uint32_t kills = 0;
    uint32_t deaths = 0;
    bool isAlive = true;
};

// ============================================================================
// Weapon definition
// ============================================================================

struct WeaponDef
{
    std::string name;
    float damage = 25.0f;
    float fireRate = 0.1f; ///< Seconds between shots
    float range = 100.0f;
    uint32_t magazineSize = 30;
    float reloadTime = 2.0f;
    bool isAutomatic = true;
};

// ============================================================================
// FPS Game Module
// ============================================================================

class FPSStarterModule : public Spark::IModule
{
  public:
    FPSStarterModule() = default;
    ~FPSStarterModule() override = default;

    Spark::ModuleInfo GetModuleInfo() const override
    {
        Spark::ModuleInfo info{};
        info.name = "FPSStarter";
        info.version = "0.1.0";
        info.sdkVersion = SPARK_SDK_VERSION;
        info.loadOrder = 1000;
        return info;
    }

    bool OnLoad(Spark::IEngineContext* context) override
    {
        m_context = context;
        InitializeWeapons();
        m_player = PlayerState{};
        return true;
    }

    void OnUnload() override { m_context = nullptr; }

    void OnUpdate(float deltaTime) override
    {
        if (!m_player.isAlive)
        {
            m_respawnTimer -= deltaTime;
            if (m_respawnTimer <= 0.0f)
            {
                Respawn();
            }
            return;
        }

        m_fireTimer -= deltaTime;
        // Game logic: check input, fire weapon, update AI, etc.
    }

    void OnRender() override
    {
        // HUD rendering: health bar, ammo counter, crosshair, kill feed
    }

  private:
    void InitializeWeapons()
    {
        m_weapons.push_back({"Pistol", 20.0f, 0.3f, 50.0f, 12, 1.5f, false});
        m_weapons.push_back({"Assault Rifle", 25.0f, 0.1f, 80.0f, 30, 2.0f, true});
        m_weapons.push_back({"Shotgun", 80.0f, 0.8f, 15.0f, 8, 2.5f, false});
        m_weapons.push_back({"Sniper Rifle", 100.0f, 1.2f, 200.0f, 5, 3.0f, false});
    }

    void Respawn()
    {
        m_player.health = m_player.maxHealth;
        m_player.isAlive = true;
        m_player.ammo = m_weapons[m_player.currentWeapon].magazineSize;
        m_respawnTimer = 3.0f;
    }

    Spark::IEngineContext* m_context = nullptr;
    PlayerState m_player;
    std::vector<WeaponDef> m_weapons;
    float m_fireTimer = 0.0f;
    float m_respawnTimer = 3.0f;
};
