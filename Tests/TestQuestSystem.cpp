// TestQuestSystem.cpp - Tests for quest tracking, objectives, and completion
// Standalone implementations for CI testing

#include "TestFramework.h"
#include "Engine/Gameplay/QuestSystem.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cstdint>

namespace TestQuest
{

    enum class QuestStatus
    {
        NotStarted,
        Active,
        Completed,
        Failed
    };
    enum class ObjectiveType
    {
        Kill,
        Collect,
        Reach,
        Interact,
        Survive
    };

    struct QuestObjective
    {
        ObjectiveType type = ObjectiveType::Kill;
        std::string description;
        uint32_t targetId = 0;
        int requiredCount = 1;
        bool optional = false;
    };

    struct ObjectiveProgress
    {
        int currentCount = 0;
        bool completed = false;
    };

    struct QuestDef
    {
        uint32_t id = 0;
        std::string name;
        std::vector<QuestObjective> objectives;
        std::vector<uint32_t> requiredQuests;
        bool isRepeatable = false;
        float timeLimit = 0.0f;
    };

    struct ActiveQuest
    {
        uint32_t questId = 0;
        QuestStatus status = QuestStatus::Active;
        std::vector<ObjectiveProgress> objectiveProgress;
        float elapsedTime = 0.0f;
    };

    struct QuestJournal
    {
        std::vector<ActiveQuest> activeQuests;
        std::unordered_set<uint32_t> completedQuestIds;
        std::unordered_set<uint32_t> failedQuestIds;
    };

    class QuestRegistry
    {
      public:
        void RegisterQuest(const QuestDef& def) { m_quests[def.id] = def; }
        const QuestDef* GetQuest(uint32_t id) const
        {
            auto it = m_quests.find(id);
            return (it != m_quests.end()) ? &it->second : nullptr;
        }
        size_t GetQuestCount() const { return m_quests.size(); }

      private:
        std::unordered_map<uint32_t, QuestDef> m_quests;
    };

    namespace QuestOps
    {

        inline bool StartQuest(QuestJournal& journal, const QuestRegistry& reg, uint32_t questId)
        {
            const QuestDef* def = reg.GetQuest(questId);
            if (!def)
                return false;
            if (!def->isRepeatable && journal.completedQuestIds.count(questId))
                return false;
            for (const auto& aq : journal.activeQuests)
                if (aq.questId == questId)
                    return false;
            for (uint32_t reqId : def->requiredQuests)
                if (!journal.completedQuestIds.count(reqId))
                    return false;

            ActiveQuest aq;
            aq.questId = questId;
            aq.objectiveProgress.resize(def->objectives.size());
            journal.activeQuests.push_back(std::move(aq));
            return true;
        }

        inline bool UpdateObjective(QuestJournal& journal, const QuestRegistry& reg, uint32_t questId, int objIndex,
                                    int increment = 1)
        {
            const QuestDef* def = reg.GetQuest(questId);
            if (!def)
                return false;
            for (auto& aq : journal.activeQuests)
            {
                if (aq.questId == questId && aq.status == QuestStatus::Active)
                {
                    if (objIndex < 0 || objIndex >= static_cast<int>(aq.objectiveProgress.size()))
                        return false;
                    auto& prog = aq.objectiveProgress[objIndex];
                    if (prog.completed)
                        return false;
                    prog.currentCount += increment;
                    if (prog.currentCount >= def->objectives[objIndex].requiredCount)
                    {
                        prog.currentCount = def->objectives[objIndex].requiredCount;
                        prog.completed = true;
                    }
                    bool allDone = true;
                    for (int i = 0; i < static_cast<int>(def->objectives.size()); ++i)
                    {
                        if (!def->objectives[i].optional && !aq.objectiveProgress[i].completed)
                        {
                            allDone = false;
                            break;
                        }
                    }
                    if (allDone)
                    {
                        aq.status = QuestStatus::Completed;
                        journal.completedQuestIds.insert(questId);
                    }
                    return true;
                }
            }
            return false;
        }

