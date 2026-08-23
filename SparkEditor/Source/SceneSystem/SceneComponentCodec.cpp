/**
 * @file SceneComponentCodec.cpp
 * @brief Built-in scene component schema registry.
 */
#include "SceneComponentCodec.h"

#include <functional>
#include <tuple>
#include <typeindex>

namespace SparkEditor
{
    namespace
    {
        template <typename Object, typename Member> struct NamedMember
        {
            std::string_view name;
            Member Object::*member;
        };

        template <typename Object, typename Member>
        constexpr NamedMember<Object, Member> Field(std::string_view name, Member Object::*member)
        {
            return {name, member};
        }

        struct Codec
        {
            ComponentType type;
            const char* name;
            std::type_index payloadType;
            std::function<std::any()> makeDefault;
            std::function<bool(const std::any&, SceneComponentFieldWriter&)> encode;
            std::function<bool(const SceneComponentFieldReader&, std::any&)> decode;
        };

        template <typename T> bool ValidatePayload(const T&) { return true; }
        template <typename T> bool InRange(T value, T minimum, T maximum)
        {
            return value >= minimum && value <= maximum;
        }
        bool Unit(float value) { return InRange(value, 0.0f, 1.0f); }
        bool Positive2(const XMFLOAT2& value) { return value.x > 0.0f && value.y > 0.0f; }
        bool Positive3(const XMFLOAT3& value) { return value.x > 0.0f && value.y > 0.0f && value.z > 0.0f; }
        bool ValidatePayload(const Light& value)
        {
            return value.type >= Light::DIRECTIONAL && value.type <= Light::AREA && value.intensity >= 0.0f &&
                   value.range > 0.0f && value.spotAngle > 0.0f && value.spotAngle <= 180.0f &&
                   value.spotInnerAngle >= 0.0f && value.spotInnerAngle <= value.spotAngle &&
                   value.shadowMapSize >= 128 && value.shadowMapSize <= 8192 &&
                   (value.shadowMapSize & (value.shadowMapSize - 1)) == 0;
        }
        bool ValidatePayload(const Camera& value)
        {
            return value.projectionType >= Camera::PERSPECTIVE && value.projectionType <= Camera::ORTHOGRAPHIC &&
                   value.fieldOfView > 0.0f && value.fieldOfView < 180.0f && value.orthographicSize > 0.0f &&
                   value.nearPlane > 0.0f && value.farPlane > value.nearPlane && value.renderTargetWidth > 0 &&
                   value.renderTargetHeight > 0;
        }
        bool ValidatePayload(const RigidBody& value)
        {
            return value.bodyType >= RigidBody::STATIC && value.bodyType <= RigidBody::DYNAMIC &&
                   (value.bodyType != RigidBody::DYNAMIC || value.mass > 0.0f) && value.mass >= 0.0f &&
                   value.drag >= 0.0f && value.angularDrag >= 0.0f;
        }
        bool ValidatePayload(const Collider& value)
        {
            return value.type >= Collider::BOX && value.type <= Collider::TERRAIN && Positive3(value.size) &&
                   value.radius > 0.0f && value.height > 0.0f && value.friction >= 0.0f && Unit(value.bounciness);
        }
        bool ValidatePayload(const AudioSource& value)
        {
            return InRange(value.volume, 0.0f, 2.0f) && value.pitch > 0.0f && value.pitch <= 4.0f &&
                   Unit(value.spatialBlend) &&
                   value.minDistance >= 0.0f && value.maxDistance > value.minDistance &&
                   InRange(value.priority, 0, 255);
        }
        bool ValidatePayload(const Camera2DData& value)
        {
            return value.orthoSize > 0.0f && value.zoom > 0.0f && value.farPlane > value.nearPlane &&
                   Unit(value.followSmoothing);
        }
        bool ValidatePayload(const RigidBody2DData& value)
        {
            return InRange(value.bodyType, 0, 2) && (value.bodyType != 2 || value.mass > 0.0f) &&
                   value.mass >= 0.0f && value.linearDamping >= 0.0f && value.angularDamping >= 0.0f &&
                   Unit(value.friction) && Unit(value.restitution);
        }
        bool ValidatePayload(const Collider2DData& value)
        {
            return InRange(value.shape, 0, 4) && Positive2(value.halfExtents) && value.radius > 0.0f &&
                   value.height > 0.0f;
        }
        bool ValidatePayload(const TilemapData& value)
        {
            return value.tileWidth > 0 && value.tileHeight > 0 && value.columns >= 0 && value.rows >= 0 &&
                   value.mapWidth >= 0 && value.mapHeight >= 0 && value.pixelsPerUnit > 0.0f;
        }
        bool ValidatePayload(const TerrainSceneData& value)
        {
            return value.heightmapResolution > 1 && value.terrainSize > 0.0f && value.heightScale > 0.0f &&
                   value.maxHeight >= value.minHeight && InRange(value.lodLevels, 1, 16) && value.lodBias > 0.0f;
        }
        bool ValidatePayload(const ParticleEmitterData& value)
        {
            return value.emissionRate >= 0.0f && value.lifetime > 0.0f && value.startSize > 0.0f &&
                   value.startSpeed >= 0.0f && value.maxParticles >= 0;
        }
        bool ValidatePayload(const AnimationControllerData&) { return true; }
        bool ValidatePayload(const NineSliceData& value)
        {
            return value.borderLeft >= 0.0f && value.borderTop >= 0.0f && value.borderRight >= 0.0f &&
                   value.borderBottom >= 0.0f && Positive2(value.size);
        }
        bool ValidatePayload(const PixelPerfectData& value)
        {
            return value.referenceWidth > 0 && value.referenceHeight > 0;
        }
        bool ValidatePayload(const HealthData& value) { return value.maxHealth > 0.0f && InRange(value.health, 0.0f, value.maxHealth); }
        bool ValidatePayload(const AIAgentData& value)
        {
            return InRange(value.aiState, 0, 5) && value.detectionRange > 0.0f && value.attackRange > 0.0f &&
                   value.attackRange <= value.detectionRange && value.moveSpeed >= 0.0f && Unit(value.accuracy) &&
                   value.reactionTime >= 0.0f;
        }
        bool ValidatePayload(const SplineData& value) { return value.pointCount >= 0; }
        bool ValidatePayload(const SplineFollowerData& value)
        {
            return value.speed >= 0.0f && InRange(value.loopMode, 0, 2);
        }
        bool ValidatePayload(const DecalData& value)
        {
            return Positive3(value.size) && value.lifetime >= 0.0f && value.fadeOutDuration >= 0.0f;
        }
        bool ValidatePayload(const ProjectileData& value)
        {
            return InRange(value.movementType, 0, 1) && InRange(value.impactBehavior, 0, 3) && value.speed >= 0.0f &&
                   value.damage >= 0.0f && value.explosionRadius >= 0.0f && value.maxRange > 0.0f &&
                   value.maxLifetime > 0.0f && value.bouncesRemaining >= 0 && value.piercesRemaining >= 0;
        }
        bool ValidatePayload(const InteractionData& value)
        {
            return InRange(value.interactionType, 0, 3) && value.interactionRadius > 0.0f &&
                   value.holdDuration >= 0.0f && value.cooldownDuration >= 0.0f && value.usesRemaining >= -1;
        }
        bool ValidatePayload(const WeatherData& value)
        {
            return InRange(value.weatherType, 0, 5) && Unit(value.intensity) && value.windSpeed >= 0.0f &&
                   value.transitionTime >= 0.0f;
        }
        bool ValidatePayload(const TriggerVolumeData& value)
        {
            return InRange(value.shape, 0, 1) && value.radius > 0.0f && Positive3(value.halfExtents);
        }
        bool ValidatePayload(const PostProcessVolumeData& value)
        {
            return Unit(value.weight) && value.blendDistance >= 0.0f && value.maxEV >= value.minEV &&
                   value.bloomIntensity >= 0.0f && value.fogDensity >= 0.0f;
        }
        bool ValidatePayload(const ReflectionProbeData& value)
        {
            const bool validResolution = value.resolution == 128 || value.resolution == 256 ||
                                         value.resolution == 512 || value.resolution == 1024;
            return validResolution && value.influenceRadius > 0.0f && Positive3(value.boxExtents) &&
                   value.refreshInterval >= 0.0f && value.importance >= 0;
        }
        bool ValidatePayload(const LightProbeData& value)
        {
            return value.influenceRadius > 0.0f && Positive3(value.gridSpacing) && InRange(value.shOrder, 1, 2);
        }
        bool ValidatePayload(const NavObstacleData& value)
        {
            return InRange(value.shape, 0, 1) && Positive3(value.halfExtents) && value.radius > 0.0f &&
                   value.height > 0.0f;
        }
        bool ValidatePayload(const WaterPlaneData& value)
        {
            return Positive2(value.size) && value.waveHeight >= 0.0f && value.waveFrequency >= 0.0f &&
                   Unit(value.reflectionStrength) && Unit(value.refractionStrength);
        }
        bool ValidatePayload(const FogVolumeData& value)
        {
            return Positive3(value.halfExtents) && value.density >= 0.0f && value.falloff >= 0.0f;
        }
        bool ValidatePayload(const LODGroupData& value)
        {
            return InRange(value.lodCount, 1, 4) && value.lodDistance0 >= 0.0f &&
                   value.lodDistance1 >= value.lodDistance0 && value.lodDistance2 >= value.lodDistance1 &&
                   value.lodDistance3 >= value.lodDistance2 && value.crossFadeDuration >= 0.0f;
        }
        bool ValidatePayload(const SpawnPointData& value)
        {
            return value.spawnRadius >= 0.0f && value.respawnDelay >= 0.0f && value.maxConcurrent >= -1;
        }
        bool ValidatePayload(const AudioReverbZoneData& value)
        {
            return value.innerRadius > 0.0f && value.outerRadius >= value.innerRadius &&
                   InRange(value.reverbPreset, 0, 6) && value.decayTime > 0.0f && Unit(value.earlyReflections) &&
                   Unit(value.lateReverbLevel) && Unit(value.diffusion) && Unit(value.roomSize);
        }
        bool ValidatePayload(const WindZoneData& value)
        {
            return InRange(value.mode, 0, 1) && value.mainStrength >= 0.0f && value.turbulenceStrength >= 0.0f &&
                   value.pulseFrequency >= 0.0f && value.radius > 0.0f;
        }
        bool ValidatePayload(const PhysicsJointData& value)
        {
            return InRange(value.jointType, 0, 5) && (!value.enableLimits || value.upperLimit >= value.lowerLimit) &&
                   value.motorMaxForce >= 0.0f && value.breakForce >= 0.0f && value.breakTorque >= 0.0f;
        }
        bool ValidatePayload(const OccluderData& value)
        {
            return InRange(value.shape, 0, 1) && Positive3(value.halfExtents);
        }
        bool ValidatePayload(const CoverPointData& value)
        {
            return InRange(value.height, 0, 1) && value.width > 0.0f && value.maxOccupants > 0;
        }
        bool ValidatePayload(const TacticalPointData& value)
        {
            return InRange(value.pointType, 0, 5) && Unit(value.qualityScore) && value.radius > 0.0f;
        }
        bool ValidatePayload(const DestructibleData& value)
        {
            return value.health > 0.0f && InRange(value.damageStages, 1, 5) && value.debrisLifetime >= 0.0f &&
                   value.explosionForce >= 0.0f && value.minDamageThreshold >= 0.0f;
        }
        bool ValidatePayload(const CinematicTriggerData& value)
        {
            return InRange(value.triggerShape, 0, 1) && value.radius > 0.0f && Positive3(value.halfExtents);
        }
        bool ValidatePayload(const DialogueTriggerData& value) { return value.interactionRadius > 0.0f; }
        bool ValidatePayload(const AreaBoundaryData& value)
        {
            return value.boundsMax.x >= value.boundsMin.x && value.boundsMax.y >= value.boundsMin.y &&
                   value.boundsMax.z >= value.boundsMin.z && value.loadRadius > 0.0f &&
                   value.unloadRadius >= value.loadRadius;
        }
        bool ValidatePayload(const BillboardData& value)
        {
            return Positive2(value.size) && InRange(value.lockAxis, 0, 2) && value.fadeStartDistance >= 0.0f &&
                   value.fadeEndDistance >= value.fadeStartDistance;
        }
        bool ValidatePayload(const AudioListenerData& value) { return value.volumeScale >= 0.0f; }
        bool ValidatePayload(const CharacterControllerData& value)
        {
            return value.height > 0.0f && value.radius > 0.0f && value.stepHeight >= 0.0f &&
                   InRange(value.slopeLimit, 0.0f, 90.0f) && value.skinWidth >= 0.0f && value.moveSpeed >= 0.0f &&
                   value.jumpForce >= 0.0f;
        }
        bool ValidatePayload(const NavRegionData& value)
        {
            return Positive3(value.halfExtents) && value.agentRadius > 0.0f && value.agentHeight > 0.0f &&
                   InRange(value.maxSlope, 0.0f, 90.0f) && value.cellSize > 0.0f;
        }
        bool ValidatePayload(const NavLinkData& value)
        {
            return value.radius > 0.0f && InRange(value.traversalType, 0, 4) && value.traversalCost >= 0.0f;
        }
        bool ValidatePayload(const SkyboxData& value)
        {
            return InRange(value.mode, 0, 3) && value.turbidity >= 0.0f && value.sunSize >= 0.0f &&
                   value.exposure >= 0.0f;
        }
        bool ValidatePayload(const ForceRegionData& value)
        {
            return InRange(value.forceType, 0, 2) && Positive3(value.halfExtents) && value.forceMagnitude >= 0.0f &&
                   value.damping >= 0.0f;
        }
        bool ValidatePayload(const RagdollData& value)
        {
            return InRange(value.mode, 0, 2) && Unit(value.blendWeight) && value.jointStiffness >= 0.0f &&
                   value.linearDamping >= 0.0f && value.angularDamping >= 0.0f;
        }
        bool ValidatePayload(const SoftBodyData& value)
        {
            return value.mass > 0.0f && value.stiffness >= 0.0f && value.damping >= 0.0f &&
                   value.windInfluence >= 0.0f && value.solverIterations > 0;
        }
        bool ValidatePayload(const VehicleData& value)
        {
            return InRange(value.vehicleType, 0, 2) && value.wheelCount > 0 && value.mass > 0.0f &&
                   value.maxEngineTorque >= 0.0f && value.maxSteerAngle >= 0.0f && value.maxBrakeForce >= 0.0f &&
                   value.suspensionLength >= 0.0f && value.suspensionStiffness >= 0.0f &&
                   value.suspensionDamping >= 0.0f && value.gearCount > 0;
        }
        bool ValidatePayload(const BuoyancyVolumeData& value)
        {
            return Positive3(value.halfExtents) && value.waterDensity > 0.0f && value.linearDrag >= 0.0f &&
                   value.angularDrag >= 0.0f;
        }
        bool ValidatePayload(const SpringArmData& value)
        {
            return value.targetLength > 0.0f && value.probeRadius >= 0.0f && value.smoothSpeed >= 0.0f &&
                   value.minLength > 0.0f && value.minLength <= value.targetLength;
        }
        bool ValidatePayload(const LineRendererData& value)
        {
            return value.startWidth >= 0.0f && value.endWidth >= 0.0f;
        }
        bool ValidatePayload(const TrailRendererData& value)
        {
            return value.lifetime >= 0.0f && value.minVertexDistance >= 0.0f && value.startWidth >= 0.0f &&
                   value.endWidth >= 0.0f;
        }
        bool ValidatePayload(const Text3DData& value)
        {
            return value.fontSize > 0.0f && InRange(value.alignment, 0, 2) && value.maxWidth >= 0.0f;
        }
        bool ValidatePayload(const FoliageVolumeData& value)
        {
            return Positive3(value.halfExtents) && value.densityScale >= 0.0f &&
                   value.maxSlopeAngle >= value.minSlopeAngle && value.maxAltitude >= value.minAltitude &&
                   value.cullDistance >= 0.0f;
        }

