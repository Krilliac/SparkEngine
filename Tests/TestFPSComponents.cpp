// TestFPSComponents.cpp - Tests for FPS gameplay components
// DecalComponent, ProjectileComponent, InteractionComponent

#include "TestFramework.h"

#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>

// ============================================================================
// Standalone DecalComponent test (doesn't need EnTT or DirectXMath)
// ============================================================================

struct TestDecalComponent
{
    std::string texturePath;
    std::string category = "generic";
    float sizeX = 0.1f, sizeY = 0.1f, sizeZ = 0.05f;
    float colorR = 1.0f, colorG = 1.0f, colorB = 1.0f, colorA = 1.0f;
    float lifetime = 30.0f;
    float remainingLifetime = 30.0f;
    float fadeOutDuration = 2.0f;
    bool receiveLighting = true;
    int sortOrder = 0;

    float GetCurrentOpacity() const
    {
        if (lifetime <= 0.0f)
            return colorA;
        if (remainingLifetime <= 0.0f)
            return 0.0f;
        if (fadeOutDuration > 0.0f && remainingLifetime < fadeOutDuration)
            return colorA * (remainingLifetime / fadeOutDuration);
        return colorA;
    }
};

TEST(DecalComponent_InitialState)
{
    TestDecalComponent decal;
    EXPECT_EQ(decal.category, std::string("generic"));
    EXPECT_NEAR(decal.lifetime, 30.0f, 0.001f);
    EXPECT_NEAR(decal.remainingLifetime, 30.0f, 0.001f);
    EXPECT_NEAR(decal.fadeOutDuration, 2.0f, 0.001f);
    EXPECT_TRUE(decal.receiveLighting);
    EXPECT_EQ(decal.sortOrder, 0);
}

TEST(DecalComponent_FullOpacity)
{
    TestDecalComponent decal;
    decal.remainingLifetime = 15.0f;
    EXPECT_NEAR(decal.GetCurrentOpacity(), 1.0f, 0.001f);
}

TEST(DecalComponent_FadeOut)
{
    TestDecalComponent decal;
    decal.remainingLifetime = 1.0f; // Within the 2s fade-out window
    float expected = 1.0f * (1.0f / 2.0f);
    EXPECT_NEAR(decal.GetCurrentOpacity(), expected, 0.001f);
}

TEST(DecalComponent_Expired)
{
    TestDecalComponent decal;
    decal.remainingLifetime = 0.0f;
    EXPECT_NEAR(decal.GetCurrentOpacity(), 0.0f, 0.001f);
}

TEST(DecalComponent_Permanent)
{
    TestDecalComponent decal;
    decal.lifetime = 0.0f; // Permanent
    decal.remainingLifetime = 0.0f;
    EXPECT_NEAR(decal.GetCurrentOpacity(), 1.0f, 0.001f);
}

TEST(DecalComponent_CustomAlphaFade)
{
    TestDecalComponent decal;
    decal.colorA = 0.5f;
    decal.remainingLifetime = 1.0f;
    float expected = 0.5f * (1.0f / 2.0f);
    EXPECT_NEAR(decal.GetCurrentOpacity(), expected, 0.001f);
}

// ============================================================================
// Standalone ProjectileComponent test
// ============================================================================

struct TestProjectileComponent
{
    enum class MovementType
    {
        Hitscan,
        Ballistic
    };

    enum class ImpactBehavior
    {
        Destroy,
        Bounce,
        Pierce,
        Stick
    };

    MovementType movementType = MovementType::Ballistic;
    ImpactBehavior impactBehavior = ImpactBehavior::Destroy;
    float dirX = 0.0f, dirY = 0.0f, dirZ = 1.0f;
    float speed = 100.0f;
    float gravityScale = 1.0f;
    float damage = 25.0f;
    float explosionRadius = 0.0f;
    float maxRange = 500.0f;
    float distanceTraveled = 0.0f;
    float maxLifetime = 10.0f;
    float age = 0.0f;
    int bouncesRemaining = 0;
    int piercesRemaining = 0;
    uint32_t ownerEntityId = 0;
    int teamId = -1;
    std::string impactEffectName;
    std::string impactDecalPath;
    std::string trailEffectName;

    bool IsExpired() const { return distanceTraveled >= maxRange || age >= maxLifetime; }
};

TEST(ProjectileComponent_InitialState)
{
    TestProjectileComponent proj;
    EXPECT_NEAR(proj.speed, 100.0f, 0.001f);
    EXPECT_NEAR(proj.damage, 25.0f, 0.001f);
    EXPECT_NEAR(proj.maxRange, 500.0f, 0.001f);
    EXPECT_NEAR(proj.distanceTraveled, 0.0f, 0.001f);
    EXPECT_FALSE(proj.IsExpired());
}

TEST(ProjectileComponent_ExpiredByRange)
{
    TestProjectileComponent proj;
    proj.distanceTraveled = 500.0f;
    EXPECT_TRUE(proj.IsExpired());
}

TEST(ProjectileComponent_ExpiredByLifetime)
{
    TestProjectileComponent proj;
    proj.age = 10.0f;
    EXPECT_TRUE(proj.IsExpired());
}

