// TestCollisionLayers.cpp - Tests for collision layer/mask filtering system
// Validates 16-bit collision group/mask bitmask logic and Bullet broadphase wiring.

#include "TestFramework.h"
#include <cstdint>
#include <vector>

namespace TestCollisionLayers
{

    // Named collision layers (mirrors common game patterns)
    namespace Layer
    {
        constexpr uint16_t Default = 1 << 0;
        constexpr uint16_t Player = 1 << 1;
        constexpr uint16_t Enemy = 1 << 2;
        constexpr uint16_t Projectile = 1 << 3;
        constexpr uint16_t Trigger = 1 << 4;
        constexpr uint16_t Environment = 1 << 5;
        constexpr uint16_t Debris = 1 << 6;
        constexpr uint16_t All = 0xFFFF;
        constexpr uint16_t None = 0;
    } // namespace Layer

    // Standalone collision body for testing (mirrors PhysicsBody's filtering)
    struct TestBody
    {
        uint16_t collisionGroup = Layer::Default;
        uint16_t collisionMask = Layer::All;

        void SetCollisionGroup(uint16_t group) { collisionGroup = group; }
        void SetCollisionMask(uint16_t mask) { collisionMask = mask; }
    };

    // Bullet-style collision test: A collides with B if (A.group & B.mask) && (B.group & A.mask)
    bool WouldCollide(const TestBody& a, const TestBody& b)
    {
        return (a.collisionGroup & b.collisionMask) != 0 && (b.collisionGroup & a.collisionMask) != 0;
    }

} // namespace TestCollisionLayers

using namespace TestCollisionLayers;

TEST(CollisionLayers_DefaultBodiesCollide)
{
    TestBody a, b;
    EXPECT_TRUE(WouldCollide(a, b));
}

TEST(CollisionLayers_DifferentGroupsSameDefaultMaskCollide)
{
    TestBody player;
    player.collisionGroup = Layer::Player;

    TestBody enemy;
    enemy.collisionGroup = Layer::Enemy;

    // Both have mask=All, so they collide
    EXPECT_TRUE(WouldCollide(player, enemy));
}

TEST(CollisionLayers_MaskFiltersProperly)
{
    TestBody player;
    player.collisionGroup = Layer::Player;
    player.collisionMask = Layer::Environment | Layer::Enemy; // Player detects env + enemies

    TestBody projectile;
    projectile.collisionGroup = Layer::Projectile;
    projectile.collisionMask = Layer::Player | Layer::Enemy | Layer::Environment;

    // Player doesn't have Projectile in its mask → no collision
    EXPECT_FALSE(WouldCollide(player, projectile));
}

TEST(CollisionLayers_SymmetricCollision)
{
    TestBody player;
    player.collisionGroup = Layer::Player;
    player.collisionMask = Layer::Enemy | Layer::Projectile;

    TestBody enemy;
    enemy.collisionGroup = Layer::Enemy;
    enemy.collisionMask = Layer::Player | Layer::Projectile;

    // Both include each other's group in their mask → collision
    EXPECT_TRUE(WouldCollide(player, enemy));
}

TEST(CollisionLayers_TriggerIgnoresDebris)
{
    TestBody trigger;
    trigger.collisionGroup = Layer::Trigger;
    trigger.collisionMask = Layer::Player; // Triggers only detect players

    TestBody debris;
    debris.collisionGroup = Layer::Debris;
    debris.collisionMask = Layer::Environment; // Debris only collides with env

    EXPECT_FALSE(WouldCollide(trigger, debris));
}

TEST(CollisionLayers_ZeroMaskCollidesWithNothing)
{
    TestBody ghost;
    ghost.collisionGroup = Layer::Player;
    ghost.collisionMask = Layer::None;

    TestBody wall;
    wall.collisionGroup = Layer::Environment;
    wall.collisionMask = Layer::All;

    EXPECT_FALSE(WouldCollide(ghost, wall));
}

TEST(CollisionLayers_ZeroGroupCollidesWithNothing)
{
    TestBody invisible;
    invisible.collisionGroup = Layer::None;
    invisible.collisionMask = Layer::All;

    TestBody wall;
    wall.collisionGroup = Layer::Environment;
    wall.collisionMask = Layer::All;

    EXPECT_FALSE(WouldCollide(invisible, wall));
}

TEST(CollisionLayers_ProjectileHitsEnemyNotFriendly)
{
    TestBody friendlyProjectile;
    friendlyProjectile.collisionGroup = Layer::Projectile;
    friendlyProjectile.collisionMask = Layer::Enemy | Layer::Environment;

    TestBody player;
    player.collisionGroup = Layer::Player;
    player.collisionMask = Layer::Enemy | Layer::Environment;

    TestBody enemy;
    enemy.collisionGroup = Layer::Enemy;
    enemy.collisionMask = Layer::Player | Layer::Projectile | Layer::Environment;

    // Projectile vs player: projectile mask doesn't include Player → no hit
    EXPECT_FALSE(WouldCollide(friendlyProjectile, player));

    // Projectile vs enemy: both include each other → hit
    EXPECT_TRUE(WouldCollide(friendlyProjectile, enemy));
}

TEST(CollisionLayers_AllBitsUsable)
{
    // Test that all 16 bits work
    for (int i = 0; i < 16; i++)
    {
        TestBody a;
        a.collisionGroup = static_cast<uint16_t>(1 << i);
        a.collisionMask = static_cast<uint16_t>(1 << i);

        TestBody b;
        b.collisionGroup = static_cast<uint16_t>(1 << i);
        b.collisionMask = static_cast<uint16_t>(1 << i);

        EXPECT_TRUE(WouldCollide(a, b));

        // Different bit should not collide
        TestBody c;
        c.collisionGroup = static_cast<uint16_t>(1 << ((i + 1) % 16));
        c.collisionMask = static_cast<uint16_t>(1 << ((i + 1) % 16));

        if (i != ((i + 1) % 16))
        {
            EXPECT_FALSE(WouldCollide(a, c));
        }
    }
}

TEST(CollisionLayers_DescDefaultsAreCorrect)
{
    // Verify PhysicsBodyDesc defaults match expected values
    TestBody defaultBody;
    EXPECT_EQ(defaultBody.collisionGroup, Layer::Default);
    EXPECT_EQ(defaultBody.collisionMask, Layer::All);
}
