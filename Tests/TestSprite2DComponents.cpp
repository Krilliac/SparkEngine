// TestSprite2DComponents.cpp - Tests for 2D ECS components and systems
// Tests component data structures, sprite animation, tilemap, camera2D, physics2D

#include "TestFramework.h"

#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdint>

// ============================================================================
// Minimal re-declarations for testing (avoids DirectXMath dependency in CI)
// ============================================================================

namespace TestHelper
{
    struct Float2
    {
        float x = 0, y = 0;
    };
    struct Float3
    {
        float x = 0, y = 0, z = 0;
    };
    struct Float4
    {
        float x = 0, y = 0, z = 0, w = 0;
    };
} // namespace TestHelper

// ============================================================================
// SpriteRenderer Tests
// ============================================================================

struct TestSpriteRenderer
{
    std::string texturePath;
    TestHelper::Float4 color{1, 1, 1, 1};
    TestHelper::Float4 sourceRect{0, 0, 1, 1};
    TestHelper::Float2 pivot{0.5f, 0.5f};
    float pixelsPerUnit = 100.0f;
    int sortingLayer = 0;
    int orderInLayer = 0;
    bool flipX = false;
    bool flipY = false;
    bool visible = true;
    int textureWidth = 0;
    int textureHeight = 0;

    TestHelper::Float2 GetWorldSize() const
    {
        if (pixelsPerUnit <= 0)
            return {0, 0};
        float w = static_cast<float>(textureWidth) * (sourceRect.z - sourceRect.x) / pixelsPerUnit;
        float h = static_cast<float>(textureHeight) * (sourceRect.w - sourceRect.y) / pixelsPerUnit;
        return {w, h};
    }
};

TEST(SpriteRenderer_DefaultState)
{
    TestSpriteRenderer sprite;
    EXPECT_TRUE(sprite.visible);
    EXPECT_FALSE(sprite.flipX);
    EXPECT_FALSE(sprite.flipY);
    EXPECT_NEAR(sprite.pixelsPerUnit, 100.0f, 0.001f);
    EXPECT_NEAR(sprite.pivot.x, 0.5f, 0.001f);
    EXPECT_NEAR(sprite.pivot.y, 0.5f, 0.001f);
    EXPECT_EQ(sprite.sortingLayer, 0);
    EXPECT_EQ(sprite.orderInLayer, 0);
}

TEST(SpriteRenderer_WorldSize_FullTexture)
{
    TestSpriteRenderer sprite;
    sprite.textureWidth = 200;
    sprite.textureHeight = 100;
    sprite.pixelsPerUnit = 100.0f;
    auto size = sprite.GetWorldSize();
    EXPECT_NEAR(size.x, 2.0f, 0.001f);
    EXPECT_NEAR(size.y, 1.0f, 0.001f);
}

TEST(SpriteRenderer_WorldSize_SubRect)
{
    TestSpriteRenderer sprite;
    sprite.textureWidth = 256;
    sprite.textureHeight = 256;
    sprite.pixelsPerUnit = 100.0f;
    sprite.sourceRect = {0.0f, 0.0f, 0.5f, 0.5f};
    auto size = sprite.GetWorldSize();
    EXPECT_NEAR(size.x, 1.28f, 0.001f);
    EXPECT_NEAR(size.y, 1.28f, 0.001f);
}

TEST(SpriteRenderer_WorldSize_ZeroPPU)
{
    TestSpriteRenderer sprite;
    sprite.textureWidth = 100;
    sprite.textureHeight = 100;
    sprite.pixelsPerUnit = 0.0f;
    auto size = sprite.GetWorldSize();
    EXPECT_NEAR(size.x, 0.0f, 0.001f);
    EXPECT_NEAR(size.y, 0.0f, 0.001f);
}

// ============================================================================
// SpriteAnimation Tests
// ============================================================================

struct TestAnimFrame
{
    TestHelper::Float4 sourceRect{0, 0, 1, 1};
    float duration = 0.1f;
};

struct TestAnimClip
{
    std::string name;
    std::vector<TestAnimFrame> frames;
    bool loop = true;
};

struct TestSpriteAnimator
{
    std::vector<TestAnimClip> clips;
    int currentClipIndex = -1;
    int currentFrameIndex = 0;
    float frameTimer = 0.0f;
    float speed = 1.0f;
    bool playing = true;
    std::string defaultClip;

    bool Play(const std::string& clipName)
    {
        for (int i = 0; i < static_cast<int>(clips.size()); ++i)
        {
            if (clips[i].name == clipName)
            {
                currentClipIndex = i;
                currentFrameIndex = 0;
                frameTimer = 0.0f;
                playing = true;
                return true;
            }
        }
        return false;
    }

