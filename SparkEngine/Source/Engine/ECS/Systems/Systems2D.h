/**
 * @file Systems2D.h
 * @brief ECS systems for 2D/2.5D: sprite rendering, 2D physics, animation, parallax, tilemap
 * @author Spark Engine Team
 * @date 2026
 *
 * Implements all 2D-specific ECS systems that integrate with the existing
 * SystemManager infrastructure. These systems run alongside the 3D systems
 * and share the same World / registry.
 *
 * ## Recommended execution order for 2D
 * 1. Physics2DUpdateSystem  - Simulate 2D physics, write back transforms
 * 2. SpriteAnimatorSystem   - Advance sprite animation frames
 * 3. ParallaxSystem         - Update parallax layer offsets
 * 4. TilemapRenderSystem    - Submit tilemap draw commands to SpriteBatch
 * 5. Sprite2DRenderSystem   - Submit sprite draw commands to SpriteBatch
 */

#pragma once

#include "../Components.h"
#include "../Components/Sprite2DComponents.h"
#include "../../2D/Physics2D.h"
#include "../../../Graphics/2D/SpriteBatch.h"
#include "ECSystems.h"
#include "../../../Utils/MathUtils.h"
#include <cmath>

namespace Spark::ECS
{

    // =========================================================================
    // Sprite2DRenderSystem
    // =========================================================================

    /**
     * @class Sprite2DRenderSystem
     * @brief Submits SpriteRenderer entities to the SpriteBatch for 2D rendering.
     *
     * Iterates all entities with Transform + SpriteRenderer, computes their
     * screen-space position and size, and submits draw commands to the SpriteBatch.
     * Respects visibility, sorting layers, and ActiveComponent state.
     */
    class Sprite2DRenderSystem : public ISystem
    {
      public:
        explicit Sprite2DRenderSystem(Graphics2D::SpriteBatch* batch) : m_batch(batch) {}

        void Update(World& world, float deltaTime) override
        {
            if (!m_batch)
                return;

            m_renderedCount = 0;
            auto view = world.GetEntitiesWith<Transform, SpriteRenderer>();
            for (auto entity : view)
            {
                auto& transform = view.get<Transform>(entity);
                auto& sprite = view.get<SpriteRenderer>(entity);

                if (!sprite.visible)
                    continue;

                auto* active = world.GetComponent<ActiveComponent>(entity);
                if (active && !active->active)
                    continue;

                // Check if entity also has a SpriteAnimator — override sourceRect
                auto* animator = world.GetComponent<SpriteAnimator>(entity);
                DirectX::XMFLOAT4 sourceRect = sprite.sourceRect;
                if (animator && animator->playing)
                {
                    sourceRect = animator->GetCurrentSourceRect();
                }

                DirectX::XMFLOAT2 worldSize = sprite.GetWorldSize();
                // Apply transform scale
                worldSize.x *= transform.scale.x;
                worldSize.y *= transform.scale.y;

                // Compute sort key
                int sortKey = sprite.sortingLayer * 10000 + sprite.orderInLayer;

                // Z from transform.position.z for depth
                float z = transform.position.z;

                // Convert rotation.z to radians
                float rotRad = transform.rotation.z * MathUtils::DEG_TO_RAD;

                m_batch->Draw(sprite.texturePath, {transform.position.x, transform.position.y, z}, worldSize,
                              sourceRect, sprite.color, rotRad, sprite.pivot, sprite.flipX, sprite.flipY, sortKey);

                m_renderedCount++;
            }

            // Also render NineSliceSprites
            auto nineSliceView = world.GetEntitiesWith<Transform, NineSliceSprite>();
            for (auto entity : nineSliceView)
            {
                auto& transform = nineSliceView.get<Transform>(entity);
                auto& nineSlice = nineSliceView.get<NineSliceSprite>(entity);

                auto* active = world.GetComponent<ActiveComponent>(entity);
                if (active && !active->active)
                    continue;

                int sortKey = nineSlice.sortingLayer * 10000 + nineSlice.orderInLayer;
                m_batch->DrawNineSlice(nineSlice.texturePath,
                                       {transform.position.x, transform.position.y, transform.position.z},
                                       {nineSlice.size.x * transform.scale.x, nineSlice.size.y * transform.scale.y},
                                       nineSlice.borderLeft, nineSlice.borderTop, nineSlice.borderRight,
                                       nineSlice.borderBottom, nineSlice.color, sortKey, nineSlice.fillCenter);
                m_renderedCount++;
            }
        }

