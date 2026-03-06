/**
 * @file Components.h
 * @brief ECS components and World class for Spark Engine
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * This file defines all Entity Component System (ECS) data types used by Spark Engine.
 * The design follows the pure ECS pattern: components are plain-old-data (POD) structs
 * that hold **state only** — no behaviour. Behaviour is implemented by ECS Systems in
 * ECSystems.h that query and mutate components each frame.
 *
 * The engine uses the **EnTT** library for the backing registry. EnTT stores components
 * in a Structure-of-Arrays (SoA) layout which improves cache performance when systems
 * iterate over large numbers of entities.
 *
 * ## Component categories
 *
 * | Category       | Components                                               |
 * |----------------|----------------------------------------------------------|
 * | Core           | NameComponent, Transform, MeshRenderer, Camera, Script  |
 * | Physics        | RigidBodyComponent, ColliderComponent                    |
 * | Audio          | AudioSourceComponent                                     |
 * | Lighting       | LightComponent                                           |
 * | Particles      | ParticleEmitterComponent                                 |
 * | Animation      | AnimationController                                      |
 * | AI             | AIComponent                                              |
 * | Networking     | NetworkIdentity                                          |
 * | Tags/Metadata  | TagComponent, ActiveComponent, HealthComponent           |
 *
 * ## Usage example
 * @code
 *   World world;
 *   EntityID e = world.CreateEntity("Player");
 *
 *   auto& tf = world.AddComponent<Transform>(e);
 *   tf.position = {0, 1, 0};
 *
 *   auto& rb = world.AddComponent<RigidBodyComponent>(e);
 *   rb.mass    = 80.0f;   // kg
 *   rb.type    = RigidBodyComponent::Type::Dynamic;
 *
 *   auto& hp = world.AddComponent<HealthComponent>(e);
 *   hp.maxHealth = 200.0f;
 *   hp.health    = 200.0f;
 * @endcode
 *
 * @note Opaque `void*` handles (e.g. `physicsBodyHandle`, `audioSourceHandle`) are set
 *       by the corresponding subsystem and must **not** be written to by user code.
 */

#pragma once
#include "../../Core/Platform.h"
#include <entt/entt.hpp>
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <functional>

/**
 * @typedef EntityID
 * @brief Opaque entity identifier provided by the EnTT library.
 *
 * An EntityID is a lightweight handle (typically a 32-bit integer) that
 * uniquely identifies an entity within a given World/registry. Entities
 * with the value `entt::null` are considered invalid.
 */
using EntityID = entt::entity;

// =============================================================================
// NameComponent
// =============================================================================

/**
 * @brief Human-readable name tag attached to an entity.
 *
 * Optional but recommended for all entities that appear in the editor or are
 * referenced by name in scripts. Names are not required to be unique, though
 * uniqueness makes search operations (`World::FindEntityByName`) faster.
 */
struct NameComponent {
    /** @brief Display name of the entity (UTF-8). */
    std::string name;
};

// =============================================================================
// World
// =============================================================================

/**
 * @class World
 * @brief Thin wrapper around an EnTT registry providing the ECS entity/component API.
 *
 * World is the **central hub** of the ECS runtime. It owns the EnTT registry and
 * exposes a strongly-typed interface for creating entities and managing their
 * components. Every Scene owns exactly one World instance.
 *
 * ### Thread safety
 * The underlying EnTT registry is **not thread-safe**. All World operations must
 * be performed from the main thread unless external synchronization is provided.
 *
 * ### Performance notes
 * - Component storage uses SoA layout (via EnTT) — iterating a single component
 *   type is cache-friendly.
 * - Prefer `GetEntitiesWith<A, B>()` views over per-entity queries in hot paths.
 * - Destroying an entity invalidates all iterators currently traversing that
 *   component's storage. Defer destruction until after the iteration loop.
 *
 * @code
 *   World world;
 *
 *   // Create an entity and add components
 *   EntityID player = world.CreateEntity("Player");
 *   world.AddComponent<Transform>(player);
 *   world.AddComponent<HealthComponent>(player);
 *
 *   // Iterate all entities with both Transform and HealthComponent
 *   for (auto [entity, tf, hp] : world.GetEntitiesWith<Transform, HealthComponent>().each())
 *   {
 *       hp.health -= 1.0f * dt;
 *   }
 * @endcode
 */
class World {
public:
    /**
     * @brief Create a new entity and optionally assign it a name.
     *
     * Allocates an entity slot in the EnTT registry. If `name` is non-empty a
     * NameComponent is automatically added so the entity appears correctly in the
     * editor hierarchy.
     *
     * @param name  Optional human-readable name. Empty string skips NameComponent.
     * @return      A valid EntityID handle for the new entity.
     */
    EntityID CreateEntity(const std::string& name = "") {
        EntityID entity = m_registry.create();
        if (!name.empty()) {
            m_registry.emplace<NameComponent>(entity, NameComponent{name});
        }
        return entity;
    }

    /**
     * @brief Immediately destroy an entity and remove all its components.
     *
     * After this call the EntityID becomes invalid. Any pointer or reference to
     * the entity's components becomes dangling. It is safe to call during an
     * iteration **only** if you are not currently iterating that entity's
     * component storage — use a deferred destroy list in that case.
     *
     * @param entity  A valid EntityID to destroy. Passing `entt::null` is a no-op.
     */
    void DestroyEntity(EntityID entity) {
        m_registry.destroy(entity);
    }

