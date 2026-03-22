/**
 * @file SceneFileTypes.h
 * @brief Scene file type definitions, enums, and data structures
 * @author Spark Engine Team
 * @date 2025
 *
 * This file contains all type definitions, structs, and constants used by
 * the scene file format. Extracted from SceneFile.h to reduce header size
 * and improve compilation times.
 */

#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#ifdef _WIN32
#include <DirectXMath.h>
#else
#include "Core/Platform.h"
#endif
using namespace DirectX;
#include "../Enums/SceneSystemEnums.h"


namespace SparkEditor
{

    /**
 * @brief Unique identifier for scene objects
 */
    using ObjectID = uint64_t;

    /**
 * @brief Invalid/null object ID constant
 */
    constexpr ObjectID INVALID_OBJECT_ID = 0;

    /**
 * @brief Scene file format version for compatibility
 */
    constexpr uint32_t SCENE_FILE_VERSION = 1;

    /**
 * @brief Magic number for scene file identification
 */
    constexpr uint32_t SCENE_FILE_MAGIC = 0x53504B53; // 'SPKS' in ASCII

    /**
 * @brief Scene file header structure
 */
    struct SceneHeader
    {
        uint32_t magic = SCENE_FILE_MAGIC;                ///< Magic number for file type identification
        uint32_t version = SCENE_FILE_VERSION;            ///< File format version
        uint32_t objectCount = 0;                         ///< Number of objects in scene
        uint32_t componentCount = 0;                      ///< Number of components in scene
        uint32_t assetReferenceCount = 0;                 ///< Number of asset references
        uint64_t timestamp = 0;                           ///< Last save timestamp
        char sceneName[64] = {};                          ///< Scene display name
        char description[256] = {};                       ///< Scene description
        XMFLOAT3 gravity = {0.0f, -9.81f, 0.0f};          ///< Scene gravity vector
        XMFLOAT4 ambientColor = {0.2f, 0.2f, 0.2f, 1.0f}; ///< Ambient lighting color
        float ambientIntensity = 1.0f;                    ///< Ambient lighting intensity
    };

    /**
 * @brief Transform component data
 */
    struct Transform
    {
        XMFLOAT3 position = {0.0f, 0.0f, 0.0f};       ///< World position
        XMFLOAT4 rotation = {0.0f, 0.0f, 0.0f, 1.0f}; ///< Rotation quaternion
        XMFLOAT3 scale = {1.0f, 1.0f, 1.0f};          ///< Local scale
        ObjectID parentID = INVALID_OBJECT_ID;        ///< Parent object ID
        std::vector<ObjectID> childIDs;               ///< Child object IDs

        /**
     * @brief Get transform matrix
     * @return 4x4 transform matrix
     */
        XMMATRIX GetMatrix() const;

        /**
     * @brief Set transform from matrix
     * @param matrix 4x4 transform matrix
     */
        void SetFromMatrix(const XMMATRIX& matrix);
    };

    /**
 * @brief Mesh renderer component data
 */
    struct MeshRenderer
    {
        std::string meshAssetPath;                     ///< Path to mesh asset
        std::string materialAssetPath;                 ///< Path to material asset
        bool castShadows = true;                       ///< Whether object casts shadows
        bool receiveShadows = true;                    ///< Whether object receives shadows
        int renderLayer = 0;                           ///< Rendering layer/priority
        XMFLOAT4 tintColor = {1.0f, 1.0f, 1.0f, 1.0f}; ///< Color tint multiplier
    };

    /**
 * @brief Light component data
 */
    struct Light
    {
        enum Type
        {
            DIRECTIONAL = 0,
            POINT = 1,
            SPOT = 2,
            AREA = 3
        };

        Type type = DIRECTIONAL;             ///< Light type
        XMFLOAT3 color = {1.0f, 1.0f, 1.0f}; ///< Light color
        float intensity = 1.0f;              ///< Light intensity
        float range = 10.0f;                 ///< Light range (for point/spot lights)
        float spotAngle = 45.0f;             ///< Spot light cone angle (degrees)
        float spotInnerAngle = 30.0f;        ///< Spot light inner cone angle (degrees)
        bool castShadows = true;             ///< Whether light casts shadows
        int shadowMapSize = 1024;            ///< Shadow map resolution
    };

