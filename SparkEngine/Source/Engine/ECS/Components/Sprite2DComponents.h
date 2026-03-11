/**
 * @file Sprite2DComponents.h
 * @brief ECS components for 2D/2.5D rendering: sprites, tilemaps, parallax, Camera2D
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a complete set of 2D-specific ECS components that integrate with
 * the existing 3D component pipeline. All components use DirectXMath types
 * for consistency and SIMD alignment.
 */

#pragma once
#include "../../../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <cstdint>
#include <array>
#include <algorithm>
#include <unordered_map>

// =============================================================================
// SpriteRenderer — 2D quad with texture, tint, flip, and layer sorting
// =============================================================================

struct SpriteRenderer
{
    /// Path to the sprite texture asset (PNG, DDS, etc.)
    std::string texturePath;

    /// Tint color multiplied with the texture sample
    DirectX::XMFLOAT4 color{1.0f, 1.0f, 1.0f, 1.0f};

    /// Source rectangle in UV space [0,1] for sprite sheet sub-regions
    DirectX::XMFLOAT4 sourceRect{0.0f, 0.0f, 1.0f, 1.0f};

    /// Pivot/anchor point in normalized coordinates (0,0)=top-left, (0.5,0.5)=center
    DirectX::XMFLOAT2 pivot{0.5f, 0.5f};

    /// Pixels per unit for world-space sizing
    float pixelsPerUnit = 100.0f;

    /// Sorting layer (higher = drawn later / on top)
    int sortingLayer = 0;

    /// Order within the same sorting layer
    int orderInLayer = 0;

    /// Horizontal flip
    bool flipX = false;

    /// Vertical flip
    bool flipY = false;

    /// Visibility toggle
    bool visible = true;

    /// Width and height in pixels of the source texture
    int textureWidth = 0;
    int textureHeight = 0;

    /// Compute the world-space size based on texture dimensions and pixelsPerUnit
    DirectX::XMFLOAT2 GetWorldSize() const
    {
        if (pixelsPerUnit <= 0.0f)
            return {0.0f, 0.0f};
        float w = static_cast<float>(textureWidth) * (sourceRect.z - sourceRect.x) / pixelsPerUnit;
        float h = static_cast<float>(textureHeight) * (sourceRect.w - sourceRect.y) / pixelsPerUnit;
        return {w, h};
    }
};

// =============================================================================
// SpriteAnimation — frame-based sprite sheet animation
// =============================================================================

struct SpriteAnimationFrame
{
    DirectX::XMFLOAT4 sourceRect{0.0f, 0.0f, 1.0f, 1.0f}; ///< UV rect for this frame
    float duration = 0.1f;                                ///< Duration of this frame in seconds
};

struct SpriteAnimationClip
{
    std::string name;
    std::vector<SpriteAnimationFrame> frames;
    bool loop = true;
};

struct SpriteAnimator
{
    /// All available animation clips keyed by name
    std::vector<SpriteAnimationClip> clips;

    /// Index into clips for the currently playing animation (-1 = none)
    int currentClipIndex = -1;

    /// Current frame index within the active clip
    int currentFrameIndex = 0;

    /// Accumulated time for the current frame
    float frameTimer = 0.0f;

    /// Playback speed multiplier
    float speed = 1.0f;

    /// Whether the animator is playing
    bool playing = true;

    /// Name of the default clip to play on start
    std::string defaultClip;

    /// Play a clip by name. Returns true if found.
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

    /// Get the current animation clip, or nullptr
    const SpriteAnimationClip* GetCurrentClip() const
    {
        if (currentClipIndex >= 0 && currentClipIndex < static_cast<int>(clips.size()))
            return &clips[currentClipIndex];
        return nullptr;
    }

    /// Get the source rect for the current frame
    DirectX::XMFLOAT4 GetCurrentSourceRect() const
    {
        const auto* clip = GetCurrentClip();
        if (!clip || clip->frames.empty())
            return {0.0f, 0.0f, 1.0f, 1.0f};
        int idx = currentFrameIndex % static_cast<int>(clip->frames.size());
        return clip->frames[idx].sourceRect;
    }
};

// =============================================================================
// Camera2D — orthographic camera for 2D scenes
// =============================================================================

struct Camera2D
{
    /// Orthographic half-height in world units
    float orthoSize = 5.0f;

    /// Near/far clip planes
    float nearPlane = -100.0f;
    float farPlane = 100.0f;

    /// Zoom multiplier (1.0 = default, 2.0 = zoomed in 2x)
    float zoom = 1.0f;

    /// Camera shake offset (applied additively)
    DirectX::XMFLOAT2 shakeOffset{0.0f, 0.0f};

    /// Dead zone for smooth follow (in world units)
    DirectX::XMFLOAT2 deadZone{0.5f, 0.5f};

    /// Follow target entity (entt::null = no follow)
    uint32_t followTarget = 0xFFFFFFFF; // entt::null equivalent