    /**
     * @brief Add (emplace) a component of type T to an entity.
     *
     * Constructs the component in-place using the supplied constructor arguments.
     * If the entity already has a component of type T the behaviour is undefined
     * (EnTT asserts in debug builds).
     *
     * @tparam T     Component type to add.
     * @tparam Args  Constructor argument types, deduced automatically.
     * @param  entity  Target entity.
     * @param  args    Arguments forwarded to T's constructor.
     * @return         Reference to the newly created component.
     *
     * @code
     *   auto& rb = world.AddComponent<RigidBodyComponent>(e);
     *   rb.mass = 10.0f;
     * @endcode
     */
    template<typename T, typename... Args>
    T& AddComponent(EntityID entity, Args&&... args) {
        return m_registry.emplace<T>(entity, std::forward<Args>(args)...);
    }

    /**
     * @brief Retrieve a pointer to a component, or nullptr if not present.
     *
     * Non-throwing alternative to a direct registry `get<T>()` call. Use this
     * in optional-component paths where the component may not exist.
     *
     * @tparam T  Component type to look up.
     * @param entity  Entity to query.
     * @return        Pointer to the component, or `nullptr` if not found.
     *
     * @code
     *   if (auto* hp = world.GetComponent<HealthComponent>(entity))
     *       hp->TakeDamage(50.0f);
     * @endcode
     */
    template<typename T>
    T* GetComponent(EntityID entity) {
        return m_registry.try_get<T>(entity);
    }

    /**
     * @brief Check whether an entity has a component of type T.
     *
     * @tparam T  Component type to test for.
     * @param entity  Entity to check.
     * @return        `true` if the entity currently has the component.
     */
    template<typename T>
    bool HasComponent(EntityID entity) {
        return m_registry.all_of<T>(entity);
    }

    /**
     * @brief Remove a component of type T from an entity.
     *
     * If the entity does not have the component this is a no-op (EnTT tolerates
     * this in release; debug builds may assert).
     *
     * @tparam T  Component type to remove.
     * @param entity  Entity to modify.
     */
    template<typename T>
    void RemoveComponent(EntityID entity) {
        m_registry.remove<T>(entity);
    }

    /**
     * @brief Create an EnTT view for entities that have **all** of the listed components.
     *
     * Views are lightweight, lazily evaluated ranges and do **not** copy component data.
     * Use structured bindings with `.each()` for the most readable iteration pattern:
     *
     * @code
     *   for (auto [e, tf, rb] : world.GetEntitiesWith<Transform, RigidBodyComponent>().each())
     *   {
     *       // tf and rb are references into the SoA storage
     *   }
     * @endcode
     *
     * @tparam Components  One or more component types to filter on.
     * @return             An EnTT view that can be iterated with range-for or `.each()`.
     */
    template<typename... Components>
    auto GetEntitiesWith() {
        return m_registry.view<Components...>();
    }

    /**
     * @brief Count all alive entities in the world.
     *
     * Iterates the entity storage once; O(N) where N is the total number of
     * alive entities. Avoid calling this in tight per-frame loops.
     *
     * @return  Number of currently alive entities.
     */
    size_t GetEntityCount() const {
        size_t count = 0;
        for ([[maybe_unused]] auto entity : m_registry.template storage<entt::entity>()->each())
            ++count;
        return count;
    }

    /**
     * @brief Direct access to the underlying EnTT registry.
     *
     * Use sparingly; prefer the typed World API. Direct registry access is needed
     * for advanced EnTT features such as component signals and custom storage.
     *
     * @return  Mutable reference to the EnTT registry.
     */
    entt::registry& GetRegistry() { return m_registry; }

    /**
     * @brief Direct const access to the underlying EnTT registry.
     * @return  Const reference to the EnTT registry.
     */
    const entt::registry& GetRegistry() const { return m_registry; }

private:
    /** @brief The backing EnTT registry that owns all entity and component storage. */
    entt::registry m_registry;
};

// =============================================================================
// Core Components
// =============================================================================

/**
 * @brief 3D spatial transform: position, rotation, and scale with optional parenting.
 *
 * Transform is the most fundamental component; almost every visible or physical
 * entity carries one. The engine's RenderSystem and PhysicsUpdateSystem both rely
 * on Transform to position objects in the world.
 *
 * ### Parenting
 * Set `parent` to a valid EntityID to establish a parent-child relationship. The
 * RenderSystem reads the parent chain to compute the final world-space matrix.
 * Setting `parent = entt::null` (the default) makes the entity a root object.
 *
 * ### Rotation representation
 * Rotation is stored as Euler angles in **radians** (X = pitch, Y = yaw, Z = roll).
 * For complex animations or smooth interpolation, prefer quaternion storage and
 * convert to Euler only when writing back to Transform.
 */
struct Transform {
    /** @brief World-space position (or local-space if the entity has a parent). Units: metres. */
    DirectX::XMFLOAT3 position{0, 0, 0};

    /**
     * @brief Euler rotation angles in radians.
     *
     * Components: X = pitch (up/down tilt), Y = yaw (left/right spin), Z = roll (tilt sideways).
     * Applied in YXZ order to match standard first-person camera conventions.
     */
    DirectX::XMFLOAT3 rotation{0, 0, 0};

    /**
     * @brief Non-uniform scale factor per axis.
     *
     * Defaults to {1, 1, 1} (no scaling). Negative values flip geometry along that axis.
     * Avoid non-uniform scaling on physics bodies as it complicates collision shapes.
     */
    DirectX::XMFLOAT3 scale{1, 1, 1};