    /**
 * @brief Camera component data
 */
    struct Camera
    {
        enum ProjectionType
        {
            PERSPECTIVE = 0,
            ORTHOGRAPHIC = 1
        };

        ProjectionType projectionType = PERSPECTIVE;    ///< Camera projection type
        float fieldOfView = 75.0f;                      ///< Field of view (degrees, perspective)
        float orthographicSize = 5.0f;                  ///< Orthographic camera size
        float nearPlane = 0.1f;                         ///< Near clipping plane
        float farPlane = 1000.0f;                       ///< Far clipping plane
        XMFLOAT4 clearColor = {0.2f, 0.3f, 0.5f, 1.0f}; ///< Camera clear color
        bool isMainCamera = false;                      ///< Whether this is the main camera
        int renderTargetWidth = 1920;                   ///< Render target width
        int renderTargetHeight = 1080;                  ///< Render target height
    };

    /**
 * @brief Physics rigid body component data
 */
    struct RigidBody
    {
        enum BodyType
        {
            STATIC = 0,
            KINEMATIC = 1,
            DYNAMIC = 2
        };

        BodyType bodyType = DYNAMIC;                   ///< Physics body type
        float mass = 1.0f;                             ///< Object mass
        float drag = 0.0f;                             ///< Linear drag coefficient
        float angularDrag = 0.05f;                     ///< Angular drag coefficient
        XMFLOAT3 velocity = {0.0f, 0.0f, 0.0f};        ///< Current velocity
        XMFLOAT3 angularVelocity = {0.0f, 0.0f, 0.0f}; ///< Current angular velocity
        bool useGravity = true;                        ///< Whether object is affected by gravity
        bool isKinematic = false;                      ///< Whether object is kinematic
        bool freezePositionX = false;                  ///< Freeze X position
        bool freezePositionY = false;                  ///< Freeze Y position
        bool freezePositionZ = false;                  ///< Freeze Z position
        bool freezeRotationX = false;                  ///< Freeze X rotation
        bool freezeRotationY = false;                  ///< Freeze Y rotation
        bool freezeRotationZ = false;                  ///< Freeze Z rotation
    };

    /**
 * @brief Collider component data
 */
    struct Collider
    {
        enum ColliderType
        {
            BOX = 0,
            SPHERE = 1,
            CAPSULE = 2,
            MESH = 3,
            TERRAIN = 4
        };

        ColliderType type = BOX;              ///< Collider shape type
        XMFLOAT3 center = {0.0f, 0.0f, 0.0f}; ///< Collider center offset
        XMFLOAT3 size = {1.0f, 1.0f, 1.0f};   ///< Collider size (box/capsule)
        float radius = 0.5f;                  ///< Collider radius (sphere/capsule)
        float height = 2.0f;                  ///< Collider height (capsule)
        std::string meshAssetPath;            ///< Mesh asset path (mesh collider)
        bool isTrigger = false;               ///< Whether collider is a trigger
        std::string physicsMaterial;          ///< Physics material asset path
        float friction = 0.6f;                ///< Friction coefficient
        float bounciness = 0.0f;              ///< Bounciness coefficient
    };

    /**
 * @brief Audio source component data
 */
    struct AudioSource
    {
        std::string audioClipPath;  ///< Audio clip asset path
        bool playOnAwake = true;    ///< Play audio when object is created
        bool loop = false;          ///< Loop the audio clip
        float volume = 1.0f;        ///< Audio volume (0-1)
        float pitch = 1.0f;         ///< Audio pitch multiplier
        float spatialBlend = 0.0f;  ///< 2D/3D spatial blend (0=2D, 1=3D)
        float minDistance = 1.0f;   ///< Minimum 3D distance
        float maxDistance = 500.0f; ///< Maximum 3D distance
        int priority = 128;         ///< Audio priority (0-255)
    };

    // =========================================================================
    // 2D/2.5D Scene Component Data
    // =========================================================================

    /**
     * @brief 2D Sprite renderer scene data
     */
    struct SpriteRendererData
    {
        std::string texturePath;
        XMFLOAT4 color = {1.0f, 1.0f, 1.0f, 1.0f};
        XMFLOAT4 sourceRect = {0.0f, 0.0f, 1.0f, 1.0f};
        XMFLOAT2 pivot = {0.5f, 0.5f};
        float pixelsPerUnit = 100.0f;
        int sortingLayer = 0;
        int orderInLayer = 0;
        bool flipX = false;
        bool flipY = false;
    };