        template <typename T, typename... Members>
        Codec MakeCodec(ComponentType type, const char* name, Members... members)
        {
            const auto fields = std::make_tuple(members...);
            return {
                type,
                name,
                std::type_index(typeid(T)),
                [] { return std::any(T{}); },
                [fields](const std::any& payload, SceneComponentFieldWriter& writer)
                {
                    const T* value = std::any_cast<T>(&payload);
                    if (!value || !ValidatePayload(*value))
                        return false;
                    bool valid = true;
                    std::apply([&](const auto&... field) { ((valid = valid && writer.Write(field.name, value->*(field.member))), ...); },
                               fields);
                    return valid;
                },
                [fields](const SceneComponentFieldReader& reader, std::any& payload)
                {
                    constexpr size_t count = sizeof...(Members);
                    std::array<std::string_view, count> names{};
                    size_t index = 0;
                    std::apply([&](const auto&... field) { ((names[index++] = field.name), ...); }, fields);
                    if (!reader.HasExactly(names))
                        return false;
                    T value{};
                    bool valid = true;
                    std::apply([&](const auto&... field) { ((valid = valid && reader.Read(field.name, value.*(field.member))), ...); },
                               fields);
                    if (!valid || !ValidatePayload(value))
                        return false;
                    payload = std::move(value);
                    return true;
                }};
        }

#define F(Type, Member) Field<Type>(#Member, &Type::Member)