    /**
     * @brief Optional parent entity for hierarchical transforms.
     *
     * When set, `position`, `rotation`, and `scale` are interpreted as **local** offsets
     * relative to the parent's world transform. The RenderSystem walks the chain to
     * produce the final world matrix. Defaults to `entt::null` (world-space root).
     */
    EntityID parent = entt::null;

    /**
     * @brief Compute the combined world-space transformation matrix.
     *
     * Builds a TRS (translate × rotate × scale) matrix from the local components.
     * @note Does NOT account for a parent transform; the RenderSystem handles
     *       parent-chain multiplication.
     * @return  Row-major 4×4 world transformation matrix.
     */
    DirectX::XMMATRIX GetWorldMatrix() const;
};

/**
 * @brief Attaches a 3D mesh and material to an entity for rendering.
 *
 * The RenderSystem reads MeshRenderer components and submits draw calls to the
 * GraphicsEngine each frame. Both `meshPath` and `materialPath` are resolved by
 * the AssetPipeline at load time; they support relative and absolute paths.
 *
 * ### Shadow casting
 * Use `castShadows = true` for opaque objects that should contribute to the
 * shadow map. Transparent or small decorative objects may disable shadow casting
 * for performance.
 */
struct MeshRenderer {
    /**
     * @brief Asset path to the 3D mesh file (e.g. "Assets/Meshes/Barrel.fbx").
     *
     * Supported formats: FBX, OBJ, GLTF (loaded via Assimp). The path is relative
     * to the project root unless it starts with a drive letter.
     */
    std::string meshPath;

    /**
     * @brief Asset path to the material override file (e.g. "Assets/Materials/Rust.mat").
     *
     * If empty, the default material embedded in the mesh file is used. The
     * MaterialSystem caches loaded materials by path to avoid duplicate GPU uploads.
     */
    std::string materialPath;

    /** @brief Whether this mesh casts shadows into the shadow map. Default: true. */
    bool castShadows = true;

    /** @brief Whether this mesh receives shadow projection from shadow-casting lights. Default: true. */
    bool receiveShadows = true;

    /** @brief Controls whether the RenderSystem submits this entity for rendering. Default: true. */
    bool visible = true;

    /** @brief World matrix computed by RenderSystem from the entity's Transform. */
    XMFLOAT4X4 cachedWorldMatrix{};

    /** @brief True when Transform has changed and cachedWorldMatrix needs recomputation. */
    bool worldMatrixDirty = true;
};

/**
 * @brief Camera projection parameters for a renderable viewpoint.
 *
 * Entities with a Camera component can serve as the active render camera. Set
 * `isMainCamera = true` on exactly one entity per scene; the GraphicsEngine will
 * use that entity's Transform and Camera to build the view/projection matrices.
 *
 * Multiple cameras are supported (e.g. minimap, cutscene camera) but only the
 * camera marked `isMainCamera` drives the primary viewport by default.
 */
struct Camera {
    /**
     * @brief Vertical field-of-view in degrees.
     *
     * Typical values: 60° (tight/cinematic), 75° (balanced), 90°+ (wide/FPS).
     * Converted internally to radians when building the projection matrix.
     */
    float fov = 60.0f;

    /**
     * @brief Near clipping plane distance in metres.
     *
     * Objects closer than this distance are not rendered. Very small values (e.g.
     * 0.001) cause Z-fighting artefacts; 0.1 is a safe default for most scenes.
     */
    float nearPlane = 0.1f;

    /**
     * @brief Far clipping plane distance in metres.
     *
     * Objects beyond this distance are culled. Balance rendering range against
     * Z-buffer precision: large far planes reduce depth precision. 5000 m is
     * reasonable for outdoor FPS levels.
     */
    float farPlane = 1000.0f;

    /**
     * @brief Marks this camera as the primary viewport camera.
     *
     * Only one camera per World should have this set to `true`. If multiple
     * cameras are marked as main the behaviour is undefined (last one wins).
     */
    bool isMainCamera = false;
};

/**
 * @brief Attaches an AngelScript script to an entity for gameplay logic.
 *
 * The scripting system instantiates `className` from `scriptPath` at level load.
 * Scripts receive `OnStart()`, `OnUpdate(float dt)`, and `OnDestroy()` callbacks
 * from the scripting runtime. Scripts can access their entity's components via
 * the engine API exposed to AngelScript.
 *
 * ### Hot reload
 * Changing the script file on disk while the editor is open triggers an automatic
 * hot-reload: the old instance is destroyed and a fresh one is created, preserving
 * the entity's other components.
 */
struct Script {
    /**
     * @brief File path to the AngelScript source file (e.g. "Assets/Scripts/Enemy.as").
     *
     * Relative to the project root. The scripting engine compiles and caches the
     * module identified by `moduleName`.
     */
    std::string scriptPath;

    /**
     * @brief Name of the AngelScript class to instantiate from `scriptPath`.
     *
     * Must match exactly (case-sensitive) the class name declared in the script file.
     */
    std::string className;

    /**
     * @brief AngelScript module name used for compilation and caching.
     *
     * Typically derived from the script file name. Two scripts can share a module
     * if they define different classes within the same file.
     */
    std::string moduleName;

    /** @brief When false, the script's `OnUpdate()` is skipped but `OnStart()` still runs. */
    bool enabled = true;

    /** @brief Set to true by the scripting runtime after `OnStart()` has been called. Do not write manually. */
    bool started = false;
};

// =============================================================================
// Physics Components
// =============================================================================

