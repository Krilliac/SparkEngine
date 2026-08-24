#pragma once

#include <Spark/SparkSDK.h>

#include <algorithm>
#include <cstdint>

struct FPSStarterWeaponState
{
    uint32_t magazine = 8;
    uint32_t reserve = 24;
    float damage = 25.0f;
    float fireInterval = 0.2f;
    float reloadDuration = 1.0f;
};

struct FPSStarterPlayerState
{
    float health = 100.0f;
    uint32_t deaths = 0;
    uint32_t kills = 0;
    bool alive = true;
};

struct FPSStarterTargetState
{
    float health = 100.0f;
    bool destroyed = false;
};

class FPSStarterModule final : public Spark::IModule
{
  public:
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
        ResetRound();
        return true;
    }

    void OnUnload() override { m_context = nullptr; }

    void OnUpdate(float deltaTime) override
    {
        if (deltaTime <= 0.0f)
            return;

        m_fireCooldown = std::max(0.0f, m_fireCooldown - deltaTime);
        if (m_reloadRemaining > 0.0f)
        {
            m_reloadRemaining = std::max(0.0f, m_reloadRemaining - deltaTime);
            if (m_reloadRemaining == 0.0f)
                FinishReload();
        }

        if (!m_player.alive)
        {
            m_respawnRemaining = std::max(0.0f, m_respawnRemaining - deltaTime);
            if (m_respawnRemaining == 0.0f)
                Respawn();
        }
    }

    bool TryFire()
    {
        if (!m_player.alive || m_target.destroyed || m_fireCooldown > 0.0f || m_reloadRemaining > 0.0f ||
            m_weapon.magazine == 0)
            return false;

        --m_weapon.magazine;
        m_fireCooldown = m_weapon.fireInterval;
        m_target.health = std::max(0.0f, m_target.health - m_weapon.damage);
        if (m_target.health == 0.0f)
        {
            m_target.destroyed = true;
            ++m_player.kills;
            m_roundWon = true;
        }
        return true;
    }

    bool BeginReload()
    {
        if (!m_player.alive || m_reloadRemaining > 0.0f || m_weapon.magazine >= kMagazineCapacity ||
            m_weapon.reserve == 0)
            return false;
        m_reloadRemaining = m_weapon.reloadDuration;
        return true;
    }

    void DamagePlayer(float amount)
    {
        if (!m_player.alive || amount <= 0.0f)
            return;
        m_player.health = std::max(0.0f, m_player.health - amount);
        if (m_player.health == 0.0f)
        {
            m_player.alive = false;
            ++m_player.deaths;
            m_respawnRemaining = kRespawnDelay;
        }
    }

    void ResetRound()
    {
        m_player = {};
        m_target = {};
        m_weapon = {};
        m_fireCooldown = 0.0f;
        m_reloadRemaining = 0.0f;
        m_respawnRemaining = 0.0f;
        m_roundWon = false;
    }

    [[nodiscard]] const FPSStarterPlayerState& GetPlayerState() const { return m_player; }
    [[nodiscard]] const FPSStarterTargetState& GetTargetState() const { return m_target; }
    [[nodiscard]] const FPSStarterWeaponState& GetWeaponState() const { return m_weapon; }
    [[nodiscard]] float GetReloadRemaining() const { return m_reloadRemaining; }
    [[nodiscard]] float GetRespawnRemaining() const { return m_respawnRemaining; }
    [[nodiscard]] bool HasWonRound() const { return m_roundWon; }

  private:
    void FinishReload()
    {
        const uint32_t needed = kMagazineCapacity - m_weapon.magazine;
        const uint32_t transferred = std::min(needed, m_weapon.reserve);
        m_weapon.magazine += transferred;
        m_weapon.reserve -= transferred;
    }

    void Respawn()
    {
        m_player.health = 100.0f;
        m_player.alive = true;
        m_weapon.magazine = kMagazineCapacity;
    }

    static constexpr uint32_t kMagazineCapacity = 8;
    static constexpr float kRespawnDelay = 2.0f;

    Spark::IEngineContext* m_context = nullptr;
    FPSStarterPlayerState m_player;
    FPSStarterTargetState m_target;
    FPSStarterWeaponState m_weapon;
    float m_fireCooldown = 0.0f;
    float m_reloadRemaining = 0.0f;
    float m_respawnRemaining = 0.0f;
    bool m_roundWon = false;
};