    /**
     * @brief 2D Camera scene data
     */
    struct Camera2DData
    {
        float orthoSize = 5.0f;
        float zoom = 1.0f;
        float nearPlane = -100.0f;
        float farPlane = 100.0f;
        float followSmoothing = 0.1f;
        XMFLOAT2 deadZone = {0.5f, 0.5f};
        XMFLOAT4 clearColor = {0.2f, 0.2f, 0.3f, 1.0f};
        bool isMain2DCamera = false;
    };

    /**
     * @brief 2D Physics rigid body scene data
     */
    struct RigidBody2DData
    {
        int bodyType = 2; ///< 0=Static, 1=Kinematic, 2=Dynamic
        float mass = 1.0f;
        float gravityScale = 1.0f;
        float linearDamping = 0.0f;
        float angularDamping = 0.05f;
        float friction = 0.3f;
        float restitution = 0.0f;
        bool fixedRotation = false;
        bool isBullet = false;
    };

    /**
     * @brief 2D Collider scene data
     */
    struct Collider2DData
    {
        int shape = 0; ///< 0=Box, 1=Circle, 2=Capsule, 3=Polygon, 4=Edge
        XMFLOAT2 halfExtents = {0.5f, 0.5f};
        float radius = 0.5f;
        float height = 1.0f;
        XMFLOAT2 offset = {0.0f, 0.0f};
        bool isTrigger = false;
        uint32_t layerMask = 0xFFFFFFFF;
    };

    /**
     * @brief Tilemap scene data (header; tile data stored separately)
     */
    struct TilemapData
    {
        std::string tilesetTexturePath;
        int tileWidth = 16;
        int tileHeight = 16;
        int columns = 0;
        int rows = 0;
        int mapWidth = 0;
        int mapHeight = 0;
        int sortingLayer = 0;
        float pixelsPerUnit = 100.0f;
        bool generateCollision = true;
    };

    /**
     * @brief Terrain component scene data
     */
    struct TerrainSceneData
    {
        int heightmapResolution = 513; ///< Heightmap width/height in samples
        float terrainSize = 1000.0f;   ///< Terrain extent in world units
        float heightScale = 1.0f;      ///< Vertical scale multiplier
        float minHeight = 0.0f;        ///< Minimum terrain height
        float maxHeight = 100.0f;      ///< Maximum terrain height
        int lodLevels = 4;             ///< Number of LOD levels
        float lodBias = 1.0f;          ///< LOD distance bias
        bool generateCollider = true;  ///< Generate physics collision
        bool castShadows = true;       ///< Terrain casts shadows
        bool receiveShadows = true;    ///< Terrain receives shadows
    };

    /**
     * @brief Parallax background scene data
     */
    struct ParallaxLayerData
    {
        std::string texturePath;
        XMFLOAT2 scrollSpeed = {0.5f, 0.5f};
        bool tileX = true;
        bool tileY = false;
        XMFLOAT4 tint = {1.0f, 1.0f, 1.0f, 1.0f};
        int sortOrder = -100;
    };

    // =========================================================================
    // Gameplay Component Data
    // =========================================================================

    /**
     * @brief Script component scene data
     */
    struct ScriptData
    {
        char scriptPath[256] = {}; ///< Path to script file
        char className[128] = {};  ///< Script class name
        bool autoStart = true;     ///< Start script on scene load
    };

    /**
     * @brief Particle emitter scene data
     */
    struct ParticleEmitterData
    {
        char effectName[128] = {};                      ///< Particle effect name
        bool autoPlay = true;                           ///< Auto-start on scene load
        float emissionRate = 10.0f;                     ///< Particles per second
        float lifetime = 1.0f;                          ///< Particle lifetime (seconds)
        XMFLOAT4 startColor = {1.0f, 1.0f, 1.0f, 1.0f}; ///< Initial particle color
        float startSize = 0.1f;                         ///< Initial particle size
        float startSpeed = 1.0f;                        ///< Initial particle speed
        float gravityMultiplier = 0.0f;                 ///< Gravity influence
        int maxParticles = 1000;                        ///< Maximum particle count
        bool loop = true;                               ///< Loop emission
    };