    const TestAnimClip* GetCurrentClip() const
    {
        if (currentClipIndex >= 0 && currentClipIndex < static_cast<int>(clips.size()))
            return &clips[currentClipIndex];
        return nullptr;
    }

    TestHelper::Float4 GetCurrentSourceRect() const
    {
        auto* clip = GetCurrentClip();
        if (!clip || clip->frames.empty())
            return {0, 0, 1, 1};
        int idx = currentFrameIndex % static_cast<int>(clip->frames.size());
        return clip->frames[idx].sourceRect;
    }
};

TEST(SpriteAnimator_PlayClip)
{
    TestSpriteAnimator anim;
    TestAnimClip clip;
    clip.name = "walk";
    clip.frames.push_back({{0, 0, 0.25f, 1}, 0.1f});
    clip.frames.push_back({{0.25f, 0, 0.5f, 1}, 0.1f});
    anim.clips.push_back(clip);

    EXPECT_TRUE(anim.Play("walk"));
    EXPECT_EQ(anim.currentClipIndex, 0);
    EXPECT_EQ(anim.currentFrameIndex, 0);
    EXPECT_TRUE(anim.playing);
}

TEST(SpriteAnimator_PlayNonexistent)
{
    TestSpriteAnimator anim;
    EXPECT_FALSE(anim.Play("nonexistent"));
    EXPECT_EQ(anim.currentClipIndex, -1);
}

TEST(SpriteAnimator_GetSourceRect)
{
    TestSpriteAnimator anim;
    TestAnimClip clip;
    clip.name = "idle";
    clip.frames.push_back({{0.5f, 0.0f, 0.75f, 0.5f}, 0.1f});
    anim.clips.push_back(clip);
    anim.Play("idle");

    auto rect = anim.GetCurrentSourceRect();
    EXPECT_NEAR(rect.x, 0.5f, 0.001f);
    EXPECT_NEAR(rect.y, 0.0f, 0.001f);
    EXPECT_NEAR(rect.z, 0.75f, 0.001f);
    EXPECT_NEAR(rect.w, 0.5f, 0.001f);
}

TEST(SpriteAnimator_NoClip_DefaultRect)
{
    TestSpriteAnimator anim;
    auto rect = anim.GetCurrentSourceRect();
    EXPECT_NEAR(rect.x, 0.0f, 0.001f);
    EXPECT_NEAR(rect.y, 0.0f, 0.001f);
    EXPECT_NEAR(rect.z, 1.0f, 0.001f);
    EXPECT_NEAR(rect.w, 1.0f, 0.001f);
}

// ============================================================================
// Camera2D Tests
// ============================================================================

struct TestCamera2D
{
    float orthoSize = 5.0f;
    float zoom = 1.0f;
    float followSmoothing = 0.1f;
    bool isMain2DCamera = false;

    float GetEffectiveOrthoSize() const { return (zoom > 0.001f) ? orthoSize / zoom : orthoSize; }
};

TEST(Camera2D_DefaultState)
{
    TestCamera2D cam;
    EXPECT_NEAR(cam.orthoSize, 5.0f, 0.001f);
    EXPECT_NEAR(cam.zoom, 1.0f, 0.001f);
    EXPECT_FALSE(cam.isMain2DCamera);
}

TEST(Camera2D_EffectiveOrthoSize_DefaultZoom)
{
    TestCamera2D cam;
    EXPECT_NEAR(cam.GetEffectiveOrthoSize(), 5.0f, 0.001f);
}

TEST(Camera2D_EffectiveOrthoSize_ZoomedIn)
{
    TestCamera2D cam;
    cam.zoom = 2.0f;
    EXPECT_NEAR(cam.GetEffectiveOrthoSize(), 2.5f, 0.001f);
}

TEST(Camera2D_EffectiveOrthoSize_ZoomedOut)
{
    TestCamera2D cam;
    cam.zoom = 0.5f;
    EXPECT_NEAR(cam.GetEffectiveOrthoSize(), 10.0f, 0.001f);
}

TEST(Camera2D_EffectiveOrthoSize_ZeroZoom)
{
    TestCamera2D cam;
    cam.zoom = 0.0f;
    EXPECT_NEAR(cam.GetEffectiveOrthoSize(), 5.0f, 0.001f);
}

// ============================================================================
// TilemapComponent Tests
// ============================================================================