    /// Follow smoothing factor (0 = instant, 1 = no movement)
    float followSmoothing = 0.1f;

    /// Whether this is the active 2D camera
    bool isMain2DCamera = false;

    /// Background clear color
    DirectX::XMFLOAT4 clearColor{0.2f, 0.2f, 0.3f, 1.0f};

    /// Viewport bounds (in pixels, 0 = use window size)
    int viewportX = 0;
    int viewportY = 0;
    int viewportWidth = 0;
    int viewportHeight = 0;

    /// Compute the effective ortho size accounting for zoom
    float GetEffectiveOrthoSize() const { return (zoom > 0.001f) ? orthoSize / zoom : orthoSize; }
};

// =============================================================================
// ParallaxLayer — scrolling background layer for 2.5D depth
// =============================================================================

struct ParallaxLayer
{
    /// Texture path for this layer
    std::string texturePath;

    /// Scroll speed multiplier relative to the camera (1.0 = same speed, 0.5 = half)
    DirectX::XMFLOAT2 scrollSpeed{0.5f, 0.5f};

    /// Whether to tile horizontally and/or vertically
    bool tileX = true;
    bool tileY = false;

    /// Offset within the texture
    DirectX::XMFLOAT2 offset{0.0f, 0.0f};

    /// Tint color
    DirectX::XMFLOAT4 tint{1.0f, 1.0f, 1.0f, 1.0f};

    /// Sorting order (lower = further back)
    int sortOrder = -100;
};

struct ParallaxBackground
{
    std::vector<ParallaxLayer> layers;
    bool autoScroll = false;
    DirectX::XMFLOAT2 autoScrollSpeed{0.0f, 0.0f};
};

// =============================================================================
// TilemapComponent — grid-based tile map
// =============================================================================

struct TilesetInfo
{
    std::string texturePath;
    int tileWidth = 16;
    int tileHeight = 16;
    int columns = 0;  ///< Number of columns in the tileset texture
    int rows = 0;     ///< Number of rows in the tileset texture
    int spacing = 0;  ///< Spacing between tiles in pixels
    int margin = 0;   ///< Margin around tiles in pixels
    int firstGID = 1; ///< First global tile ID in this tileset
};

struct TilemapComponent
{
    /// Tileset information
    TilesetInfo tileset;

    /// Map dimensions in tiles
    int width = 0;
    int height = 0;

    /// Tile data: row-major array of tile IDs (0 = empty)
    std::vector<int32_t> tiles;

    /// Collision tile flags: same dimensions as tiles, true = solid
    std::vector<bool> collisionFlags;

    /// Sorting layer for rendering order
    int sortingLayer = 0;

    /// Pixels per unit for world sizing
    float pixelsPerUnit = 100.0f;

    /// Whether to generate collision geometry from collisionFlags
    bool generateCollision = true;

    /// Grid color for editor visualization
    DirectX::XMFLOAT4 gridColor{0.3f, 0.3f, 0.3f, 0.5f};

    /// Get tile ID at grid position (returns 0 if out of bounds)
    int32_t GetTile(int x, int y) const
    {
        if (x < 0 || x >= width || y < 0 || y >= height)
            return 0;
        return tiles[static_cast<size_t>(y) * width + x];
    }

    /// Set tile ID at grid position
    void SetTile(int x, int y, int32_t tileID)
    {
        if (x < 0 || x >= width || y < 0 || y >= height)
            return;
        tiles[static_cast<size_t>(y) * width + x] = tileID;
    }

    /// Get collision flag at grid position
    bool IsCollisionTile(int x, int y) const
    {
        if (x < 0 || x >= width || y < 0 || y >= height)
            return false;
        return collisionFlags[static_cast<size_t>(y) * width + x];
    }

    /// Set collision flag at grid position
    void SetCollisionTile(int x, int y, bool solid)
    {
        if (x < 0 || x >= width || y < 0 || y >= height)
            return;
        collisionFlags[static_cast<size_t>(y) * width + x] = solid;
    }

    /// Resize the tilemap, preserving existing tile data where possible
    void Resize(int newWidth, int newHeight)
    {
        std::vector<int32_t> newTiles(static_cast<size_t>(newWidth) * newHeight, 0);
        std::vector<bool> newCollision(static_cast<size_t>(newWidth) * newHeight, false);

        int copyW = (std::min)(width, newWidth);
        int copyH = (std::min)(height, newHeight);

        for (int y = 0; y < copyH; ++y)
        {
            for (int x = 0; x < copyW; ++x)
            {
                newTiles[static_cast<size_t>(y) * newWidth + x] = tiles[static_cast<size_t>(y) * width + x];
                newCollision[static_cast<size_t>(y) * newWidth + x] =
                    collisionFlags[static_cast<size_t>(y) * width + x];
            }
        }

        width = newWidth;
        height = newHeight;
        tiles = std::move(newTiles);
        collisionFlags = std::move(newCollision);
    }

