/**
 * @file PlacementComponents.h
 * @brief ECS components for editor-placeable gameplay objects
 * @author Spark Engine Team
 * @date 2026
 *
 * Components for objects commonly placed in level editors across game engines:
 * wind zones, physics joints, occluders, cover/tactical points, destructibles,
 * cinematic/dialogue triggers, area boundaries, and billboards.
 *
 * Each bridges to an existing engine system (CoverSystem, TacticalPointSystem,
 * DestructionSystem, Sequencer, DialogueSystem, SeamlessAreaManager, etc.).
 */

#pragma once
#include "../../../Core/Platform.h"
#include "../../../Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>

// =============================================================================
// WindZoneComponent — Directional or spherical wind force
// =============================================================================

/**
 * @brief Applies wind force to particles, vegetation, and cloth within range.
 *
 * Directional mode applies a global wind direction (like weather).
 * Spherical mode applies radial force from the entity position (like an explosion).
 */
struct WindZoneComponent
{
    enum class Mode : uint8_t
    {
        Directional, ///< Wind blows in a direction.
        Spherical    ///< Wind radiates outward from center.
    };

    Mode mode = Mode::Directional;
    DirectX::XMFLOAT3 direction{1.0f, 0.0f, 0.0f}; ///< Wind direction (Directional mode).
    float mainStrength = 1.0f;                     ///< Main wind speed (m/s).
    float turbulenceStrength = 0.5f;               ///< Turbulence intensity.
    float pulseFrequency = 0.0f;                   ///< Pulsing frequency (Hz, 0=constant).
    float radius = 10.0f;                          ///< Effect radius (Spherical mode).
    bool affectsParticles = true;                  ///< Influence particle systems.
    bool affectsVegetation = true;                 ///< Influence vegetation sway.
    bool affectsCloth = true;                      ///< Influence cloth simulation.

    bool Validate() const
    {
        ASSERT_MSG(mainStrength >= 0.0f, "WindZone mainStrength must be non-negative");
        ASSERT_MSG(radius > 0.0f, "WindZone radius must be positive");
        return mainStrength >= 0.0f && radius > 0.0f;
    }
};

// =============================================================================
// PhysicsJointComponent — Constraint between two rigid bodies
// =============================================================================

/**
 * @brief Defines a physics constraint/joint between two rigid bodies.
 *
 * Bridges to Jolt Physics joint system. Supports fixed, hinge, slider,
 * ball-socket, distance, and cone twist joints.
 */
struct PhysicsJointComponent
{
    enum class JointType : uint8_t
    {
        Fixed,      ///< No relative movement.
        Hinge,      ///< Rotation around one axis (doors, wheels).
        Slider,     ///< Translation along one axis (pistons).
        BallSocket, ///< Free rotation, no translation (shoulders, pendulums).
        Distance,   ///< Maintains distance range between bodies.
        ConeTwist   ///< Limited cone of rotation (ragdoll limbs).
    };

    JointType jointType = JointType::Fixed;
    uint32_t connectedEntityId = 0;             ///< Entity of the other body (0=world anchor).
    DirectX::XMFLOAT3 anchor{0.0f, 0.0f, 0.0f}; ///< Local-space anchor point.
    DirectX::XMFLOAT3 axis{0.0f, 1.0f, 0.0f};   ///< Joint axis (hinge/slider/cone).
    float lowerLimit = 0.0f;                    ///< Lower angular/linear limit.
    float upperLimit = 0.0f;                    ///< Upper angular/linear limit.
    bool enableLimits = false;                  ///< Enable joint limits.
    bool enableMotor = false;                   ///< Enable joint motor.
    float motorSpeed = 0.0f;                    ///< Motor target speed.
    float motorMaxForce = 100.0f;               ///< Motor maximum force.
    float breakForce = 0.0f;                    ///< Force to break joint (0=unbreakable).
    float breakTorque = 0.0f;                   ///< Torque to break joint (0=unbreakable).
    uint32_t runtimeJointId = 0;                ///< ID assigned by physics system.

    bool Validate() const
    {
        ASSERT_MSG(motorMaxForce >= 0.0f, "Joint motorMaxForce must be non-negative");
        ASSERT_MSG(breakForce >= 0.0f, "Joint breakForce must be non-negative");
        return motorMaxForce >= 0.0f && breakForce >= 0.0f;
    }
};

// =============================================================================
// OccluderComponent — Simplified proxy for occlusion culling
// =============================================================================