struct TestTilemapComponent
{
    int width = 0;
    int height = 0;
    std::vector<int32_t> tiles;
    std::vector<bool> collisionFlags;

    int32_t GetTile(int x, int y) const
    {
        if (x < 0 || x >= width || y < 0 || y >= height)
            return 0;
        return tiles[static_cast<size_t>(y) * width + x];
    }

    void SetTile(int x, int y, int32_t tileID)
    {
        if (x < 0 || x >= width || y < 0 || y >= height)
            return;
        tiles[static_cast<size_t>(y) * width + x] = tileID;
    }

    bool IsCollisionTile(int x, int y) const
    {
        if (x < 0 || x >= width || y < 0 || y >= height)
            return false;
        return collisionFlags[static_cast<size_t>(y) * width + x];
    }

    void SetCollisionTile(int x, int y, bool solid)
    {
        if (x < 0 || x >= width || y < 0 || y >= height)
            return;
        collisionFlags[static_cast<size_t>(y) * width + x] = solid;
    }

    void Resize(int newWidth, int newHeight)
    {
        std::vector<int32_t> newTiles(static_cast<size_t>(newWidth) * newHeight, 0);
        std::vector<bool> newCol(static_cast<size_t>(newWidth) * newHeight, false);
        int copyW = (std::min)(width, newWidth);
        int copyH = (std::min)(height, newHeight);
        for (int y = 0; y < copyH; ++y)
            for (int x = 0; x < copyW; ++x)
            {
                newTiles[static_cast<size_t>(y) * newWidth + x] = tiles[static_cast<size_t>(y) * width + x];
                newCol[static_cast<size_t>(y) * newWidth + x] = collisionFlags[static_cast<size_t>(y) * width + x];
            }
        width = newWidth;
        height = newHeight;
        tiles = std::move(newTiles);
        collisionFlags = std::move(newCol);
    }
};

TEST(Tilemap_SetAndGetTile)
{
    TestTilemapComponent tm;
    tm.width = 10;
    tm.height = 10;
    tm.tiles.resize(100, 0);
    tm.collisionFlags.resize(100, false);

    tm.SetTile(3, 5, 42);
    EXPECT_EQ(tm.GetTile(3, 5), 42);
    EXPECT_EQ(tm.GetTile(0, 0), 0);
}

TEST(Tilemap_OutOfBounds)
{
    TestTilemapComponent tm;
    tm.width = 5;
    tm.height = 5;
    tm.tiles.resize(25, 0);
    tm.collisionFlags.resize(25, false);

    EXPECT_EQ(tm.GetTile(-1, 0), 0);
    EXPECT_EQ(tm.GetTile(5, 0), 0);
    EXPECT_EQ(tm.GetTile(0, -1), 0);
    EXPECT_EQ(tm.GetTile(0, 5), 0);

    // Setting out of bounds should be a no-op
    tm.SetTile(-1, 0, 99);
    tm.SetTile(5, 5, 99);
}

TEST(Tilemap_CollisionFlags)
{
    TestTilemapComponent tm;
    tm.width = 4;
    tm.height = 4;
    tm.tiles.resize(16, 0);
    tm.collisionFlags.resize(16, false);

    EXPECT_FALSE(tm.IsCollisionTile(1, 1));
    tm.SetCollisionTile(1, 1, true);
    EXPECT_TRUE(tm.IsCollisionTile(1, 1));
    tm.SetCollisionTile(1, 1, false);
    EXPECT_FALSE(tm.IsCollisionTile(1, 1));
}

TEST(Tilemap_Resize_Expand)
{
    TestTilemapComponent tm;
    tm.width = 3;
    tm.height = 3;
    tm.tiles = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    tm.collisionFlags.resize(9, false);
    tm.collisionFlags[4] = true; // (1,1) is solid

    tm.Resize(5, 5);
    EXPECT_EQ(tm.width, 5);
    EXPECT_EQ(tm.height, 5);
    EXPECT_EQ(tm.GetTile(0, 0), 1);
    EXPECT_EQ(tm.GetTile(2, 2), 9);
    EXPECT_EQ(tm.GetTile(3, 3), 0); // New area is zero
    EXPECT_TRUE(tm.IsCollisionTile(1, 1));
}

TEST(Tilemap_Resize_Shrink)
{
    TestTilemapComponent tm;
    tm.width = 4;
    tm.height = 4;
    tm.tiles.resize(16);
    tm.collisionFlags.resize(16, false);
    for (int i = 0; i < 16; ++i)
        tm.tiles[i] = i + 1;

    tm.Resize(2, 2);
    EXPECT_EQ(tm.width, 2);
    EXPECT_EQ(tm.height, 2);
    EXPECT_EQ(tm.GetTile(0, 0), 1);
    EXPECT_EQ(tm.GetTile(1, 0), 2);
    EXPECT_EQ(tm.GetTile(0, 1), 5);
    EXPECT_EQ(tm.GetTile(1, 1), 6);
}