    /// Get the UV rect for a specific tile ID
    DirectX::XMFLOAT4 GetTileUV(int32_t tileID) const
    {
        if (tileID <= 0 || tileset.columns <= 0 || tileset.rows <= 0)
            return {0.0f, 0.0f, 0.0f, 0.0f};

        int localID = tileID - tileset.firstGID;
        if (localID < 0)
            return {0.0f, 0.0f, 0.0f, 0.0f};

        int col = localID % tileset.columns;
        int row = localID / tileset.columns;

        float texW = static_cast<float>(tileset.columns * (tileset.tileWidth + tileset.spacing) + 2 * tileset.margin);
        float texH = static_cast<float>(tileset.rows * (tileset.tileHeight + tileset.spacing) + 2 * tileset.margin);

        float u0 = static_cast<float>(tileset.margin + col * (tileset.tileWidth + tileset.spacing)) / texW;
        float v0 = static_cast<float>(tileset.margin + row * (tileset.tileHeight + tileset.spacing)) / texH;
        float u1 = u0 + static_cast<float>(tileset.tileWidth) / texW;
        float v1 = v0 + static_cast<float>(tileset.tileHeight) / texH;

        return {u0, v0, u1, v1};
    }
};

// =============================================================================
// RigidBody2D — 2D physics body
// =============================================================================

struct RigidBody2D
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
    float linearDamping = 0.0f;
    float angularDamping = 0.05f;
    bool fixedRotation = false;
    bool isBullet = false; ///< Use continuous collision detection
    DirectX::XMFLOAT2 linearVelocity{0.0f, 0.0f};
    float angularVelocity = 0.0f;
    float friction = 0.3f;
    float restitution = 0.0f; ///< Bounciness (0 = no bounce, 1 = perfectly elastic)

    /// Apply an impulse at the center of mass
    void ApplyImpulse(float x, float y)
    {
        if (bodyType != BodyType::Dynamic)
            return;
        linearVelocity.x += x / mass;
        linearVelocity.y += y / mass;
    }

    /// Apply a force scaled by delta time
    void ApplyForce(float fx, float fy, float dt)
    {
        if (bodyType != BodyType::Dynamic || mass <= 0.0f)
            return;
        linearVelocity.x += (fx / mass) * dt;
        linearVelocity.y += (fy / mass) * dt;
    }
};

// =============================================================================
// Collider2D — 2D collision shapes
// =============================================================================

struct Collider2D
{
    enum class Shape
    {
        Box,
        Circle,
        Capsule,
        Polygon,
        Edge
    };

    Shape shape = Shape::Box;

    /// Box half-extents (for Shape::Box)
    DirectX::XMFLOAT2 halfExtents{0.5f, 0.5f};

    /// Radius (for Shape::Circle, Shape::Capsule)
    float radius = 0.5f;

    /// Height (for Shape::Capsule, total height including caps)
    float height = 1.0f;

    /// Offset from the entity's transform position
    DirectX::XMFLOAT2 offset{0.0f, 0.0f};

    /// Whether this collider is a trigger (no physical response, only events)
    bool isTrigger = false;

    /// Collision layer mask (bitfield)
    uint32_t layerMask = 0xFFFFFFFF;

    /// Polygon vertices (for Shape::Polygon, max 8 vertices)
    std::vector<DirectX::XMFLOAT2> polygonVertices;

    /// Edge start/end (for Shape::Edge)
    DirectX::XMFLOAT2 edgeStart{-0.5f, 0.0f};
    DirectX::XMFLOAT2 edgeEnd{0.5f, 0.0f};
};

// =============================================================================
// NineSliceSprite — 9-slice scalable sprite for UI elements
// =============================================================================

struct NineSliceSprite
{
    std::string texturePath;

    /// Border sizes in pixels (left, top, right, bottom)
    float borderLeft = 8.0f;
    float borderTop = 8.0f;
    float borderRight = 8.0f;
    float borderBottom = 8.0f;

    /// Size in world units
    DirectX::XMFLOAT2 size{1.0f, 1.0f};

    /// Tint color
    DirectX::XMFLOAT4 color{1.0f, 1.0f, 1.0f, 1.0f};

    /// Fill center toggle
    bool fillCenter = true;

    int sortingLayer = 0;
    int orderInLayer = 0;
};

// =============================================================================
// PixelPerfect — pixel-perfect rendering constraint
// =============================================================================

struct PixelPerfectComponent
{
    /// Reference resolution for pixel-perfect rendering
    int referenceWidth = 320;
    int referenceHeight = 240;

    /// Whether to upscale the render to fill the screen
    bool upscaleToFill = true;

    /// Whether to crop edges to maintain exact pixel ratio
    bool cropToFit = false;

    /// Current computed scale factor
    float currentScaleFactor = 1.0f;
};