TEST(ProjectileComponent_NotExpired)
{
    TestProjectileComponent proj;
    proj.distanceTraveled = 250.0f;
    proj.age = 5.0f;
    EXPECT_FALSE(proj.IsExpired());
}

TEST(ProjectileComponent_BounceBehavior)
{
    TestProjectileComponent proj;
    proj.impactBehavior = TestProjectileComponent::ImpactBehavior::Bounce;
    proj.bouncesRemaining = 3;
    EXPECT_EQ(proj.bouncesRemaining, 3);
    proj.bouncesRemaining--;
    EXPECT_EQ(proj.bouncesRemaining, 2);
}

TEST(ProjectileComponent_PierceBehavior)
{
    TestProjectileComponent proj;
    proj.impactBehavior = TestProjectileComponent::ImpactBehavior::Pierce;
    proj.piercesRemaining = 2;
    proj.piercesRemaining--;
    EXPECT_EQ(proj.piercesRemaining, 1);
}

TEST(ProjectileComponent_HitscanType)
{
    TestProjectileComponent proj;
    proj.movementType = TestProjectileComponent::MovementType::Hitscan;
    EXPECT_TRUE(proj.movementType == TestProjectileComponent::MovementType::Hitscan);
}

TEST(ProjectileComponent_SplashDamage)
{
    TestProjectileComponent proj;
    proj.explosionRadius = 5.0f;
    EXPECT_TRUE(proj.explosionRadius > 0.0f);
}

// ============================================================================
// Standalone InteractionComponent test
// ============================================================================

struct TestInteractionComponent
{
    enum class InteractionType
    {
        Use,
        Pickup,
        Hold,
        Toggle
    };

    enum class State
    {
        Idle,
        Active,
        Cooldown,
        Disabled,
        Destroyed
    };

    InteractionType type = InteractionType::Use;
    State state = State::Idle;
    std::string displayName;
    std::string actionVerb = "Use";
    float interactionRadius = 2.5f;
    float holdDuration = 0.0f;
    float holdProgress = 0.0f;
    float cooldownDuration = 0.0f;
    float cooldownRemaining = 0.0f;
    int usesRemaining = -1;
    bool showHighlight = true;
    std::string requiredItemId;
    std::string onInteractEvent;
    std::string interactSoundName;

    bool CanInteract() const { return state == State::Idle && (usesRemaining != 0); }

    void ConsumeUse()
    {
        if (usesRemaining > 0)
            --usesRemaining;
    }
};

TEST(InteractionComponent_InitialState)
{
    TestInteractionComponent interact;
    EXPECT_EQ(interact.actionVerb, std::string("Use"));
    EXPECT_NEAR(interact.interactionRadius, 2.5f, 0.001f);
    EXPECT_EQ(interact.usesRemaining, -1);
    EXPECT_TRUE(interact.showHighlight);
    EXPECT_TRUE(interact.CanInteract());
}

TEST(InteractionComponent_CanInteract_Idle)
{
    TestInteractionComponent interact;
    interact.state = TestInteractionComponent::State::Idle;
    EXPECT_TRUE(interact.CanInteract());
}

TEST(InteractionComponent_CannotInteract_Disabled)
{
    TestInteractionComponent interact;
    interact.state = TestInteractionComponent::State::Disabled;
    EXPECT_FALSE(interact.CanInteract());
}

TEST(InteractionComponent_CannotInteract_Cooldown)
{
    TestInteractionComponent interact;
    interact.state = TestInteractionComponent::State::Cooldown;
    EXPECT_FALSE(interact.CanInteract());
}

TEST(InteractionComponent_CannotInteract_Destroyed)
{
    TestInteractionComponent interact;
    interact.state = TestInteractionComponent::State::Destroyed;
    EXPECT_FALSE(interact.CanInteract());
}

TEST(InteractionComponent_ConsumeUse_Limited)
{
    TestInteractionComponent interact;
    interact.usesRemaining = 3;
    EXPECT_TRUE(interact.CanInteract());

    interact.ConsumeUse();
    EXPECT_EQ(interact.usesRemaining, 2);

    interact.ConsumeUse();
    interact.ConsumeUse();
    EXPECT_EQ(interact.usesRemaining, 0);
    EXPECT_FALSE(interact.CanInteract());
}

TEST(InteractionComponent_ConsumeUse_Unlimited)
{
    TestInteractionComponent interact;
    interact.usesRemaining = -1; // Unlimited
    interact.ConsumeUse();
    EXPECT_EQ(interact.usesRemaining, -1); // Should not decrement
    EXPECT_TRUE(interact.CanInteract());
}

TEST(InteractionComponent_HoldType)
{
    TestInteractionComponent interact;
    interact.type = TestInteractionComponent::InteractionType::Hold;
    interact.holdDuration = 3.0f;
    interact.holdProgress = 0.5f;
    EXPECT_NEAR(interact.holdProgress, 0.5f, 0.001f);
    EXPECT_TRUE(interact.holdProgress < interact.holdDuration);
}

TEST(InteractionComponent_ToggleType)
{
    TestInteractionComponent interact;
    interact.type = TestInteractionComponent::InteractionType::Toggle;
    EXPECT_TRUE(interact.type == TestInteractionComponent::InteractionType::Toggle);
}