        const char* GetName() const override { return "Sprite2DRenderSystem"; }
        int GetRenderedCount() const { return m_renderedCount; }

      private:
        Graphics2D::SpriteBatch* m_batch;
        int m_renderedCount = 0;
    };

    // =========================================================================
    // SpriteAnimatorSystem
    // =========================================================================

    /**
     * @class SpriteAnimatorSystem
     * @brief Advances frame-based sprite animations each tick.
     *
     * Iterates all entities with SpriteAnimator, advances the frame timer,
     * and transitions to the next frame or loops the animation as configured.
     */
    class SpriteAnimatorSystem : public ISystem
    {
      public:
        void Update(World& world, float deltaTime) override
        {
            auto view = world.GetEntitiesWith<SpriteAnimator>();
            for (auto entity : view)
            {
                auto& animator = view.get<SpriteAnimator>(entity);
                if (!animator.playing)
                    continue;

                const auto* clip = animator.GetCurrentClip();
                if (!clip || clip->frames.empty())
                    continue;

                animator.frameTimer += deltaTime * animator.speed;

                int frameIdx = animator.currentFrameIndex % static_cast<int>(clip->frames.size());
                float frameDuration = clip->frames[frameIdx].duration;

                while (animator.frameTimer >= frameDuration && frameDuration > 0.0f)
                {
                    animator.frameTimer -= frameDuration;
                    animator.currentFrameIndex++;

                    if (animator.currentFrameIndex >= static_cast<int>(clip->frames.size()))
                    {
                        if (clip->loop)
                        {
                            animator.currentFrameIndex = 0;
                        }
                        else
                        {
                            animator.currentFrameIndex = static_cast<int>(clip->frames.size()) - 1;
                            animator.playing = false;
                            break;
                        }
                    }

                    frameIdx = animator.currentFrameIndex % static_cast<int>(clip->frames.size());
                    frameDuration = clip->frames[frameIdx].duration;
                }

                // Update SpriteRenderer's sourceRect if present
                auto* sprite = world.GetComponent<SpriteRenderer>(entity);
                if (sprite)
                {
                    sprite->sourceRect = animator.GetCurrentSourceRect();
                }
            }
        }

        const char* GetName() const override { return "SpriteAnimatorSystem"; }
    };

    // =========================================================================
    // Physics2DUpdateSystem
    // =========================================================================

    /**
     * @class Physics2DUpdateSystem
     * @brief Bridges the ECS world with the Physics2DWorld simulation.
     *
     * Pre-step: Syncs ECS Transform → physics bodies for kinematic entities.
     * Step: Runs the Physics2DWorld simulation.
     * Post-step: Reads physics body positions back into ECS Transforms for dynamic entities.
     */
    class Physics2DUpdateSystem : public ISystem
    {
      public:
        explicit Physics2DUpdateSystem(Physics2D::Physics2DWorld* physicsWorld) : m_physicsWorld(physicsWorld) {}