/**
 * @brief Describes the physics simulation behaviour of an entity.
 *
 * Adding a RigidBodyComponent to an entity registers it with the PhysicsSystem
 * (backed by Bullet Physics). The PhysicsUpdateSystem then syncs the entity's
 * Transform with the simulated rigid body each frame.
 *
 * ### Body types
 * - **Static**    – Never moves; ideal for terrain, walls, and environmental geometry.
 * - **Kinematic** – Moves under animator/code control (e.g. platforms, doors); not
 *                   affected by forces but pushes Dynamic bodies.
 * - **Dynamic**   – Fully simulated: responds to gravity, forces, and collisions.
 *
 * ### Interaction with ColliderComponent
 * A RigidBodyComponent alone does not define a collision shape. Add a ColliderComponent
 * to the same entity to specify the shape used for collision detection.
 *
 * @warning `physicsBodyHandle` is an opaque pointer managed by the PhysicsSystem.
 *          Never write to this field from user code.
 */
struct RigidBodyComponent {
    /**
     * @brief Simulation body type.
     *
     * Determines how the physics engine moves (or doesn't move) the body.
     */
    enum class Type { Static, Kinematic, Dynamic };

    /** @brief The simulation type for this body. Default: Dynamic. */
    Type type = Type::Dynamic;

    /**
     * @brief Mass of the body in kilograms.
     *
     * Only meaningful for Dynamic bodies. Setting mass to 0 on a Dynamic body
     * may produce unexpected results; use Static instead.
     */
    float mass = 1.0f;

    /**
     * @brief Coulomb friction coefficient (0 = frictionless, 1 = very grippy).
     *
     * Combined with the friction of the surface being contacted to determine the
     * frictional force. Typical values: 0.2 (ice) to 0.8 (rubber).
     */
    float friction = 0.5f;

    /**
     * @brief Coefficient of restitution / bounciness (0 = inelastic, 1 = perfectly elastic).
     *
     * Controls how much kinetic energy is preserved after a collision. Typical game
     * objects use 0.0–0.3; bouncy balls use 0.6–0.9.
     */
    float restitution = 0.1f;

    /**
     * @brief Linear velocity damping factor applied each simulation step.
     *
     * Simulates air resistance and general drag. Range [0, 1]; higher values make the
     * body decelerate faster. 0.1 is a good general-purpose starting value.
     */
    float linearDamping = 0.1f;

    /**
     * @brief Angular velocity damping factor applied each simulation step.
     *
     * Controls how quickly spinning motion slows down. Increase this to prevent
     * objects from tumbling indefinitely.
     */
    float angularDamping = 0.1f;

    /**
     * @brief When true, the body acts as a trigger volume.
     *
     * Trigger volumes generate overlap/enter/exit callbacks but do NOT generate
     * physical collision response (objects pass through them). Useful for
     * damage zones, checkpoints, and pick-up detection radii.
     */
    bool isTrigger = false;

    /** @brief Current linear velocity of the body (metres per second). Written by the physics simulation. */
    DirectX::XMFLOAT3 linearVelocity{0, 0, 0};

    /** @brief Current angular velocity of the body (radians per second). Written by the physics simulation. */
    DirectX::XMFLOAT3 angularVelocity{0, 0, 0};

    /**
     * @brief Opaque handle to the PhysicsBody object managed by the PhysicsSystem.
     *
     * Set internally when the PhysicsSystem creates the Bullet rigid body. Do not
     * read or write this pointer from user or script code.
     */
    void* physicsBodyHandle = nullptr;
};

/**
 * @brief Defines the collision shape used for physics queries and contact generation.
 *
 * A ColliderComponent specifies the geometry that the physics engine uses to detect
 * overlaps and contacts. It is interpreted alongside RigidBodyComponent to create
 * the full physics actor.
 *
 * ### Shape–field mapping
 * | Shape   | Relevant fields                          |
 * |---------|------------------------------------------|
 * | Box     | `halfExtents`                            |
 * | Sphere  | `radius`                                 |
 * | Capsule | `radius`, `height`                       |
 * | Mesh    | Derived from the entity's MeshRenderer   |
 *
 * @note Mesh shapes are expensive for Dynamic bodies; prefer Box or Capsule for
 *       moving objects and reserve Mesh shapes for Static environment geometry.
 */
struct ColliderComponent {
    /**
     * @brief Available primitive collision shape types.
     */
    enum class Shape { Box, Sphere, Capsule, Mesh };

    /** @brief The collision shape type. Default: Box. */
    Shape shape = Shape::Box;

    /**
     * @brief Half-extents of a Box collider in each axis (metres).
     *
     * For example {0.5, 1.0, 0.5} creates a box 1 m wide, 2 m tall, and 1 m deep.
     * Only used when `shape == Shape::Box`.
     */
    DirectX::XMFLOAT3 halfExtents{0.5f, 0.5f, 0.5f};

    /**
     * @brief Radius of a Sphere or Capsule collider (metres).
     *
     * Used when `shape == Shape::Sphere` or `shape == Shape::Capsule`.
     */
    float radius = 0.5f;

    /**
     * @brief Total height of a Capsule collider (metres), including the two end hemispheres.
     *
     * The cylindrical body height = `height - 2 * radius`. Ensure `height >= 2 * radius`.
     * Only used when `shape == Shape::Capsule`.
     */
    float height = 1.0f;

    /**
     * @brief Local-space offset of the collider from the entity's Transform origin.
     *
     * Use this to reposition the collision shape without moving the visual mesh.
     * For example, offset a Capsule upward so it aligns with an upright character.
     */
    DirectX::XMFLOAT3 offset{0, 0, 0};
};

