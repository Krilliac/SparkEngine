/**
 * @file TestFPSComponentsReal.cpp
 * @brief Production-source tests for the shipped FPS gameplay components
 *
 * Exercises the real DecalComponent, ProjectileComponent, and InteractionComponent
 * from FPSComponents.h. (RDY-010 retired the test-local TestFPSComponents.cpp
 * mirror these tests replaced.)
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

    // The fade scales the tint alpha, not a fixed 1.0.
    decal.color.w = 0.5f;
    decal.remainingLifetime = 1.0f;
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

    projectile.distanceTraveled = projectile.maxRange * 0.5f;
    projectile.age = projectile.maxLifetime * 0.5f;
    EXPECT_FALSE(projectile.IsExpired());

    projectile.distanceTraveled = projectile.maxRange;
    EXPECT_TRUE(projectile.IsExpired());

    // Both limits are inclusive: a projectile exactly at maxLifetime is expired.
    ProjectileComponent aged;
    aged.age = aged.maxLifetime;
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
    EXPECT_NEAR(projectile.speed, 100.0f, 0.0001f);
    EXPECT_NEAR(projectile.damage, 25.0f, 0.0001f);
    EXPECT_NEAR(projectile.maxRange, 500.0f, 0.0001f);
    EXPECT_NEAR(projectile.maxLifetime, 10.0f, 0.0001f);
    EXPECT_NEAR(projectile.gravityScale, 1.0f, 0.0001f);
    EXPECT_NEAR(projectile.explosionRadius, 0.0f, 0.0001f);
    EXPECT_NEAR(projectile.distanceTraveled, 0.0f, 0.0001f);
    EXPECT_EQ(projectile.bouncesRemaining, 0);
    EXPECT_EQ(projectile.piercesRemaining, 0);
    EXPECT_NEAR(projectile.direction.z, 1.0f, 0.0001f);
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
    interaction.state = InteractionComponent::State::Destroyed;
    EXPECT_FALSE(interaction.CanInteract());
    interaction.state = InteractionComponent::State::Active;
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
    EXPECT_TRUE(interaction.state == InteractionComponent::State::Idle);
    EXPECT_TRUE(interaction.showHighlight);
    EXPECT_NEAR(interaction.holdDuration, 0.0f, 0.0001f);
    EXPECT_NEAR(interaction.cooldownDuration, 0.0f, 0.0001f);
}