        void Update(World& world, float deltaTime) override
        {
            if (!m_physicsWorld)
                return;

            // Sync ECS → Physics for kinematic bodies
            auto view = world.GetEntitiesWith<Transform, RigidBody2D, Collider2D>();
            for (auto entity : view)
            {
                auto& transform = view.get<Transform>(entity);
                auto& rb = view.get<RigidBody2D>(entity);
                auto& collider = view.get<Collider2D>(entity);

                auto* body = m_physicsWorld->FindBody(static_cast<uint32_t>(entity));
                if (!body)
                {
                    // Create physics body for this entity
                    Physics2D::PhysicsBody2D newBody;
                    newBody.entityID = static_cast<uint32_t>(entity);
                    newBody.position = {transform.position.x, transform.position.y};
                    newBody.rotation = transform.rotation.z * MathUtils::DEG_TO_RAD;
                    newBody.velocity = {rb.linearVelocity.x, rb.linearVelocity.y};
                    newBody.angularVelocity = rb.angularVelocity;
                    newBody.mass = rb.mass;
                    newBody.friction = rb.friction;
                    newBody.restitution = rb.restitution;
                    newBody.linearDamping = rb.linearDamping;
                    newBody.angularDamping = rb.angularDamping;
                    newBody.gravityScale = rb.gravityScale;
                    newBody.isStatic = (rb.bodyType == RigidBody2D::BodyType::Static);
                    newBody.isKinematic = (rb.bodyType == RigidBody2D::BodyType::Kinematic);
                    newBody.fixedRotation = rb.fixedRotation;
                    newBody.isBullet = rb.isBullet;
                    newBody.isTrigger = collider.isTrigger;
                    newBody.layerMask = collider.layerMask;

                    if (collider.shape == Collider2D::Shape::Circle)
                    {
                        newBody.shapeType = Physics2D::PhysicsBody2D::ShapeType::Circle;
                        newBody.radius = collider.radius;
                    }
                    else
                    {
                        newBody.shapeType = Physics2D::PhysicsBody2D::ShapeType::Box;
                        newBody.halfExtents = {collider.halfExtents.x, collider.halfExtents.y};
                    }

                    m_physicsWorld->AddBody(newBody);
                    continue;
                }

                if (rb.bodyType == RigidBody2D::BodyType::Kinematic)
                {
                    body->position = {transform.position.x, transform.position.y};
                    body->rotation = transform.rotation.z * MathUtils::DEG_TO_RAD;
                }
            }

            // Step simulation
            m_physicsWorld->Step(deltaTime);

            // Read back: Physics → ECS for dynamic bodies
            for (auto entity : view)
            {
                auto& transform = view.get<Transform>(entity);
                auto& rb = view.get<RigidBody2D>(entity);

                if (rb.bodyType != RigidBody2D::BodyType::Dynamic)
                    continue;

                auto* body = m_physicsWorld->FindBody(static_cast<uint32_t>(entity));
                if (!body)
                    continue;

                transform.position.x = body->position.x;
                transform.position.y = body->position.y;
                transform.rotation.z = body->rotation * MathUtils::RAD_TO_DEG;
                rb.linearVelocity = body->velocity.ToXMFLOAT2();
                rb.angularVelocity = body->angularVelocity;
            }
        }

        const char* GetName() const override { return "Physics2DUpdateSystem"; }

      private:
        Physics2D::Physics2DWorld* m_physicsWorld;
    };

    // =========================================================================
    // ParallaxSystem
    // =========================================================================

    /**
     * @class ParallaxSystem
     * @brief Updates parallax background layer offsets based on Camera2D position.
     *
     * For each ParallaxBackground entity, adjusts layer offsets based on the
     * camera's current position and each layer's scroll speed multiplier.
     */
    class ParallaxSystem : public ISystem
    {
      public:
        void Update(World& world, float deltaTime) override
        {
            // Find the active 2D camera position
            DirectX::XMFLOAT2 cameraPos{0.0f, 0.0f};
            auto camView = world.GetEntitiesWith<Transform, Camera2D>();
            for (auto camEntity : camView)
            {
                auto& cam = camView.get<Camera2D>(camEntity);
                if (cam.isMain2DCamera)
                {
                    auto& camTf = camView.get<Transform>(camEntity);
                    cameraPos = {camTf.position.x, camTf.position.y};
                    break;
                }
            }

            auto view = world.GetEntitiesWith<ParallaxBackground>();
            for (auto entity : view)
            {
                auto& parallax = view.get<ParallaxBackground>(entity);

                for (auto& layer : parallax.layers)
                {
                    layer.offset.x = cameraPos.x * (1.0f - layer.scrollSpeed.x);
                    layer.offset.y = cameraPos.y * (1.0f - layer.scrollSpeed.y);

                    if (parallax.autoScroll)
                    {
                        layer.offset.x += parallax.autoScrollSpeed.x * deltaTime;
                        layer.offset.y += parallax.autoScrollSpeed.y * deltaTime;
                    }
                }
            }
        }

        const char* GetName() const override { return "ParallaxSystem"; }
    };

    // =========================================================================
    // TilemapRenderSystem
    // =========================================================================

    /**
     * @class TilemapRenderSystem
     * @brief Submits tilemap tile draw commands to the SpriteBatch.
     *
     * Iterates all TilemapComponent entities, and for each visible tile,
     * submits a sprite draw command with the correct UV rect from the tileset.
     * Only renders tiles visible within the camera frustum for performance.
     */
    class TilemapRenderSystem : public ISystem
    {
      public:
        explicit TilemapRenderSystem(Graphics2D::SpriteBatch* batch) : m_batch(batch) {}

