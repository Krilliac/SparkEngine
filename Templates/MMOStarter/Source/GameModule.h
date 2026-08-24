#pragma once

#include <Spark/SparkSDK.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

enum class MMOStarterFaction : uint8_t
{
    Unassigned,
    Azure,
    Ember
};

struct MMOStarterState
{
    std::string characterName;
    MMOStarterFaction faction = MMOStarterFaction::Unassigned;
    float playerHealth = 100.0f;
    float botHealth = 75.0f;
    float captureProgress = 0.0f;
    float respawnRemaining = 0.0f;
    uint32_t deaths = 0;
    uint32_t botDefeats = 0;
    bool serverRunning = false;
    bool clientConnected = false;
    bool characterCreated = false;
    bool playerAlive = true;
    bool objectiveCaptured = false;
};

class MMOStarterModule final : public Spark::IModule
{
  public:
    Spark::ModuleInfo GetModuleInfo() const override
    {
        Spark::ModuleInfo info{};
        info.name = "MMOStarter";
        info.version = "0.1.0";
        info.sdkVersion = SPARK_SDK_VERSION;
        info.loadOrder = 1000;
        return info;
    }

    bool OnLoad(Spark::IEngineContext* context) override
    {
        m_context = context;
        ResetSession();
        return true;
    }

    void OnUnload() override
    {
        m_context = nullptr;
        m_state.serverRunning = false;
        m_state.clientConnected = false;
    }

    void OnUpdate(float deltaTime) override
    {
        if (deltaTime <= 0.0f || m_state.playerAlive)
            return;
        m_state.respawnRemaining = std::max(0.0f, m_state.respawnRemaining - deltaTime);
        if (m_state.respawnRemaining == 0.0f)
        {
            m_state.playerHealth = 100.0f;
            m_state.playerAlive = true;
        }
    }

    bool StartLocalSession()
    {
        if (m_state.serverRunning)
            return false;
        m_state.serverRunning = true;
        m_state.clientConnected = true;
        return true;
    }

    bool CreateCharacter(const std::string& name)
    {
        if (!m_state.clientConnected || name.size() < 3 || name.size() > 16)
            return false;
        m_state.characterName = name;
        m_state.characterCreated = true;
        return true;
    }

    bool SelectFaction(MMOStarterFaction faction)
    {
        if (!m_state.characterCreated || faction == MMOStarterFaction::Unassigned)
            return false;
        m_state.faction = faction;
        return true;
    }

    bool AdvanceCapture(float seconds)
    {
        if (!CanPlay() || seconds <= 0.0f || m_state.objectiveCaptured)
            return false;
        m_state.captureProgress = std::min(100.0f, m_state.captureProgress + seconds * 20.0f);
        m_state.objectiveCaptured = m_state.captureProgress == 100.0f;
        return true;
    }

    bool AttackBot()
    {
        if (!CanPlay() || m_state.botHealth <= 0.0f)
            return false;
        m_state.botHealth = std::max(0.0f, m_state.botHealth - 25.0f);
        if (m_state.botHealth == 0.0f)
            ++m_state.botDefeats;
        return true;
    }

    void DamagePlayer(float amount)
    {
        if (!CanPlay() || amount <= 0.0f)
            return;
        m_state.playerHealth = std::max(0.0f, m_state.playerHealth - amount);
        if (m_state.playerHealth == 0.0f)
        {
            m_state.playerAlive = false;
            m_state.respawnRemaining = 3.0f;
            ++m_state.deaths;
        }
    }

    bool SubmitChat(const std::string& message)
    {
        if (!m_state.clientConnected || message.empty() || message.size() > 120)
            return false;
        if (m_chatLog.size() == 8)
            m_chatLog.erase(m_chatLog.begin());
        m_chatLog.push_back(m_state.characterName.empty() ? message : m_state.characterName + ": " + message);
        return true;
    }

    void ResetSession()
    {
        m_state = {};
        m_chatLog.clear();
    }

    [[nodiscard]] const MMOStarterState& GetState() const { return m_state; }
    [[nodiscard]] const std::vector<std::string>& GetChatLog() const { return m_chatLog; }
    [[nodiscard]] bool CanPlay() const
    {
        return m_state.serverRunning && m_state.clientConnected && m_state.characterCreated &&
               m_state.faction != MMOStarterFaction::Unassigned && m_state.playerAlive;
    }

  private:
    Spark::IEngineContext* m_context = nullptr;
    MMOStarterState m_state;
    std::vector<std::string> m_chatLog;
};
