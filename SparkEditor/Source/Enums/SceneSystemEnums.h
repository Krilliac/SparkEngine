/**
 * @file SceneSystemEnums.h
 * @brief Enumerations for the scene management system
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file contains all enumerations related to scene management,
 * including component types, light types, projection types, and physics body types.
 */

#pragma once

#include <cstdint>

namespace SparkEditor
{

    /**
 * @brief Component type enumeration
 */
    enum class ComponentType : uint32_t
    {
        TRANSFORM = 0,        ///< Transform component
        MESH_RENDERER = 1,    ///< Mesh renderer component
        LIGHT = 2,            ///< Light component
        CAMERA = 3,           ///< Camera component
        RIGID_BODY = 4,       ///< Rigid body component
        COLLIDER = 5,         ///< Collider component
        AUDIO_SOURCE = 6,     ///< Audio source component
        SCRIPT = 7,           ///< Script component
        PARTICLE_SYSTEM = 8,  ///< Particle system component
        ANIMATION = 9,        ///< Animation component
        SPRITE_RENDERER = 10, ///< 2D sprite renderer component
        SPRITE_ANIMATOR = 11, ///< 2D sprite animation component
        CAMERA_2D = 12,       ///< 2D orthographic camera component
        TILEMAP = 13,         ///< 2D tilemap component
        RIGID_BODY_2D = 14,   ///< 2D physics rigid body
        COLLIDER_2D = 15,     ///< 2D collision shape
        PARALLAX_BG = 16,     ///< Parallax scrolling background
        NINE_SLICE = 17,      ///< Nine-slice sprite for UI
        PIXEL_PERFECT = 18,   ///< Pixel-perfect rendering constraint
        TERRAIN = 19,         ///< Terrain heightmap component

        // Gameplay components
        HEALTH = 20,           ///< Health/damage component
        AI_AGENT = 21,         ///< AI behavior and pathfinding
        SPLINE = 22,           ///< Spline path component
        SPLINE_FOLLOWER = 23,  ///< Entity follows a spline
        DECAL = 24,            ///< Decal projection component
        PROJECTILE = 25,       ///< Projectile/ballistic component
        INTERACTION = 26,      ///< Interaction trigger component
        WEATHER = 27,          ///< Weather zone component
        NETWORK_IDENTITY = 28, ///< Network replication identity

        // Spatial / volume components
        TRIGGER_VOLUME = 29,      ///< Proximity trigger volume (sphere/AABB)
        POST_PROCESS_VOLUME = 30, ///< Post-processing parameter volume
        REFLECTION_PROBE = 31,    ///< Reflection probe (cubemap capture)
        LIGHT_PROBE = 32,         ///< Light probe (SH indirect lighting)
        NAV_OBSTACLE = 33,        ///< NavMesh dynamic obstacle
        WATER_PLANE = 34,         ///< Water surface plane
        FOG_VOLUME = 35,          ///< Local volumetric fog region
        LOD_GROUP = 36,           ///< LOD distance switching group
        SPAWN_POINT = 37,         ///< Generic spawn point marker
        AUDIO_REVERB_ZONE = 38,   ///< Audio reverb/echo zone

        // Gameplay / AI placement components
        WIND_ZONE = 39,         ///< Directional/spherical wind force
        PHYSICS_JOINT = 40,     ///< Physics constraint between bodies
        OCCLUDER = 41,          ///< Occlusion culling proxy geometry
        COVER_POINT = 42,       ///< AI cover position marker
        TACTICAL_POINT = 43,    ///< AI tactical position marker
        DESTRUCTIBLE = 44,      ///< Destructible object with fracture
        CINEMATIC_TRIGGER = 45, ///< Starts a cinematic sequence on enter
        DIALOGUE_TRIGGER = 46,  ///< Starts dialogue tree on interaction
        AREA_BOUNDARY = 47,     ///< Level streaming area boundary
        BILLBOARD = 48,         ///< Always-face-camera sprite