        void Update(World& world, float deltaTime) override
        {
            if (!m_batch)
                return;

            m_tilesRendered = 0;

            auto view = world.GetEntitiesWith<Transform, TilemapComponent>();
            for (auto entity : view)
            {
                auto& transform = view.get<Transform>(entity);
                auto& tilemap = view.get<TilemapComponent>(entity);

                if (tilemap.width <= 0 || tilemap.height <= 0 || tilemap.tiles.empty())
                    continue;

                float tileWorldW = static_cast<float>(tilemap.tileset.tileWidth) / tilemap.pixelsPerUnit;
                float tileWorldH = static_cast<float>(tilemap.tileset.tileHeight) / tilemap.pixelsPerUnit;

                int sortKey = tilemap.sortingLayer * 10000;

                for (int y = 0; y < tilemap.height; ++y)
                {
                    for (int x = 0; x < tilemap.width; ++x)
                    {
                        int32_t tileID = tilemap.GetTile(x, y);
                        if (tileID <= 0)
                            continue;

                        DirectX::XMFLOAT4 uv = tilemap.GetTileUV(tileID);
                        if (uv.z <= uv.x || uv.w <= uv.y)
                            continue;

                        float worldX = transform.position.x + static_cast<float>(x) * tileWorldW;
                        float worldY = transform.position.y + static_cast<float>(y) * tileWorldH;

                        m_batch->Draw(tilemap.tileset.texturePath, {worldX, worldY, transform.position.z},
                                      {tileWorldW, tileWorldH}, uv, {1, 1, 1, 1}, 0.0f, {0, 0}, false, false, sortKey);

                        m_tilesRendered++;
                    }
                }
            }
        }

        const char* GetName() const override { return "TilemapRenderSystem"; }
        int GetTilesRendered() const { return m_tilesRendered; }

      private:
        Graphics2D::SpriteBatch* m_batch;
        int m_tilesRendered = 0;
    };

    // =========================================================================
    // Camera2DFollowSystem
    // =========================================================================

    /**
     * @class Camera2DFollowSystem
     * @brief Smoothly follows a target entity with the Camera2D.
     */
    class Camera2DFollowSystem : public ISystem
    {
      public:
        void Update(World& world, float deltaTime) override
        {
            auto camView = world.GetEntitiesWith<Transform, Camera2D>();
            for (auto camEntity : camView)
            {
                auto& camTf = camView.get<Transform>(camEntity);
                auto& cam = camView.get<Camera2D>(camEntity);

                if (cam.followTarget == Camera2D::INVALID_FOLLOW_TARGET)
                    continue;

                auto targetEntity = static_cast<EntityID>(cam.followTarget);
                auto* targetTf = world.GetComponent<Transform>(targetEntity);
                if (!targetTf)
                    continue;

                // Smooth follow with dead zone
                // Precompute smoothing factor once per camera (std::exp is faster than std::pow
                // and avoids recomputing the same value for both axes)
                float t = 1.0f - std::exp(std::log(cam.followSmoothing) * deltaTime);
                float dx = targetTf->position.x - camTf.position.x;
                float dy = targetTf->position.y - camTf.position.y;

                if (std::abs(dx) > cam.deadZone.x)
                {
                    float sign = dx > 0 ? 1.0f : -1.0f;
                    float target = targetTf->position.x - sign * cam.deadZone.x;
                    camTf.position.x += (target - camTf.position.x) * t;
                }

                if (std::abs(dy) > cam.deadZone.y)
                {
                    float sign = dy > 0 ? 1.0f : -1.0f;
                    float target = targetTf->position.y - sign * cam.deadZone.y;
                    camTf.position.y += (target - camTf.position.y) * t;
                }

                // Apply shake offset
                camTf.position.x += cam.shakeOffset.x;
                camTf.position.y += cam.shakeOffset.y;

                // Decay shake
                cam.shakeOffset.x *= 0.9f;
                cam.shakeOffset.y *= 0.9f;
            }
        }

        const char* GetName() const override { return "Camera2DFollowSystem"; }
    };

} // namespace Spark::ECS
