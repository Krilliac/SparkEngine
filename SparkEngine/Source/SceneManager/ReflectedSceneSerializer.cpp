#include "SceneManager/ReflectedSceneSerializer.h"
#include "SceneManager/ReflectedSceneValidation.h"
#include "Engine/ECS/Components.h"
#include "Core/Reflection.h"
#include "Utils/LogMacros.h"

#include <nlohmann_json.h>
#include <format>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

using nlohmann::json;

namespace Spark
{

    using namespace ReflectedSceneDetail;

    namespace
    {
        // Emit one component's fields via reflection.
        json SerializeComponentFields(const std::string& typeName, const void* comp)
        {
            json fields = json::object();
            const TypeInfo* ti = TypeRegistry::Get().FindTypeByName(typeName);
            if (!ti)
                return fields;
            for (const FieldInfo& f : ti->fields)
            {
                if (!f.serialized)
                    continue;
                switch (f.type)
                {
                case FieldType::Bool:
                case FieldType::Int:
                case FieldType::Float:
                case FieldType::Double:
                case FieldType::String:
                case FieldType::Vector2:
                case FieldType::Vector3:
                case FieldType::Vector4:
                case FieldType::Enum:
                    fields[f.fieldName] = GetFieldAsString(comp, f);
                    break;
                default:
                    SPARK_LOG_WARN(Spark::LogCategory::Core, "[ReflectedScene] skip unsupported field %s.%s (type %d)",
                                   typeName.c_str(), f.fieldName.c_str(), (int)f.type);
                    break;
                }
            }
            return fields;
        }

        std::string JsonFieldValueToString(const json& value)
        {
            if (value.is_string())
                return value.get<std::string>();
            if (value.is_array())
            {
                std::string result;
                for (const json& item : value)
                {
                    if (!result.empty())
                        result += ',';
                    result += item.is_string() ? item.get<std::string>() : item.dump();
                }
                return result;
            }
            return value.dump();
        }

        const json* FindLegacyField(const json& fields, const std::string& type, const std::string& fieldName)
        {
            if (fields.contains(fieldName))
                return &fields[fieldName];
            if (type == "MeshRenderer" && fieldName == "meshPath" && fields.contains("mesh"))
                return &fields["mesh"];
            if (type == "Camera" && fieldName == "nearPlane" && fields.contains("nearClip"))
                return &fields["nearClip"];
            if (type == "Camera" && fieldName == "farPlane" && fields.contains("farClip"))
                return &fields["farClip"];
            return nullptr;
        }
    } // namespace

    std::string SerializeWorld(const World& world)
    {
        json root;
        root["version"] = kCurrentSceneVersion;
        json entities = json::array();

        auto& factory = ComponentFactory::Get();
        const std::vector<std::string> names = factory.GetRegisteredNames();
        const entt::registry& reg = world.GetRegistry();
        // Non-const World handle for the factory (its ops take void* world, uint32 entity).
        World& mutWorld = const_cast<World&>(world);

        auto entityStorage = reg.storage<entt::entity>();
        for (auto&& [entity] : entityStorage->each())
        {
            json ent;
            ent["id"] = static_cast<int32_t>(static_cast<uint32_t>(entity));
            if (const NameComponent* nc = world.GetComponent<NameComponent>(entity))
                ent["name"] = nc->name;
            else
                ent["name"] = "";
            int parentId = -1;
            if (const Transform* t = world.GetComponent<Transform>(entity))
                if (t->parent != entt::null)
                    parentId = static_cast<int>(static_cast<uint32_t>(t->parent));
            ent["parent"] = parentId;

            json comps = json::array();
            for (const std::string& type : names)
            {
                if (IsEntityLevel(type))
                    continue; // name handled above
                if (!factory.HasComponent(type, &mutWorld, static_cast<uint32_t>(entity)))
                    continue;
                void* comp = factory.GetComponentRaw(type, &mutWorld, static_cast<uint32_t>(entity));
                if (!comp)
                    continue;
                json c;
                c["type"] = type;
                c["fields"] = SerializeComponentFields(type, comp);
                comps.push_back(std::move(c));
            }
            ent["components"] = std::move(comps);
            entities.push_back(std::move(ent));
        }

        root["entities"] = std::move(entities);
        return root.dump(2);
    }

