/**
 * @file TestGameplayTagsReal.cpp
 * @brief Real-class tests for Spark::Gameplay::GameplayTagRegistry and GameplayTagContainer
 */

#include "TestFramework.h"
#include "Engine/Gameplay/GameplayTags.h"

namespace
{
    void ResetRegistry()
    {
        auto& reg = Spark::Gameplay::GameplayTagRegistry::GetInstance();
        reg.Shutdown();
        reg.Initialize();
    }
} // namespace

TEST(GameplayTagsReal_SingletonStable)
{
    auto& a = Spark::Gameplay::GameplayTagRegistry::GetInstance();
    auto& b = Spark::Gameplay::GameplayTagRegistry::GetInstance();
    EXPECT_TRUE(&a == &b);
}

TEST(GameplayTagsReal_RegisterAndGetId)
{
    ResetRegistry();
    auto& reg = Spark::Gameplay::GameplayTagRegistry::GetInstance();
    const auto id = reg.RegisterTag(std::string("Damage.Fire"));
    EXPECT_TRUE(id != Spark::Gameplay::INVALID_TAG_ID);

    const auto lookupId = reg.GetTagId(std::string("Damage.Fire"));
    EXPECT_EQ(lookupId, id);
}

TEST(GameplayTagsReal_GetUnknownReturnsInvalid)
{
    ResetRegistry();
    auto& reg = Spark::Gameplay::GameplayTagRegistry::GetInstance();
    const auto id = reg.GetTagId(std::string("Nonexistent.Tag.PhaseNN"));
    EXPECT_EQ(id, Spark::Gameplay::INVALID_TAG_ID);
}

TEST(GameplayTagsReal_ParentTagImplicit)
{
    ResetRegistry();
    auto& reg = Spark::Gameplay::GameplayTagRegistry::GetInstance();
    // Registering a deep tag should implicitly register parents.
    reg.RegisterTag(std::string("Damage.Fire.DoT"));
    const auto parentId = reg.GetTagId(std::string("Damage.Fire"));
    EXPECT_TRUE(parentId != Spark::Gameplay::INVALID_TAG_ID);

    const auto rootId = reg.GetTagId(std::string("Damage"));
    EXPECT_TRUE(rootId != Spark::Gameplay::INVALID_TAG_ID);
}

TEST(GameplayTagsReal_ContainerAddRemoveHas)
{
    ResetRegistry();
    auto& reg = Spark::Gameplay::GameplayTagRegistry::GetInstance();
    const auto fireId = reg.RegisterTag(std::string("Element.Fire"));
    const auto iceId = reg.RegisterTag(std::string("Element.Ice"));

    Spark::Gameplay::GameplayTagContainer container;
    container.AddTag(fireId);
    EXPECT_TRUE(container.HasTag(fireId));
    EXPECT_FALSE(container.HasTag(iceId));

    container.AddTag(iceId);
    EXPECT_TRUE(container.HasTag(iceId));

    container.RemoveTag(fireId);
    EXPECT_FALSE(container.HasTag(fireId));
    EXPECT_TRUE(container.HasTag(iceId));
}

TEST(GameplayTagsReal_RegisterSameTagReturnsSameId)
{
    ResetRegistry();
    auto& reg = Spark::Gameplay::GameplayTagRegistry::GetInstance();
    const auto id1 = reg.RegisterTag(std::string("Ability.Attack"));
    const auto id2 = reg.RegisterTag(std::string("Ability.Attack"));
    EXPECT_EQ(id1, id2);
}

TEST(GameplayTagsReal_ShutdownClearsTags)
{
    auto& reg = Spark::Gameplay::GameplayTagRegistry::GetInstance();
    reg.Initialize();
    reg.RegisterTag(std::string("Damage.Test_PhaseNN_ShutdownClear"));
    reg.Shutdown();
    reg.Initialize();
    const auto id = reg.GetTagId(std::string("Damage.Test_PhaseNN_ShutdownClear"));
    EXPECT_EQ(id, Spark::Gameplay::INVALID_TAG_ID);
}
