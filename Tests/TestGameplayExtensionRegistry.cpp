// TestGameplayExtensionRegistry.cpp - Tests for GameplayExtensionRegistry
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <memory>
#include <string>
#include <vector>

namespace TestGameplayExtension
{

    // Minimal local types matching engine interfaces
    struct QuestDefinition
    {
        uint32_t questId = 0;
        std::string name;
        std::string typeName;
        std::vector<std::string> objectiveIds;
    };

    struct QuestContext
    {
        uint32_t entityId = 0;
        uint32_t questId = 0;
        bool isActive = false;
        bool isComplete = false;
        std::vector<int32_t> objectiveProgress;
    };

    class IQuestTypeExtension
    {
      public:
        virtual ~IQuestTypeExtension() = default;
        virtual std::string GetTypeName() const = 0;
        virtual bool CanStart(const QuestDefinition& quest, const QuestContext& ctx) const = 0;
        virtual void OnStarted(const QuestDefinition& quest, QuestContext& ctx) = 0;
        virtual void OnObjectiveProgress(const QuestDefinition& quest, QuestContext& ctx,
                                         const std::string& objectiveId, int32_t progress) = 0;
        virtual bool IsComplete(const QuestDefinition& quest, const QuestContext& ctx) const = 0;
        virtual void OnCompleted(const QuestDefinition& quest, QuestContext& ctx) = 0;
    };

    class IDialogueExtension
    {
      public:
        virtual ~IDialogueExtension() = default;
        virtual std::string GetExtensionName() const = 0;
        virtual bool EvaluateCondition(const std::string& condType, const std::string& params) const = 0;
        virtual void ExecuteAction(const std::string& actionType, const std::string& params) = 0;
    };

    class GameplayExtensionRegistry
    {
      public:
        void RegisterQuestExtension(std::unique_ptr<IQuestTypeExtension> ext)
        {
            if (ext)
                m_questExtensions.push_back(std::move(ext));
        }

        void RegisterDialogueExtension(std::unique_ptr<IDialogueExtension> ext)
        {
            if (ext)
                m_dialogueExtensions.push_back(std::move(ext));
        }

        IQuestTypeExtension* GetQuestExtension(const std::string& typeName) const
        {
            for (const auto& ext : m_questExtensions)
            {
                if (ext->GetTypeName() == typeName)
                    return ext.get();
            }
            return nullptr;
        }

        IDialogueExtension* GetDialogueExtension(const std::string& name) const
        {
            for (const auto& ext : m_dialogueExtensions)
            {
                if (ext->GetExtensionName() == name)
                    return ext.get();
            }
            return nullptr;
        }

        std::vector<std::string> GetRegisteredQuestTypes() const
        {
            std::vector<std::string> types;
            for (const auto& ext : m_questExtensions)
                types.push_back(ext->GetTypeName());
            return types;
        }

        std::vector<std::string> GetRegisteredDialogueExtensions() const
        {
            std::vector<std::string> names;
            for (const auto& ext : m_dialogueExtensions)
                names.push_back(ext->GetExtensionName());
            return names;
        }

        void Clear()
        {
            m_questExtensions.clear();
            m_dialogueExtensions.clear();
        }

      private:
        std::vector<std::unique_ptr<IQuestTypeExtension>> m_questExtensions;
        std::vector<std::unique_ptr<IDialogueExtension>> m_dialogueExtensions;
    };

    // Test quest extension
    class EscortQuestExtension : public IQuestTypeExtension
    {
      public:
        std::string GetTypeName() const override { return "escort"; }

        bool CanStart(const QuestDefinition& quest, const QuestContext& ctx) const override { return !ctx.isActive; }

        void OnStarted(const QuestDefinition& quest, QuestContext& ctx) override { ctx.isActive = true; }

        void OnObjectiveProgress(const QuestDefinition& quest, QuestContext& ctx, const std::string& objectiveId,
                                 int32_t progress) override
        {
            if (!ctx.objectiveProgress.empty())
            {
                ctx.objectiveProgress[0] += progress;
            }
        }

        bool IsComplete(const QuestDefinition& quest, const QuestContext& ctx) const override { return ctx.isComplete; }

        void OnCompleted(const QuestDefinition& quest, QuestContext& ctx) override { ctx.isComplete = true; }
    };

    class CraftingQuestExtension : public IQuestTypeExtension
    {
      public:
        std::string GetTypeName() const override { return "crafting"; }
        bool CanStart(const QuestDefinition&, const QuestContext&) const override { return true; }
        void OnStarted(const QuestDefinition&, QuestContext& ctx) override { ctx.isActive = true; }
        void OnObjectiveProgress(const QuestDefinition&, QuestContext&, const std::string&, int32_t) override {}
        bool IsComplete(const QuestDefinition&, const QuestContext& ctx) const override { return ctx.isComplete; }
        void OnCompleted(const QuestDefinition&, QuestContext& ctx) override { ctx.isComplete = true; }
    };