    /**
     * @brief Animation controller scene data
     */
    struct AnimationControllerData
    {
        char defaultAnimation[128] = {}; ///< Default animation clip name
        float playbackSpeed = 1.0f;      ///< Playback speed multiplier
        bool playing = true;             ///< Start playing on load
        bool loop = true;                ///< Loop animation
    };

    /**
     * @brief Nine-slice sprite scene data
     */
    struct NineSliceData
    {
        char texturePath[256] = {};                ///< Texture path
        float borderLeft = 8.0f;                   ///< Left border (pixels)
        float borderTop = 8.0f;                    ///< Top border (pixels)
        float borderRight = 8.0f;                  ///< Right border (pixels)
        float borderBottom = 8.0f;                 ///< Bottom border (pixels)
        XMFLOAT2 size = {1.0f, 1.0f};              ///< World-space size
        XMFLOAT4 color = {1.0f, 1.0f, 1.0f, 1.0f}; ///< Tint color
        bool fillCenter = true;                    ///< Fill center region
        int sortingLayer = 0;                      ///< Render sort layer
    };

    /**
     * @brief Pixel-perfect rendering scene data
     */
    struct PixelPerfectData
    {
        int referenceWidth = 320;  ///< Reference resolution width
        int referenceHeight = 240; ///< Reference resolution height
        bool upscaleToFill = true; ///< Upscale to fill screen
        bool cropToFit = false;    ///< Crop to fit screen
    };

    /**
     * @brief Health component scene data
     */
    struct HealthData
    {
        float health = 100.0f;    ///< Current health
        float maxHealth = 100.0f; ///< Maximum health
    };

    /**
     * @brief AI agent scene data
     */
    struct AIAgentData
    {
        int aiState = 0;                 ///< 0=Idle,1=Patrol,2=Alert,3=Combat,4=Flee,5=Dead
        char behaviorTreeName[128] = {}; ///< Behavior tree template name
        float detectionRange = 30.0f;    ///< Detection radius
        float attackRange = 15.0f;       ///< Attack radius
        float moveSpeed = 5.0f;          ///< Movement speed
        float accuracy = 0.7f;           ///< Aim accuracy [0,1]
        float reactionTime = 0.5f;       ///< Reaction delay (seconds)
    };

    /**
     * @brief Spline path scene data
     */
    struct SplineData
    {
        bool debugVisible = false; ///< Show spline in editor
        bool closed = false;       ///< Closed loop spline
        int pointCount = 0;        ///< Number of control points (data stored separately)
    };

    /**
     * @brief Spline follower scene data
     */
    struct SplineFollowerData
    {
        float speed = 1.0f;       ///< Movement speed
        int loopMode = 0;         ///< 0=Once, 1=Loop, 2=PingPong
        bool playing = true;      ///< Playback active
        bool orientToPath = true; ///< Align rotation to tangent
    };

    /**
     * @brief Decal scene data
     */
    struct DecalData
    {
        char texturePath[256] = {};                ///< Decal texture path
        char category[64] = "generic";             ///< Decal category
        XMFLOAT3 size = {0.1f, 0.1f, 0.05f};       ///< Projection half-extents
        XMFLOAT4 color = {1.0f, 1.0f, 1.0f, 1.0f}; ///< Tint RGBA
        float lifetime = 30.0f;                    ///< Lifetime (0=permanent)
        float fadeOutDuration = 2.0f;              ///< Fade time at end
        bool receiveLighting = true;               ///< Receive scene lighting
        int sortOrder = 0;                         ///< Z-order priority
    };

    /**
     * @brief Projectile scene data
     */
    struct ProjectileData
    {
        int movementType = 1;         ///< 0=Hitscan, 1=Ballistic
        int impactBehavior = 0;       ///< 0=Destroy, 1=Bounce, 2=Pierce, 3=Stick
        float speed = 100.0f;         ///< Projectile speed
        float damage = 25.0f;         ///< Damage on hit
        float gravityScale = 1.0f;    ///< Gravity influence
        float explosionRadius = 0.0f; ///< Explosion radius (0=none)
        float maxRange = 500.0f;      ///< Maximum travel distance
        float maxLifetime = 10.0f;    ///< Maximum lifetime
        int bouncesRemaining = 0;     ///< Bounce count (Bounce mode)
        int piercesRemaining = 0;     ///< Pierce count (Pierce mode)
    };