// =============================================================================
// Audio Components
// =============================================================================

/**
 * @brief Attaches a spatialized or 2D audio source to an entity.
 *
 * The AudioUpdateSystem reads this component each frame and updates the 3D
 * position/velocity of the underlying XAudio2 source voice based on the entity's
 * Transform. For 2D (non-spatialized) sounds set `is3D = false`.
 *
 * ### Lifecycle
 * - If `playOnAwake = true`, the AudioUpdateSystem starts playback on the first
 *   frame the component is encountered.
 * - Looping sources play indefinitely until stopped programmatically.
 * - The `isPlaying` flag reflects current playback state; write it to start/stop.
 *
 * @warning `audioSourceHandle` is managed by the AudioEngine. Do not modify it.
 */
struct AudioSourceComponent {
    /**
     * @brief Name of the sound effect to play, as registered with AudioEngine::LoadSound().
     *
     * Must match a sound name already loaded into the AudioEngine before playback starts.
     */
    std::string soundName;

    /** @brief Playback volume (0.0 = silent, 1.0 = full). Applied before 3D attenuation. */
    float volume = 1.0f;

    /** @brief Pitch multiplier (1.0 = normal, 2.0 = one octave up, 0.5 = one octave down). */
    float pitch = 1.0f;

    /**
     * @brief Minimum distance (metres) at which full volume is heard.
     *
     * For distances shorter than `minDistance` the volume does not increase further.
     * Only relevant when `is3D = true`.
     */
    float minDistance = 1.0f;

    /**
     * @brief Maximum audible distance (metres).
     *
     * Beyond this distance the sound is inaudible (volume = 0). Set to a large value
     * for ambient sounds that should be heard everywhere in a level.
     */
    float maxDistance = 50.0f;

    /** @brief Enables 3D positional audio based on the entity's Transform. Default: true. */
    bool is3D = true;

    /** @brief When true, the sound repeats indefinitely until explicitly stopped. */
    bool loop = false;

    /** @brief Automatically start playback when the component is first processed. */
    bool playOnAwake = false;

    /** @brief Current playback state. Set to `true` to begin playback, `false` to stop. */
    bool isPlaying = false;

    /**
     * @brief Opaque handle to the runtime AudioSource object in the AudioEngine pool.
     *
     * Managed by the AudioUpdateSystem. Do not read or modify from external code.
     */
    void* audioSourceHandle = nullptr;

    /** @brief Previous frame position used to compute velocity for Doppler effects. */
    XMFLOAT3 previousPosition{0, 0, 0};
};

// =============================================================================
// Lighting Components
// =============================================================================

/**
 * @brief Defines a dynamic light source attached to an entity.
 *
 * The LightingSystem collects all entities with a LightComponent each frame and
 * submits them to the GraphicsEngine for shadow map generation and light accumulation.
 *
 * ### Light types
 * - **Directional** – Infinite parallel light (sun/moon). Position ignored; uses the
 *                     entity's rotation to determine light direction.
 * - **Point**       – Omnidirectional point light with distance-based attenuation,
 *                     controlled by `range`.
 * - **Spot**        – Conical spot light. Uses `spotAngle` for the outer cone and
 *                     `spotInnerAngle` for the smooth inner falloff cone.
 *
 * ### Shadow maps
 * Shadow map generation is expensive. Keep `castShadows = false` on lights that do
 * not need precise shadows (e.g. fill lights, rim lights). Use `shadowMapResolution`
 * to trade shadow quality against GPU memory.
 */
struct LightComponent {
    /** @brief Discriminates between Directional, Point, and Spot variants. */
    enum class Type { Directional, Point, Spot };

    /** @brief Type of light. Default: Point. */
    Type type = Type::Point;

    /**
     * @brief Linear RGB colour of the emitted light.
     *
     * Components are in [0, 1] range for SDR; values above 1.0 represent HDR
     * emission when the rendering pipeline has HDR enabled. Default: white {1, 1, 1}.
     */
    DirectX::XMFLOAT3 color{1, 1, 1};

    /**
     * @brief Scalar intensity multiplier applied to `color`.
     *
     * Higher values make the light brighter. For physically-based rendering use
     * luminous intensity in candela; for legacy rendering any positive float works.
     */
    float intensity = 1.0f;

    /**
     * @brief Effective radius (metres) for Point and Spot lights.
     *
     * The light has no effect beyond this distance. Smaller ranges improve
     * culling performance; use the minimum range that covers the intended area.
     */
    float range = 10.0f;

    /**
     * @brief Half-angle of the Spot light outer cone in degrees.
     *
     * Determines the total spread of the spot: a 45° value means the cone is
     * 90° wide. Objects outside this cone receive no illumination.
     * Only used when `type == Type::Spot`.
     */
    float spotAngle = 45.0f;

    /**
     * @brief Half-angle of the Spot light inner cone in degrees.
     *
     * The region between `spotInnerAngle` and `spotAngle` has a smooth penumbra
     * falloff. Set equal to `spotAngle` for a hard-edged spot.
     * Only used when `type == Type::Spot`.
     */
    float spotInnerAngle = 30.0f;

    /**
     * @brief Enable real-time shadow casting for this light.
     *
     * Shadow-casting lights require a shadow map render pass, which has a
     * significant performance cost. Limit the number of shadow-casting lights
     * per scene (typically 1–3 for real-time use).
     */
    bool castShadows = false;