// ============================================================================
// RigidBody2D Tests
// ============================================================================

struct TestRigidBody2D
{
    enum class BodyType
    {
        Static,
        Kinematic,
        Dynamic
    };

    BodyType bodyType = BodyType::Dynamic;
    float mass = 1.0f;
    float gravityScale = 1.0f;
    bool fixedRotation = false;
    TestHelper::Float2 linearVelocity{0, 0};
    float angularVelocity = 0.0f;
    float friction = 0.3f;
    float restitution = 0.0f;

    void ApplyImpulse(float x, float y)
    {
        if (bodyType != BodyType::Dynamic)
            return;
        linearVelocity.x += x / mass;
        linearVelocity.y += y / mass;
    }

    void ApplyForce(float fx, float fy, float dt)
    {
        if (bodyType != BodyType::Dynamic || mass <= 0.0f)
            return;
        linearVelocity.x += (fx / mass) * dt;
        linearVelocity.y += (fy / mass) * dt;
    }
};

TEST(RigidBody2D_DefaultState)
{
    TestRigidBody2D rb;
    EXPECT_NEAR(rb.mass, 1.0f, 0.001f);
    EXPECT_NEAR(rb.gravityScale, 1.0f, 0.001f);
    EXPECT_FALSE(rb.fixedRotation);
    EXPECT_NEAR(rb.linearVelocity.x, 0.0f, 0.001f);
    EXPECT_NEAR(rb.linearVelocity.y, 0.0f, 0.001f);
}

TEST(RigidBody2D_ApplyImpulse)
{
    TestRigidBody2D rb;
    rb.mass = 2.0f;
    rb.ApplyImpulse(10.0f, 20.0f);
    EXPECT_NEAR(rb.linearVelocity.x, 5.0f, 0.001f);
    EXPECT_NEAR(rb.linearVelocity.y, 10.0f, 0.001f);
}

TEST(RigidBody2D_ApplyImpulse_Static)
{
    TestRigidBody2D rb;
    rb.bodyType = TestRigidBody2D::BodyType::Static;
    rb.ApplyImpulse(10.0f, 10.0f);
    EXPECT_NEAR(rb.linearVelocity.x, 0.0f, 0.001f);
    EXPECT_NEAR(rb.linearVelocity.y, 0.0f, 0.001f);
}

TEST(RigidBody2D_ApplyForce)
{
    TestRigidBody2D rb;
    rb.mass = 1.0f;
    rb.ApplyForce(100.0f, 0.0f, 0.016f); // ~60Hz
    EXPECT_NEAR(rb.linearVelocity.x, 1.6f, 0.001f);
}

// ============================================================================
// Physics2D Collision Tests
// ============================================================================

struct SimpleAABB
{
    TestHelper::Float2 min;
    TestHelper::Float2 max;
};

bool TestAABBOverlap(const SimpleAABB& a, const SimpleAABB& b)
{
    return !(a.max.x < b.min.x || a.min.x > b.max.x || a.max.y < b.min.y || a.min.y > b.max.y);
}

TEST(Physics2D_AABB_Overlap)
{
    SimpleAABB a = {{0, 0}, {2, 2}};
    SimpleAABB b = {{1, 1}, {3, 3}};
    EXPECT_TRUE(TestAABBOverlap(a, b));
}

TEST(Physics2D_AABB_NoOverlap)
{
    SimpleAABB a = {{0, 0}, {1, 1}};
    SimpleAABB b = {{2, 2}, {3, 3}};
    EXPECT_FALSE(TestAABBOverlap(a, b));
}

TEST(Physics2D_AABB_EdgeTouch)
{
    SimpleAABB a = {{0, 0}, {1, 1}};
    SimpleAABB b = {{1, 0}, {2, 1}};
    EXPECT_TRUE(TestAABBOverlap(a, b)); // Edge touching = overlapping (standard physics convention)
}

TEST(Physics2D_AABB_Contained)
{
    SimpleAABB outer = {{0, 0}, {10, 10}};
    SimpleAABB inner = {{2, 2}, {5, 5}};
    EXPECT_TRUE(TestAABBOverlap(outer, inner));
    EXPECT_TRUE(TestAABBOverlap(inner, outer));
}

