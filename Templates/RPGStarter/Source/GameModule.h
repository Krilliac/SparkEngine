#pragma once

#include <Spark/SparkSDK.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

enum class RPGStarterQuestStage : uint8_t
{
    NotStarted,
    FindRelic,
    DefeatWarden,
    ReturnToElder,
    Complete
};

struct RPGStarterState
{
    float x = 0.0f;
    float z = 0.0f;
    float health = 100.0f;
    float enemyHealth = 60.0f;
    uint32_t gold = 0;
    uint32_t experience = 0;
    RPGStarterQuestStage questStage = RPGStarterQuestStage::NotStarted;
    bool dialogueOpen = false;
    bool enemyDefeated = false;
};

class RPGStarterModule final : public Spark::IModule
{
  public:
    Spark::ModuleInfo GetModuleInfo() const override
    {
        Spark::ModuleInfo info{};
        info.name = "RPGStarter";
        info.version = "0.1.0";
        info.sdkVersion = SPARK_SDK_VERSION;
        info.loadOrder = 1000;
        return info;
    }

    bool OnLoad(Spark::IEngineContext* context) override
    {
        m_context = context;
        m_savedState = {};
        m_savedInventory.clear();
        m_savedRewardClaimed = false;
        m_hasSave = false;
        NewGame();
        return true;
    }

    void OnUnload() override { m_context = nullptr; }
    void OnUpdate(float deltaTime) override { (void)deltaTime; }

    void Move(float xAxis, float zAxis, float deltaTime)
    {
        if (deltaTime <= 0.0f || m_state.dialogueOpen)
            return;
        m_state.x += std::clamp(xAxis, -1.0f, 1.0f) * 4.0f * deltaTime;
        m_state.z += std::clamp(zAxis, -1.0f, 1.0f) * 4.0f * deltaTime;
    }

    void TalkToElder()
    {
        m_state.dialogueOpen = true;
        if (m_state.questStage == RPGStarterQuestStage::NotStarted)
            m_state.questStage = RPGStarterQuestStage::FindRelic;
        else if (m_state.questStage == RPGStarterQuestStage::ReturnToElder)
            m_state.questStage = RPGStarterQuestStage::Complete;
    }

    void CloseDialogue() { m_state.dialogueOpen = false; }

    bool PickUpRelic()
    {
        if (m_state.questStage != RPGStarterQuestStage::FindRelic || HasItem("Lost Relic"))
            return false;
        m_inventory.emplace_back("Lost Relic");
        m_state.questStage = RPGStarterQuestStage::DefeatWarden;
        return true;
    }

    bool AttackWarden()
    {
        if (m_state.questStage != RPGStarterQuestStage::DefeatWarden || m_state.enemyDefeated)
            return false;
        m_state.enemyHealth = std::max(0.0f, m_state.enemyHealth - 20.0f);
        if (m_state.enemyHealth == 0.0f)
        {
            m_state.enemyDefeated = true;
            m_state.questStage = RPGStarterQuestStage::ReturnToElder;
        }
        return true;
    }

    bool ClaimReward()
    {
        if (m_state.questStage != RPGStarterQuestStage::Complete || m_rewardClaimed)
            return false;
        m_state.gold += 50;
        m_state.experience += 100;
        m_rewardClaimed = true;
        return true;
    }

    void SaveToSlot()
    {
        m_savedState = m_state;
        m_savedInventory = m_inventory;
        m_savedRewardClaimed = m_rewardClaimed;
        m_hasSave = true;
    }

    bool LoadFromSlot()
    {
        if (!m_hasSave)
            return false;
        m_state = m_savedState;
        m_inventory = m_savedInventory;
        m_rewardClaimed = m_savedRewardClaimed;
        return true;
    }

    void NewGame()
    {
        m_state = {};
        m_inventory.clear();
        m_rewardClaimed = false;
    }

    [[nodiscard]] const RPGStarterState& GetState() const { return m_state; }
    [[nodiscard]] const std::vector<std::string>& GetInventory() const { return m_inventory; }
    [[nodiscard]] bool HasSave() const { return m_hasSave; }
    [[nodiscard]] bool IsRewardClaimed() const { return m_rewardClaimed; }
    [[nodiscard]] bool HasItem(const std::string& item) const
    {
        return std::find(m_inventory.begin(), m_inventory.end(), item) != m_inventory.end();
    }

  private:
    Spark::IEngineContext* m_context = nullptr;
    RPGStarterState m_state;
    RPGStarterState m_savedState;
    std::vector<std::string> m_inventory;
    std::vector<std::string> m_savedInventory;
    bool m_rewardClaimed = false;
    bool m_savedRewardClaimed = false;
    bool m_hasSave = false;
};