    /**
     * @brief Interaction scene data
     */
    struct InteractionData
    {
        int interactionType = 0;        ///< 0=Use, 1=Pickup, 2=Hold, 3=Toggle
        char displayName[128] = {};     ///< Display name
        char actionVerb[64] = "Use";    ///< Action prompt text
        float interactionRadius = 2.5f; ///< Interaction range
        float holdDuration = 0.0f;      ///< Hold time for Hold type
        float cooldownDuration = 0.0f;  ///< Cooldown between uses
        int usesRemaining = -1;         ///< -1 = unlimited
        bool showHighlight = true;      ///< Highlight when in range
    };

    /**
     * @brief Weather zone scene data
     */
    struct WeatherData
    {
        int weatherType = 0;                         ///< 0=Clear,1=Cloudy,2=Rain,3=Snow,4=Fog,5=Storm
        float intensity = 0.0f;                      ///< Weather intensity [0,1]
        float windSpeed = 0.0f;                      ///< Wind speed
        XMFLOAT3 windDirection = {1.0f, 0.0f, 0.0f}; ///< Wind direction
        float transitionTime = 3.0f;                 ///< Transition duration (seconds)
        bool enabled = true;                         ///< Weather active
    };

    /**
     * @brief Network identity scene data
     */
    struct NetworkIdentityData
    {
        bool replicateTransform = true; ///< Replicate position/rotation
        bool replicateHealth = true;    ///< Replicate health state
        bool isLocalAuthority = false;  ///< Local authority flag
    };

    // =========================================================================
    // Spatial / Volume Component Data
    // =========================================================================

    /**
     * @brief Trigger volume scene data (sphere or AABB proximity trigger)
     */
    struct TriggerVolumeData
    {
        int shape = 0;                             ///< 0=Sphere, 1=AABB
        float radius = 5.0f;                       ///< Sphere radius
        XMFLOAT3 halfExtents = {5.0f, 5.0f, 5.0f}; ///< AABB half-extents
        char onEnterEvent[128] = {};               ///< Script event on enter
        char onExitEvent[128] = {};                ///< Script event on exit
        bool enabled = true;                       ///< Active state
        bool oneShot = false;                      ///< Fire only once then disable
    };

    /**
     * @brief Post-processing volume scene data
     */
    struct PostProcessVolumeData
    {
        bool isGlobal = false;      ///< Global (always-active) or local (bounded)
        int priority = 0;           ///< Higher priority wins on overlap
        float weight = 1.0f;        ///< Blend weight [0,1]
        float blendDistance = 2.0f; ///< Fade-in distance for local volumes

        // Exposure
        bool overrideExposure = false; ///< Override exposure settings
        float exposure = 0.0f;         ///< Fixed exposure value
        float minEV = -2.0f;           ///< Auto-exposure minimum EV
        float maxEV = 14.0f;           ///< Auto-exposure maximum EV

        // Bloom
        bool overrideBloom = false;  ///< Override bloom settings
        float bloomIntensity = 1.0f; ///< Bloom intensity
        float bloomThreshold = 0.9f; ///< Bloom threshold

        // Color Grading
        bool overrideColorGrading = false; ///< Override color grading
        float saturation = 1.0f;           ///< Color saturation
        float contrast = 1.0f;             ///< Contrast
        float temperature = 0.0f;          ///< Color temperature offset

        // Fog
        bool overrideFog = false; ///< Override fog settings
        float fogDensity = 0.01f; ///< Fog density
        float fogHeight = 0.2f;   ///< Height-based fog falloff
    };

    /**
     * @brief Reflection probe scene data
     */
    struct ReflectionProbeData
    {
        int resolution = 256;                        ///< Cubemap resolution (128/256/512/1024)
        float influenceRadius = 10.0f;               ///< Influence sphere radius
        XMFLOAT3 boxExtents = {5.0f, 5.0f, 5.0f};    ///< Box projection extents
        bool useBoxProjection = false;               ///< Enable parallax correction
        bool isDynamic = false;                      ///< Re-render each frame vs baked
        float refreshInterval = 0.0f;                ///< Dynamic refresh interval (0=every frame)
        int importance = 1;                          ///< Priority for probe blending
        XMFLOAT3 captureOffset = {0.0f, 0.0f, 0.0f}; ///< Offset from entity position
    };