bool TestCircleOverlap(float x1, float y1, float r1, float x2, float y2, float r2)
{
    float dx = x2 - x1;
    float dy = y2 - y1;
    float distSq = dx * dx + dy * dy;
    float rSum = r1 + r2;
    return distSq < rSum * rSum;
}

TEST(Physics2D_Circle_Overlap)
{
    EXPECT_TRUE(TestCircleOverlap(0, 0, 1, 1.5f, 0, 1));
}

TEST(Physics2D_Circle_NoOverlap)
{
    EXPECT_FALSE(TestCircleOverlap(0, 0, 1, 3, 0, 1));
}

TEST(Physics2D_Circle_Coincident)
{
    EXPECT_TRUE(TestCircleOverlap(5, 5, 1, 5, 5, 1));
}

// ============================================================================
// SpriteBatch Sorting Tests
// ============================================================================

struct TestSortEntry
{
    int sortKey;
    std::string texturePath;
};

TEST(SpriteBatch_SortOrder)
{
    std::vector<TestSortEntry> entries = {
        {3, "tex_c"}, {1, "tex_a"}, {2, "tex_b"}, {1, "tex_b"}, {2, "tex_a"},
    };

    std::stable_sort(entries.begin(), entries.end(),
                     [](const TestSortEntry& a, const TestSortEntry& b)
                     {
                         if (a.sortKey != b.sortKey)
                             return a.sortKey < b.sortKey;
                         return a.texturePath < b.texturePath;
                     });

    EXPECT_EQ(entries[0].sortKey, 1);
    EXPECT_EQ(entries[0].texturePath, "tex_a");
    EXPECT_EQ(entries[1].sortKey, 1);
    EXPECT_EQ(entries[1].texturePath, "tex_b");
    EXPECT_EQ(entries[2].sortKey, 2);
    EXPECT_EQ(entries[4].sortKey, 3);
}

// ============================================================================
// NineSlice Tests
// ============================================================================

struct TestNineSlice
{
    float borderLeft = 8.0f;
    float borderTop = 8.0f;
    float borderRight = 8.0f;
    float borderBottom = 8.0f;
    TestHelper::Float2 size{1.0f, 1.0f};
    bool fillCenter = true;
};

TEST(NineSlice_DefaultBorders)
{
    TestNineSlice ns;
    EXPECT_NEAR(ns.borderLeft, 8.0f, 0.001f);
    EXPECT_NEAR(ns.borderRight, 8.0f, 0.001f);
    EXPECT_TRUE(ns.fillCenter);
}

TEST(NineSlice_CenterSize)
{
    TestNineSlice ns;
    ns.size = {100, 100};
    float centerW = ns.size.x - ns.borderLeft - ns.borderRight;
    float centerH = ns.size.y - ns.borderTop - ns.borderBottom;
    EXPECT_NEAR(centerW, 84.0f, 0.001f);
    EXPECT_NEAR(centerH, 84.0f, 0.001f);
}

// ============================================================================
// Parallax Tests
// ============================================================================

struct TestParallaxLayer
{
    TestHelper::Float2 scrollSpeed{0.5f, 0.5f};
    TestHelper::Float2 offset{0, 0};
    bool tileX = true;
    bool tileY = false;
    int sortOrder = -100;
};

TEST(Parallax_OffsetCalculation)
{
    TestParallaxLayer layer;
    layer.scrollSpeed = {0.5f, 0.3f};

    // Simulating camera at (10, 5)
    float cameraX = 10.0f;
    float cameraY = 5.0f;

    layer.offset.x = cameraX * (1.0f - layer.scrollSpeed.x);
    layer.offset.y = cameraY * (1.0f - layer.scrollSpeed.y);

    EXPECT_NEAR(layer.offset.x, 5.0f, 0.001f);
    EXPECT_NEAR(layer.offset.y, 3.5f, 0.001f);
}

TEST(Parallax_ZeroSpeed_NoMovement)
{
    TestParallaxLayer layer;
    layer.scrollSpeed = {0.0f, 0.0f};
    layer.offset.x = 10.0f * (1.0f - 0.0f);
    EXPECT_NEAR(layer.offset.x, 10.0f, 0.001f); // Full offset = static background
}

TEST(Parallax_FullSpeed_MatchesCamera)
{
    TestParallaxLayer layer;
    layer.scrollSpeed = {1.0f, 1.0f};
    layer.offset.x = 10.0f * (1.0f - 1.0f);
    EXPECT_NEAR(layer.offset.x, 0.0f, 0.001f); // Zero offset = moves with camera
}