    // Test dialogue extension
    class RPGDialogueExtension : public IDialogueExtension
    {
      public:
        std::string GetExtensionName() const override { return "rpg_conditions"; }

        bool EvaluateCondition(const std::string& condType, const std::string& params) const override
        {
            if (condType == "has_item")
                return true;
            return false;
        }

        void ExecuteAction(const std::string& actionType, const std::string& params) override
        {
            m_lastAction = actionType;
        }

        mutable std::string m_lastAction;
    };

} // namespace TestGameplayExtension

TEST(GameplayExtRegistry_QuestRegistrationAndLookup)
{
    using namespace TestGameplayExtension;
    GameplayExtensionRegistry registry;

    registry.RegisterQuestExtension(std::make_unique<EscortQuestExtension>());

    auto* ext = registry.GetQuestExtension("escort");
    EXPECT_TRUE(ext != nullptr);
    EXPECT_EQ(ext->GetTypeName(), std::string("escort"));

    auto* missing = registry.GetQuestExtension("nonexistent");
    EXPECT_TRUE(missing == nullptr);
}

TEST(GameplayExtRegistry_DialogueRegistration)
{
    using namespace TestGameplayExtension;
    GameplayExtensionRegistry registry;

    registry.RegisterDialogueExtension(std::make_unique<RPGDialogueExtension>());

    auto* ext = registry.GetDialogueExtension("rpg_conditions");
    EXPECT_TRUE(ext != nullptr);
    EXPECT_EQ(ext->GetExtensionName(), std::string("rpg_conditions"));

    EXPECT_TRUE(ext->EvaluateCondition("has_item", "sword"));
    EXPECT_FALSE(ext->EvaluateCondition("unknown_cond", ""));
}

TEST(GameplayExtRegistry_ClearForHotReload)
{
    using namespace TestGameplayExtension;
    GameplayExtensionRegistry registry;

    registry.RegisterQuestExtension(std::make_unique<EscortQuestExtension>());
    registry.RegisterDialogueExtension(std::make_unique<RPGDialogueExtension>());

    EXPECT_TRUE(registry.GetQuestExtension("escort") != nullptr);
    EXPECT_TRUE(registry.GetDialogueExtension("rpg_conditions") != nullptr);

    registry.Clear();

    EXPECT_TRUE(registry.GetQuestExtension("escort") == nullptr);
    EXPECT_TRUE(registry.GetDialogueExtension("rpg_conditions") == nullptr);
}

TEST(GameplayExtRegistry_GetRegisteredQuestTypes)
{
    using namespace TestGameplayExtension;
    GameplayExtensionRegistry registry;

    registry.RegisterQuestExtension(std::make_unique<EscortQuestExtension>());
    registry.RegisterQuestExtension(std::make_unique<CraftingQuestExtension>());

    auto types = registry.GetRegisteredQuestTypes();
    EXPECT_EQ(types.size(), size_t(2));
    EXPECT_EQ(types[0], std::string("escort"));
    EXPECT_EQ(types[1], std::string("crafting"));
}

TEST(GameplayExtRegistry_GetRegisteredDialogueExtensions)
{
    using namespace TestGameplayExtension;
    GameplayExtensionRegistry registry;

    auto names = registry.GetRegisteredDialogueExtensions();
    EXPECT_EQ(names.size(), size_t(0));

    registry.RegisterDialogueExtension(std::make_unique<RPGDialogueExtension>());
    names = registry.GetRegisteredDialogueExtensions();
    EXPECT_EQ(names.size(), size_t(1));
}

TEST(GameplayExtRegistry_ExtensionLifecycle)
{
    using namespace TestGameplayExtension;
    GameplayExtensionRegistry registry;

    registry.RegisterQuestExtension(std::make_unique<EscortQuestExtension>());
    auto* ext = registry.GetQuestExtension("escort");
    EXPECT_TRUE(ext != nullptr);

    QuestDefinition quest;
    quest.questId = 1;
    quest.name = "Escort the Merchant";
    quest.typeName = "escort";

    QuestContext ctx;
    ctx.entityId = 42;
    ctx.objectiveProgress = {0};

    EXPECT_TRUE(ext->CanStart(quest, ctx));
    ext->OnStarted(quest, ctx);
    EXPECT_TRUE(ctx.isActive);

    ext->OnObjectiveProgress(quest, ctx, "reach_dest", 1);
    EXPECT_EQ(ctx.objectiveProgress[0], 1);

    ext->OnCompleted(quest, ctx);
    EXPECT_TRUE(ctx.isComplete);
}

TEST(GameplayExtRegistry_NullExtensionIgnored)
{
    using namespace TestGameplayExtension;
    GameplayExtensionRegistry registry;

    registry.RegisterQuestExtension(nullptr);
    registry.RegisterDialogueExtension(nullptr);

    auto types = registry.GetRegisteredQuestTypes();
    EXPECT_EQ(types.size(), size_t(0));
}
