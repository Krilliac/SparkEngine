// TestGameplayTags.cpp - Tests for Spark::Gameplay::GameplayTagRegistry
#include "TestFramework.h"
#include "Engine/Gameplay/GameplayTags.h"

// ============================================================================
// Registration
// ============================================================================

TEST(GameplayTags_RegisterTag)
{
    auto& reg = Spark::Gameplay::GameplayTagRegistry::GetInstance();
    reg.Initialize();

    auto id = reg.RegisterTag("Damage.Fire");
    EXPECT_TRUE(id != Spark::Gameplay::INVALID_TAG_ID);
    EXPECT_EQ(reg.GetTagCount(), static_cast<size_t>(2)); // "Damage" + "Damage.Fire"
    reg.Shutdown();
}

TEST(GameplayTags_RegisterTag_CreatesParents)
{
    auto& reg = Spark::Gameplay::GameplayTagRegistry::GetInstance();
    reg.Initialize();

    reg.RegisterTag("Damage.Fire.DoT");
    // Should create: "Damage", "Damage.Fire", "Damage.Fire.DoT"
    EXPECT_EQ(reg.GetTagCount(), static_cast<size_t>(3));
    EXPECT_TRUE(reg.GetTagId("Damage") != Spark::Gameplay::INVALID_TAG_ID);
    EXPECT_TRUE(reg.GetTagId("Damage.Fire") != Spark::Gameplay::INVALID_TAG_ID);
    EXPECT_TRUE(reg.GetTagId("Damage.Fire.DoT") != Spark::Gameplay::INVALID_TAG_ID);
    reg.Shutdown();
}

TEST(GameplayTags_RegisterDuplicate_ReturnsSameId)
{
    auto& reg = Spark::Gameplay::GameplayTagRegistry::GetInstance();
    reg.Initialize();

    auto id1 = reg.RegisterTag("Status.Buff");
    auto id2 = reg.RegisterTag("Status.Buff");
    EXPECT_EQ(id1, id2);
    reg.Shutdown();
}

TEST(GameplayTags_RegisterEmpty_ReturnsInvalid)
{
    auto& reg = Spark::Gameplay::GameplayTagRegistry::GetInstance();
    reg.Initialize();
    EXPECT_EQ(reg.RegisterTag(""), Spark::Gameplay::INVALID_TAG_ID);
    reg.Shutdown();
}

// ============================================================================
// Lookup
// ============================================================================

TEST(GameplayTags_GetTagId_Unknown)
{
    auto& reg = Spark::Gameplay::GameplayTagRegistry::GetInstance();
    reg.Initialize();
    EXPECT_EQ(reg.GetTagId("NonExistent"), Spark::Gameplay::INVALID_TAG_ID);
    reg.Shutdown();
}

TEST(GameplayTags_GetTagInfo)
{
    auto& reg = Spark::Gameplay::GameplayTagRegistry::GetInstance();
    reg.Initialize();

    reg.RegisterTag("Ability.Ultimate");
    auto id = reg.GetTagId("Ability.Ultimate");
    const auto* info = reg.GetTagInfo(id);
    EXPECT_TRUE(info != nullptr);
    EXPECT_EQ(info->fullName, std::string("Ability.Ultimate"));
    reg.Shutdown();
}

// ============================================================================
// Hierarchy
// ============================================================================

TEST(GameplayTags_IsTagChildOf)
{
    auto& reg = Spark::Gameplay::GameplayTagRegistry::GetInstance();
    reg.Initialize();

    reg.RegisterTag("Damage.Fire.DoT");

    auto dotId = reg.GetTagId("Damage.Fire.DoT");
    auto fireId = reg.GetTagId("Damage.Fire");
    auto damageId = reg.GetTagId("Damage");

    EXPECT_TRUE(reg.IsTagChildOf(dotId, damageId));
    EXPECT_TRUE(reg.IsTagChildOf(dotId, fireId));
    EXPECT_TRUE(reg.IsTagChildOf(dotId, dotId));     // Self
    EXPECT_FALSE(reg.IsTagChildOf(damageId, dotId)); // Parent is not child
    reg.Shutdown();
}