    /**
     * @brief Resolution (pixels) of the shadow map texture.
     *
     * Powers of two only (e.g. 512, 1024, 2048). Higher values sharpen shadow
     * edges at the cost of GPU memory and bandwidth. 1024 is a sensible default.
     * Only relevant when `castShadows = true`.
     */
    int shadowMapResolution = 1024;
};

// =============================================================================
// Particle Components
// =============================================================================

/**
 * @brief Attaches a particle effect emitter to an entity.
 *
 * The ParticleSystem processes entities with this component each frame, spawning
 * and updating particle instances according to the effect preset named by
 * `effectName`. Particles are emitted relative to the entity's world Transform.
 *
 * @warning `emitterHandle` is managed by the ParticleSystem. Do not write to it.
 */
struct ParticleEmitterComponent {
    /**
     * @brief Name of the particle effect preset to instantiate.
     *
     * Must match a preset registered with the ParticleSystem before the entity
     * is first processed. Effect presets define emitter rates, textures, and
     * simulation parameters.
     */
    std::string effectName;

    /** @brief Automatically start emission when the component is first processed. Default: true. */
    bool autoPlay = true;

    /** @brief Current emission state. Set false to pause without destroying the emitter. */
    bool isPlaying = false;

    /**
     * @brief Number of particles emitted per second.
     *
     * Combined with `lifetime` this controls the steady-state particle count:
     * count ≈ emissionRate * lifetime.
     */
    float emissionRate = 10.0f;

    /**
     * @brief Duration each particle lives before being recycled (seconds).
     */
    float lifetime = 1.0f;

    /**
     * @brief Initial RGBA colour of newly spawned particles.
     *
     * Alpha channel controls initial transparency (0 = invisible, 1 = opaque).
     * Many effect presets animate colour over the particle's lifetime.
     */
    DirectX::XMFLOAT4 startColor{1, 1, 1, 1};

    /** @brief Initial size of each particle in world units. */
    float startSize = 0.1f;

    /** @brief Initial speed of each particle in metres per second along its spawn direction. */
    float startSpeed = 1.0f;

    /**
     * @brief Opaque handle to the runtime ParticleEmitter object.
     *
     * Set by the ParticleSystem when the effect is instantiated. Do not modify.
     */
    void* emitterHandle = nullptr;
};

// =============================================================================
// Animation Components
// =============================================================================

/**
 * @brief Controls skeletal animation playback for a mesh entity.
 *
 * When combined with a MeshRenderer that uses a skinned mesh, the AnimationUpdateSystem
 * evaluates the animation clip each frame and uploads the resulting bone matrices to
 * the GPU for GPU skinning.
 *
 * ### Layers and blending
 * For simple single-clip animation use `currentAnimation` and `playbackSpeed`.
 * For blended animations (e.g. upper-body shooting while legs run) use the full
 * Spark::Animation::AnimationInstance API accessed via `animInstanceHandle`.
 *
 * @warning `animInstanceHandle` is managed by the AnimationUpdateSystem.
 */
struct AnimationController {
    /**
     * @brief Name of the animation clip currently being played.
     *
     * Must match a clip registered with the AnimationManager. An empty string
     * plays nothing and holds the mesh in its bind pose.
     */
    std::string currentAnimation;

    /**
     * @brief Clip to play when no other animation is active.
     *
     * Typically set to an idle animation so the character has a fallback.
     */
    std::string defaultAnimation;

    /**
     * @brief Playback speed multiplier.
     *
     * 1.0 = normal speed, 2.0 = double speed, 0.5 = half speed, negative values
     * play the animation in reverse.
     */
    float playbackSpeed = 1.0f;

    /**
     * @brief Current time position within the active clip (seconds).
     *
     * Written by the AnimationUpdateSystem each frame. Read this to synchronize
     * gameplay events (e.g. footstep sounds) with animation keyframes.
     */
    float currentTime = 0.0f;

    /** @brief Enable or pause animation evaluation without changing the current time. */
    bool playing = true;

    /** @brief When true the clip restarts from the beginning when it reaches its end. */
    bool loop = true;

    /** @brief Total duration of the current animation clip in seconds. 0 = unknown/infinite. */
    float duration = 0.0f;

    /** @brief Normalized playback progress [0, 1]. Computed by AnimationUpdateSystem. */
    float normalizedTime = 0.0f;

    /**
     * @brief All clip names available for this entity's skeleton.
     *
     * Populated by the AnimationUpdateSystem at load time. Scripts can read this
     * to present valid animation choices in UI or trigger transitions.
     */
    std::vector<std::string> availableAnimations;

    /**
     * @brief Opaque handle to the Spark::Animation::AnimationInstance runtime object.
     *
     * Provides access to the full blending and IK API. Managed by the AnimationUpdateSystem.
     * Do not write to this pointer.
     */
    void* animInstanceHandle = nullptr;
};

// =============================================================================
// AI Components
// =============================================================================

/**
 * @brief Drives NPC behaviour using a behavior tree and navmesh-based pathfinding.
 *
 * Entities with an AIComponent are processed by the AIUpdateSystem each frame.
 * The system evaluates the behavior tree referenced by `behaviorTreeName`, updates
 * perception (sight, hearing), plans a path via the NavMesh, and translates that
 * into Transform updates.
 *
 * ### State machine
 * The `state` enum tracks the agent's high-level intent. Behavior tree actions
 * write to this field to communicate what the agent is currently doing (which the
 * HUD or audio system may react to). External code can also write `state` to force
 * an agent into a specific mode (e.g. `Dead` to disable AI processing).
 *
 * ### Config
 * The nested `Config` struct controls perception and combat parameters. Adjust
 * `detectionRange`, `attackRange`, and `accuracy` to tune difficulty.
 *
 * @warning Both opaque handle fields are managed by the AISystem.
 */