        const std::vector<Codec>& Codecs()
        {
            static const std::vector<Codec> codecs = {
                MakeCodec<MeshRenderer>(ComponentType::MESH_RENDERER, "MeshRenderer", F(MeshRenderer, meshAssetPath),
                                        F(MeshRenderer, materialAssetPath), F(MeshRenderer, castShadows),
                                        F(MeshRenderer, receiveShadows), F(MeshRenderer, renderLayer),
                                        F(MeshRenderer, tintColor)),
                MakeCodec<Light>(ComponentType::LIGHT, "Light", F(Light, type), F(Light, color), F(Light, intensity),
                                 F(Light, range), F(Light, spotAngle), F(Light, spotInnerAngle), F(Light, castShadows),
                                 F(Light, shadowMapSize)),
                MakeCodec<Camera>(ComponentType::CAMERA, "Camera", F(Camera, projectionType), F(Camera, fieldOfView),
                                  F(Camera, orthographicSize), F(Camera, nearPlane), F(Camera, farPlane),
                                  F(Camera, clearColor), F(Camera, isMainCamera), F(Camera, renderTargetWidth),
                                  F(Camera, renderTargetHeight)),
                MakeCodec<RigidBody>(ComponentType::RIGID_BODY, "RigidBody", F(RigidBody, bodyType), F(RigidBody, mass),
                                     F(RigidBody, drag), F(RigidBody, angularDrag), F(RigidBody, velocity),
                                     F(RigidBody, angularVelocity), F(RigidBody, useGravity), F(RigidBody, isKinematic),
                                     F(RigidBody, freezePositionX), F(RigidBody, freezePositionY),
                                     F(RigidBody, freezePositionZ), F(RigidBody, freezeRotationX),
                                     F(RigidBody, freezeRotationY), F(RigidBody, freezeRotationZ)),
                MakeCodec<Collider>(ComponentType::COLLIDER, "Collider", F(Collider, type), F(Collider, center),
                                    F(Collider, size), F(Collider, radius), F(Collider, height),
                                    F(Collider, meshAssetPath), F(Collider, isTrigger), F(Collider, physicsMaterial),
                                    F(Collider, friction), F(Collider, bounciness)),
                MakeCodec<AudioSource>(ComponentType::AUDIO_SOURCE, "AudioSource", F(AudioSource, audioClipPath),
                                       F(AudioSource, playOnAwake), F(AudioSource, loop), F(AudioSource, volume),
                                       F(AudioSource, pitch), F(AudioSource, spatialBlend), F(AudioSource, minDistance),
                                       F(AudioSource, maxDistance), F(AudioSource, priority)),
                MakeCodec<ScriptData>(ComponentType::SCRIPT, "Script", F(ScriptData, scriptPath),
                                      F(ScriptData, className), F(ScriptData, autoStart)),
                MakeCodec<ParticleEmitterData>(
                    ComponentType::PARTICLE_SYSTEM, "ParticleSystem", F(ParticleEmitterData, effectName),
                    F(ParticleEmitterData, autoPlay), F(ParticleEmitterData, emissionRate),
                    F(ParticleEmitterData, lifetime), F(ParticleEmitterData, startColor),
                    F(ParticleEmitterData, startSize), F(ParticleEmitterData, startSpeed),
                    F(ParticleEmitterData, gravityMultiplier), F(ParticleEmitterData, maxParticles),
                    F(ParticleEmitterData, loop)),
                MakeCodec<AnimationControllerData>(ComponentType::ANIMATION, "Animation",
                                                   F(AnimationControllerData, defaultAnimation),
                                                   F(AnimationControllerData, playbackSpeed),
                                                   F(AnimationControllerData, playing),
                                                   F(AnimationControllerData, loop)),
                MakeCodec<SpriteRendererData>(
                    ComponentType::SPRITE_RENDERER, "SpriteRenderer", F(SpriteRendererData, texturePath),
                    F(SpriteRendererData, color), F(SpriteRendererData, sourceRect), F(SpriteRendererData, pivot),
                    F(SpriteRendererData, pixelsPerUnit), F(SpriteRendererData, sortingLayer),
                    F(SpriteRendererData, orderInLayer), F(SpriteRendererData, flipX), F(SpriteRendererData, flipY)),
                MakeCodec<Camera2DData>(ComponentType::CAMERA_2D, "Camera2D", F(Camera2DData, orthoSize),
                                        F(Camera2DData, zoom), F(Camera2DData, nearPlane), F(Camera2DData, farPlane),
                                        F(Camera2DData, followSmoothing), F(Camera2DData, deadZone),
                                        F(Camera2DData, clearColor), F(Camera2DData, isMain2DCamera)),
                MakeCodec<TilemapData>(ComponentType::TILEMAP, "Tilemap", F(TilemapData, tilesetTexturePath),
                                       F(TilemapData, tileWidth), F(TilemapData, tileHeight), F(TilemapData, columns),
                                       F(TilemapData, rows), F(TilemapData, mapWidth), F(TilemapData, mapHeight),
                                       F(TilemapData, sortingLayer), F(TilemapData, pixelsPerUnit),
                                       F(TilemapData, generateCollision)),
                MakeCodec<RigidBody2DData>(ComponentType::RIGID_BODY_2D, "RigidBody2D", F(RigidBody2DData, bodyType),
                                           F(RigidBody2DData, mass), F(RigidBody2DData, gravityScale),
                                           F(RigidBody2DData, linearDamping), F(RigidBody2DData, angularDamping),
                                           F(RigidBody2DData, friction), F(RigidBody2DData, restitution),
                                           F(RigidBody2DData, fixedRotation), F(RigidBody2DData, isBullet)),
                MakeCodec<Collider2DData>(ComponentType::COLLIDER_2D, "Collider2D", F(Collider2DData, shape),
                                          F(Collider2DData, halfExtents), F(Collider2DData, radius),
                                          F(Collider2DData, height), F(Collider2DData, offset),
                                          F(Collider2DData, isTrigger), F(Collider2DData, layerMask)),
                MakeCodec<ParallaxLayerData>(ComponentType::PARALLAX_BG, "ParallaxBackground",
                                             F(ParallaxLayerData, texturePath), F(ParallaxLayerData, scrollSpeed),
                                             F(ParallaxLayerData, tileX), F(ParallaxLayerData, tileY),
                                             F(ParallaxLayerData, tint), F(ParallaxLayerData, sortOrder)),
                MakeCodec<NineSliceData>(ComponentType::NINE_SLICE, "NineSlice", F(NineSliceData, texturePath),
                                         F(NineSliceData, borderLeft), F(NineSliceData, borderTop),
                                         F(NineSliceData, borderRight), F(NineSliceData, borderBottom),
                                         F(NineSliceData, size), F(NineSliceData, color), F(NineSliceData, fillCenter),
                                         F(NineSliceData, sortingLayer)),
                MakeCodec<PixelPerfectData>(ComponentType::PIXEL_PERFECT, "PixelPerfect",
                                            F(PixelPerfectData, referenceWidth), F(PixelPerfectData, referenceHeight),
                                            F(PixelPerfectData, upscaleToFill), F(PixelPerfectData, cropToFit)),
                MakeCodec<TerrainSceneData>(
                    ComponentType::TERRAIN, "Terrain", F(TerrainSceneData, heightmapResolution),
                    F(TerrainSceneData, terrainSize), F(TerrainSceneData, heightScale), F(TerrainSceneData, minHeight),
                    F(TerrainSceneData, maxHeight), F(TerrainSceneData, lodLevels), F(TerrainSceneData, lodBias),
                    F(TerrainSceneData, generateCollider), F(TerrainSceneData, castShadows),
                    F(TerrainSceneData, receiveShadows)),
                MakeCodec<HealthData>(ComponentType::HEALTH, "Health", F(HealthData, health), F(HealthData, maxHealth)),
                MakeCodec<AIAgentData>(ComponentType::AI_AGENT, "AIAgent", F(AIAgentData, aiState),
                                       F(AIAgentData, behaviorTreeName), F(AIAgentData, detectionRange),
                                       F(AIAgentData, attackRange), F(AIAgentData, moveSpeed), F(AIAgentData, accuracy),
                                       F(AIAgentData, reactionTime)),
                MakeCodec<SplineData>(ComponentType::SPLINE, "Spline", F(SplineData, debugVisible),
                                      F(SplineData, closed), F(SplineData, pointCount)),
                MakeCodec<SplineFollowerData>(ComponentType::SPLINE_FOLLOWER, "SplineFollower",
                                              F(SplineFollowerData, speed), F(SplineFollowerData, loopMode),
                                              F(SplineFollowerData, playing), F(SplineFollowerData, orientToPath)),
                MakeCodec<DecalData>(ComponentType::DECAL, "Decal", F(DecalData, texturePath), F(DecalData, category),
                                     F(DecalData, size), F(DecalData, color), F(DecalData, lifetime),
                                     F(DecalData, fadeOutDuration), F(DecalData, receiveLighting),
                                     F(DecalData, sortOrder)),
                MakeCodec<ProjectileData>(ComponentType::PROJECTILE, "Projectile", F(ProjectileData, movementType),
                                          F(ProjectileData, impactBehavior), F(ProjectileData, speed),
                                          F(ProjectileData, damage), F(ProjectileData, gravityScale),
                                          F(ProjectileData, explosionRadius), F(ProjectileData, maxRange),
                                          F(ProjectileData, maxLifetime), F(ProjectileData, bouncesRemaining),
                                          F(ProjectileData, piercesRemaining)),
                MakeCodec<InteractionData>(ComponentType::INTERACTION, "Interaction",
                                           F(InteractionData, interactionType), F(InteractionData, displayName),
                                           F(InteractionData, actionVerb), F(InteractionData, interactionRadius),
                                           F(InteractionData, holdDuration), F(InteractionData, cooldownDuration),
                                           F(InteractionData, usesRemaining), F(InteractionData, showHighlight)),
                MakeCodec<WeatherData>(ComponentType::WEATHER, "Weather", F(WeatherData, weatherType),
                                       F(WeatherData, intensity), F(WeatherData, windSpeed),
                                       F(WeatherData, windDirection), F(WeatherData, transitionTime),
                                       F(WeatherData, enabled)),
                MakeCodec<NetworkIdentityData>(ComponentType::NETWORK_IDENTITY, "NetworkIdentity",
                                               F(NetworkIdentityData, replicateTransform),
                                               F(NetworkIdentityData, replicateHealth),
                                               F(NetworkIdentityData, isLocalAuthority)),
                MakeCodec<TriggerVolumeData>(ComponentType::TRIGGER_VOLUME, "TriggerVolume",
                                             F(TriggerVolumeData, shape), F(TriggerVolumeData, radius),
                                             F(TriggerVolumeData, halfExtents), F(TriggerVolumeData, onEnterEvent),
                                             F(TriggerVolumeData, onExitEvent), F(TriggerVolumeData, enabled),
                                             F(TriggerVolumeData, oneShot)),
                MakeCodec<PostProcessVolumeData>(
                    ComponentType::POST_PROCESS_VOLUME, "PostProcessVolume", F(PostProcessVolumeData, isGlobal),
                    F(PostProcessVolumeData, priority), F(PostProcessVolumeData, weight),
                    F(PostProcessVolumeData, blendDistance), F(PostProcessVolumeData, overrideExposure),
                    F(PostProcessVolumeData, exposure), F(PostProcessVolumeData, minEV), F(PostProcessVolumeData, maxEV),
                    F(PostProcessVolumeData, overrideBloom), F(PostProcessVolumeData, bloomIntensity),
                    F(PostProcessVolumeData, bloomThreshold), F(PostProcessVolumeData, overrideColorGrading),
                    F(PostProcessVolumeData, saturation), F(PostProcessVolumeData, contrast),
                    F(PostProcessVolumeData, temperature), F(PostProcessVolumeData, overrideFog),
                    F(PostProcessVolumeData, fogDensity), F(PostProcessVolumeData, fogHeight)),
                MakeCodec<ReflectionProbeData>(
                    ComponentType::REFLECTION_PROBE, "ReflectionProbe", F(ReflectionProbeData, resolution),
                    F(ReflectionProbeData, influenceRadius), F(ReflectionProbeData, boxExtents),
                    F(ReflectionProbeData, useBoxProjection), F(ReflectionProbeData, isDynamic),
                    F(ReflectionProbeData, refreshInterval), F(ReflectionProbeData, importance),
                    F(ReflectionProbeData, captureOffset)),
                MakeCodec<LightProbeData>(ComponentType::LIGHT_PROBE, "LightProbe", F(LightProbeData, influenceRadius),
                                          F(LightProbeData, gridSpacing), F(LightProbeData, baked),
                                          F(LightProbeData, shOrder)),
                MakeCodec<NavObstacleData>(ComponentType::NAV_OBSTACLE, "NavObstacle", F(NavObstacleData, shape),
                                           F(NavObstacleData, halfExtents), F(NavObstacleData, radius),
                                           F(NavObstacleData, height), F(NavObstacleData, carveOnMove)),
                MakeCodec<WaterPlaneData>(
                    ComponentType::WATER_PLANE, "WaterPlane", F(WaterPlaneData, size), F(WaterPlaneData, shallowColor),
                    F(WaterPlaneData, deepColor), F(WaterPlaneData, waveHeight), F(WaterPlaneData, waveSpeed),
                    F(WaterPlaneData, waveFrequency), F(WaterPlaneData, reflectionStrength),
                    F(WaterPlaneData, refractionStrength), F(WaterPlaneData, receiveShadows)),
                MakeCodec<FogVolumeData>(ComponentType::FOG_VOLUME, "FogVolume", F(FogVolumeData, halfExtents),
                                         F(FogVolumeData, density), F(FogVolumeData, color), F(FogVolumeData, falloff),
                                         F(FogVolumeData, heightFalloff)),
                MakeCodec<LODGroupData>(ComponentType::LOD_GROUP, "LODGroup", F(LODGroupData, lodDistance0),
                                        F(LODGroupData, lodDistance1), F(LODGroupData, lodDistance2),
                                        F(LODGroupData, lodDistance3), F(LODGroupData, lodCount),
                                        F(LODGroupData, crossFadeDuration), F(LODGroupData, autoGenerate)),
                MakeCodec<SpawnPointData>(ComponentType::SPAWN_POINT, "SpawnPoint", F(SpawnPointData, spawnTag),
                                          F(SpawnPointData, teamID), F(SpawnPointData, spawnRadius),
                                          F(SpawnPointData, respawnDelay), F(SpawnPointData, maxConcurrent),
                                          F(SpawnPointData, enabled), F(SpawnPointData, priority)),
                MakeCodec<AudioReverbZoneData>(
                    ComponentType::AUDIO_REVERB_ZONE, "AudioReverbZone", F(AudioReverbZoneData, innerRadius),
                    F(AudioReverbZoneData, outerRadius), F(AudioReverbZoneData, reverbPreset),
                    F(AudioReverbZoneData, decayTime), F(AudioReverbZoneData, earlyReflections),
                    F(AudioReverbZoneData, lateReverbLevel), F(AudioReverbZoneData, diffusion),
                    F(AudioReverbZoneData, roomSize)),
                MakeCodec<WindZoneData>(ComponentType::WIND_ZONE, "WindZone", F(WindZoneData, mode),
                                        F(WindZoneData, direction), F(WindZoneData, mainStrength),
                                        F(WindZoneData, turbulenceStrength), F(WindZoneData, pulseFrequency),
                                        F(WindZoneData, radius), F(WindZoneData, affectsParticles),
                                        F(WindZoneData, affectsVegetation), F(WindZoneData, affectsCloth)),
                MakeCodec<PhysicsJointData>(
                    ComponentType::PHYSICS_JOINT, "PhysicsJoint", F(PhysicsJointData, jointType),
                    F(PhysicsJointData, connectedBody), F(PhysicsJointData, anchor), F(PhysicsJointData, axis),
                    F(PhysicsJointData, lowerLimit), F(PhysicsJointData, upperLimit),
                    F(PhysicsJointData, enableLimits), F(PhysicsJointData, enableMotor),
                    F(PhysicsJointData, motorSpeed), F(PhysicsJointData, motorMaxForce),
                    F(PhysicsJointData, breakForce), F(PhysicsJointData, breakTorque)),
                MakeCodec<OccluderData>(ComponentType::OCCLUDER, "Occluder", F(OccluderData, shape),
                                        F(OccluderData, halfExtents), F(OccluderData, doubleSided)),
                MakeCodec<CoverPointData>(ComponentType::COVER_POINT, "CoverPoint", F(CoverPointData, height),
                                          F(CoverPointData, width), F(CoverPointData, coverNormal),
                                          F(CoverPointData, canLeanLeft), F(CoverPointData, canLeanRight),
                                          F(CoverPointData, canFireOver), F(CoverPointData, maxOccupants)),
                MakeCodec<TacticalPointData>(ComponentType::TACTICAL_POINT, "TacticalPoint",
                                             F(TacticalPointData, pointType), F(TacticalPointData, qualityScore),
                                             F(TacticalPointData, radius), F(TacticalPointData, enabled)),
                MakeCodec<DestructibleData>(
                    ComponentType::DESTRUCTIBLE, "Destructible", F(DestructibleData, health),
                    F(DestructibleData, damageStages), F(DestructibleData, fracturePattern),
                    F(DestructibleData, debrisLifetime), F(DestructibleData, explosionForce),
                    F(DestructibleData, minDamageThreshold), F(DestructibleData, generateColliders),
                    F(DestructibleData, chainReaction)),
                MakeCodec<CinematicTriggerData>(
                    ComponentType::CINEMATIC_TRIGGER, "CinematicTrigger", F(CinematicTriggerData, sequenceName),
                    F(CinematicTriggerData, triggerShape), F(CinematicTriggerData, radius),
                    F(CinematicTriggerData, halfExtents), F(CinematicTriggerData, playOnce),
                    F(CinematicTriggerData, skipable), F(CinematicTriggerData, pauseGameplay)),
                MakeCodec<DialogueTriggerData>(
                    ComponentType::DIALOGUE_TRIGGER, "DialogueTrigger", F(DialogueTriggerData, dialogueTreeName),
                    F(DialogueTriggerData, speakerName), F(DialogueTriggerData, interactionRadius),
                    F(DialogueTriggerData, requiresInteract), F(DialogueTriggerData, oneShot),
                    F(DialogueTriggerData, facePlayer)),
                MakeCodec<AreaBoundaryData>(ComponentType::AREA_BOUNDARY, "AreaBoundary", F(AreaBoundaryData, areaName),
                                            F(AreaBoundaryData, scenePath), F(AreaBoundaryData, boundsMin),
                                            F(AreaBoundaryData, boundsMax), F(AreaBoundaryData, priority),
                                            F(AreaBoundaryData, loadRadius), F(AreaBoundaryData, unloadRadius),
                                            F(AreaBoundaryData, alwaysLoaded)),
                MakeCodec<BillboardData>(ComponentType::BILLBOARD, "Billboard", F(BillboardData, texturePath),
                                         F(BillboardData, color), F(BillboardData, size), F(BillboardData, lockAxis),
                                         F(BillboardData, fadeStartDistance), F(BillboardData, fadeEndDistance),
                                         F(BillboardData, sortingLayer)),
                MakeCodec<AudioListenerData>(ComponentType::AUDIO_LISTENER, "AudioListener",
                                             F(AudioListenerData, isActive), F(AudioListenerData, volumeScale)),
                MakeCodec<CharacterControllerData>(
                    ComponentType::CHARACTER_CONTROLLER, "CharacterController", F(CharacterControllerData, height),
                    F(CharacterControllerData, radius), F(CharacterControllerData, stepHeight),
                    F(CharacterControllerData, slopeLimit), F(CharacterControllerData, skinWidth),
                    F(CharacterControllerData, gravity), F(CharacterControllerData, moveSpeed),
                    F(CharacterControllerData, jumpForce)),
                MakeCodec<NavRegionData>(ComponentType::NAV_REGION, "NavRegion", F(NavRegionData, halfExtents),
                                         F(NavRegionData, agentRadius), F(NavRegionData, agentHeight),
                                         F(NavRegionData, maxSlope), F(NavRegionData, cellSize),
                                         F(NavRegionData, autoRebuild)),
                MakeCodec<NavLinkData>(ComponentType::NAV_LINK, "NavLink", F(NavLinkData, endOffset),
                                       F(NavLinkData, radius), F(NavLinkData, traversalType),
                                       F(NavLinkData, traversalCost), F(NavLinkData, bidirectional),
                                       F(NavLinkData, enabled)),
                MakeCodec<SkyboxData>(ComponentType::SKYBOX, "Skybox", F(SkyboxData, mode),
                                      F(SkyboxData, cubemapPath), F(SkyboxData, topColor),
                                      F(SkyboxData, bottomColor), F(SkyboxData, turbidity), F(SkyboxData, sunSize),
                                      F(SkyboxData, exposure), F(SkyboxData, rotation)),
                MakeCodec<ConstantForceData>(ComponentType::CONSTANT_FORCE, "ConstantForce",
                                             F(ConstantForceData, force), F(ConstantForceData, torque),
                                             F(ConstantForceData, relativeForce), F(ConstantForceData, relativeTorque),
                                             F(ConstantForceData, enabled)),
                MakeCodec<ForceRegionData>(ComponentType::FORCE_REGION, "ForceRegion", F(ForceRegionData, forceType),
                                           F(ForceRegionData, halfExtents), F(ForceRegionData, forceDirection),
                                           F(ForceRegionData, forceMagnitude), F(ForceRegionData, damping),
                                           F(ForceRegionData, enabled)),
                MakeCodec<RagdollData>(ComponentType::RAGDOLL, "Ragdoll", F(RagdollData, mode),
                                       F(RagdollData, definitionName), F(RagdollData, blendWeight),
                                       F(RagdollData, jointStiffness), F(RagdollData, linearDamping),
                                       F(RagdollData, angularDamping), F(RagdollData, selfCollision)),
                MakeCodec<SoftBodyData>(ComponentType::SOFT_BODY, "SoftBody", F(SoftBodyData, mass),
                                        F(SoftBodyData, stiffness), F(SoftBodyData, damping),
                                        F(SoftBodyData, windInfluence), F(SoftBodyData, gravityScale),
                                        F(SoftBodyData, solverIterations), F(SoftBodyData, selfCollision),
                                        F(SoftBodyData, twoSided)),
                MakeCodec<VehicleData>(ComponentType::VEHICLE, "Vehicle", F(VehicleData, vehicleType),
                                       F(VehicleData, wheelCount), F(VehicleData, mass),
                                       F(VehicleData, maxEngineTorque), F(VehicleData, maxSteerAngle),
                                       F(VehicleData, maxBrakeForce), F(VehicleData, suspensionLength),
                                       F(VehicleData, suspensionStiffness), F(VehicleData, suspensionDamping),
                                       F(VehicleData, gearCount), F(VehicleData, antiRollBar)),
                MakeCodec<BuoyancyVolumeData>(
                    ComponentType::BUOYANCY_VOLUME, "BuoyancyVolume", F(BuoyancyVolumeData, halfExtents),
                    F(BuoyancyVolumeData, waterDensity), F(BuoyancyVolumeData, linearDrag),
                    F(BuoyancyVolumeData, angularDrag), F(BuoyancyVolumeData, flowSpeed),
                    F(BuoyancyVolumeData, flowDirection), F(BuoyancyVolumeData, enabled)),
                MakeCodec<SpringArmData>(ComponentType::SPRING_ARM, "SpringArm", F(SpringArmData, targetLength),
                                         F(SpringArmData, probeRadius), F(SpringArmData, smoothSpeed),
                                         F(SpringArmData, minLength), F(SpringArmData, doCollisionTest)),
                MakeCodec<LineRendererData>(
                    ComponentType::LINE_RENDERER, "LineRenderer", F(LineRendererData, startWidth),
                    F(LineRendererData, endWidth), F(LineRendererData, startColor), F(LineRendererData, endColor),
                    F(LineRendererData, useWorldSpace), F(LineRendererData, loop), F(LineRendererData, sortingLayer)),
                MakeCodec<TrailRendererData>(
                    ComponentType::TRAIL_RENDERER, "TrailRenderer", F(TrailRendererData, lifetime),
                    F(TrailRendererData, minVertexDistance), F(TrailRendererData, startWidth),
                    F(TrailRendererData, endWidth), F(TrailRendererData, startColor), F(TrailRendererData, endColor),
                    F(TrailRendererData, emitting), F(TrailRendererData, sortingLayer)),
                MakeCodec<Text3DData>(ComponentType::TEXT_3D, "Text3D", F(Text3DData, text), F(Text3DData, fontPath),
                                      F(Text3DData, fontSize), F(Text3DData, color), F(Text3DData, faceCamera),
                                      F(Text3DData, castShadows), F(Text3DData, alignment), F(Text3DData, maxWidth),
                                      F(Text3DData, sortingLayer)),
                MakeCodec<FoliageVolumeData>(
                    ComponentType::FOLIAGE_VOLUME, "FoliageVolume", F(FoliageVolumeData, halfExtents),
                    F(FoliageVolumeData, seed), F(FoliageVolumeData, densityScale),
                    F(FoliageVolumeData, minSlopeAngle), F(FoliageVolumeData, maxSlopeAngle),
                    F(FoliageVolumeData, minAltitude), F(FoliageVolumeData, maxAltitude),
                    F(FoliageVolumeData, alignToSurface), F(FoliageVolumeData, castShadows),
                    F(FoliageVolumeData, cullDistance), F(FoliageVolumeData, enabled)),
            };
            return codecs;
        }

#undef F