        // Physics / rendering / navigation placement
        AUDIO_LISTENER = 49,       ///< Spatial audio listener position
        CHARACTER_CONTROLLER = 50, ///< Jolt character controller (walking/sliding)
        NAV_REGION = 51,           ///< NavMesh baking region bounds
        NAV_LINK = 52,             ///< Off-mesh navigation link (jumps, ladders)
        SKYBOX = 53,               ///< Sky/atmosphere rendering
        CONSTANT_FORCE = 54,       ///< Persistent force on a rigidbody
        FORCE_REGION = 55,         ///< Physics volume applying forces to bodies
        RAGDOLL = 56,              ///< Ragdoll physics on skeletal mesh
        SOFT_BODY = 57,            ///< Soft body / cloth simulation
        VEHICLE = 58,              ///< Vehicle physics (wheeled/tracked/motorcycle)
        BUOYANCY_VOLUME = 59,      ///< Water buoyancy region
        SPRING_ARM = 60,           ///< Camera collision avoidance arm
        LINE_RENDERER = 61,        ///< World-space line/polyline rendering
        TRAIL_RENDERER = 62,       ///< Trail effect behind moving objects
        TEXT_3D = 63,              ///< World-space 3D text rendering
        FOLIAGE_VOLUME = 64,       ///< Vegetation scatter/placement region

        CUSTOM = 1000 ///< Custom components start at 1000
    };

    /**
 * @brief Light component types
 */
    enum class LightType
    {
        DIRECTIONAL = 0, ///< Directional light (sun)
        POINT = 1,       ///< Point light (bulb)
        SPOT = 2,        ///< Spot light (flashlight)
        AREA = 3         ///< Area light (panel)
    };

    /**
 * @brief Camera projection types
 */
    enum class ProjectionType
    {
        PERSPECTIVE = 0, ///< Perspective projection
        ORTHOGRAPHIC = 1 ///< Orthographic projection
    };

    /**
 * @brief Physics rigid body types
 */
    enum class BodyType
    {
        STATIC = 0,    ///< Static body (doesn't move)
        KINEMATIC = 1, ///< Kinematic body (scripted movement)
        DYNAMIC = 2    ///< Dynamic body (physics simulation)
    };

    /**
 * @brief Collider shape types
 */
    enum class ColliderType
    {
        BOX = 0,     ///< Box collider
        SPHERE = 1,  ///< Sphere collider
        CAPSULE = 2, ///< Capsule collider
        MESH = 3,    ///< Mesh collider
        TERRAIN = 4, ///< Terrain collider
        COMPOUND = 5 ///< Compound collider
    };

    /**
 * @brief Sky rendering types
 */
    enum class SkyType
    {
        SOLID_COLOR = 0, ///< Solid color sky
        GRADIENT = 1,    ///< Gradient sky
        SKYBOX = 2,      ///< Skybox texture
        PROCEDURAL = 3   ///< Procedural sky
    };

    /**
 * @brief Scene loading states
 */
    enum class SceneLoadState
    {
        UNLOADED = 0,  ///< Scene is not loaded
        LOADING = 1,   ///< Scene is loading
        LOADED = 2,    ///< Scene is fully loaded
        UNLOADING = 3, ///< Scene is unloading
        LOAD_ERROR = 4 ///< Error loading scene
    };

    /**
 * @brief Scene object layer types
 */
    enum class ObjectLayer
    {
        DEFAULT = 0,         ///< Default layer
        UI = 1,              ///< UI layer
        BACKGROUND = 2,      ///< Background layer
        FOREGROUND = 3,      ///< Foreground layer
        EFFECTS = 4,         ///< Effects layer
        TRANSPARENT_OBJ = 5, ///< Transparent objects layer
        IGNORE_RAYCAST = 6,  ///< Ignore raycast layer
        WATER = 7,           ///< Water layer
        CUSTOM_1 = 8,        ///< Custom layer 1
        CUSTOM_2 = 9,        ///< Custom layer 2
        CUSTOM_3 = 10,       ///< Custom layer 3
        CUSTOM_4 = 11        ///< Custom layer 4
    };

    /**
 * @brief Transform space types
 */
    enum class TransformSpace
    {
        LOCAL = 0, ///< Local space (relative to parent)
        WORLD = 1, ///< World space (absolute)
        PARENT = 2 ///< Parent space
    };

    /**
 * @brief Animation play modes
 */
    enum class AnimationPlayMode
    {
        ONCE = 0,         ///< Play once
        LOOP = 1,         ///< Loop animation
        PING_PONG = 2,    ///< Ping-pong (back and forth)
        CLAMP_FOREVER = 3 ///< Play once and hold last frame
    };

} // namespace SparkEditor