struct AIComponent {
    /**
     * @brief High-level AI state communicated between the behavior tree and external systems.
     */
    enum class State { Idle, Patrolling, Alert, Combat, Fleeing, Dead };

    /** @brief The agent's current high-level state. Default: Idle. */
    State state = State::Idle;

    /**
     * @brief Name of the behavior tree template to instantiate for this agent.
     *
     * Must match a tree registered via AISystem::RegisterBehavior(). The AISystem
     * clones the template on first use.
     */
    std::string behaviorTreeName;

    /**
     * @brief Opaque handle to the agent's private BehaviorTree instance.
     *
     * Created and owned by the AISystem when the behavior is first evaluated.
     * Do not modify from user code.
     */
    void* behaviorTreeHandle = nullptr;

    /**
     * @brief Opaque handle to the NavMeshQuery object used for pathfinding.
     *
     * Created by the AISystem from the active NavMesh at agent initialization.
     * Do not modify from user code.
     */
    void* navQueryHandle = nullptr;

    // ---- Perception data ----------------------------------------------------

    /** @brief EntityID of the current primary target (e.g. the player). `entt::null` if no target. */
    EntityID targetEntity = entt::null;

    /** @brief Last confirmed world position of the target, used when line-of-sight is lost. */
    DirectX::XMFLOAT3 lastKnownTargetPos{0, 0, 0};

    /**
     * @brief Time in seconds since the target was last directly observed.
     *
     * The behavior tree uses this to decide when to give up a search or return to patrol.
     */
    float timeSinceLastSeen = 0.0f;

    /**
     * @brief Duration (seconds) the agent has been in Alert state.
     *
     * Allows behavior tree conditions like "alert for > 3 seconds → enter Combat".
     */
    float alertTimer = 0.0f;

    // ---- Pathfinding data ---------------------------------------------------

    /**
     * @brief Sequence of waypoints produced by the last successful pathfinding query.
     *
     * Updated by the AISystem when the agent requests a new path. The movement
     * subsystem advances `currentPathIndex` as waypoints are reached.
     */
    std::vector<DirectX::XMFLOAT3> currentPath;

    /** @brief Index into `currentPath` pointing to the next waypoint to navigate towards. */
    size_t currentPathIndex = 0;

    // ---- Per-agent configuration --------------------------------------------

    /**
     * @brief Tunable parameters that control perception range, speed, and combat ability.
     */
    struct Config {
        /** @brief Radius (metres) within which the agent detects the player. */
        float detectionRange = 30.0f;

        /** @brief Radius (metres) within which the agent can engage the player with ranged attacks. */
        float attackRange = 15.0f;

        /** @brief Movement speed (metres/second) during pathfinding traversal. */
        float moveSpeed = 5.0f;

        /** @brief Delay (seconds) between the agent perceiving a threat and reacting to it. */
        float reactionTime = 0.5f;

        /**
         * @brief Shot accuracy factor in [0, 1].
         *
         * 1.0 = perfect accuracy; 0.0 = completely random. Combined with range to
         * determine the spread applied to outgoing projectiles.
         */
        float accuracy = 0.7f;
    } config;
};

// =============================================================================
// Networking Components
// =============================================================================

/**
 * @brief Marks an entity for network synchronization in multiplayer sessions.
 *
 * Entities with a NetworkIdentity are tracked by the NetworkManager and may have
 * their state replicated to remote clients. The `ownerClientID` determines which
 * client has authoritative control over the entity.
 *
 * ### Authority model
 * - **Server-authoritative**: Only the server writes Transform/Health; clients receive updates.
 * - **Client-authoritative**: The owning client (`ownerClientID`) can write state;
 *   other clients receive replicated snapshots.
 *
 * Set `isLocalAuthority = true` only on entities that this client owns and controls.
 */
struct NetworkIdentity {
    /** @brief Unique network-wide entity identifier assigned by the server at spawn. */
    uint32_t networkID = 0;

    /** @brief ID of the client that has authoritative ownership of this entity. */
    uint32_t ownerClientID = 0;

    /**
     * @brief True when this machine is the authoritative owner of the entity.
     *
     * Only the authoritative client/server should apply local physics simulation
     * and write position/rotation to avoid conflicting updates.
     */
    bool isLocalAuthority = false;

    /** @brief Whether Transform changes should be replicated to remote clients. */
    bool replicateTransform = true;

    /** @brief Whether HealthComponent changes should be replicated to remote clients. */
    bool replicateHealth = true;
};

// =============================================================================
// Tag and Metadata Components
// =============================================================================

/**
 * @brief Flexible string-based tagging for categorizing and querying entities.
 *
 * Tags are free-form strings used to group or identify entities without requiring
 * dedicated components. Examples: "Enemy", "Destructible", "Collectible", "SpawnPoint".
 *
 * Tags are stored in a vector so an entity can carry multiple tags. For performance-
 * sensitive lookups across many entities, prefer dedicated components or ECS views
 * over tag-based searches.
 *
 * @code
 *   auto& tags = world.AddComponent<TagComponent>(entity);
 *   tags.AddTag("Enemy");
 *   tags.AddTag("Elite");
 *
 *   if (tags.HasTag("Enemy")) { ... }
 * @endcode
 */