/**
 * @brief Marks an entity as an occluder for the software occlusion system.
 *
 * Bridges to OcclusionCullingSystem. The system uses this simplified geometry
 * to rasterize a low-res depth buffer for culling objects behind it.
 */
struct OccluderComponent
{
    enum class Shape : uint8_t
    {
        Box,
        Quad
    };

    Shape shape = Shape::Box;
    DirectX::XMFLOAT3 halfExtents{1.0f, 1.0f, 0.1f}; ///< Box half-extents or quad size.
    bool doubleSided = false;                        ///< Cull from both sides.
};

// =============================================================================
// CoverPointComponent — AI cover position marker
// =============================================================================

/**
 * @brief Marks an entity position as a cover point for AI combat.
 *
 * Bridges to CoverSystem. AI agents query nearby cover points during combat
 * to find safe positions for shooting, reloading, and flanking.
 */
struct CoverPointComponent
{
    enum class Height : uint8_t
    {
        Low, ///< Crouch cover (can fire over top).
        High ///< Standing cover (must lean to fire).
    };

    Height height = Height::Low;
    float width = 1.0f;                              ///< Cover width (meters).
    DirectX::XMFLOAT3 coverNormal{0.0f, 0.0f, 1.0f}; ///< Direction entity faces when in cover.
    bool canLeanLeft = true;                         ///< Can lean left to fire.
    bool canLeanRight = true;                        ///< Can lean right to fire.
    bool canFireOver = false;                        ///< Can fire over top (low cover).
    int maxOccupants = 1;                            ///< Max AI using this point simultaneously.
    int currentOccupants = 0;                        ///< Runtime: current occupant count.

    bool IsAvailable() const { return currentOccupants < maxOccupants; }

    bool Validate() const
    {
        ASSERT_MSG(width > 0.0f, "CoverPoint width must be positive");
        ASSERT_MSG(maxOccupants > 0, "CoverPoint maxOccupants must be positive");
        return width > 0.0f && maxOccupants > 0;
    }
};

// =============================================================================
// TacticalPointComponent — AI tactical position marker
// =============================================================================

/**
 * @brief Marks an entity position as a tactical point for AI decision-making.
 *
 * Bridges to TacticalPointSystem. AI agents evaluate nearby tactical points
 * by type and quality score to choose optimal positions for combat maneuvers.
 */
struct TacticalPointComponent
{
    enum class PointType : uint8_t
    {
        Cover,   ///< Defensive position.
        Vantage, ///< High ground with line of sight.
        Ambush,  ///< Hidden position for surprise attacks.
        Flank,   ///< Side/rear approach position.
        Retreat, ///< Fallback position.
        Rally    ///< Group gathering point.
    };

    PointType pointType = PointType::Cover;
    float qualityScore = 1.0f; ///< Base quality [0,1] for AI evaluation.
    float radius = 2.0f;       ///< Effective radius of this position.
    bool enabled = true;       ///< Whether this point is currently available.

    bool Validate() const
    {
        ASSERT_MSG(qualityScore >= 0.0f && qualityScore <= 1.0f, "TacticalPoint quality must be in [0,1]");
        ASSERT_MSG(radius > 0.0f, "TacticalPoint radius must be positive");
        return qualityScore >= 0.0f && qualityScore <= 1.0f && radius > 0.0f;
    }
};

// =============================================================================
// DestructibleComponent — Destructible object with fracture
// =============================================================================

/**
 * @brief Marks an entity as destructible with fracture on damage.
 *
 * Bridges to DestructionSystem. When health reaches zero, the system
 * fractures the mesh into debris pieces using the specified pattern.
 */
struct DestructibleComponent
{
    float health = 100.0f;                   ///< Current health.
    float maxHealth = 100.0f;                ///< Maximum health.
    int damageStages = 3;                    ///< Number of visual damage stages (1-5).
    std::string fracturePattern = "default"; ///< Fracture pattern name.
    float debrisLifetime = 10.0f;            ///< How long debris persists (seconds).
    float explosionForce = 5.0f;             ///< Force applied to debris on fracture.
    float minDamageThreshold = 5.0f;         ///< Minimum damage to cause any effect.
    bool generateColliders = true;           ///< Generate colliders for debris pieces.
    bool chainReaction = false;              ///< Can trigger nearby destructibles.
    int currentStage = 0;                    ///< Runtime: current damage stage.

    bool Validate() const
    {
        ASSERT_MSG(maxHealth > 0.0f, "Destructible maxHealth must be positive");
        ASSERT_MSG(damageStages >= 1 && damageStages <= 5, "Destructible damageStages must be in [1,5]");
        return maxHealth > 0.0f && damageStages >= 1 && damageStages <= 5;
    }
};