        inline bool FailQuest(QuestJournal& journal, uint32_t questId)
        {
            for (auto& aq : journal.activeQuests)
            {
                if (aq.questId == questId && aq.status == QuestStatus::Active)
                {
                    aq.status = QuestStatus::Failed;
                    journal.failedQuestIds.insert(questId);
                    return true;
                }
            }
            return false;
        }

        inline const ActiveQuest* GetActiveQuest(const QuestJournal& journal, uint32_t questId)
        {
            for (const auto& aq : journal.activeQuests)
                if (aq.questId == questId)
                    return &aq;
            return nullptr;
        }

        inline int GetActiveQuestCount(const QuestJournal& journal)
        {
            int count = 0;
            for (const auto& aq : journal.activeQuests)
                if (aq.status == QuestStatus::Active)
                    ++count;
            return count;
        }

        inline void UpdateTimers(QuestJournal& journal, const QuestRegistry& reg, float dt)
        {
            for (auto& aq : journal.activeQuests)
            {
                if (aq.status != QuestStatus::Active)
                    continue;
                aq.elapsedTime += dt;
                const QuestDef* def = reg.GetQuest(aq.questId);
                if (def && def->timeLimit > 0.0f && aq.elapsedTime >= def->timeLimit)
                {
                    aq.status = QuestStatus::Failed;
                    journal.failedQuestIds.insert(aq.questId);
                }
            }
        }

    } // namespace QuestOps
} // namespace TestQuest

// =============================================================================
// Tests
// =============================================================================

TEST(Quest_StartQuest)
{
    TestQuest::QuestRegistry reg;
    TestQuest::QuestDef def;
    def.id = 1;
    def.name = "Kill Enemies";
    def.objectives.push_back({TestQuest::ObjectiveType::Kill, "Kill 5 enemies", 0, 5});
    reg.RegisterQuest(def);

    TestQuest::QuestJournal journal;
    bool started = TestQuest::QuestOps::StartQuest(journal, reg, 1);
    EXPECT_TRUE(started);
    EXPECT_EQ(TestQuest::QuestOps::GetActiveQuestCount(journal), 1);
}

TEST(Quest_CannotStartTwice)
{
    TestQuest::QuestRegistry reg;
    TestQuest::QuestDef def;
    def.id = 1;
    def.name = "Test";
    def.objectives.push_back({TestQuest::ObjectiveType::Kill, "Test", 0, 1});
    reg.RegisterQuest(def);

    TestQuest::QuestJournal journal;
    EXPECT_TRUE(TestQuest::QuestOps::StartQuest(journal, reg, 1));
    EXPECT_FALSE(TestQuest::QuestOps::StartQuest(journal, reg, 1));
}

TEST(Quest_UpdateObjective)
{
    TestQuest::QuestRegistry reg;
    TestQuest::QuestDef def;
    def.id = 1;
    def.objectives.push_back({TestQuest::ObjectiveType::Kill, "Kill 3 enemies", 0, 3});
    reg.RegisterQuest(def);

    TestQuest::QuestJournal journal;
    TestQuest::QuestOps::StartQuest(journal, reg, 1);

    EXPECT_TRUE(TestQuest::QuestOps::UpdateObjective(journal, reg, 1, 0, 1));
    auto* aq = TestQuest::QuestOps::GetActiveQuest(journal, 1);
    EXPECT_EQ(aq->objectiveProgress[0].currentCount, 1);
    EXPECT_FALSE(aq->objectiveProgress[0].completed);

    TestQuest::QuestOps::UpdateObjective(journal, reg, 1, 0, 2);
    aq = TestQuest::QuestOps::GetActiveQuest(journal, 1);
    EXPECT_TRUE(aq->objectiveProgress[0].completed);
    EXPECT_EQ((int)aq->status, (int)TestQuest::QuestStatus::Completed);
}