struct TagComponent {
    /** @brief List of string tags attached to this entity. */
    std::vector<std::string> tags;

    /**
     * @brief Check whether the entity has a specific tag.
     * @param tag  Tag string to search for (case-sensitive).
     * @return     `true` if the tag is present.
     */
    bool HasTag(const std::string& tag) const {
        for (const auto& t : tags)
            if (t == tag) return true;
        return false;
    }

    /**
     * @brief Add a tag if it is not already present (no duplicates).
     * @param tag  Tag string to add.
     */
    void AddTag(const std::string& tag) {
        if (!HasTag(tag))
            tags.push_back(tag);
    }
};

/**
 * @brief Simple boolean flag indicating whether an entity should be processed by systems.
 *
 * Systems that respect this component skip inactive entities during their update pass.
 * Deactivating an entity with `active = false` is a lightweight alternative to
 * destroying and re-creating it (components are preserved).
 */
struct ActiveComponent {
    /**
     * @brief When false, most ECS systems will skip this entity.
     *
     * Note: not all systems check this flag. Systems that always need to process
     * an entity (e.g. network replication) may ignore it.
     */
    bool active = true;
};

/**
 * @brief Tracks hit points for any entity that can be damaged or healed.
 *
 * The LifecycleSystem monitors HealthComponent each frame and fires a death
 * callback when `isDead` becomes true. Damage and healing are applied through
 * the provided helper methods which enforce the valid [0, maxHealth] range and
 * update `isDead` automatically.
 *
 * @code
 *   auto& hp = world.GetComponent<HealthComponent>(entity);
 *   hp.TakeDamage(25.0f);                    // Subtract 25 HP
 *   if (hp.isDead) HandleDeath(entity);
 * @endcode
 */
struct HealthComponent {
    /** @brief Current hit points. Clamped to [0, maxHealth] by TakeDamage/Heal. */
    float health = 100.0f;

    /** @brief Maximum hit points. Can be modified to apply health bonuses or upgrades. */
    float maxHealth = 100.0f;

    /** @brief Set to true automatically when health reaches 0; cleared on Heal(). */
    bool isDead = false;

    /** @brief Guards against the LifecycleSystem firing the death callback more than once. */
    bool deathProcessed = false;

    /**
     * @brief Subtract `amount` hit points, clamping health to zero and flagging death.
     *
     * @param amount  Damage to deal (positive float). Negative values are ignored.
     */
    void TakeDamage(float amount) {
        health = (std::max)(health - amount, 0.0f);
        isDead = (health <= 0.0f);
    }

    /**
     * @brief Restore `amount` hit points, clamping to `maxHealth` and clearing the dead flag.
     *
     * @param amount  Hit points to restore (positive float).
     */
    void Heal(float amount) {
        health = (std::min)(health + amount, maxHealth);
        isDead = false;
        deathProcessed = false;
    }
};

// =============================================================================
// Weather Component
// =============================================================================

/**
 * @brief Per-scene weather configuration for the weather system.
 *
 * Attach to a scene root entity to define the active weather parameters.
 * The WeatherSystem reads this component to drive precipitation, fog,
 * and lighting modifications.
 *
 * @code
 *   auto& weather = world.AddComponent<WeatherComponent>(sceneEntity);
 *   weather.weatherType = 2;    // Rain
 *   weather.intensity = 0.8f;
 * @endcode
 */
struct WeatherComponent {
    /** @brief Active weather type (maps to Spark::WeatherType enum values). */
    int weatherType = 0;               // 0=Clear, 1=Cloudy, 2=Rain, 3=Snow, 4=Fog, 5=Storm

    /** @brief Weather intensity [0, 1]. */
    float intensity = 0.0f;

    /** @brief Wind direction (normalized). */
    float windX = 1.0f, windY = 0.0f, windZ = 0.0f;

    /** @brief Wind speed in m/s. */
    float windSpeed = 0.0f;

    /** @brief Transition time when switching weather types (seconds). */
    float transitionTime = 3.0f;

    /** @brief Whether the weather system is active for this scene. */
    bool enabled = true;
};

// =============================================================================
// Inventory Component
// =============================================================================

/**
 * @brief Lightweight ECS inventory reference for entities with inventories.
 *
 * Stores basic inventory metadata as an ECS component. The full inventory
 * data (slots, items) lives in Spark::InventoryComponent from InventorySystem.h.
 * This component marks an entity as having an inventory and stores capacity info.
 *
 * @code
 *   auto& inv = world.AddComponent<InventoryTag>(entity);
 *   inv.maxSlots = 30;
 *   inv.maxWeight = 150.0f;
 * @endcode
 */
struct InventoryTag {
    int maxSlots = 20;                 ///< Maximum inventory slots
    float maxWeight = 100.0f;          ///< Maximum carry weight in kg
    int currency = 0;                  ///< Currency/gold amount
    bool hasInventory = true;          ///< Whether inventory is active
};

// =============================================================================
// Quest Journal Component
// =============================================================================

/**
 * @brief ECS marker for entities that can accept and track quests.
 *
 * Stores quest tracking metadata. The full quest journal data lives in
 * Spark::QuestJournalComponent from QuestSystem.h.
 *
 * @code
 *   auto& qj = world.AddComponent<QuestTrackerTag>(entity);
 *   qj.activeQuestCount = 3;
 * @endcode
 */
struct QuestTrackerTag {
    int activeQuestCount = 0;          ///< Number of currently active quests
    int completedQuestCount = 0;       ///< Total completed quests
    bool questLogOpen = false;         ///< UI state: quest log visible
};
