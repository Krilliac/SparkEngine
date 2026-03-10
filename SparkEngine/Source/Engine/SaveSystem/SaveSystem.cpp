/**
 * @file SaveSystem.cpp
 * @brief Save/load system implementation with JSON serialization
 */

#include "SaveSystem.h"
#include "Utils/LocalFileCache.h"
#include <cstring>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

namespace Spark
{

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

    void ComponentSerializerRegistry::RegisterBuiltins()
    {
        // Transform
        Register(
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
                auto get = [&](const std::string& key, float def) -> float
                {
                    auto it = data.properties.find(key);
                    return (it != data.properties.end()) ? std::stof(it->second) : def;
                };
                t.position = {get("px", 0), get("py", 0), get("pz", 0)};
                t.rotation = {get("rx", 0), get("ry", 0), get("rz", 0)};
                t.scale = {get("sx", 1), get("sy", 1), get("sz", 1)};
            });

        // MeshRenderer
        Register(
            "MeshRenderer",
            [](const void* comp) -> SerializedComponent
            {
                const auto* m = static_cast<const MeshRenderer*>(comp);
                SerializedComponent sc;
                sc.typeName = "MeshRenderer";
                sc.properties["meshPath"] = m->meshPath;
                sc.properties["materialPath"] = m->materialPath;
                sc.properties["castShadows"] = m->castShadows ? "1" : "0";
                sc.properties["receiveShadows"] = m->receiveShadows ? "1" : "0";
                sc.properties["visible"] = m->visible ? "1" : "0";
                return sc;
            },
            [](World& world, EntityID entity, const SerializedComponent& data)
            {
                auto& m = world.AddComponent<MeshRenderer>(entity);
                auto get = [&](const std::string& key) -> std::string
                {
                    auto it = data.properties.find(key);
                    return (it != data.properties.end()) ? it->second : "";
                };
                m.meshPath = get("meshPath");
                m.materialPath = get("materialPath");
                m.castShadows = get("castShadows") != "0";
                m.receiveShadows = get("receiveShadows") != "0";
                m.visible = get("visible") != "0";
            });