TEST(Quest_MultipleObjectives)
{
    TestQuest::QuestRegistry reg;
    TestQuest::QuestDef def;
    def.id = 1;
    def.objectives.push_back({TestQuest::ObjectiveType::Kill, "Kill 2", 0, 2});
    def.objectives.push_back({TestQuest::ObjectiveType::Collect, "Collect 3", 0, 3});
    reg.RegisterQuest(def);

    TestQuest::QuestJournal journal;
    TestQuest::QuestOps::StartQuest(journal, reg, 1);

    // Complete first objective only
    TestQuest::QuestOps::UpdateObjective(journal, reg, 1, 0, 2);
    auto* aq = TestQuest::QuestOps::GetActiveQuest(journal, 1);
    EXPECT_EQ((int)aq->status, (int)TestQuest::QuestStatus::Active); // Not completed yet

    // Complete second objective
    TestQuest::QuestOps::UpdateObjective(journal, reg, 1, 1, 3);
    aq = TestQuest::QuestOps::GetActiveQuest(journal, 1);
    EXPECT_EQ((int)aq->status, (int)TestQuest::QuestStatus::Completed);
}

TEST(Quest_OptionalObjective)
{
    TestQuest::QuestRegistry reg;
    TestQuest::QuestDef def;
    def.id = 1;
    def.objectives.push_back({TestQuest::ObjectiveType::Kill, "Required", 0, 1, false});
    def.objectives.push_back({TestQuest::ObjectiveType::Collect, "Optional", 0, 5, true});
    reg.RegisterQuest(def);

    TestQuest::QuestJournal journal;
    TestQuest::QuestOps::StartQuest(journal, reg, 1);

    // Complete only the required objective
    TestQuest::QuestOps::UpdateObjective(journal, reg, 1, 0, 1);
    auto* aq = TestQuest::QuestOps::GetActiveQuest(journal, 1);
    EXPECT_EQ((int)aq->status, (int)TestQuest::QuestStatus::Completed);
}

TEST(Quest_FailQuest)
{
    TestQuest::QuestRegistry reg;
    TestQuest::QuestDef def;
    def.id = 1;
    def.objectives.push_back({TestQuest::ObjectiveType::Kill, "Test", 0, 1});
    reg.RegisterQuest(def);

    TestQuest::QuestJournal journal;
    TestQuest::QuestOps::StartQuest(journal, reg, 1);

    EXPECT_TRUE(TestQuest::QuestOps::FailQuest(journal, 1));
    auto* aq = TestQuest::QuestOps::GetActiveQuest(journal, 1);
    EXPECT_EQ((int)aq->status, (int)TestQuest::QuestStatus::Failed);
    EXPECT_TRUE(journal.failedQuestIds.count(1) > 0);
}

TEST(Quest_Prerequisites)
{
    TestQuest::QuestRegistry reg;
    TestQuest::QuestDef quest1;
    quest1.id = 1;
    quest1.objectives.push_back({TestQuest::ObjectiveType::Kill, "Test", 0, 1});
    reg.RegisterQuest(quest1);

    TestQuest::QuestDef quest2;
    quest2.id = 2;
    quest2.objectives.push_back({TestQuest::ObjectiveType::Kill, "Test", 0, 1});
    quest2.requiredQuests = {1};
    reg.RegisterQuest(quest2);

    TestQuest::QuestJournal journal;
    // Can't start quest 2 without completing quest 1
    EXPECT_FALSE(TestQuest::QuestOps::StartQuest(journal, reg, 2));

    // Complete quest 1
    TestQuest::QuestOps::StartQuest(journal, reg, 1);
    TestQuest::QuestOps::UpdateObjective(journal, reg, 1, 0, 1);

    // Now quest 2 should be available
    EXPECT_TRUE(TestQuest::QuestOps::StartQuest(journal, reg, 2));
}