        const Codec* FindCodec(ComponentType type)
        {
            for (const Codec& codec : Codecs())
                if (codec.type == type)
                    return &codec;
            return nullptr;
        }

        constexpr std::array<const char*, 65> kTypeNames = {
            "Transform",          "MeshRenderer",       "Light",             "Camera",
            "RigidBody",          "Collider",           "AudioSource",       "Script",
            "ParticleSystem",     "Animation",          "SpriteRenderer",    "SpriteAnimator",
            "Camera2D",           "Tilemap",            "RigidBody2D",       "Collider2D",
            "ParallaxBackground", "NineSlice",          "PixelPerfect",      "Terrain",
            "Health",             "AIAgent",            "Spline",            "SplineFollower",
            "Decal",              "Projectile",         "Interaction",       "Weather",
            "NetworkIdentity",    "TriggerVolume",      "PostProcessVolume", "ReflectionProbe",
            "LightProbe",         "NavObstacle",        "WaterPlane",        "FogVolume",
            "LODGroup",           "SpawnPoint",         "AudioReverbZone",   "WindZone",
            "PhysicsJoint",       "Occluder",           "CoverPoint",        "TacticalPoint",
            "Destructible",       "CinematicTrigger",   "DialogueTrigger",   "AreaBoundary",
            "Billboard",          "AudioListener",      "CharacterController", "NavRegion",
            "NavLink",            "Skybox",             "ConstantForce",     "ForceRegion",
            "Ragdoll",            "SoftBody",           "Vehicle",           "BuoyancyVolume",
            "SpringArm",          "LineRenderer",       "TrailRenderer",     "Text3D",
            "FoliageVolume"};
    } // namespace