        // HealthComponent
        Register(
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
                auto get = [&](const std::string& key, float def) -> float
                {
                    auto it = data.properties.find(key);
                    return (it != data.properties.end()) ? std::stof(it->second) : def;
                };
                h.health = get("health", 100.0f);
                h.maxHealth = get("maxHealth", 100.0f);
                h.isDead = (data.properties.count("isDead") && data.properties.at("isDead") == "1");
            });

        // LightComponent
        Register(
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
                auto get = [&](const std::string& key, float def) -> float
                {
                    auto it = data.properties.find(key);
                    return (it != data.properties.end()) ? std::stof(it->second) : def;
                };
                l.type = static_cast<LightComponent::Type>(static_cast<int>(get("type", 1)));
                l.color = {get("cr", 1), get("cg", 1), get("cb", 1)};
                l.intensity = get("intensity", 1.0f);
                l.range = get("range", 10.0f);
                l.castShadows = (data.properties.count("castShadows") && data.properties.at("castShadows") == "1");
            });

        // AudioSourceComponent
        Register(
            "AudioSourceComponent",
            [](const void* comp) -> SerializedComponent
            {
                const auto* a = static_cast<const AudioSourceComponent*>(comp);
                SerializedComponent sc;
                sc.typeName = "AudioSourceComponent";
                sc.properties["soundName"] = a->soundName;
                sc.properties["volume"] = std::to_string(a->volume);
                sc.properties["is3D"] = a->is3D ? "1" : "0";
                sc.properties["loop"] = a->loop ? "1" : "0";
                sc.properties["playOnAwake"] = a->playOnAwake ? "1" : "0";
                return sc;
            },
            [](World& world, EntityID entity, const SerializedComponent& data)
            {
                auto& a = world.AddComponent<AudioSourceComponent>(entity);
                auto get = [&](const std::string& key) -> std::string
                {
                    auto it = data.properties.find(key);
                    return (it != data.properties.end()) ? it->second : "";
                };
                a.soundName = get("soundName");
                a.volume = data.properties.count("volume") ? std::stof(data.properties.at("volume")) : 1.0f;
                a.is3D = get("is3D") != "0";
                a.loop = get("loop") == "1";
                a.playOnAwake = get("playOnAwake") == "1";
            });

        // Camera
        Register(
            "Camera",
            [](const void* comp) -> SerializedComponent
            {
                const auto* c = static_cast<const Camera*>(comp);
                SerializedComponent sc;
                sc.typeName = "Camera";
                sc.properties["fov"] = std::to_string(c->fov);
                sc.properties["nearPlane"] = std::to_string(c->nearPlane);
                sc.properties["farPlane"] = std::to_string(c->farPlane);
                sc.properties["isMainCamera"] = c->isMainCamera ? "1" : "0";
                return sc;
            },
            [](World& world, EntityID entity, const SerializedComponent& data)
            {
                auto& c = world.AddComponent<Camera>(entity);
                auto get = [&](const std::string& key, float def) -> float
                {
                    auto it = data.properties.find(key);
                    return (it != data.properties.end()) ? std::stof(it->second) : def;
                };
                c.fov = get("fov", 60.0f);
                c.nearPlane = get("nearPlane", 0.1f);
                c.farPlane = get("farPlane", 1000.0f);
                c.isMainCamera = (data.properties.count("isMainCamera") && data.properties.at("isMainCamera") == "1");
            });

        // Script
        Register(
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
                auto get = [&](const std::string& key) -> std::string
                {
                    auto it = data.properties.find(key);
                    return (it != data.properties.end()) ? it->second : "";
                };
                s.scriptPath = get("scriptPath");
                s.className = get("className");
                s.moduleName = get("moduleName");
                s.enabled = get("enabled") != "0";
                s.started = get("started") == "1";
            });

        // RigidBodyComponent
        Register(
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
                // physicsBodyHandle is runtime-only, skip
                return sc;
            },
            [](World& world, EntityID entity, const SerializedComponent& data)
            {
                auto& rb = world.AddComponent<RigidBodyComponent>(entity);
                auto get = [&](const std::string& key, float def) -> float
                {
                    auto it = data.properties.find(key);
                    return (it != data.properties.end()) ? std::stof(it->second) : def;
                };
                rb.type = static_cast<RigidBodyComponent::Type>(static_cast<int>(get("type", 2)));
                rb.mass = get("mass", 1.0f);
                rb.friction = get("friction", 0.5f);
                rb.restitution = get("restitution", 0.1f);
                rb.linearDamping = get("linearDamping", 0.1f);
                rb.angularDamping = get("angularDamping", 0.1f);
                rb.isTrigger = (data.properties.count("isTrigger") && data.properties.at("isTrigger") == "1");
                rb.linearVelocity = {get("lvx", 0), get("lvy", 0), get("lvz", 0)};
                rb.angularVelocity = {get("avx", 0), get("avy", 0), get("avz", 0)};
                rb.physicsBodyHandle = nullptr;
            });

        // ColliderComponent
        Register(
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
                auto get = [&](const std::string& key, float def) -> float
                {
                    auto it = data.properties.find(key);
                    return (it != data.properties.end()) ? std::stof(it->second) : def;
                };
                col.shape = static_cast<ColliderComponent::Shape>(static_cast<int>(get("shape", 0)));
                col.halfExtents = {get("hex", 0.5f), get("hey", 0.5f), get("hez", 0.5f)};
                col.radius = get("radius", 0.5f);
                col.height = get("height", 1.0f);
                col.offset = {get("ox", 0), get("oy", 0), get("oz", 0)};
            });

        // ParticleEmitterComponent
        Register(
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
                // emitterHandle is runtime-only, skip
                return sc;
            },
            [](World& world, EntityID entity, const SerializedComponent& data)
            {
                auto& pe = world.AddComponent<ParticleEmitterComponent>(entity);
                auto getf = [&](const std::string& key, float def) -> float
                {
                    auto it = data.properties.find(key);
                    return (it != data.properties.end()) ? std::stof(it->second) : def;
                };
                auto gets = [&](const std::string& key) -> std::string
                {
                    auto it = data.properties.find(key);
                    return (it != data.properties.end()) ? it->second : "";
                };
                pe.effectName = gets("effectName");
                pe.autoPlay = gets("autoPlay") != "0";
                pe.isPlaying = gets("isPlaying") == "1";
                pe.emissionRate = getf("emissionRate", 10.0f);
                pe.lifetime = getf("lifetime", 1.0f);
                pe.startColor = {getf("scr", 1), getf("scg", 1), getf("scb", 1), getf("sca", 1)};
                pe.startSize = getf("startSize", 0.1f);
                pe.startSpeed = getf("startSpeed", 1.0f);
                pe.emitterHandle = nullptr;
            });

        // AnimationController
        Register(
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
                // Serialize availableAnimations as comma-separated string
                std::string animList;
                for (size_t i = 0; i < ac->availableAnimations.size(); ++i)
                {
                    if (i > 0)
                        animList += ",";
                    animList += ac->availableAnimations[i];
                }
                sc.properties["availableAnimations"] = animList;
                // animInstanceHandle is runtime-only, skip
                return sc;
            },
            [](World& world, EntityID entity, const SerializedComponent& data)
            {
                auto& ac = world.AddComponent<AnimationController>(entity);
                auto getf = [&](const std::string& key, float def) -> float
                {
                    auto it = data.properties.find(key);
                    return (it != data.properties.end()) ? std::stof(it->second) : def;
                };
                auto gets = [&](const std::string& key) -> std::string
                {
                    auto it = data.properties.find(key);
                    return (it != data.properties.end()) ? it->second : "";
                };
                ac.currentAnimation = gets("currentAnimation");
                ac.defaultAnimation = gets("defaultAnimation");
                ac.playbackSpeed = getf("playbackSpeed", 1.0f);
                ac.currentTime = getf("currentTime", 0.0f);
                ac.playing = gets("playing") != "0";
                ac.loop = gets("loop") != "0";
                // Deserialize availableAnimations from comma-separated string
                std::string animList = gets("availableAnimations");
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

        // NetworkIdentity
        Register(
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
                auto getu = [&](const std::string& key, uint32_t def) -> uint32_t
                {
                    auto it = data.properties.find(key);
                    return (it != data.properties.end()) ? static_cast<uint32_t>(std::stoul(it->second)) : def;
                };
                auto gets = [&](const std::string& key) -> std::string
                {
                    auto it = data.properties.find(key);
                    return (it != data.properties.end()) ? it->second : "";
                };
                ni.networkID = getu("networkID", 0);
                ni.ownerClientID = getu("ownerClientID", 0);
                ni.isLocalAuthority = gets("isLocalAuthority") == "1";
                ni.replicateTransform = gets("replicateTransform") != "0";
                ni.replicateHealth = gets("replicateHealth") != "0";
            });

        // TagComponent
        Register(
            "TagComponent",
            [](const void* comp) -> SerializedComponent
            {
                const auto* tc = static_cast<const TagComponent*>(comp);
                SerializedComponent sc;
                sc.typeName = "TagComponent";
                // Serialize tags as comma-separated string
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
                auto it = data.properties.find("tags");
                if (it != data.properties.end() && !it->second.empty())
                {
                    std::istringstream iss(it->second);
                    std::string tag;
                    while (std::getline(iss, tag, ','))
                    {
                        if (!tag.empty())
                            tc.tags.insert(tag);
                    }
                }
            });

        // ActiveComponent
        Register(
            "ActiveComponent",
            [](const void* comp) -> SerializedComponent
            {
                const auto* ac = static_cast<const ActiveComponent*>(comp);
                SerializedComponent sc;
                sc.typeName = "ActiveComponent";
                sc.properties["active"] = ac->active ? "1" : "0";
                return sc;
            },
            [](World& world, EntityID entity, const SerializedComponent& data)
            {
                auto& ac = world.AddComponent<ActiveComponent>(entity);
                auto it = data.properties.find("active");
                ac.active = (it == data.properties.end()) || (it->second != "0");
            });
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
        m_saveDirectory = saveDirectory;
        try
        {
            fs::create_directories(m_saveDirectory);
            ComponentSerializerRegistry::GetInstance().RegisterBuiltins();
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool SaveSystem::Save(const std::string& slotName, World& world, const SaveMetadata& metadata)
    {
        SaveData data = SerializeWorld(world, metadata);
        return WriteToFile(GetSavePath(slotName), data);
    }

    bool SaveSystem::Load(const std::string& slotName, World& world)
    {
        SaveData data;
        if (!ReadFromFile(GetSavePath(slotName), data))
            return false;
        return DeserializeWorld(data, world);
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
        try
        {
            std::string path = GetSavePath(slotName);
            if (!fs::exists(path))
                return true;
            return fs::remove(path);
        }
        catch (...)
        {
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
                    SaveData data;
                    if (ReadFromFile(entry.path().string(), data))
                    {
                        slots.push_back(data.metadata);
                    }
                }
            }
        }
        catch (...)
        {
        }

        // Sort by timestamp (newest first)
        std::sort(slots.begin(), slots.end(),
                  [](const SaveMetadata& a, const SaveMetadata& b) { return a.timestamp > b.timestamp; });
        return slots;
    }

    bool SaveSystem::GetSaveMetadata(const std::string& slotName, SaveMetadata& outMetadata) const
    {
        SaveData data;
        if (!ReadFromFile(GetSavePath(slotName), data))
            return false;
        outMetadata = data.metadata;
        return true;
    }

    bool SaveSystem::SaveExists(const std::string& slotName) const
    {
        return fs::exists(GetSavePath(slotName));
    }

    SaveData SaveSystem::SerializeWorld(World& world, const SaveMetadata& metadata) const
    {
        SaveData data;
        data.metadata = metadata;
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

            // Serialize Transform
            if (auto* t = world.GetComponent<Transform>(entity))
            {
                if (serializerRegistry.HasSerializer("Transform"))
                    se.components.push_back(serializerRegistry.Serialize("Transform", t));
            }

            // Serialize MeshRenderer
            if (auto* m = world.GetComponent<MeshRenderer>(entity))
            {
                if (serializerRegistry.HasSerializer("MeshRenderer"))
                    se.components.push_back(serializerRegistry.Serialize("MeshRenderer", m));
            }

            // Serialize HealthComponent
            if (auto* h = world.GetComponent<HealthComponent>(entity))
            {
                if (serializerRegistry.HasSerializer("HealthComponent"))
                    se.components.push_back(serializerRegistry.Serialize("HealthComponent", h));
            }

            // Serialize LightComponent
            if (auto* l = world.GetComponent<LightComponent>(entity))
            {
                if (serializerRegistry.HasSerializer("LightComponent"))
                    se.components.push_back(serializerRegistry.Serialize("LightComponent", l));
            }

            // Serialize AudioSourceComponent
            if (auto* a = world.GetComponent<AudioSourceComponent>(entity))
            {
                if (serializerRegistry.HasSerializer("AudioSourceComponent"))
                    se.components.push_back(serializerRegistry.Serialize("AudioSourceComponent", a));
            }

            // Serialize Camera
            if (auto* c = world.GetComponent<Camera>(entity))
            {
                if (serializerRegistry.HasSerializer("Camera"))
                    se.components.push_back(serializerRegistry.Serialize("Camera", c));
            }

            // Serialize Script
            if (auto* s = world.GetComponent<Script>(entity))
            {
                if (serializerRegistry.HasSerializer("Script"))
                    se.components.push_back(serializerRegistry.Serialize("Script", s));
            }

            // Serialize RigidBodyComponent
            if (auto* rb = world.GetComponent<RigidBodyComponent>(entity))
            {
                if (serializerRegistry.HasSerializer("RigidBodyComponent"))
                    se.components.push_back(serializerRegistry.Serialize("RigidBodyComponent", rb));
            }

            // Serialize ColliderComponent
            if (auto* col = world.GetComponent<ColliderComponent>(entity))
            {
                if (serializerRegistry.HasSerializer("ColliderComponent"))
                    se.components.push_back(serializerRegistry.Serialize("ColliderComponent", col));
            }

            // Serialize ParticleEmitterComponent
            if (auto* pe = world.GetComponent<ParticleEmitterComponent>(entity))
            {
                if (serializerRegistry.HasSerializer("ParticleEmitterComponent"))
                    se.components.push_back(serializerRegistry.Serialize("ParticleEmitterComponent", pe));
            }

            // Serialize AnimationController
            if (auto* ac = world.GetComponent<AnimationController>(entity))
            {
                if (serializerRegistry.HasSerializer("AnimationController"))
                    se.components.push_back(serializerRegistry.Serialize("AnimationController", ac));
            }

            // Serialize NetworkIdentity
            if (auto* ni = world.GetComponent<NetworkIdentity>(entity))
            {
                if (serializerRegistry.HasSerializer("NetworkIdentity"))
                    se.components.push_back(serializerRegistry.Serialize("NetworkIdentity", ni));
            }

            // Serialize TagComponent
            if (auto* tc = world.GetComponent<TagComponent>(entity))
            {
                if (serializerRegistry.HasSerializer("TagComponent"))
                    se.components.push_back(serializerRegistry.Serialize("TagComponent", tc));
            }

            // Serialize ActiveComponent
            if (auto* ac = world.GetComponent<ActiveComponent>(entity))
            {
                if (serializerRegistry.HasSerializer("ActiveComponent"))
                    se.components.push_back(serializerRegistry.Serialize("ActiveComponent", ac));
            }

            if (!se.components.empty())
                data.entities.push_back(se);
        }

        return data;
    }

    bool SaveSystem::DeserializeWorld(const SaveData& data, World& world) const
    {
        // Clear all existing entities before restoring saved state
        auto& registry = world.GetRegistry();
        {
            auto&& entityStorage = registry.storage<entt::entity>();
            std::vector<entt::entity> toDestroy;
            for (auto&& [entity] : entityStorage.each())
                toDestroy.push_back(entity);
            for (auto e : toDestroy)
                registry.destroy(e);
        }

        const auto& serializerRegistry = ComponentSerializerRegistry::GetInstance();

        for (const auto& se : data.entities)
        {
            EntityID entity = world.CreateEntity(se.name);

            for (const auto& comp : se.components)
            {
                if (serializerRegistry.HasSerializer(comp.typeName))
                {
                    serializerRegistry.Deserialize(comp.typeName, world, entity, comp);
                }
            }
        }

        return true;
    }

    bool SaveSystem::WriteToFile(const std::string& filepath, const SaveData& data) const
    {
        try
        {
            std::ofstream file(filepath, std::ios::binary);
            if (!file.is_open())
                return false;

            // Write header
            const char magic[] = "SPRK";
            file.write(magic, 4);
            uint32_t version = data.metadata.version;
            file.write(reinterpret_cast<const char*>(&version), sizeof(version));

            // Write metadata as text block
            std::ostringstream metaStream;
            metaStream << data.metadata.saveName << "\n";
            metaStream << data.metadata.sceneName << "\n";
            metaStream << data.metadata.playerClass << "\n";
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

            // Invalidate any cached copy so future reads see the new data
            if (m_fileCache)
            {
                m_fileCache->Invalidate(filepath);
            }

            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool SaveSystem::ReadFromFile(const std::string& filepath, SaveData& outData) const
    {
        try
        {
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
                    return false;

                file.seekg(0, std::ios::end);
                auto size = file.tellg();
                file.seekg(0, std::ios::beg);
                fileData.resize(static_cast<size_t>(size));
                file.read(reinterpret_cast<char*>(fileData.data()), size);
            }

            if (fileData.size() < 8)
                return false;

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
                return false;
            if (std::string(magic, 4) != "SPRK")
                return false;

            uint32_t version;
            if (!readBytes(&version, sizeof(version)))
                return false;
            outData.metadata.version = version;

            // Read metadata
            uint32_t metaSize;
            if (!readBytes(&metaSize, sizeof(metaSize)))
                return false;
            if (offset + metaSize > fileData.size())
                return false;
            std::string metaStr(reinterpret_cast<const char*>(fileData.data() + offset), metaSize);
            offset += metaSize;

            std::istringstream metaStream(metaStr);
            std::getline(metaStream, outData.metadata.saveName);
            std::getline(metaStream, outData.metadata.sceneName);
            std::getline(metaStream, outData.metadata.playerClass);
            metaStream >> outData.metadata.timestamp;
            metaStream >> outData.metadata.playTime;
            metaStream >> outData.metadata.playerHealth;
            metaStream >> outData.metadata.playerArmor;
            metaStream >> outData.metadata.playerPosition.x >> outData.metadata.playerPosition.y >>
                outData.metadata.playerPosition.z;
            metaStream >> outData.metadata.playerKills;
            metaStream >> outData.metadata.playerDeaths;

            // Read entities
            uint32_t entityCount;
            if (!readBytes(&entityCount, sizeof(entityCount)))
                return false;

            for (uint32_t i = 0; i < entityCount; ++i)
            {
                SerializedEntity entity;

                uint16_t nameLen;
                if (!readBytes(&nameLen, sizeof(nameLen)))
                    return false;
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

                outData.entities.push_back(entity);
            }

            // Read custom state key-value pairs (if present in file)
            uint32_t customStateCount = 0;
            if (readBytes(&customStateCount, sizeof(customStateCount)))
            {
                for (uint32_t i = 0; i < customStateCount; ++i)
                {
                    uint16_t keyLen;
                    if (!readBytes(&keyLen, sizeof(keyLen)))
                        break;
                    std::string key(keyLen, '\0');
                    if (!readBytes(key.data(), keyLen))
                        break;

                    uint16_t valLen;
                    if (!readBytes(&valLen, sizeof(valLen)))
                        break;
                    std::string val(valLen, '\0');
                    if (!readBytes(val.data(), valLen))
                        break;

                    outData.customState[key] = val;
                }
            }

            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    std::string SaveSystem::GetSavePath(const std::string& slotName) const
    {
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
