// TestMaterialEffects.cpp - Tests for material effect registration and lookup
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace TestMatFX
{

    enum class InteractionType : uint8_t
    {
        Bullet,
        Footstep,
        Explosion,
        Melee,
        Count
    };

    enum class SurfaceType : uint8_t
    {
        Concrete,
        Metal,
        Wood,
        Dirt,
        Water,
        Glass,
        Count
    };

    struct MaterialEffect
    {
        std::string particleEffect;
        std::string soundEvent;
        std::string decalMaterial;
        float decalSize = 0.1f;
    };

    struct MaterialEffectSystem
    {
        // Key: (interaction << 8) | surface
        std::unordered_map<uint16_t, MaterialEffect> effects;
        bool initialized = false;

        static uint16_t MakeKey(InteractionType interaction, SurfaceType surface)
        {
            return (static_cast<uint16_t>(interaction) << 8) | static_cast<uint16_t>(surface);
        }

        bool Initialize()
        {
            initialized = true;
            return true;
        }

        void Shutdown()
        {
            effects.clear();
            initialized = false;
        }

        void RegisterEffect(InteractionType interaction, SurfaceType surface, const MaterialEffect& effect)
        {
            effects[MakeKey(interaction, surface)] = effect;
        }

        const MaterialEffect* GetEffect(InteractionType interaction, SurfaceType surface) const
        {
            auto it = effects.find(MakeKey(interaction, surface));
            return it != effects.end() ? &it->second : nullptr;
        }
    };

} // namespace TestMatFX

// ============================================================================
// Initialization
// ============================================================================

TEST(MaterialEffects_InitializeAndShutdown)
{
    TestMatFX::MaterialEffectSystem sys;
    EXPECT_TRUE(sys.Initialize());
    EXPECT_TRUE(sys.initialized);
    sys.Shutdown();
    EXPECT_FALSE(sys.initialized);
}

// ============================================================================
// Registration and Lookup
// ============================================================================

TEST(MaterialEffects_RegisterAndGetEffect)
{
    TestMatFX::MaterialEffectSystem sys;
    sys.Initialize();

    sys.RegisterEffect(TestMatFX::InteractionType::Bullet, TestMatFX::SurfaceType::Metal,
                       {"spark_burst", "impact_metal", "decal_bullet_metal", 0.05f});

    const auto* fx = sys.GetEffect(TestMatFX::InteractionType::Bullet, TestMatFX::SurfaceType::Metal);
    ASSERT_TRUE(fx != nullptr);
    EXPECT_EQ(fx->particleEffect, std::string("spark_burst"));
    EXPECT_EQ(fx->soundEvent, std::string("impact_metal"));
    EXPECT_NEAR(fx->decalSize, 0.05f, 0.001f);

    sys.Shutdown();
}

TEST(MaterialEffects_DifferentInteractionTypes)
{
    TestMatFX::MaterialEffectSystem sys;
    sys.Initialize();

    sys.RegisterEffect(TestMatFX::InteractionType::Bullet, TestMatFX::SurfaceType::Wood,
                       {"wood_chips", "impact_wood", "decal_bullet_wood", 0.04f});
    sys.RegisterEffect(TestMatFX::InteractionType::Footstep, TestMatFX::SurfaceType::Wood,
                       {"dust_puff", "footstep_wood", "", 0.0f});

    const auto* bulletFx = sys.GetEffect(TestMatFX::InteractionType::Bullet, TestMatFX::SurfaceType::Wood);
    const auto* stepFx = sys.GetEffect(TestMatFX::InteractionType::Footstep, TestMatFX::SurfaceType::Wood);
    EXPECT_TRUE(bulletFx != nullptr);
    EXPECT_TRUE(stepFx != nullptr);
    EXPECT_EQ(bulletFx->particleEffect, std::string("wood_chips"));
    EXPECT_EQ(stepFx->particleEffect, std::string("dust_puff"));

    sys.Shutdown();
}

TEST(MaterialEffects_GetEffect_NotFound)
{
    TestMatFX::MaterialEffectSystem sys;
    sys.Initialize();

    const auto* fx = sys.GetEffect(TestMatFX::InteractionType::Explosion, TestMatFX::SurfaceType::Glass);
    EXPECT_TRUE(fx == nullptr);

    sys.Shutdown();
}

TEST(MaterialEffects_DifferentSurfaceTypes)
{
    TestMatFX::MaterialEffectSystem sys;
    sys.Initialize();

    sys.RegisterEffect(TestMatFX::InteractionType::Bullet, TestMatFX::SurfaceType::Concrete,
                       {"concrete_dust", "impact_concrete", "decal_concrete", 0.06f});
    sys.RegisterEffect(TestMatFX::InteractionType::Bullet, TestMatFX::SurfaceType::Water,
                       {"water_splash", "impact_water", "", 0.0f});

    const auto* concreteFx = sys.GetEffect(TestMatFX::InteractionType::Bullet, TestMatFX::SurfaceType::Concrete);
    const auto* waterFx = sys.GetEffect(TestMatFX::InteractionType::Bullet, TestMatFX::SurfaceType::Water);
    EXPECT_TRUE(concreteFx != nullptr);
    EXPECT_TRUE(waterFx != nullptr);
    EXPECT_EQ(concreteFx->particleEffect, std::string("concrete_dust"));
    EXPECT_EQ(waterFx->particleEffect, std::string("water_splash"));

    sys.Shutdown();
}