TEST(Quest_TimeLimitExpiry)
{
    TestQuest::QuestRegistry reg;
    TestQuest::QuestDef def;
    def.id = 1;
    def.objectives.push_back({TestQuest::ObjectiveType::Survive, "Survive 10s", 0, 1});
    def.timeLimit = 10.0f;
    reg.RegisterQuest(def);

    TestQuest::QuestJournal journal;
    TestQuest::QuestOps::StartQuest(journal, reg, 1);

    TestQuest::QuestOps::UpdateTimers(journal, reg, 5.0f);
    auto* aq = TestQuest::QuestOps::GetActiveQuest(journal, 1);
    EXPECT_EQ((int)aq->status, (int)TestQuest::QuestStatus::Active);

    TestQuest::QuestOps::UpdateTimers(journal, reg, 6.0f);
    aq = TestQuest::QuestOps::GetActiveQuest(journal, 1);
    EXPECT_EQ((int)aq->status, (int)TestQuest::QuestStatus::Failed);
}

TEST(Quest_CannotRestartCompleted)
{
    TestQuest::QuestRegistry reg;
    TestQuest::QuestDef def;
    def.id = 1;
    def.objectives.push_back({TestQuest::ObjectiveType::Kill, "Test", 0, 1});
    reg.RegisterQuest(def);

    TestQuest::QuestJournal journal;
    TestQuest::QuestOps::StartQuest(journal, reg, 1);
    TestQuest::QuestOps::UpdateObjective(journal, reg, 1, 0, 1);

    EXPECT_FALSE(TestQuest::QuestOps::StartQuest(journal, reg, 1));
}

TEST(Quest_StartInvalidQuest)
{
    TestQuest::QuestRegistry reg;
    TestQuest::QuestJournal journal;
    EXPECT_FALSE(TestQuest::QuestOps::StartQuest(journal, reg, 999));
}

TEST(QuestSystemReal_SnapshotValidationAndRestoreAreTransactional)
{
    using namespace Spark::Gameplay;
    auto& quests = QuestSystem::GetInstance();
    quests.Shutdown();
    quests.Initialize();

    QuestDefinition definition;
    definition.questId = 70001;
    definition.name = "Snapshot contract";
    definition.objectives.push_back({"Complete two steps", QuestObjective::Type::Interact, 9, 2, 0});
    quests.RegisterQuest(definition);
    EXPECT_TRUE(quests.StartQuest(42, definition.questId));
    quests.ReportProgress(42, QuestObjective::Type::Interact, 9, 1);

    const auto original = quests.CaptureEntityState(42);
    EXPECT_EQ(original.size(), static_cast<size_t>(1));
    EXPECT_EQ(original[0].objectiveCounts[0], 1u);
    EXPECT_TRUE(quests.ValidateEntityState(original));

    auto impossible = original;
    impossible[0].state = QuestState::Completed;
    EXPECT_FALSE(quests.ValidateEntityState(impossible));
    EXPECT_FALSE(quests.RestoreEntityState(42, impossible));
    EXPECT_EQ(quests.CaptureEntityState(42)[0].objectiveCounts[0], 1u);

    auto duplicate = original;
    duplicate.push_back(original[0]);
    EXPECT_FALSE(quests.RestoreEntityState(42, duplicate));
    auto unknown = original;
    unknown[0].questId = 999999;
    EXPECT_FALSE(quests.RestoreEntityState(42, unknown));

    auto completed = original;
    completed[0].state = QuestState::Completed;
    completed[0].objectiveCounts[0] = 2;
    EXPECT_TRUE(quests.RestoreEntityState(42, completed));
    EXPECT_EQ(static_cast<int>(quests.CaptureEntityState(42)[0].state), static_cast<int>(QuestState::Completed));
    quests.ClearEntityState(42);
    EXPECT_TRUE(quests.CaptureEntityState(42).empty());
    quests.Shutdown();
}