    /**
     * @brief Light probe scene data (SH-encoded indirect illumination)
     */
    struct LightProbeData
    {
        float influenceRadius = 10.0f;             ///< Probe influence radius
        XMFLOAT3 gridSpacing = {4.0f, 4.0f, 4.0f}; ///< Auto-placement grid spacing
        bool baked = false;                        ///< Whether SH coefficients are baked
        int shOrder = 2;                           ///< Spherical harmonics order (1 or 2)
    };

    /**
     * @brief NavMesh obstacle scene data
     */
    struct NavObstacleData
    {
        int shape = 0;                             ///< 0=Box, 1=Cylinder
        XMFLOAT3 halfExtents = {1.0f, 1.0f, 1.0f}; ///< Box half-extents
        float radius = 1.0f;                       ///< Cylinder radius
        float height = 2.0f;                       ///< Cylinder height
        bool carveOnMove = true;                   ///< Re-carve when entity moves
    };

    /**
     * @brief Water plane scene data
     */
    struct WaterPlaneData
    {
        XMFLOAT2 size = {100.0f, 100.0f};                 ///< Water surface size
        XMFLOAT4 shallowColor = {0.2f, 0.5f, 0.6f, 0.8f}; ///< Shallow water color
        XMFLOAT4 deepColor = {0.05f, 0.1f, 0.2f, 0.95f};  ///< Deep water color
        float waveHeight = 0.3f;                          ///< Gerstner wave height
        float waveSpeed = 1.0f;                           ///< Wave animation speed
        float waveFrequency = 0.5f;                       ///< Wave frequency
        float reflectionStrength = 0.5f;                  ///< Reflection intensity [0,1]
        float refractionStrength = 0.3f;                  ///< Refraction intensity [0,1]
        bool receiveShadows = true;                       ///< Receive shadows from above
    };

    /**
     * @brief Fog volume scene data (local volumetric fog region)
     */
    struct FogVolumeData
    {
        XMFLOAT3 halfExtents = {10.0f, 5.0f, 10.0f}; ///< Volume half-extents
        float density = 0.05f;                       ///< Local fog density
        XMFLOAT4 color = {0.7f, 0.7f, 0.7f, 1.0f};   ///< Fog color + opacity
        float falloff = 1.0f;                        ///< Edge falloff exponent
        float heightFalloff = 0.0f;                  ///< Height-based density falloff
    };

    /**
     * @brief LOD group scene data
     */
    struct LODGroupData
    {
        float lodDistance0 = 10.0f;     ///< Distance to switch from LOD0 to LOD1
        float lodDistance1 = 25.0f;     ///< Distance to switch from LOD1 to LOD2
        float lodDistance2 = 50.0f;     ///< Distance to switch from LOD2 to LOD3
        float lodDistance3 = 100.0f;    ///< Distance to switch from LOD3 to cull
        int lodCount = 3;               ///< Number of LOD levels (1-4)
        float crossFadeDuration = 0.5f; ///< Crossfade transition time (seconds)
        bool autoGenerate = true;       ///< Auto-generate LODs from highest detail mesh
    };

    /**
     * @brief Spawn point scene data
     */
    struct SpawnPointData
    {
        char spawnTag[64] = "default"; ///< Tag for spawn group selection (e.g. "team_a", "boss")
        int teamID = 0;                ///< Team assignment (0=neutral)
        float spawnRadius = 1.0f;      ///< Random offset radius around point
        float respawnDelay = 5.0f;     ///< Delay before reuse (seconds)
        int maxConcurrent = -1;        ///< Max simultaneous spawns (-1=unlimited)
        bool enabled = true;           ///< Active state
        int priority = 0;              ///< Selection priority (higher = preferred)
    };

    /**
     * @brief Audio reverb zone scene data
     */
    struct AudioReverbZoneData
    {
        float innerRadius = 5.0f;      ///< Full-effect radius
        float outerRadius = 15.0f;     ///< Fade-out radius
        int reverbPreset = 0;          ///< 0=Generic,1=Room,2=Hall,3=Cave,4=Arena,5=Forest,6=Underwater
        float decayTime = 1.5f;        ///< Reverb decay time (seconds)
        float earlyReflections = 0.5f; ///< Early reflection level [0,1]
        float lateReverbLevel = 0.5f;  ///< Late reverb level [0,1]
        float diffusion = 1.0f;        ///< Reverb diffusion [0,1]
        float roomSize = 0.5f;         ///< Room size factor [0,1]
    };