    bool DeserializeInto(World& world, const std::string& jsonText, SceneDeserializeMode mode, std::string* error)
    {
        if (error)
            error->clear();
        try
        {
            json root;
            root = json::parse(jsonText);
            if (!root.is_object())
                return Reject(error, std::format("scene root must be a JSON object, found {}", JsonTypeName(root)));

            // SparkEditor versions before the reflected serializer shipped
            // `sceneVersion` plus inline component values. Keep those projects
            // loadable and migrate naturally on their next explicit save.
            bool legacyScene = false;
            if (!CheckSceneVersion(root, legacyScene, error))
                return false;
            if (!root.contains("entities") || !root["entities"].is_array())
                return Reject(error, "scene has no 'entities' array");

            auto& factory = ComponentFactory::Get();
            const bool strictRecovery = mode == SceneDeserializeMode::StrictRecovery;
            if (strictRecovery && !ValidateStrictRecoveryDocument(root, factory, error))
                return false;
            std::unordered_map<uint32_t, entt::entity> idMap; // serialized id -> live entity
            const auto& entities = root["entities"];

            // Resolve every serialized ID before mutating the destination World.
            // Older scenes may omit IDs for some entities, including scenes that
            // mix explicit and implicit IDs.  The old single counter could hand
            // an implicit entity an ID that a later explicit entity also used;
            // idMap would then silently overwrite the first mapping and parent
            // links could target the wrong entity.
            std::vector<uint32_t> serializedIds(entities.size());
            std::unordered_set<uint32_t> reservedIds;
            for (size_t index = 0; index < entities.size(); ++index)
            {
                const json& ent = entities[index];
                if (!ent.is_object())
                {
                    return Reject(error, std::format("{} must be a JSON object, found {}", DescribeEntity(index, ent),
                                                     JsonTypeName(ent)));
                }
                if (ent.contains("parent") && !ent["parent"].is_number_integer())
                {
                    return Reject(error, std::format("{} has parent {}; parent must be an integer entity id or -1",
                                                     DescribeEntity(index, ent), ent["parent"].dump()));
                }
                if (!ent.contains("id"))
                    continue;
                const json& idValue = ent["id"];
                const bool integerId = idValue.is_number_integer() || idValue.is_number_unsigned();
                const int64_t rawId = integerId && !(idValue.is_number_unsigned() &&
                                                     idValue.get<uint64_t>() > std::numeric_limits<uint32_t>::max())
                                          ? idValue.get<int64_t>()
                                          : -1;
                const uint32_t id = static_cast<uint32_t>(rawId);
                if (rawId < 0 || static_cast<uint64_t>(rawId) > std::numeric_limits<uint32_t>::max() ||
                    static_cast<entt::entity>(id) == entt::null)
                {
                    return Reject(error,
                                  std::format("{} has id {}; ids must be integers in 0..{}", DescribeEntity(index, ent),
                                              idValue.dump(), std::numeric_limits<uint32_t>::max() - 1));
                }
                if (!reservedIds.insert(id).second)
                    return Reject(error, std::format("{} repeats id {}", DescribeEntity(index, ent), id));
                serializedIds[index] = id;
            }

            uint32_t fallbackSerializedId = 0;
            for (size_t index = 0; index < entities.size(); ++index)
            {
                if (entities[index].contains("id"))
                    continue;
                while (reservedIds.contains(fallbackSerializedId) ||
                       static_cast<entt::entity>(fallbackSerializedId) == entt::null)
                {
                    if (fallbackSerializedId == std::numeric_limits<uint32_t>::max())
                        return Reject(error, "scene has more entities than the entity id space can hold");
                    ++fallbackSerializedId;
                }
                serializedIds[index] = fallbackSerializedId;
                reservedIds.insert(fallbackSerializedId);
                if (fallbackSerializedId != std::numeric_limits<uint32_t>::max())
                    ++fallbackSerializedId;
            }

            struct PendingParent
            {
                entt::entity child;
                uint32_t parentId;
            };
            std::vector<PendingParent> pending;

            size_t entityIndex = 0;
            for (const json& ent : entities)
            {
                const std::string name = ent.value("name", std::string());
                const uint32_t sid = serializedIds[entityIndex++];
                auto& registry = world.GetRegistry();
                const entt::entity hint = static_cast<entt::entity>(sid);
                if (registry.valid(hint))
                {
                    return Reject(error, std::format("{} id {} collides with an entity already in the target world",
                                                     DescribeEntity(entityIndex - 1, ent), sid));
                }
                const entt::entity e = registry.create(hint);
                if (!name.empty())
                    registry.emplace<NameComponent>(e, NameComponent{name});
                idMap.emplace(sid, e);

                if (ent.contains("components") && ent["components"].is_array())
                {
                    for (const json& c : ent["components"])
                    {
                        const std::string sourceType = c.value("type", std::string());
                        std::string type = sourceType;
                        if (legacyScene && sourceType == "DirectionalLight")
                            type = "LightComponent";
                        else if (legacyScene && sourceType == "CharacterController")
                            type = "CharacterControllerComponent";
                        if (type.empty() || !factory.IsRegistered(type))
                        {
                            SPARK_LOG_WARN(Spark::LogCategory::Core,
                                           "[ReflectedScene] unknown component type '%s' skipped", type.c_str());
                            if (strictRecovery)
                            {
                                return Reject(error, std::format("{} has unregistered component type '{}'",
                                                                 DescribeEntity(entityIndex - 1, ent), type));
                            }
                            continue;
                        }
                        if (!factory.HasComponent(type, &world, (uint32_t)e))
                            factory.AddComponent(type, &world, (uint32_t)e);
                        void* comp = factory.GetComponentRaw(type, &world, (uint32_t)e);
                        if (!comp)
                        {
                            if (strictRecovery)
                            {
                                return Reject(error, std::format("{} component '{}' could not be created",
                                                                 DescribeEntity(entityIndex - 1, ent), type));
                            }
                            continue;
                        }
                        const TypeInfo* ti = TypeRegistry::Get().FindTypeByName(type);
                        if (!ti)
                        {
                            if (strictRecovery)
                            {
                                return Reject(error, std::format("{} component '{}' has no reflection data",
                                                                 DescribeEntity(entityIndex - 1, ent), type));
                            }
                            continue;
                        }
                        const json& fields = c.contains("fields") ? c["fields"] : c;
                        for (const FieldInfo& f : ti->fields)
                        {
                            // In the legacy inline schema, c["type"] is the
                            // component discriminator, not a reflected field.
                            if (legacyScene && !c.contains("fields") && f.fieldName == "type")
                                continue;
                            const json* fieldValue =
                                legacyScene ? FindLegacyField(fields, type, f.fieldName)
                                            : (fields.contains(f.fieldName) ? &fields[f.fieldName] : nullptr);
                            if (!fieldValue)
                            {
                                if (strictRecovery && IsRoundTrippableField(f))
                                {
                                    return Reject(error,
                                                  std::format("{} is missing field '{}.{}'",
                                                              DescribeEntity(entityIndex - 1, ent), type, f.fieldName));
                                }
                                continue;
                            }
                            if (!legacyScene && !fieldValue->is_string())
                            {
                                if (strictRecovery)
                                {
                                    return Reject(error, std::format("{} field '{}.{}' must be a string, found {}",
                                                                     DescribeEntity(entityIndex - 1, ent), type,
                                                                     f.fieldName, JsonTypeName(*fieldValue)));
                                }
                                continue;
                            }
                            if (!SetFieldFromString(comp, f, JsonFieldValueToString(*fieldValue)) && strictRecovery)
                            {
                                return Reject(error, std::format("{} field '{}.{}' value {} could not be applied",
                                                                 DescribeEntity(entityIndex - 1, ent), type,
                                                                 f.fieldName, fieldValue->dump()));
                            }
                        }

                        if (legacyScene && sourceType == "DirectionalLight")
                            static_cast<LightComponent*>(comp)->type = LightComponent::Type::Directional;
                        if (legacyScene && type == "Camera" && name == "Main Camera")
                            static_cast<Camera*>(comp)->isMainCamera = true;
                    }
                }
                const int parentId = ent.value("parent", -1);
                if (parentId >= 0)
                    pending.push_back({e, (uint32_t)parentId});
            }

            // Second pass: resolve parents now that all ids exist.
            for (const PendingParent& p : pending)
            {
                auto it = idMap.find(p.parentId);
                if (it == idMap.end())
                {
                    if (strictRecovery)
                        return Reject(error, std::format("parent id {} is not in the scene", p.parentId));
                    continue;
                }
                if (!world.SetParent(p.child, it->second) && strictRecovery)
                    return Reject(error, std::format("parent id {} would create an invalid hierarchy", p.parentId));
            }
            return true;
        }
        catch (const std::exception& ex)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "[ReflectedScene] deserialization error: %s", ex.what());
            return Reject(error, std::format("scene could not be read: {}", ex.what()));
        }
    }

} // namespace Spark
