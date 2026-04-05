// TestSparkGamePlatformer.cpp - Tests for Platformer game module mechanics
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <cmath>
#include <algorithm>

namespace TestPlatformer
{

    struct PlatformerCharacter
    {
        float posX = 0.0f;
        float posY = 0.0f;
        float velX = 0.0f;
        float velY = 0.0f;

        bool onGround = true;
        bool againstWall = false;
        bool hasDoubleJump = true;
        bool doubleJumpUsed = false;
        bool groundPoundActive = false;

        float gravity = -20.0f;
        float jumpForce = 12.0f;
        float doubleJumpForce = 8.0f;
        float wallSlideSpeed = -2.0f;
        float groundPoundSpeed = -25.0f;
        float coyoteTimeWindow = 0.12f;
        float timeSinceLeftGround = 0.0f;

        void Jump()
        {
            if (onGround || timeSinceLeftGround <= coyoteTimeWindow)
            {
                velY = jumpForce;
                onGround = false;
                timeSinceLeftGround = coyoteTimeWindow + 1.0f; // Consume coyote time
                doubleJumpUsed = false;
            }
            else if (hasDoubleJump && !doubleJumpUsed)
            {
                velY = doubleJumpForce;
                doubleJumpUsed = true;
            }
        }

        void ActivateGroundPound()
        {
            if (!onGround)
            {
                groundPoundActive = true;
                velY = groundPoundSpeed;
                velX = 0.0f;
            }
        }

        void Update(float dt)
        {
            if (groundPoundActive)
            {
                // Ground pound overrides normal gravity
                posY += velY * dt;
                if (posY <= 0.0f)
                {
                    posY = 0.0f;
                    velY = 0.0f;
                    onGround = true;
                    groundPoundActive = false;
                }
                return;
            }

            if (againstWall && !onGround && velY < wallSlideSpeed)
            {
                velY = wallSlideSpeed;
            }
            else if (!onGround)
            {
                velY += gravity * dt;
            }

            posX += velX * dt;
            posY += velY * dt;

            if (posY <= 0.0f)
            {
                posY = 0.0f;
                velY = 0.0f;
                onGround = true;
                doubleJumpUsed = false;
                timeSinceLeftGround = 0.0f;
            }

            if (!onGround)
            {
                timeSinceLeftGround += dt;
            }
        }

        void LeaveGround()
        {
            onGround = false;
            timeSinceLeftGround = 0.0f;
        }
    };

} // namespace TestPlatformer

TEST(Platformer_JumpArc)
{
    TestPlatformer::PlatformerCharacter character;
    EXPECT_TRUE(character.onGround);

    character.Jump();
    EXPECT_FALSE(character.onGround);
    EXPECT_NEAR(character.velY, 12.0f, 0.001f);

    // Record positions to verify parabolic arc
    float prevY = character.posY;
    float peakY = 0.0f;
    bool ascending = true;

    for (int i = 0; i < 100; i++)
    {
        character.Update(0.016f);
        if (character.posY > peakY)
        {
            peakY = character.posY;
        }
        if (ascending && character.velY < 0.0f)
        {
            ascending = false;
        }
    }

    // Should have reached a peak above ground
    EXPECT_GT(peakY, 0.0f);

    // Should have come back to ground
    EXPECT_TRUE(character.onGround);
    EXPECT_NEAR(character.posY, 0.0f, 0.001f);
}

TEST(Platformer_CoyoteTime)
{
    TestPlatformer::PlatformerCharacter character;

    // Walk off edge (not jumping)
    character.LeaveGround();
    EXPECT_FALSE(character.onGround);
    EXPECT_NEAR(character.timeSinceLeftGround, 0.0f, 0.001f);

    // Advance a small amount of time within coyote window
    character.timeSinceLeftGround = 0.08f; // Within 0.12s window

    // Jump should still work during coyote time
    character.Jump();
    EXPECT_NEAR(character.velY, 12.0f, 0.001f);

    // Reset and test outside coyote window
    TestPlatformer::PlatformerCharacter character2;
    character2.LeaveGround();
    character2.timeSinceLeftGround = 0.20f; // Outside 0.12s window
    character2.doubleJumpUsed = true;       // Disable double jump for this test

    character2.Jump();
    // Should NOT get jump force (outside coyote time, double jump used)
    EXPECT_NEAR(character2.velY, 0.0f, 0.001f);
}

TEST(Platformer_WallSlide)
{
    TestPlatformer::PlatformerCharacter character;
    character.onGround = false;
    character.againstWall = true;
    character.velY = -10.0f; // Falling fast
    character.posY = 5.0f;

    character.Update(0.016f);

    // Velocity should be clamped to wall slide speed
    EXPECT_NEAR(character.velY, -2.0f, 0.001f);

    // Advance several frames: should maintain wall slide speed
    for (int i = 0; i < 10; i++)
    {
        character.velY = -10.0f; // Reset to fast fall each frame
        character.Update(0.016f);
        EXPECT_NEAR(character.velY, -2.0f, 0.001f);
    }
}

TEST(Platformer_DoubleJump)
{
    TestPlatformer::PlatformerCharacter character;

    // First jump from ground
    character.Jump();
    EXPECT_FALSE(character.onGround);
    EXPECT_NEAR(character.velY, 12.0f, 0.001f);
    EXPECT_FALSE(character.doubleJumpUsed);

    // Advance a bit so we're clearly in the air past coyote time
    character.Update(0.2f);
    EXPECT_FALSE(character.onGround);

    // Second jump in air (double jump)
    character.Jump();
    EXPECT_TRUE(character.doubleJumpUsed);
    EXPECT_NEAR(character.velY, 8.0f, 0.001f);

    // Third jump should do nothing
    float velBeforeThird = character.velY;
    character.Update(0.016f);
    float velAfterTick = character.velY;
    character.Jump();
    // Velocity should not have been set to doubleJumpForce again
    EXPECT_TRUE(character.doubleJumpUsed);
    EXPECT_NE(character.velY, 8.0f);
}

TEST(Platformer_GroundPound)
{
    TestPlatformer::PlatformerCharacter character;
    character.onGround = false;
    character.posY = 10.0f;
    character.velY = 2.0f;
    character.velX = 5.0f;

    character.ActivateGroundPound();

    EXPECT_TRUE(character.groundPoundActive);
    EXPECT_NEAR(character.velY, -25.0f, 0.001f);
    EXPECT_NEAR(character.velX, 0.0f, 0.001f);

    // Ground pound should not activate on ground
    TestPlatformer::PlatformerCharacter grounded;
    grounded.onGround = true;
    grounded.ActivateGroundPound();
    EXPECT_FALSE(grounded.groundPoundActive);
}
