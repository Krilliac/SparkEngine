// TestGameplaySystemExtension.cpp — tests for the quest/dialogue extension
// registry used by game modules to plug genre-specific behavior into the
// engine QuestSystem and DialogueSystem.
//
// The registry is a header-only singleton with no external dependencies;
// these tests exercise the real class from
// SparkEngine/Source/Engine/Gameplay/GameplaySystemExtension.h. A separate
// Tests/TestGameplayExtensionRegistry.cpp already exists and uses a
// standalone reimplementation; this file is additive and targets the
// real engine types.

#include "TestFramework.h"
#include "Engine/Gameplay/GameplaySystemExtension.h"

#include <memory>
#include <string>

using namespace Spark::Gameplay;

namespace
{
    class TestQuestExtension : public IQuestTypeExtension
    {
      public:
        explicit TestQuestExtension(std::string typeName) : m_typeName(std::move(typeName)) {}

        std::string GetTypeName() const override { return m_typeName; }
        bool CanStart(const QuestExtensionInput&, const QuestExtensionState&) const override { return true; }
        void OnStarted(const QuestExtensionInput&, QuestExtensionState& ctx) override { ctx.isActive = true; }
        void OnObjectiveProgress(const QuestExtensionInput&, QuestExtensionState& ctx, const std::string&,
                                 int32_t progress) override
        {
            ctx.objectiveProgress.push_back(progress);
            ++m_progressCount;
        }
        bool IsComplete(const QuestExtensionInput&, const QuestExtensionState& ctx) const override
        {
            return ctx.isComplete;
        }
        void OnCompleted(const QuestExtensionInput&, QuestExtensionState&) override { ++m_completedCount; }

        int m_progressCount = 0;
        int m_completedCount = 0;

      private:
        std::string m_typeName;
    };

    class TestDialogueExtension : public IDialogueExtension
    {
      public:
        explicit TestDialogueExtension(std::string name) : m_name(std::move(name)) {}

        std::string GetExtensionName() const override { return m_name; }
        bool EvaluateCondition(const std::string&, const std::string&) const override { return m_conditionValue; }
        void ExecuteAction(const std::string&, const std::string&) override { ++m_actionCount; }

        bool m_conditionValue = true;
        int m_actionCount = 0;

      private:
        std::string m_name;
    };
} // namespace

TEST(GameplayExtensionRegistryReal_SingletonAlive)
{
    auto& reg = GameplayExtensionRegistry::GetInstance();
    reg.Clear();
    EXPECT_EQ(reg.GetRegisteredQuestTypes().size(), static_cast<size_t>(0));
    EXPECT_EQ(reg.GetRegisteredDialogueExtensions().size(), static_cast<size_t>(0));
}

TEST(GameplayExtensionRegistryReal_RegisterAndLookupQuestExtension)
{
    auto& reg = GameplayExtensionRegistry::GetInstance();
    reg.Clear();

    reg.RegisterQuestExtension(std::make_unique<TestQuestExtension>("escort"));
    reg.RegisterQuestExtension(std::make_unique<TestQuestExtension>("faction_reputation"));

    auto types = reg.GetRegisteredQuestTypes();
    EXPECT_EQ(types.size(), static_cast<size_t>(2));

    auto* escort = reg.GetQuestExtension("escort");
    EXPECT_NE(escort, nullptr);
    if (escort)
        EXPECT_EQ(escort->GetTypeName(), std::string("escort"));

    auto* missing = reg.GetQuestExtension("does_not_exist");
    EXPECT_EQ(missing, nullptr);

    reg.Clear();
}

TEST(GameplayExtensionRegistryReal_RegisterAndLookupDialogueExtension)
{
    auto& reg = GameplayExtensionRegistry::GetInstance();
    reg.Clear();

    reg.RegisterDialogueExtension(std::make_unique<TestDialogueExtension>("trade"));
    reg.RegisterDialogueExtension(std::make_unique<TestDialogueExtension>("skill_check"));

    auto names = reg.GetRegisteredDialogueExtensions();
    EXPECT_EQ(names.size(), static_cast<size_t>(2));

    auto* trade = reg.GetDialogueExtension("trade");
    EXPECT_NE(trade, nullptr);
    if (trade)
    {
        EXPECT_TRUE(trade->EvaluateCondition("has_item", "iron_sword"));
        trade->ExecuteAction("give_item", "gold_coin:10");
    }

    reg.Clear();
}

TEST(GameplayExtensionRegistryReal_NullExtensionIgnored)
{
    auto& reg = GameplayExtensionRegistry::GetInstance();
    reg.Clear();

    reg.RegisterQuestExtension(nullptr);
    reg.RegisterDialogueExtension(nullptr);
    EXPECT_EQ(reg.GetRegisteredQuestTypes().size(), static_cast<size_t>(0));
    EXPECT_EQ(reg.GetRegisteredDialogueExtensions().size(), static_cast<size_t>(0));
}

TEST(GameplayExtensionRegistryReal_ClearWipesBothLists)
{
    auto& reg = GameplayExtensionRegistry::GetInstance();
    reg.Clear();

    reg.RegisterQuestExtension(std::make_unique<TestQuestExtension>("escort"));
    reg.RegisterDialogueExtension(std::make_unique<TestDialogueExtension>("trade"));
    EXPECT_GT(reg.GetRegisteredQuestTypes().size(), static_cast<size_t>(0));
    EXPECT_GT(reg.GetRegisteredDialogueExtensions().size(), static_cast<size_t>(0));

    reg.Clear();
    EXPECT_EQ(reg.GetRegisteredQuestTypes().size(), static_cast<size_t>(0));
    EXPECT_EQ(reg.GetRegisteredDialogueExtensions().size(), static_cast<size_t>(0));
}

TEST(GameplayExtensionRegistryReal_QuestExtensionLifecycleMethods)
{
    auto& reg = GameplayExtensionRegistry::GetInstance();
    reg.Clear();

    reg.RegisterQuestExtension(std::make_unique<TestQuestExtension>("escort"));
    auto* ext = reg.GetQuestExtension("escort");
    EXPECT_NE(ext, nullptr);

    QuestExtensionInput def;
    def.questId = 1;
    def.typeName = "escort";

    QuestExtensionState ctx;
    ctx.questId = 1;

    EXPECT_TRUE(ext->CanStart(def, ctx));
    ext->OnStarted(def, ctx);
    EXPECT_TRUE(ctx.isActive);

    ext->OnObjectiveProgress(def, ctx, "kill_10_rats", 5);
    ext->OnObjectiveProgress(def, ctx, "kill_10_rats", 5);
    EXPECT_EQ(static_cast<TestQuestExtension*>(ext)->m_progressCount, 2);
    EXPECT_EQ(ctx.objectiveProgress.size(), static_cast<size_t>(2));

    EXPECT_FALSE(ext->IsComplete(def, ctx));
    ctx.isComplete = true;
    EXPECT_TRUE(ext->IsComplete(def, ctx));

    ext->OnCompleted(def, ctx);
    EXPECT_EQ(static_cast<TestQuestExtension*>(ext)->m_completedCount, 1);

    reg.Clear();
}
