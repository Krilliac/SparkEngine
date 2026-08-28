/**
 * @file SaveSystem.cpp
 * @brief Transactional, versioned binary save/load system implementation
 */

#include "SaveSystem.h"
#include "../../Core/Reflection.h"
#include "../../Utils/Assert.h"
#include "../../Utils/EventBus.h"
#include "../../Utils/Validate.h"
#include "Utils/LocalFileCache.h"
#include <cstring>
#include <charconv>
#include <cmath>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <unordered_set>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace Spark
{

    // ============================================================================
    // Save/Load events published on the global EventBus
    // ============================================================================

    /// @brief Published on the global EventBus before serialization begins.
    struct SaveBeginEvent
    {
        std::string slotName;
    };
    /// @brief Published on the global EventBus after a successful save.
    struct SaveCompleteEvent
    {
        std::string slotName;
        bool success = false;
    };
    /// @brief Published on the global EventBus before deserialization begins.
    struct LoadBeginEvent
    {
        std::string slotName;
    };
    /// @brief Published on the global EventBus after a load attempt (success or failure).
    struct LoadCompleteEvent
    {
        std::string slotName;
        bool success = false;
    };

    namespace
    {
        bool FlushFileDurably(const std::filesystem::path& path, std::error_code& error)
        {
#if defined(_WIN32)
            const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                              FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                error = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
                return false;
            }

            const bool flushed = ::FlushFileBuffers(file) != FALSE;
            const DWORD flushError = flushed ? ERROR_SUCCESS : ::GetLastError();
            ::CloseHandle(file);
            if (!flushed)
            {
                error = std::error_code(static_cast<int>(flushError), std::system_category());
                return false;
            }
            return true;
#else
            const int file = ::open(path.c_str(), O_RDONLY);
            if (file < 0)
            {
                error = std::error_code(errno, std::generic_category());
                return false;
            }

            const bool flushed = ::fsync(file) == 0;
            const int flushError = flushed ? 0 : errno;
            ::close(file);
            if (!flushed)
            {
                error = std::error_code(flushError, std::generic_category());
                return false;
            }
            return true;
#endif
        }

        bool ReplaceFileAtomically(const std::filesystem::path& temporary, const std::filesystem::path& destination,
                                   std::error_code& error)
        {
#if defined(_WIN32)
            if (::MoveFileExW(temporary.c_str(), destination.c_str(),
                              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                return true;
            }
            error = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
            return false;
#else
            std::filesystem::rename(temporary, destination, error);
            if (error)
                return false;

            const std::filesystem::path directory = destination.has_parent_path() ? destination.parent_path() : ".";
#if defined(O_DIRECTORY)
            const int directoryFile = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
#else
            const int directoryFile = ::open(directory.c_str(), O_RDONLY);
#endif
            if (directoryFile < 0)
            {
                error = std::error_code(errno, std::generic_category());
                return false;
            }
            const bool flushed = ::fsync(directoryFile) == 0;
            const int flushError = flushed ? 0 : errno;
            ::close(directoryFile);
            if (!flushed)
            {
                error = std::error_code(flushError, std::generic_category());
                return false;
            }
            return true;
#endif
        }

        bool IsSupportedSaveVersion(uint32_t version)
        {
            return version >= kOldestSupportedSaveVersion && version <= kCurrentSaveVersion;
        }

        void LogUnsupportedSaveVersion(const std::string& filepath, uint32_t version, const char* operation)
        {
            if (version > kCurrentSaveVersion)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Save,
                               "%s: save '%s' uses version %u, but this build supports versions %u..%u; "
                               "load it with a newer SparkEngine build",
                               operation, filepath.c_str(), version, kOldestSupportedSaveVersion, kCurrentSaveVersion);
            }
            else
            {
                SPARK_LOG_WARN(Spark::LogCategory::Save,
                               "%s: save '%s' uses version %u, but this build supports versions %u..%u; "
                               "restore or convert the save with a compatible older build",
                               operation, filepath.c_str(), version, kOldestSupportedSaveVersion, kCurrentSaveVersion);
            }
        }

        bool ParseMetadataBlock(uint32_t sourceVersion, const std::string& metadataBlock, SaveMetadata& outMetadata)
        {
            SaveMetadata parsedMetadata;
            parsedMetadata.version = sourceVersion;

            std::istringstream stream(metadataBlock);
            if (!std::getline(stream, parsedMetadata.saveName) || !std::getline(stream, parsedMetadata.sceneName) ||
                !std::getline(stream, parsedMetadata.playerClass))
            {
                return false;
            }

            if (sourceVersion >= 2 && !std::getline(stream, parsedMetadata.screenshotPath))
                return false;

            stream >> parsedMetadata.timestamp;
            stream >> parsedMetadata.playTime;
            stream >> parsedMetadata.playerHealth;
            stream >> parsedMetadata.playerArmor;
            stream >> parsedMetadata.playerPosition.x >> parsedMetadata.playerPosition.y >>
                parsedMetadata.playerPosition.z;
            stream >> parsedMetadata.playerKills;
            stream >> parsedMetadata.playerDeaths;
            if (!stream)
                return false;

            stream >> std::ws;
            if (!stream.eof())
                return false;

            outMetadata = std::move(parsedMetadata);
            return true;
        }
    } // namespace

    // ============================================================================
    // ComponentSerializerRegistry
    // ============================================================================

    ComponentSerializerRegistry& ComponentSerializerRegistry::GetInstance()
    {
        static ComponentSerializerRegistry instance;
        return instance;
    }

    void ComponentSerializerRegistry::Register(const std::string& typeName, SerializeFunc serialize,
                                               DeserializeFunc deserialize)
    {
        m_serializers[typeName] = {std::move(serialize), std::move(deserialize)};
    }

    bool ComponentSerializerRegistry::Unregister(const std::string& typeName)
    {
        return m_serializers.erase(typeName) != 0;
    }

    bool ComponentSerializerRegistry::HasSerializer(const std::string& typeName) const
    {
        return m_serializers.contains(typeName);
    }

    SerializedComponent ComponentSerializerRegistry::Serialize(const std::string& typeName, const void* component) const
    {
        auto it = m_serializers.find(typeName);
        if (it != m_serializers.end())
            return it->second.serialize(component);
        return {};
    }

    void ComponentSerializerRegistry::Deserialize(const std::string& typeName, World& world, EntityID entity,
                                                  const SerializedComponent& data) const
    {
        auto it = m_serializers.find(typeName);
        if (it != m_serializers.end())
            it->second.deserialize(world, entity, data);
    }

    // ============================================================================
    // Strict deserialization helpers — a malformed required field invalidates the snapshot
    // ============================================================================

    // Must match the writer's limit (rejectIfTooLong in WriteToFile): every string on the wire
    // carries a uint16 length prefix, so anything the writer accepts must load back cleanly.
    static constexpr size_t kMaxPropertyLen = std::numeric_limits<uint16_t>::max();

    static const std::string& RequireString(const std::unordered_map<std::string, std::string>& props,
                                            const std::string& key)
    {
        const auto it = props.find(key);
        if (it == props.end())
            throw std::runtime_error("missing required save property '" + key + "'");
        if (it->second.size() > kMaxPropertyLen)
            throw std::runtime_error("save property '" + key + "' exceeds the uint16 wire limit");
        return it->second;
    }

    static float RequireFloat(const std::unordered_map<std::string, std::string>& props, const std::string& key)
    {
        const std::string& value = RequireString(props, key);
        try
        {
            size_t consumed = 0;
            const float parsed = std::stof(value, &consumed);
            if (consumed != value.size() || !std::isfinite(parsed))
                throw std::invalid_argument("not a finite, complete float");
            return parsed;
        }
        catch (const std::exception&)
        {
            throw std::runtime_error("save property '" + key + "' is not a valid finite float");
        }
    }

    template <typename Integer>
    static Integer RequireInteger(const std::unordered_map<std::string, std::string>& props, const std::string& key)
    {
        const std::string& value = RequireString(props, key);
        Integer parsed{};
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (error != std::errc{} || end != value.data() + value.size())
            throw std::runtime_error("save property '" + key + "' is not a valid integer");
        return parsed;
    }

    static bool RequireBool(const std::unordered_map<std::string, std::string>& props, const std::string& key)
    {
        const std::string& value = RequireString(props, key);
        if (value == "1")
            return true;
        if (value == "0")
            return false;
        throw std::runtime_error("save property '" + key + "' is not encoded as 0 or 1");
    }

    // ============================================================================
    // Domain-specific serializer registration (called from RegisterBuiltins)
    // ============================================================================

    static void RegisterCoreSerializers(ComponentSerializerRegistry& reg)
    {
        reg.Register(
            "Transform",
            [](const void* comp) -> SerializedComponent
            {
                const auto* t = static_cast<const Transform*>(comp);
                SerializedComponent sc;
                sc.typeName = "Transform";
                sc.properties["px"] = std::to_string(t->position.x);
                sc.properties["py"] = std::to_string(t->position.y);
                sc.properties["pz"] = std::to_string(t->position.z);
                sc.properties["rx"] = std::to_string(t->rotation.x);
                sc.properties["ry"] = std::to_string(t->rotation.y);
                sc.properties["rz"] = std::to_string(t->rotation.z);
                sc.properties["sx"] = std::to_string(t->scale.x);
                sc.properties["sy"] = std::to_string(t->scale.y);
                sc.properties["sz"] = std::to_string(t->scale.z);
                return sc;
            },
            [](World& world, EntityID entity, const SerializedComponent& data)
            {
                auto& t = world.AddComponent<Transform>(entity);
                auto& p = data.properties;
                t.position = {RequireFloat(p, "px"), RequireFloat(p, "py"), RequireFloat(p, "pz")};
                t.rotation = {RequireFloat(p, "rx"), RequireFloat(p, "ry"), RequireFloat(p, "rz")};
                t.scale = {RequireFloat(p, "sx"), RequireFloat(p, "sy"), RequireFloat(p, "sz")};
            });

        // MeshRenderer — handled by RegisterReflectedSerializers() (field names match)
        // Camera — handled by RegisterReflectedSerializers() (field names match)

        reg.Register(
            "Script",
            [](const void* comp) -> SerializedComponent
            {
                const auto* s = static_cast<const Script*>(comp);
                SerializedComponent sc;
                sc.typeName = "Script";
                sc.properties["scriptPath"] = s->scriptPath;
                sc.properties["className"] = s->className;
                sc.properties["moduleName"] = s->moduleName;
                sc.properties["enabled"] = s->enabled ? "1" : "0";
                sc.properties["started"] = s->started ? "1" : "0";
                return sc;
            },
            [](World& world, EntityID entity, const SerializedComponent& data)
            {
                auto& s = world.AddComponent<Script>(entity);
                auto& p = data.properties;
                s.scriptPath = RequireString(p, "scriptPath");
                s.className = RequireString(p, "className");
                s.moduleName = RequireString(p, "moduleName");
                s.enabled = RequireBool(p, "enabled");
                s.started = RequireBool(p, "started");
            });

        reg.Register(
            "TagComponent",
            [](const void* comp) -> SerializedComponent
            {
                const auto* tc = static_cast<const TagComponent*>(comp);
                SerializedComponent sc;
                sc.typeName = "TagComponent";
                std::string tagList;
                bool first = true;
                for (const auto& tag : tc->tags)
                {
                    if (!first)
                        tagList += ",";
                    tagList += tag;
                    first = false;
                }
                sc.properties["tags"] = tagList;
                return sc;
            },
            [](World& world, EntityID entity, const SerializedComponent& data)
            {
                auto& tc = world.AddComponent<TagComponent>(entity);
                const std::string& tags = RequireString(data.properties, "tags");
                if (!tags.empty())
                {
                    std::istringstream iss(tags);
                    std::string tag;
                    while (std::getline(iss, tag, ','))
                    {
                        if (!tag.empty())
                            tc.tags.insert(tag);
                    }
                }
            });

        // ActiveComponent — handled by RegisterReflectedSerializers() (field names match)
    }

    static void RegisterPhysicsSerializers(ComponentSerializerRegistry& reg)
    {
        reg.Register(
            "RigidBodyComponent",
            [](const void* comp) -> SerializedComponent
            {
                const auto* rb = static_cast<const RigidBodyComponent*>(comp);
                SerializedComponent sc;
                sc.typeName = "RigidBodyComponent";
                sc.properties["type"] = std::to_string(static_cast<int>(rb->type));
                sc.properties["mass"] = std::to_string(rb->mass);
                sc.properties["friction"] = std::to_string(rb->friction);
                sc.properties["restitution"] = std::to_string(rb->restitution);
                sc.properties["linearDamping"] = std::to_string(rb->linearDamping);
                sc.properties["angularDamping"] = std::to_string(rb->angularDamping);
                sc.properties["isTrigger"] = rb->isTrigger ? "1" : "0";
                sc.properties["lvx"] = std::to_string(rb->linearVelocity.x);
                sc.properties["lvy"] = std::to_string(rb->linearVelocity.y);
                sc.properties["lvz"] = std::to_string(rb->linearVelocity.z);
                sc.properties["avx"] = std::to_string(rb->angularVelocity.x);
                sc.properties["avy"] = std::to_string(rb->angularVelocity.y);
                sc.properties["avz"] = std::to_string(rb->angularVelocity.z);
                return sc;
            },
            [](World& world, EntityID entity, const SerializedComponent& data)
            {
                auto& rb = world.AddComponent<RigidBodyComponent>(entity);
                auto& p = data.properties;
                rb.type = static_cast<RigidBodyComponent::Type>(RequireInteger<int>(p, "type"));
                rb.mass = RequireFloat(p, "mass");
                rb.friction = RequireFloat(p, "friction");
                rb.restitution = RequireFloat(p, "restitution");
                rb.linearDamping = RequireFloat(p, "linearDamping");
                rb.angularDamping = RequireFloat(p, "angularDamping");
                rb.isTrigger = RequireBool(p, "isTrigger");
                rb.linearVelocity = {RequireFloat(p, "lvx"), RequireFloat(p, "lvy"), RequireFloat(p, "lvz")};
                rb.angularVelocity = {RequireFloat(p, "avx"), RequireFloat(p, "avy"), RequireFloat(p, "avz")};
                rb.physicsBodyHandle = nullptr;
            });

        reg.Register(
            "ColliderComponent",
            [](const void* comp) -> SerializedComponent
            {
                const auto* col = static_cast<const ColliderComponent*>(comp);
                SerializedComponent sc;
                sc.typeName = "ColliderComponent";
                sc.properties["shape"] = std::to_string(static_cast<int>(col->shape));
                sc.properties["hex"] = std::to_string(col->halfExtents.x);
                sc.properties["hey"] = std::to_string(col->halfExtents.y);
                sc.properties["hez"] = std::to_string(col->halfExtents.z);
                sc.properties["radius"] = std::to_string(col->radius);
                sc.properties["height"] = std::to_string(col->height);
                sc.properties["ox"] = std::to_string(col->offset.x);
                sc.properties["oy"] = std::to_string(col->offset.y);
                sc.properties["oz"] = std::to_string(col->offset.z);
                return sc;
            },
            [](World& world, EntityID entity, const SerializedComponent& data)
            {
                auto& col = world.AddComponent<ColliderComponent>(entity);
                auto& p = data.properties;
                col.shape = static_cast<ColliderComponent::Shape>(RequireInteger<int>(p, "shape"));
                col.halfExtents = {RequireFloat(p, "hex"), RequireFloat(p, "hey"), RequireFloat(p, "hez")};
                col.radius = RequireFloat(p, "radius");
                col.height = RequireFloat(p, "height");
                col.offset = {RequireFloat(p, "ox"), RequireFloat(p, "oy"), RequireFloat(p, "oz")};
            });
    }

    static void RegisterGameplaySerializers(ComponentSerializerRegistry& reg)
    {
        reg.Register(
            "HealthComponent",
            [](const void* comp) -> SerializedComponent
            {
                const auto* h = static_cast<const HealthComponent*>(comp);
                SerializedComponent sc;
                sc.typeName = "HealthComponent";
                sc.properties["health"] = std::to_string(h->health);
                sc.properties["maxHealth"] = std::to_string(h->maxHealth);
                sc.properties["isDead"] = h->isDead ? "1" : "0";
                return sc;
            },
            [](World& world, EntityID entity, const SerializedComponent& data)
            {
                auto& h = world.AddComponent<HealthComponent>(entity);
                auto& p = data.properties;
                h.health = RequireFloat(p, "health");
                h.maxHealth = RequireFloat(p, "maxHealth");
                h.isDead = RequireBool(p, "isDead");
            });

        reg.Register(
            "LightComponent",
            [](const void* comp) -> SerializedComponent
            {
                const auto* l = static_cast<const LightComponent*>(comp);
                SerializedComponent sc;
                sc.typeName = "LightComponent";
                sc.properties["type"] = std::to_string(static_cast<int>(l->type));
                sc.properties["cr"] = std::to_string(l->color.x);
                sc.properties["cg"] = std::to_string(l->color.y);
                sc.properties["cb"] = std::to_string(l->color.z);
                sc.properties["intensity"] = std::to_string(l->intensity);
                sc.properties["range"] = std::to_string(l->range);
                sc.properties["castShadows"] = l->castShadows ? "1" : "0";
                return sc;
            },
            [](World& world, EntityID entity, const SerializedComponent& data)
            {
                auto& l = world.AddComponent<LightComponent>(entity);
                auto& p = data.properties;
                l.type = static_cast<LightComponent::Type>(RequireInteger<int>(p, "type"));
                l.color = {RequireFloat(p, "cr"), RequireFloat(p, "cg"), RequireFloat(p, "cb")};
                l.intensity = RequireFloat(p, "intensity");
                l.range = RequireFloat(p, "range");
                l.castShadows = RequireBool(p, "castShadows");
            });

        // AudioSourceComponent — handled by RegisterReflectedSerializers()
        // (reflection covers all hand-written fields plus pitch, minDistance, maxDistance)

        reg.Register(
            "ParticleEmitterComponent",
            [](const void* comp) -> SerializedComponent
            {
                const auto* pe = static_cast<const ParticleEmitterComponent*>(comp);
                SerializedComponent sc;
                sc.typeName = "ParticleEmitterComponent";
                sc.properties["effectName"] = pe->effectName;
                sc.properties["autoPlay"] = pe->autoPlay ? "1" : "0";
                sc.properties["isPlaying"] = pe->isPlaying ? "1" : "0";
                sc.properties["emissionRate"] = std::to_string(pe->emissionRate);
                sc.properties["lifetime"] = std::to_string(pe->lifetime);
                sc.properties["scr"] = std::to_string(pe->startColor.x);
                sc.properties["scg"] = std::to_string(pe->startColor.y);
                sc.properties["scb"] = std::to_string(pe->startColor.z);
                sc.properties["sca"] = std::to_string(pe->startColor.w);
                sc.properties["startSize"] = std::to_string(pe->startSize);
                sc.properties["startSpeed"] = std::to_string(pe->startSpeed);
                return sc;
            },
            [](World& world, EntityID entity, const SerializedComponent& data)
            {
                auto& pe = world.AddComponent<ParticleEmitterComponent>(entity);
                auto& p = data.properties;
                pe.effectName = RequireString(p, "effectName");
                pe.autoPlay = RequireBool(p, "autoPlay");
                pe.isPlaying = RequireBool(p, "isPlaying");
                pe.emissionRate = RequireFloat(p, "emissionRate");
                pe.lifetime = RequireFloat(p, "lifetime");
                pe.startColor = {RequireFloat(p, "scr"), RequireFloat(p, "scg"), RequireFloat(p, "scb"),
                                 RequireFloat(p, "sca")};
                pe.startSize = RequireFloat(p, "startSize");
                pe.startSpeed = RequireFloat(p, "startSpeed");
                pe.emitterHandle = nullptr;
            });

        reg.Register(
            "AnimationController",
            [](const void* comp) -> SerializedComponent
            {
                const auto* ac = static_cast<const AnimationController*>(comp);
                SerializedComponent sc;
                sc.typeName = "AnimationController";
                sc.properties["currentAnimation"] = ac->currentAnimation;
                sc.properties["defaultAnimation"] = ac->defaultAnimation;
                sc.properties["playbackSpeed"] = std::to_string(ac->playbackSpeed);
                sc.properties["currentTime"] = std::to_string(ac->currentTime);
                sc.properties["playing"] = ac->playing ? "1" : "0";
                sc.properties["loop"] = ac->loop ? "1" : "0";
                std::string animList;
                for (size_t i = 0; i < ac->availableAnimations.size(); ++i)
                {
                    if (i > 0)
                        animList += ",";
                    animList += ac->availableAnimations[i];
                }
                sc.properties["availableAnimations"] = animList;
                return sc;
            },
            [](World& world, EntityID entity, const SerializedComponent& data)
            {
                auto& ac = world.AddComponent<AnimationController>(entity);
                auto& p = data.properties;
                ac.currentAnimation = RequireString(p, "currentAnimation");
                ac.defaultAnimation = RequireString(p, "defaultAnimation");
                ac.playbackSpeed = RequireFloat(p, "playbackSpeed");
                ac.currentTime = RequireFloat(p, "currentTime");
                ac.playing = RequireBool(p, "playing");
                ac.loop = RequireBool(p, "loop");
                std::string animList = RequireString(p, "availableAnimations");
                if (!animList.empty())
                {
                    std::istringstream iss(animList);
                    std::string anim;
                    while (std::getline(iss, anim, ','))
                    {
                        if (!anim.empty())
                            ac.availableAnimations.push_back(anim);
                    }
                }
                ac.animInstanceHandle = nullptr;
            });

        reg.Register(
            "NetworkIdentity",
            [](const void* comp) -> SerializedComponent
            {
                const auto* ni = static_cast<const NetworkIdentity*>(comp);
                SerializedComponent sc;
                sc.typeName = "NetworkIdentity";
                sc.properties["networkID"] = std::to_string(ni->networkID);
                sc.properties["ownerClientID"] = std::to_string(ni->ownerClientID);
                sc.properties["isLocalAuthority"] = ni->isLocalAuthority ? "1" : "0";
                sc.properties["replicateTransform"] = ni->replicateTransform ? "1" : "0";
                sc.properties["replicateHealth"] = ni->replicateHealth ? "1" : "0";
                return sc;
            },
            [](World& world, EntityID entity, const SerializedComponent& data)
            {
                auto& ni = world.AddComponent<NetworkIdentity>(entity);
                auto& p = data.properties;
                ni.networkID = RequireInteger<uint32_t>(p, "networkID");
                ni.ownerClientID = RequireInteger<uint32_t>(p, "ownerClientID");
                ni.isLocalAuthority = RequireBool(p, "isLocalAuthority");
                ni.replicateTransform = RequireBool(p, "replicateTransform");
                ni.replicateHealth = RequireBool(p, "replicateHealth");
            });
    }

    void ComponentSerializerRegistry::RegisterBuiltins()
    {
        RegisterCoreSerializers(*this);
        RegisterPhysicsSerializers(*this);
        RegisterGameplaySerializers(*this);
        RegisterReflectedSerializers();
    }

    // ============================================================================
    // Reflection-driven auto-serialization
    // ============================================================================

    void ComponentSerializerRegistry::RegisterReflectedSerializers()
    {
        auto& typeRegistry = Spark::TypeRegistry::Get();
        auto& factory = Spark::ComponentFactory::Get();

        for (const auto& typeName : factory.GetRegisteredNames())
        {
            // Skip types that already have hand-written serializers
            if (HasSerializer(typeName))
                continue;

            const auto* typeInfo = typeRegistry.FindTypeByName(typeName);
            if (!typeInfo || typeInfo->fields.empty())
                continue;

            // Capture the type name and field list for the lambdas
            std::string capturedName = typeName;
            std::vector<Spark::FieldInfo> capturedFields = typeInfo->fields;

            Register(
                typeName,
                // Serialize: iterate reflected fields, call GetFieldAsString
                [capturedName, capturedFields](const void* comp) -> SerializedComponent
                {
                    SerializedComponent sc;
                    sc.typeName = capturedName;
                    for (const auto& field : capturedFields)
                    {
                        sc.properties[field.fieldName] = Spark::GetFieldAsString(comp, field);
                    }
                    return sc;
                },
                // Deserialize: add component via factory, set fields from properties
                [capturedName, capturedFields](World& world, EntityID entity, const SerializedComponent& data)
                {
                    auto& f = Spark::ComponentFactory::Get();
                    auto entityId = static_cast<uint32_t>(entity);
                    f.AddComponent(capturedName, &world, entityId);
                    void* raw = f.GetComponentRaw(capturedName, &world, entityId);
                    if (!raw)
                    {
                        throw std::runtime_error("reflected component '" + capturedName +
                                                 "' could not be retrieved after construction");
                    }
                    for (const auto& field : capturedFields)
                    {
                        const auto it = data.properties.find(field.fieldName);
                        if (it == data.properties.end())
                            throw std::runtime_error("reflected component '" + capturedName +
                                                     "' is missing required field '" + field.fieldName + "'");
                        if (it->second.size() > kMaxPropertyLen)
                            throw std::runtime_error("reflected field '" + capturedName + "." + field.fieldName +
                                                     "' exceeds the uint16 wire limit");
                        if (!Spark::SetFieldFromString(raw, field, it->second))
                            throw std::runtime_error("reflected field '" + capturedName + "." + field.fieldName +
                                                     "' has an invalid serialized value");
                    }
                });
        }
    }

    // ============================================================================
    // SaveSystem
    // ============================================================================

    SaveSystem& SaveSystem::GetInstance()
    {
        static SaveSystem instance;
        return instance;
    }

    bool SaveSystem::Initialize(const std::string& saveDirectory)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Save);
        SPARK_REQUIRE_MSG(Spark::LogCategory::Save, !saveDirectory.empty(),
                          "SaveSystem::Initialize — saveDirectory must not be empty");
        SPARK_LOG_INFO(Spark::LogCategory::Save, "SaveSystem initializing with directory '%s'", saveDirectory.c_str());
        m_saveDirectory = saveDirectory;
        try
        {
            fs::create_directories(m_saveDirectory);
            ComponentSerializerRegistry::GetInstance().RegisterBuiltins();
            return true;
        }
        catch (const std::exception& e)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "Save system initialization error: %s", e.what());
            return false;
        }
        catch (...)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "Save system: unknown exception during initialization");
            return false;
        }
    }

    bool SaveSystem::Save(const std::string& slotName, World& world, const SaveMetadata& metadata)
    {
        return Save(slotName, world, metadata, {});
    }

    bool SaveSystem::Save(const std::string& slotName, World& world, const SaveMetadata& metadata,
                          const std::unordered_map<std::string, std::string>& customState)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Save);
        SPARK_REQUIRE_MSG(Spark::LogCategory::Save, !slotName.empty(), "SaveSystem::Save — slotName must not be empty");
        if (!IsValidSlotName(slotName))
            return false;
        EventBus::Global().Publish<SaveBeginEvent>({slotName});
        SaveData data = SerializeWorld(world, metadata);
        data.customState = customState;
        bool ok = WriteToFile(GetSavePath(slotName), data);
        EventBus::Global().Publish<SaveCompleteEvent>({slotName, ok});
        return ok;
    }

    bool SaveSystem::Load(const std::string& slotName, World& world)
    {
        std::unordered_map<std::string, std::string> ignoredCustomState;
        return Load(slotName, world, ignoredCustomState);
    }

    bool SaveSystem::Load(const std::string& slotName, World& world,
                          std::unordered_map<std::string, std::string>& outCustomState)
    {
        return Load(slotName, world, outCustomState, {});
    }

    bool SaveSystem::Load(const std::string& slotName, World& world,
                          std::unordered_map<std::string, std::string>& outCustomState,
                          const CustomStateValidator& customStateValidator)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Save);
        SPARK_REQUIRE_MSG(Spark::LogCategory::Save, !slotName.empty(), "SaveSystem::Load — slotName must not be empty");
        if (!IsValidSlotName(slotName))
            return false;
        EventBus::Global().Publish<LoadBeginEvent>({slotName});
        SaveData data;
        if (!ReadFromFile(GetSavePath(slotName), data))
        {
            EventBus::Global().Publish<LoadCompleteEvent>({slotName, false});
            return false;
        }
        if (customStateValidator && !customStateValidator(data.customState))
        {
            EventBus::Global().Publish<LoadCompleteEvent>({slotName, false});
            return false;
        }
        bool ok = false;
        try
        {
            // Copy caller-visible side data before the World commit. The final
            // map exchange is noexcept for std::allocator-backed unordered_map.
            auto stagedCustomState = data.customState;
            ok = DeserializeWorld(data, world);
            if (ok)
            {
                static_assert(noexcept(outCustomState.swap(stagedCustomState)));
                outCustomState.swap(stagedCustomState);
            }
        }
        catch (const std::exception& error)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Save,
                           "Load: failed while staging transactional state: %s; caller custom state was not changed",
                           error.what());
        }
        catch (...)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Save,
                           "Load: unknown failure while staging transactional state; caller custom state was not "
                           "changed");
        }
        EventBus::Global().Publish<LoadCompleteEvent>({slotName, ok});
        return ok;
    }

    bool SaveSystem::QuickSave(World& world, const SaveMetadata& metadata)
    {
        SaveMetadata meta = metadata;
        if (meta.saveName.empty())
            meta.saveName = "Quick Save";
        return Save(m_quickSaveSlot, world, meta);
    }

    bool SaveSystem::QuickLoad(World& world)
    {
        return Load(m_quickSaveSlot, world);
    }

    bool SaveSystem::AutoSave(World& world, const SaveMetadata& metadata)
    {
        std::string slotName = "__autosave_" + std::to_string(m_currentAutoSaveIndex);
        m_currentAutoSaveIndex = (m_currentAutoSaveIndex + 1) % m_maxAutoSaves;
        return Save(slotName, world, metadata);
    }

    bool SaveSystem::DeleteSave(const std::string& slotName)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Save);
        SPARK_REQUIRE_MSG(Spark::LogCategory::Save, !slotName.empty(),
                          "SaveSystem::DeleteSave — slotName must not be empty");
        if (!IsValidSlotName(slotName))
            return false;
        try
        {
            std::string path = GetSavePath(slotName);
            if (path.empty() || !fs::exists(path))
                return true;
            return fs::remove(path);
        }
        catch (const std::exception& e)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "Save system error (DeleteSave): %s", e.what());
            return false;
        }
        catch (...)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "Save system: unknown exception in DeleteSave");
            return false;
        }
    }

    std::vector<SaveMetadata> SaveSystem::GetSaveSlots() const
    {
        std::vector<SaveMetadata> slots;
        try
        {
            for (const auto& entry : fs::directory_iterator(m_saveDirectory))
            {
                if (entry.path().extension() == ".spark_save")
                {
                    // Metadata-only read: parse just the header + metadata block and stop
                    // before the (potentially large) entity data. Enumerating N save slots
                    // must not cost O(total bytes of all saves).
                    SaveMetadata meta;
                    if (ReadMetadataOnly(entry.path().string(), meta))
                    {
                        slots.push_back(meta);
                    }
                }
            }
        }
        catch (const std::exception& e)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "Save system error (GetSaveSlots): %s", e.what());
        }
        catch (...)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "Save system: unknown exception in GetSaveSlots");
        }

        // Sort by timestamp (newest first)
        std::sort(slots.begin(), slots.end(),
                  [](const SaveMetadata& a, const SaveMetadata& b) { return a.timestamp > b.timestamp; });
        return slots;
    }

    bool SaveSystem::GetSaveMetadata(const std::string& slotName, SaveMetadata& outMetadata) const
    {
        // Fast path: read only the metadata header, not the full entity payload.
        return ReadMetadataOnly(GetSavePath(slotName), outMetadata);
    }

    bool SaveSystem::SaveExists(const std::string& slotName) const
    {
        return fs::exists(GetSavePath(slotName));
    }

    template <typename T>
    static void TrySerialize(World& world, entt::entity entity, const ComponentSerializerRegistry& reg,
                             const char* typeName, std::vector<SerializedComponent>& out)
    {
        if (auto* comp = world.GetComponent<T>(entity))
        {
            if (reg.HasSerializer(typeName))
                out.push_back(reg.Serialize(typeName, comp));
        }
    }

    SaveData SaveSystem::SerializeWorld(World& world, const SaveMetadata& metadata) const
    {
        SaveData data;
        data.metadata = metadata;
        data.metadata.version = kCurrentSaveVersion;
        data.metadata.timestamp = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count());

        auto& registry = world.GetRegistry();
        const auto& serializerRegistry = ComponentSerializerRegistry::GetInstance();

        auto&& entityStorage = registry.storage<entt::entity>();
        for (auto&& [entity] : entityStorage.each())
        {
            SerializedEntity se;
            se.entityID = static_cast<uint32_t>(entity);

            auto* name = world.GetComponent<NameComponent>(entity);
            se.name = name ? name->name : "";

            TrySerialize<Transform>(world, entity, serializerRegistry, "Transform", se.components);
            TrySerialize<MeshRenderer>(world, entity, serializerRegistry, "MeshRenderer", se.components);
            TrySerialize<HealthComponent>(world, entity, serializerRegistry, "HealthComponent", se.components);
            TrySerialize<LightComponent>(world, entity, serializerRegistry, "LightComponent", se.components);
            TrySerialize<AudioSourceComponent>(world, entity, serializerRegistry, "AudioSourceComponent",
                                               se.components);
            TrySerialize<Camera>(world, entity, serializerRegistry, "Camera", se.components);
            TrySerialize<Script>(world, entity, serializerRegistry, "Script", se.components);
            TrySerialize<RigidBodyComponent>(world, entity, serializerRegistry, "RigidBodyComponent", se.components);
            TrySerialize<ColliderComponent>(world, entity, serializerRegistry, "ColliderComponent", se.components);
            TrySerialize<ParticleEmitterComponent>(world, entity, serializerRegistry, "ParticleEmitterComponent",
                                                   se.components);
            TrySerialize<AnimationController>(world, entity, serializerRegistry, "AnimationController", se.components);
            TrySerialize<NetworkIdentity>(world, entity, serializerRegistry, "NetworkIdentity", se.components);
            TrySerialize<TagComponent>(world, entity, serializerRegistry, "TagComponent", se.components);
            TrySerialize<ActiveComponent>(world, entity, serializerRegistry, "ActiveComponent", se.components);

            if (!se.components.empty())
                data.entities.push_back(se);
        }

        return data;
    }

    bool SaveSystem::MigrateToCurrentVersion(SaveData& data)
    {
        if (!IsSupportedSaveVersion(data.metadata.version))
            return false;

        // Work on a copy so future multi-step migrations can retain the same
        // fail-without-mutation contract if any individual step rejects data.
        SaveData migrated = data;
        while (migrated.metadata.version < kCurrentSaveVersion)
        {
            switch (migrated.metadata.version)
            {
            case 1:
                // v1 did not carry screenshotPath on disk. Its exact v2 value is
                // therefore empty, even if a caller manually populated a v1 struct.
                migrated.metadata.screenshotPath.clear();
                migrated.metadata.version = 2;
                break;
            default:
                return false;
            }
        }

        data = std::move(migrated);
        return true;
    }

    bool SaveSystem::DeserializeWorld(const SaveData& data, World& world) const
    {
        SaveData migratedData = data;
        if (!MigrateToCurrentVersion(migratedData))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Save,
                           "DeserializeWorld: save version %u is unsupported; supported versions are %u..%u",
                           data.metadata.version, kOldestSupportedSaveVersion, kCurrentSaveVersion);
            return false;
        }

        const auto& serializerRegistry = ComponentSerializerRegistry::GetInstance();
        const auto& componentFactory = Spark::ComponentFactory::Get();
        World candidateWorld;
        std::vector<Spark::ComponentOps::SwapStorageContentsFn> stagedStorageSwaps;
        std::vector<EntityID> liveEntities;
        std::vector<EntityID> incomingEntities;
        try
        {
            for (const auto& serializedEntity : migratedData.entities)
            {
                const EntityID entity = candidateWorld.CreateEntity(serializedEntity.name);

                for (const auto& component : serializedEntity.components)
                {
                    if (!serializerRegistry.HasSerializer(component.typeName))
                    {
                        SPARK_LOG_WARN(Spark::LogCategory::Save,
                                       "DeserializeWorld: no deserializer is registered for component '%s'; "
                                       "the live world was not changed",
                                       component.typeName.c_str());
                        return false;
                    }

                    serializerRegistry.Deserialize(component.typeName, candidateWorld, entity, component);
                }
            }

            // Resolve and materialize every component pool before touching the
            // live world. The stored callbacks swap only basic_storage payloads,
            // leaving the live EnTT signal mixins (and their observers) in place.
            std::unordered_set<std::string> stagedTypes;
            auto stageComponentType = [&](const std::string& typeName)
            {
                if (!stagedTypes.insert(typeName).second)
                    return;

                const Spark::ComponentOps* operations = componentFactory.GetOperations(typeName);
                if (!operations || !operations->prepareStorage || !operations->swapStorageContents)
                    throw std::runtime_error("component '" + typeName +
                                             "' has no transactional storage operations registered");

                operations->prepareStorage(&world);
                operations->prepareStorage(&candidateWorld);
                stagedStorageSwaps.push_back(operations->swapStorageContents);
            };

            stageComponentType("NameComponent");
            for (const auto& serializedEntity : migratedData.entities)
            {
                for (const auto& component : serializedEntity.components)
                    stageComponentType(component.typeName);
            }

            auto&& currentStorage = world.GetRegistry().storage<entt::entity>();
            liveEntities.reserve(world.GetEntityCount());
            for (auto&& [entity] : currentStorage.each())
                liveEntities.push_back(entity);

            auto&& incomingStorage = candidateWorld.GetRegistry().storage<entt::entity>();
            incomingEntities.reserve(candidateWorld.GetEntityCount());
            for (auto&& [entity] : incomingStorage.each())
                incomingEntities.push_back(entity);
        }
        catch (const std::exception& error)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Save,
                           "DeserializeWorld: component deserializer failed: %s; the live world was not changed",
                           error.what());
            return false;
        }
        catch (...)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Save,
                           "DeserializeWorld: component deserializer threw an unknown exception; "
                           "the live world was not changed");
            return false;
        }

        try
        {
            // Use the World's lifecycle path so hierarchy links, EnTT destroy
            // observers, and per-entity event subscriptions are retired before
            // incoming identifiers can be reused.
            for (const EntityID entity : liveEntities)
                world.DestroyEntity(entity);
            for (const EntityID entity : incomingEntities)
                Spark::EntityEventBus::Global().RemoveEntity(static_cast<Spark::EventEntityID>(entity));

            // All allocations and type lookups are complete. These exchanges
            // are noexcept and intentionally avoid registry move-assignment,
            // which would transplant signal objects away from live observers.
            for (const auto swapStorageContents : stagedStorageSwaps)
                swapStorageContents(&world, &candidateWorld);

            using EntityPayloadStorage = entt::basic_storage<entt::entity>;
            auto& destinationEntities = world.GetRegistry().storage<entt::entity>();
            auto& sourceEntities = candidateWorld.GetRegistry().storage<entt::entity>();
            static_cast<EntityPayloadStorage&>(destinationEntities)
                .swap(static_cast<EntityPayloadStorage&>(sourceEntities));
        }
        catch (const std::exception& error)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Save, "DeserializeWorld: lifecycle commit failed: %s", error.what());
            return false;
        }
        catch (...)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Save, "DeserializeWorld: lifecycle commit failed unexpectedly");
            return false;
        }
        return true;
    }

    bool SaveSystem::WriteToFile(const std::string& filepath, const SaveData& data) const
    {
        try
        {
            if (data.metadata.version != kCurrentSaveVersion)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Save,
                               "WriteToFile: refusing to emit version %u; this build writes version %u only",
                               data.metadata.version, kCurrentSaveVersion);
                return false;
            }

            // Reject any string that would silently truncate to uint16_t on the wire.
            // Silent truncation corrupts saves: the length prefix disagrees with the payload.
            constexpr size_t kMaxStr16 = std::numeric_limits<uint16_t>::max();
            auto rejectIfTooLong = [&](const std::string& s, const char* field)
            {
                if (s.size() > kMaxStr16)
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Core,
                                   "Save system: %s length %zu exceeds uint16 max (%zu); refusing to truncate", field,
                                   s.size(), kMaxStr16);
                    return true;
                }
                return false;
            };
            // The metadata block is stored as newline-delimited text and parsed back with
            // std::getline. An embedded '\n'/'\r' in any of the first three fields shifts
            // every subsequent getline/>> read and corrupts the rest of the metadata on
            // load, so reject such values instead of writing a corrupt save.
            auto rejectIfHasNewline = [&](const std::string& s, const char* field)
            {
                if (s.find_first_of("\n\r") != std::string::npos)
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Core,
                                   "Save system: %s contains an embedded newline; refusing to write a corrupt save",
                                   field);
                    return true;
                }
                return false;
            };
            if (rejectIfHasNewline(data.metadata.saveName, "metadata.saveName") ||
                rejectIfHasNewline(data.metadata.sceneName, "metadata.sceneName") ||
                rejectIfHasNewline(data.metadata.playerClass, "metadata.playerClass") ||
                rejectIfHasNewline(data.metadata.screenshotPath, "metadata.screenshotPath"))
            {
                return false;
            }
            for (const auto& entity : data.entities)
            {
                if (rejectIfTooLong(entity.name, "entity.name"))
                    return false;
                if (entity.components.size() > kMaxStr16)
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Core, "Save system: component count %zu exceeds uint16 max",
                                   entity.components.size());
                    return false;
                }
                for (const auto& comp : entity.components)
                {
                    if (rejectIfTooLong(comp.typeName, "component.typeName"))
                        return false;
                    if (comp.properties.size() > kMaxStr16)
                    {
                        SPARK_LOG_WARN(Spark::LogCategory::Core, "Save system: property count %zu exceeds uint16 max",
                                       comp.properties.size());
                        return false;
                    }
                    for (const auto& [key, value] : comp.properties)
                    {
                        if (rejectIfTooLong(key, "property.key") || rejectIfTooLong(value, "property.value"))
                            return false;
                    }
                }
            }
            for (const auto& [key, value] : data.customState)
            {
                if (rejectIfTooLong(key, "customState.key") || rejectIfTooLong(value, "customState.value"))
                    return false;
            }

            // Write to temp file first, then rename for atomic save (prevents corruption on crash)
            std::string tmpPath = filepath + ".tmp";
            std::ofstream file(tmpPath, std::ios::binary);
            if (!file.is_open())
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Save,
                                "WriteToFile: cannot open temp file '%s' for writing (errno=%d)", tmpPath.c_str(),
                                errno);
                return false;
            }

            // Write header
            const char magic[] = "SPRK";
            file.write(magic, 4);
            const uint32_t version = kCurrentSaveVersion;
            file.write(reinterpret_cast<const char*>(&version), sizeof(version));

            // Write metadata as text block
            std::ostringstream metaStream;
            metaStream << data.metadata.saveName << "\n";
            metaStream << data.metadata.sceneName << "\n";
            metaStream << data.metadata.playerClass << "\n";
            metaStream << data.metadata.screenshotPath << "\n";
            metaStream << data.metadata.timestamp << "\n";
            metaStream << data.metadata.playTime << "\n";
            metaStream << data.metadata.playerHealth << "\n";
            metaStream << data.metadata.playerArmor << "\n";
            metaStream << data.metadata.playerPosition.x << " " << data.metadata.playerPosition.y << " "
                       << data.metadata.playerPosition.z << "\n";
            metaStream << data.metadata.playerKills << "\n";
            metaStream << data.metadata.playerDeaths << "\n";

            std::string metaStr = metaStream.str();
            uint32_t metaSize = static_cast<uint32_t>(metaStr.size());
            file.write(reinterpret_cast<const char*>(&metaSize), sizeof(metaSize));
            file.write(metaStr.c_str(), metaSize);

            // Write entity count
            uint32_t entityCount = static_cast<uint32_t>(data.entities.size());
            file.write(reinterpret_cast<const char*>(&entityCount), sizeof(entityCount));

            // Write each entity
            for (const auto& entity : data.entities)
            {
                // Entity name
                uint16_t nameLen = static_cast<uint16_t>(entity.name.size());
                file.write(reinterpret_cast<const char*>(&nameLen), sizeof(nameLen));
                file.write(entity.name.c_str(), nameLen);

                // Component count
                uint16_t compCount = static_cast<uint16_t>(entity.components.size());
                file.write(reinterpret_cast<const char*>(&compCount), sizeof(compCount));

                for (const auto& comp : entity.components)
                {
                    // Type name
                    uint16_t typeLen = static_cast<uint16_t>(comp.typeName.size());
                    file.write(reinterpret_cast<const char*>(&typeLen), sizeof(typeLen));
                    file.write(comp.typeName.c_str(), typeLen);

                    // Properties
                    uint16_t propCount = static_cast<uint16_t>(comp.properties.size());
                    file.write(reinterpret_cast<const char*>(&propCount), sizeof(propCount));

                    for (const auto& [key, value] : comp.properties)
                    {
                        uint16_t keyLen = static_cast<uint16_t>(key.size());
                        file.write(reinterpret_cast<const char*>(&keyLen), sizeof(keyLen));
                        file.write(key.c_str(), keyLen);

                        uint16_t valLen = static_cast<uint16_t>(value.size());
                        file.write(reinterpret_cast<const char*>(&valLen), sizeof(valLen));
                        file.write(value.c_str(), valLen);
                    }
                }
            }

            // Write custom state key-value pairs
            uint32_t customStateCount = static_cast<uint32_t>(data.customState.size());
            file.write(reinterpret_cast<const char*>(&customStateCount), sizeof(customStateCount));

            for (const auto& [key, value] : data.customState)
            {
                uint16_t keyLen = static_cast<uint16_t>(key.size());
                file.write(reinterpret_cast<const char*>(&keyLen), sizeof(keyLen));
                file.write(key.c_str(), keyLen);

                uint16_t valLen = static_cast<uint16_t>(value.size());
                file.write(reinterpret_cast<const char*>(&valLen), sizeof(valLen));
                file.write(value.c_str(), valLen);
            }

            file.close();
            if (file.fail())
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core, "Save system: write failed for %s", tmpPath.c_str());
                std::error_code rmEc;
                std::filesystem::remove(tmpPath, rmEc);
                return false;
            }

            std::error_code ec;
            if (!FlushFileDurably(tmpPath, ec))
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core, "Save system: durable flush failed for %s: %s",
                               tmpPath.c_str(), ec.message().c_str());
                std::error_code removeError;
                std::filesystem::remove(tmpPath, removeError);
                return false;
            }

            // Replace the destination atomically. std::filesystem::rename does not
            // replace an existing file on Windows, which broke every second save to
            // the same slot (including QuickSave).
            if (!ReplaceFileAtomically(tmpPath, filepath, ec))
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core, "Save system: atomic replace failed %s -> %s: %s",
                               tmpPath.c_str(), filepath.c_str(), ec.message().c_str());
                std::error_code removeError;
                std::filesystem::remove(tmpPath, removeError);
                return false;
            }

            // Invalidate any cached copy so future reads see the new data
            if (m_fileCache)
            {
                m_fileCache->Invalidate(filepath);
            }

            return true;
        }
        catch (const std::exception& e)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "Save system error (WriteToFile): %s", e.what());
            // Clean up temp file on failure
            std::error_code ec;
            std::filesystem::remove(filepath + ".tmp", ec);
            return false;
        }
        catch (...)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "Save system: unknown exception in WriteToFile");
            std::error_code ec;
            std::filesystem::remove(filepath + ".tmp", ec);
            return false;
        }
    }

    bool SaveSystem::ReadFromFile(const std::string& filepath, SaveData& outData) const
    {
        try
        {
            constexpr uintmax_t kMaxSaveFileSize = 512ull * 1024ull * 1024ull;

            // Parse transactionally so a malformed file never leaves callers
            // with a partially populated SaveData object.
            SaveData parsedData;

            // Enforce the same cap before consulting LocalFileCache. Otherwise a
            // cached read can materialize an oversized file and bypass the direct
            // stream branch's limit entirely.
            std::error_code sizeError;
            const uintmax_t onDiskSize = std::filesystem::file_size(filepath, sizeError);
            if (!sizeError && onDiskSize > kMaxSaveFileSize)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core, "Save file too large: %llu bytes",
                               static_cast<unsigned long long>(onDiskSize));
                return false;
            }

            // Try reading via file cache for binary data
            std::vector<uint8_t> fileData;
            bool fromCache = false;

            if (m_fileCache)
            {
                auto result = m_fileCache->ReadBinary(filepath);
                if (result.IsOk())
                {
                    fileData = result.Value();
                    fromCache = true;
                }
            }

            if (!fromCache)
            {
                std::ifstream file(filepath, std::ios::binary);
                if (!file.is_open())
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Save, "ReadFromFile: failed to open save file '%s' (errno=%d)",
                                   filepath.c_str(), errno);
                    return false;
                }

                file.seekg(0, std::ios::end);
                auto size = file.tellg();
                if (size < 0)
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Save,
                                   "ReadFromFile: tellg() returned negative for '%s' (file unreadable)",
                                   filepath.c_str());
                    return false;
                }
                file.seekg(0, std::ios::beg);

                // Sanity cap: reject unreasonably large save files (512 MB)
                if (static_cast<uintmax_t>(size) > kMaxSaveFileSize)
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Core, "Save file too large: %lld bytes",
                                   static_cast<long long>(size));
                    return false;
                }

                fileData.resize(static_cast<size_t>(size));
                file.read(reinterpret_cast<char*>(fileData.data()), size);
                if (!file)
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Core, "Save file read incomplete: expected %lld bytes",
                                   static_cast<long long>(size));
                    return false;
                }
            }

            // A cache entry may outlive an external file replacement. Keep the
            // parser cap authoritative even when the on-disk preflight raced or
            // the value came from an existing cache entry.
            if (fileData.size() > kMaxSaveFileSize)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core, "Cached save file too large: %zu bytes", fileData.size());
                return false;
            }

            if (fileData.size() < 8)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Save,
                               "ReadFromFile: save file '%s' too small (%zu bytes, need at least 8)", filepath.c_str(),
                               fileData.size());
                return false;
            }

            // Parse from the byte buffer using an offset cursor
            size_t offset = 0;

            auto readBytes = [&](void* dest, size_t count) -> bool
            {
                if (offset + count > fileData.size())
                    return false;
                std::memcpy(dest, fileData.data() + offset, count);
                offset += count;
                return true;
            };

            // Read and verify header
            char magic[4];
            if (!readBytes(magic, 4))
            {
                SPARK_LOG_WARN(Spark::LogCategory::Save, "ReadFromFile: truncated magic header in '%s'",
                               filepath.c_str());
                return false;
            }
            if (std::string(magic, 4) != "SPRK")
            {
                SPARK_LOG_WARN(Spark::LogCategory::Save,
                               "ReadFromFile: invalid magic '%c%c%c%c' in '%s' (expected 'SPRK')", magic[0], magic[1],
                               magic[2], magic[3], filepath.c_str());
                return false;
            }

            uint32_t version;
            if (!readBytes(&version, sizeof(version)))
                return false;
            parsedData.metadata.version = version;

            if (!IsSupportedSaveVersion(version))
            {
                LogUnsupportedSaveVersion(filepath, version, "ReadFromFile");
                return false;
            }

            // Read metadata
            uint32_t metaSize;
            if (!readBytes(&metaSize, sizeof(metaSize)))
                return false;
            if (offset + metaSize > fileData.size())
                return false;
            std::string metaStr(reinterpret_cast<const char*>(fileData.data() + offset), metaSize);
            offset += metaSize;

            if (!ParseMetadataBlock(version, metaStr, parsedData.metadata))
            {
                SPARK_LOG_WARN(Spark::LogCategory::Save, "ReadFromFile: save '%s' has malformed version %u metadata",
                               filepath.c_str(), version);
                return false;
            }

            // Read entities
            uint32_t entityCount;
            if (!readBytes(&entityCount, sizeof(entityCount)))
                return false;

            // Sanity cap: prevent malformed files from causing huge allocations
            constexpr uint32_t kMaxEntities = 1'000'000;
            if (entityCount > kMaxEntities)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core, "Save file entity count %u exceeds limit %u", entityCount,
                               kMaxEntities);
                return false;
            }

            for (uint32_t i = 0; i < entityCount; ++i)
            {
                SerializedEntity entity;

                uint16_t nameLen;
                if (!readBytes(&nameLen, sizeof(nameLen)))
                    return false;
                // No explicit cap: the uint16 length prefix bounds allocations to 65535 bytes,
                // matching the writer's rejectIfTooLong limit. A tighter cap here would make
                // saves the writer accepted unloadable.
                entity.name.resize(nameLen);
                if (!readBytes(entity.name.data(), nameLen))
                    return false;

                uint16_t compCount;
                if (!readBytes(&compCount, sizeof(compCount)))
                    return false;

                for (uint16_t c = 0; c < compCount; ++c)
                {
                    SerializedComponent comp;

                    uint16_t typeLen;
                    if (!readBytes(&typeLen, sizeof(typeLen)))
                        return false;
                    comp.typeName.resize(typeLen);
                    if (!readBytes(comp.typeName.data(), typeLen))
                        return false;

                    uint16_t propCount;
                    if (!readBytes(&propCount, sizeof(propCount)))
                        return false;

                    for (uint16_t p = 0; p < propCount; ++p)
                    {
                        uint16_t keyLen;
                        if (!readBytes(&keyLen, sizeof(keyLen)))
                            return false;
                        std::string key(keyLen, '\0');
                        if (!readBytes(key.data(), keyLen))
                            return false;

                        uint16_t valLen;
                        if (!readBytes(&valLen, sizeof(valLen)))
                            return false;
                        std::string val(valLen, '\0');
                        if (!readBytes(val.data(), valLen))
                            return false;

                        comp.properties[key] = val;
                    }

                    entity.components.push_back(comp);
                }

                parsedData.entities.push_back(entity);
            }

            // Version 1 always ends with a custom-state count, even when zero.
            uint32_t customStateCount = 0;
            if (!readBytes(&customStateCount, sizeof(customStateCount)))
                return false;

            constexpr uint32_t kMaxCustomState = 100'000;
            if (customStateCount > kMaxCustomState)
                return false;
            for (uint32_t i = 0; i < customStateCount; ++i)
            {
                uint16_t keyLen;
                if (!readBytes(&keyLen, sizeof(keyLen)))
                    return false;
                std::string key(keyLen, '\0');
                if (!readBytes(key.data(), keyLen))
                    return false;

                uint16_t valLen;
                if (!readBytes(&valLen, sizeof(valLen)))
                    return false;
                std::string val(valLen, '\0');
                if (!readBytes(val.data(), valLen))
                    return false;

                parsedData.customState[key] = val;
            }

            if (offset != fileData.size())
                return false;

            if (version < kCurrentSaveVersion)
            {
                SPARK_LOG_INFO(Spark::LogCategory::Save, "ReadFromFile: migrating save '%s' from version %u to %u",
                               filepath.c_str(), version, kCurrentSaveVersion);
            }
            if (!MigrateToCurrentVersion(parsedData))
                return false;

            outData = std::move(parsedData);
            return true;
        }
        catch (const std::exception& e)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "Save system error (ReadFromFile): %s", e.what());
            return false;
        }
        catch (...)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "Save system: unknown exception in ReadFromFile");
            return false;
        }
    }

    bool SaveSystem::ReadMetadataOnly(const std::string& filepath, SaveMetadata& outMetadata) const
    {
        try
        {
            std::ifstream file(filepath, std::ios::binary);
            if (!file.is_open())
                return false;

            // Header: 4-byte magic + uint32 version.
            char magic[4];
            file.read(magic, 4);
            if (!file || std::string(magic, 4) != "SPRK")
                return false;

            uint32_t version = 0;
            file.read(reinterpret_cast<char*>(&version), sizeof(version));
            if (!file)
                return false;
            if (!IsSupportedSaveVersion(version))
            {
                LogUnsupportedSaveVersion(filepath, version, "ReadMetadataOnly");
                return false;
            }

            SaveMetadata parsedMetadata;

            // Metadata is a length-prefixed text block immediately after the header.
            uint32_t metaSize = 0;
            file.read(reinterpret_cast<char*>(&metaSize), sizeof(metaSize));
            if (!file)
                return false;
            // Guard against a corrupt/oversized length before allocating.
            constexpr uint32_t kMaxMetaSize = 64 * 1024;
            if (metaSize > kMaxMetaSize)
                return false;

            std::string metaStr(metaSize, '\0');
            if (metaSize > 0)
            {
                file.read(metaStr.data(), metaSize);
                if (!file)
                    return false;
            }

            if (!ParseMetadataBlock(version, metaStr, parsedMetadata))
                return false;

            SaveData metadataOnly;
            metadataOnly.metadata = std::move(parsedMetadata);
            if (!MigrateToCurrentVersion(metadataOnly))
                return false;

            outMetadata = std::move(metadataOnly.metadata);
            return true;
        }
        catch (const std::exception& e)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "Save system error (ReadMetadataOnly): %s", e.what());
            return false;
        }
        catch (...)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "Save system: unknown exception in ReadMetadataOnly");
            return false;
        }
    }

    bool SaveSystem::IsValidSlotName(const std::string& slotName)
    {
        if (slotName.empty() || slotName.size() > 64)
            return false;
        for (char c : slotName)
        {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-')
                return false;
        }
        return true;
    }

    std::string SaveSystem::GetSavePath(const std::string& slotName) const
    {
        if (!IsValidSlotName(slotName))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "SaveSystem: Invalid slot name '%s' — rejected",
                            slotName.c_str());
            return {};
        }
        return m_saveDirectory + "/" + slotName + ".spark_save";
    }

    std::string SaveSystem::Console_ListSaves() const
    {
        auto slots = GetSaveSlots();
        std::ostringstream ss;
        ss << "=== Save Slots (" << slots.size() << ") ===\n";
        for (const auto& slot : slots)
        {
            ss << "  " << slot.saveName << " [" << slot.sceneName << ", HP:" << slot.playerHealth
               << ", Time:" << slot.playTime << "s]\n";
        }
        return ss.str();
    }

    std::string SaveSystem::Console_GetSaveInfo(const std::string& slotName) const
    {
        SaveMetadata meta;
        if (!GetSaveMetadata(slotName, meta))
            return "Save '" + slotName + "' not found\n";

        std::ostringstream ss;
        ss << "=== Save: " << meta.saveName << " ===\n";
        ss << "Scene: " << meta.sceneName << "\n";
        ss << "Class: " << meta.playerClass << "\n";
        ss << "Play Time: " << meta.playTime << "s\n";
        ss << "Health: " << meta.playerHealth << "\n";
        ss << "Armor: " << meta.playerArmor << "\n";
        ss << "K/D: " << meta.playerKills << "/" << meta.playerDeaths << "\n";
        ss << "Position: (" << meta.playerPosition.x << ", " << meta.playerPosition.y << ", " << meta.playerPosition.z
           << ")\n";
        return ss.str();
    }

} // namespace Spark
