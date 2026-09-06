/**
 * @file TestFPSComponentsReal.cpp
 * @brief Production-source companion for TestFPSComponents.cpp
 *
 * TestFPSComponents.cpp exercises TestDecalComponent — a test-local struct that
 * mirrors DecalComponent. A regression in the shipped component is invisible to
 * it. This file includes the real header and exercises the shipped structs:
 * DecalComponent, ProjectileComponent, and InteractionComponent.
 *
 * Header-only structs — no additional production .cpp is required.
 */

#include "TestFramework.h"
#include "Engine/ECS/Components/FPSComponents.h"

TEST(FPSComponentsReal_DecalDefaultsAreShippedDefaults)
{
    DecalComponent decal;
    EXPECT_TRUE(decal.category == "generic");
    EXPECT_NEAR(decal.lifetime, 30.0f, 0.0001f);
    EXPECT_NEAR(decal.remainingLifetime, 30.0f, 0.0001f);
    EXPECT_NEAR(decal.fadeOutDuration, 2.0f, 0.0001f);
    EXPECT_TRUE(decal.receiveLighting);
    EXPECT_EQ(decal.sortOrder, 0);
    EXPECT_NEAR(decal.color.w, 1.0f, 0.0001f);
    EXPECT_NEAR(decal.surfaceNormal.y, 1.0f, 0.0001f);
}

TEST(FPSComponentsReal_DecalOpacityFullBeforeFadeWindow)
{
    DecalComponent decal;
    decal.color.w = 0.8f;
    decal.remainingLifetime = 10.0f;
    decal.fadeOutDuration = 2.0f;
    EXPECT_NEAR(decal.GetCurrentOpacity(), 0.8f, 0.0001f);
}

TEST(FPSComponentsReal_DecalOpacityScalesInsideFadeWindow)
{
    DecalComponent decal;
    decal.color.w = 1.0f;
    decal.fadeOutDuration = 2.0f;
    decal.remainingLifetime = 1.0f;
    EXPECT_NEAR(decal.GetCurrentOpacity(), 0.5f, 0.0001f);

    decal.remainingLifetime = 0.5f;
    EXPECT_NEAR(decal.GetCurrentOpacity(), 0.25f, 0.0001f);
}

TEST(FPSComponentsReal_DecalOpacityZeroWhenExpired)
{
    DecalComponent decal;
    decal.remainingLifetime = 0.0f;
    EXPECT_NEAR(decal.GetCurrentOpacity(), 0.0f, 0.0001f);
}

TEST(FPSComponentsReal_PermanentDecalIgnoresRemainingLifetime)
{
    DecalComponent decal;
    decal.lifetime = 0.0f;
    decal.color.w = 0.6f;
    decal.remainingLifetime = 0.0f;
    // lifetime == 0 means permanent, so the expired branch must not be taken.
    EXPECT_NEAR(decal.GetCurrentOpacity(), 0.6f, 0.0001f);
}

TEST(FPSComponentsReal_ProjectileExpiresOnRangeOrLifetime)
{
    ProjectileComponent projectile;
    EXPECT_FALSE(projectile.IsExpired());

    projectile.distanceTraveled = projectile.maxRange;
    EXPECT_TRUE(projectile.IsExpired());

    ProjectileComponent aged;
    aged.age = aged.maxLifetime + 1.0f;
    EXPECT_TRUE(aged.IsExpired());
}

TEST(FPSComponentsReal_ProjectileDefaultsValidate)
{
    ProjectileComponent projectile;
    EXPECT_TRUE(projectile.Validate());
    EXPECT_TRUE(projectile.movementType == ProjectileComponent::MovementType::Ballistic);
    EXPECT_TRUE(projectile.impactBehavior == ProjectileComponent::ImpactBehavior::Destroy);
    EXPECT_EQ(projectile.teamId, -1);
    EXPECT_EQ(projectile.ownerEntityId, 0u);
}

TEST(FPSComponentsReal_InteractionUnlimitedUsesStayInteractable)
{
    InteractionComponent interaction;
    EXPECT_TRUE(interaction.CanInteract());
    EXPECT_EQ(interaction.usesRemaining, -1);

    interaction.ConsumeUse();
    // -1 means unlimited: ConsumeUse must not decrement it into 0/"exhausted".
    EXPECT_EQ(interaction.usesRemaining, -1);
    EXPECT_TRUE(interaction.CanInteract());
}

TEST(FPSComponentsReal_InteractionExhaustsLimitedUses)
{
    InteractionComponent interaction;
    interaction.usesRemaining = 2;
    interaction.ConsumeUse();
    EXPECT_EQ(interaction.usesRemaining, 1);
    interaction.ConsumeUse();
    EXPECT_EQ(interaction.usesRemaining, 0);
    EXPECT_FALSE(interaction.CanInteract());
}

TEST(FPSComponentsReal_InteractionBlockedOutsideIdleState)
{
    InteractionComponent interaction;
    interaction.state = InteractionComponent::State::Cooldown;
    EXPECT_FALSE(interaction.CanInteract());
    interaction.state = InteractionComponent::State::Disabled;
    EXPECT_FALSE(interaction.CanInteract());
    interaction.state = InteractionComponent::State::Idle;
    EXPECT_TRUE(interaction.CanInteract());
}

TEST(FPSComponentsReal_InteractionDefaultsValidate)
{
    InteractionComponent interaction;
    EXPECT_TRUE(interaction.Validate());
    EXPECT_TRUE(interaction.actionVerb == "Use");
    EXPECT_TRUE(interaction.type == InteractionComponent::InteractionType::Use);
    EXPECT_NEAR(interaction.interactionRadius, 2.5f, 0.0001f);
}