// =============================================================================
// CinematicTriggerComponent — Starts a cinematic sequence on enter
// =============================================================================

/**
 * @brief Trigger volume that starts a cinematic sequence when entered.
 *
 * Bridges to Sequencer. When the player enters the trigger volume,
 * the specified sequence begins playing.
 */
struct CinematicTriggerComponent
{
    std::string sequenceName;  ///< Name of the cinematic sequence.
    float radius = 5.0f;       ///< Trigger radius (sphere).
    bool playOnce = true;      ///< Only trigger once per play session.
    bool skipable = true;      ///< Player can skip the cinematic.
    bool pauseGameplay = true; ///< Freeze gameplay during cinematic.
    bool hasTriggered = false; ///< Runtime: whether this has already fired.

    bool Validate() const
    {
        ASSERT_MSG(radius > 0.0f, "CinematicTrigger radius must be positive");
        return radius > 0.0f;
    }
};

// =============================================================================
// DialogueTriggerComponent — Starts dialogue tree on interaction
// =============================================================================

/**
 * @brief Marks an entity as a dialogue interaction point.
 *
 * Bridges to DialogueSystem. When the player interacts (or enters range),
 * the specified dialogue tree begins.
 */
struct DialogueTriggerComponent
{
    std::string dialogueTreeName;   ///< Name of the dialogue tree to start.
    std::string speakerName;        ///< NPC speaker display name.
    float interactionRadius = 3.0f; ///< Max distance to start dialogue.
    bool requiresInteract = true;   ///< Requires player input vs. auto-start.
    bool oneShot = false;           ///< Only trigger once.
    bool facePlayer = true;         ///< NPC turns to face player during dialogue.

    bool Validate() const
    {
        ASSERT_MSG(interactionRadius > 0.0f, "DialogueTrigger interactionRadius must be positive");
        return interactionRadius > 0.0f;
    }
};

// =============================================================================
// AreaBoundaryComponent — Level streaming area boundary
// =============================================================================

/**
 * @brief Defines a streaming area boundary for seamless world loading.
 *
 * Bridges to SeamlessAreaManager. The system uses these boundaries to
 * determine which areas to load/unload based on player position.
 */
struct AreaBoundaryComponent
{
    std::string areaName;                                ///< Area identifier.
    std::string scenePath;                               ///< Scene file to stream in.
    DirectX::XMFLOAT3 boundsMin{0.0f, 0.0f, 0.0f};       ///< AABB minimum corner.
    DirectX::XMFLOAT3 boundsMax{100.0f, 100.0f, 100.0f}; ///< AABB maximum corner.
    int priority = 0;                                    ///< Loading priority.
    float loadRadius = 50.0f;                            ///< Distance to start loading.
    float unloadRadius = 100.0f;                         ///< Distance to unload.
    bool alwaysLoaded = false;                           ///< Keep loaded regardless of distance.

    bool Validate() const
    {
        ASSERT_MSG(loadRadius > 0.0f, "AreaBoundary loadRadius must be positive");
        ASSERT_MSG(unloadRadius >= loadRadius, "AreaBoundary unloadRadius must be >= loadRadius");
        return loadRadius > 0.0f && unloadRadius >= loadRadius;
    }
};

// =============================================================================
// BillboardComponent — Always-face-camera sprite in 3D space
// =============================================================================

/**
 * @brief Renders a textured quad that always faces the camera.
 *
 * Used for distant trees, particle impostors, icons, and other objects
 * that should face the viewer regardless of camera angle.
 */
struct BillboardComponent
{
    std::string texturePath;                         ///< Texture path.
    DirectX::XMFLOAT4 color{1.0f, 1.0f, 1.0f, 1.0f}; ///< Tint color.
    DirectX::XMFLOAT2 size{1.0f, 1.0f};              ///< Billboard size (world units).

    enum class LockAxis : uint8_t
    {
        None,    ///< Full billboard (faces camera in all axes).
        YAxisUp, ///< Locked to world Y-axis (cylindrical billboard).
        Flat     ///< No rotation, rendered flat.
    };

    LockAxis lockAxis = LockAxis::None;
    float fadeStartDistance = 50.0f; ///< Distance to start fading.
    float fadeEndDistance = 100.0f;  ///< Distance to fully fade out.
    int sortingLayer = 0;            ///< Render sort order.
};