    const char* SceneComponentTypeName(ComponentType type)
    {
        const uint32_t value = static_cast<uint32_t>(type);
        return value < kTypeNames.size() ? kTypeNames[value] : nullptr;
    }

    bool TryParseSceneComponentTypeName(std::string_view name, ComponentType& type)
    {
        for (uint32_t index = 0; index < kTypeNames.size(); ++index)
        {
            if (name == kTypeNames[index])
            {
                type = static_cast<ComponentType>(index);
                return true;
            }
        }
        return false;
    }

    bool HasSceneComponentPayloadCodec(ComponentType type) { return FindCodec(type) != nullptr; }

    std::vector<ComponentType> GetSceneComponentPayloadTypes()
    {
        std::vector<ComponentType> result;
        result.reserve(Codecs().size());
        for (const Codec& codec : Codecs())
            result.push_back(codec.type);
        return result;
    }

    bool InitializeDefaultSceneComponentPayload(Component& component, std::string& error)
    {
        const Codec* codec = FindCodec(component.type);
        if (!codec)
        {
            error = "component type has no built-in payload codec";
            return false;
        }
        component.data = codec->makeDefault();
        return true;
    }

    bool EncodeSceneComponentPayload(const Component& component, SceneComponentFieldWriter& writer, std::string& error)
    {
        const Codec* codec = FindCodec(component.type);
        if (!codec)
        {
            error = "component type has no built-in payload codec";
            return false;
        }
        if (!component.HasData() || std::type_index(component.DataType()) != codec->payloadType)
        {
            error = "component payload type does not match its ComponentType";
            return false;
        }
        if (!codec->encode(component.data, writer))
        {
            error = "component payload contains an invalid or unsupported field value";
            return false;
        }
        return true;
    }

    bool DecodeSceneComponentPayload(ComponentType type, const SceneComponentFieldReader& reader, Component& component,
                                     std::string& error)
    {
        const Codec* codec = FindCodec(type);
        if (!codec)
        {
            error = "component type has no built-in payload codec";
            return false;
        }
        std::any payload;
        if (!codec->decode(reader, payload))
        {
            error = "component payload schema, fields, or value types are invalid";
            return false;
        }
        component.type = type;
        component.data = std::move(payload);
        return true;
    }
} // namespace SparkEditor