    // ComponentType is defined in ../Enums/SceneSystemEnums.h to avoid ODR violations

    /**
 * @brief Generic component wrapper
 */
    struct Component
    {
        ComponentType type;        ///< Component type identifier
        ObjectID objectID;         ///< Object this component belongs to
        bool enabled = true;       ///< Whether component is enabled
        std::vector<uint8_t> data; ///< Serialized component data

        /**
     * @brief Get component data as specific type
     * @tparam T Component data type
     * @return Pointer to component data, or nullptr if wrong type
     */
        template <typename T> T* GetData();

        /**
     * @brief Set component data from specific type
     * @tparam T Component data type
     * @param componentData Data to store in component
     */
        template <typename T> void SetData(const T& componentData);
    };

    /**
 * @brief Scene object data
 */
    struct SceneObject
    {
        ObjectID id = INVALID_OBJECT_ID;           ///< Unique object identifier
        std::string name = "GameObject";           ///< Object display name
        std::string tag = "Default";               ///< Object tag for categorization
        int layer = 0;                             ///< Object layer for rendering/physics
        bool active = true;                        ///< Whether object is active
        bool staticObject = false;                 ///< Whether object is static (optimization hint)
        std::vector<ComponentType> componentTypes; ///< Types of components attached

        // Transform component is always present
        Transform transform; ///< Object transform
    };

    /**
 * @brief Asset reference for dependency tracking
 */
    struct AssetReference
    {
        std::string assetPath;                 ///< Path to asset file
        std::string assetType;                 ///< Type of asset (mesh, texture, audio, etc.)
        uint64_t lastModified = 0;             ///< Last modification timestamp
        uint64_t fileSize = 0;                 ///< Asset file size
        std::string checksum;                  ///< Asset file checksum for validation
        std::vector<std::string> dependencies; ///< Other assets this asset depends on
    };

    /**
 * @brief Environment and scene settings
 */
    struct EnvironmentSettings
    {
        // Sky settings
        enum SkyType
        {
            SOLID_COLOR = 0,
            GRADIENT = 1,
            SKYBOX = 2,
            PROCEDURAL = 3
        };

        SkyType skyType = SOLID_COLOR;                    ///< Sky rendering type
        XMFLOAT4 skyColor = {0.5f, 0.8f, 1.0f, 1.0f};     ///< Solid sky color
        XMFLOAT4 horizonColor = {0.9f, 0.9f, 0.9f, 1.0f}; ///< Horizon color (gradient)
        std::string skyboxAssetPath;                      ///< Skybox texture asset path

        // Fog settings
        bool fogEnabled = false;                      ///< Whether fog is enabled
        XMFLOAT4 fogColor = {0.7f, 0.7f, 0.7f, 1.0f}; ///< Fog color
        float fogDensity = 0.01f;                     ///< Fog density
        float fogStart = 10.0f;                       ///< Fog start distance
        float fogEnd = 100.0f;                        ///< Fog end distance

        // Wind settings
        XMFLOAT3 windDirection = {1.0f, 0.0f, 0.0f}; ///< Wind direction vector
        float windStrength = 1.0f;                   ///< Wind strength multiplier
        float windTurbulence = 0.1f;                 ///< Wind turbulence amount

        // Post-processing settings
        bool bloomEnabled = false;      ///< Bloom post-processing
        float bloomIntensity = 1.0f;    ///< Bloom intensity
        float bloomThreshold = 1.0f;    ///< Bloom threshold
        bool tonemappingEnabled = true; ///< Tone mapping
        float exposure = 1.0f;          ///< Exposure adjustment
        float gamma = 2.2f;             ///< Gamma correction
    };

    // Template implementations
    template <typename T> T* Component::GetData()
    {
        if (data.size() != sizeof(T))
        {
            return nullptr;
        }
        return reinterpret_cast<T*>(data.data());
    }

    template <typename T> void Component::SetData(const T& componentData)
    {
        data.resize(sizeof(T));
        memcpy(data.data(), &componentData, sizeof(T));
    }

} // namespace SparkEditor