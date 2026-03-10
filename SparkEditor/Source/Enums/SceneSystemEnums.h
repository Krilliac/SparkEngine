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
        TRANSFORM = 0,       ///< Transform component
        MESH_RENDERER = 1,   ///< Mesh renderer component
        LIGHT = 2,           ///< Light component
        CAMERA = 3,          ///< Camera component
        RIGID_BODY = 4,      ///< Rigid body component
        COLLIDER = 5,        ///< Collider component
        AUDIO_SOURCE = 6,    ///< Audio source component
        SCRIPT = 7,          ///< Script component
        PARTICLE_SYSTEM = 8, ///< Particle system component
        ANIMATION = 9,       ///< Animation component
        CUSTOM = 1000        ///< Custom components start at 1000
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