// ============================================================================
// Container
// ============================================================================

TEST(GameplayTags_Container_AddAndHas)
{
    auto& reg = Spark::Gameplay::GameplayTagRegistry::GetInstance();
    reg.Initialize();

    auto id = reg.RegisterTag("Status.Immune");
    Spark::Gameplay::GameplayTagContainer container;
    container.AddTag(id);

    EXPECT_TRUE(container.HasTag(id));
    EXPECT_EQ(container.Count(), static_cast<size_t>(1));
    reg.Shutdown();
}

TEST(GameplayTags_Container_HasAny)
{
    auto& reg = Spark::Gameplay::GameplayTagRegistry::GetInstance();
    reg.Initialize();

    auto fireId = reg.RegisterTag("Damage.Fire");
    auto iceId = reg.RegisterTag("Damage.Ice");
    auto healId = reg.RegisterTag("Effect.Heal");

    Spark::Gameplay::GameplayTagContainer entity;
    entity.AddTag(fireId);

    Spark::Gameplay::GameplayTagContainer query;
    query.AddTag(iceId);
    query.AddTag(fireId);

    EXPECT_TRUE(entity.HasAny(query));

    Spark::Gameplay::GameplayTagContainer noMatch;
    noMatch.AddTag(healId);
    EXPECT_FALSE(entity.HasAny(noMatch));
    reg.Shutdown();
}

TEST(GameplayTags_Container_HasAll)
{
    auto& reg = Spark::Gameplay::GameplayTagRegistry::GetInstance();
    reg.Initialize();

    auto a = reg.RegisterTag("Tag.A");
    auto b = reg.RegisterTag("Tag.B");
    auto c = reg.RegisterTag("Tag.C");

    Spark::Gameplay::GameplayTagContainer entity;
    entity.AddTag(a);
    entity.AddTag(b);
    entity.AddTag(c);

    Spark::Gameplay::GameplayTagContainer required;
    required.AddTag(a);
    required.AddTag(b);
    EXPECT_TRUE(entity.HasAll(required));

    auto d = reg.RegisterTag("Tag.D");
    required.AddTag(d);
    EXPECT_FALSE(entity.HasAll(required));
    reg.Shutdown();
}

TEST(GameplayTags_Container_Remove)
{
    Spark::Gameplay::GameplayTagContainer container;
    container.AddTag(1);
    container.AddTag(2);
    EXPECT_EQ(container.Count(), static_cast<size_t>(2));
    container.RemoveTag(1);
    EXPECT_FALSE(container.HasTag(1));
    EXPECT_EQ(container.Count(), static_cast<size_t>(1));
}

TEST(GameplayTags_ContainerMatchesTag_Hierarchical)
{
    auto& reg = Spark::Gameplay::GameplayTagRegistry::GetInstance();
    reg.Initialize();

    reg.RegisterTag("Damage.Fire.DoT");
    auto dotId = reg.GetTagId("Damage.Fire.DoT");

    Spark::Gameplay::GameplayTagContainer container;
    container.AddTag(dotId);

    // Should match parent tags
    EXPECT_TRUE(reg.ContainerMatchesTag(container, "Damage"));
    EXPECT_TRUE(reg.ContainerMatchesTag(container, "Damage.Fire"));
    EXPECT_TRUE(reg.ContainerMatchesTag(container, "Damage.Fire.DoT"));
    EXPECT_FALSE(reg.ContainerMatchesTag(container, "Status"));
    reg.Shutdown();
}

TEST(GameplayTags_GetAllTagNames)
{
    auto& reg = Spark::Gameplay::GameplayTagRegistry::GetInstance();
    reg.Initialize();

    reg.RegisterTag("B.Child");
    reg.RegisterTag("A.Child");

    auto names = reg.GetAllTagNames();
    EXPECT_EQ(names.size(), static_cast<size_t>(4)); // A, A.Child, B, B.Child
    // Should be sorted alphabetically
    EXPECT_TRUE(names[0] <= names[1]);
    reg.Shutdown();
}
