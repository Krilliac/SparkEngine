#include "SceneManager/ReflectedSceneSerializer.h"
#include "SceneManager/ReflectedSceneValidation.h"
#include "Engine/ECS/Components.h"
#include "Core/Reflection.h"
#include "Utils/LogMacros.h"

#include <nlohmann_json.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using nlohmann::json;

namespace Spark
{

    using namespace ReflectedSceneDetail;

    namespace
    {
        // Serialized ids and parents are the full entity value, version bits
        // included, so they span the whole unsigned width of entt::entity. A
        // recycled slot whose version reaches 2048 already has an id >= 2^31.
        static_assert(std::is_same_v<std::underlying_type_t<entt::entity>, uint32_t>,
                      "the reflected scene format stores entity ids as unsigned 32-bit integers");

        json EntityIdToJson(entt::entity entity)
        {
            // Return the integer itself: a braced json{...} would build a one-element array.
            return static_cast<uint64_t>(static_cast<uint32_t>(entity));
        }

        // Accept an integer in 0..UINT32_MAX that does not name entt::null.
        bool ReadSerializedEntityId(const json& value, uint32_t& id)
        {
            uint64_t raw = 0;
            if (value.is_number_unsigned())
                raw = value.get<uint64_t>();
            else if (value.is_number_integer() && value.get<int64_t>() >= 0)
                raw = static_cast<uint64_t>(value.get<int64_t>());
            else
                return false;
            if (raw > std::numeric_limits<uint32_t>::max())
            {
                return false;
            }
            id = static_cast<uint32_t>(raw);
            return static_cast<entt::entity>(id) != entt::null;
        }

        /// True when @p f's stored value is one SetFieldFromString will accept back.
        /// GetFieldAsString prints NaN and infinity ("nan", "inf"), but the reader
        /// accepts only finite Float, Double and Vector components.
        bool FieldValueIsReadable(const void* comp, const FieldInfo& f)
        {
            const auto* src = static_cast<const char*>(comp) + f.offset;
            const auto finiteFloats = [src](size_t count)
            {
                for (size_t i = 0; i < count; ++i)
                {
                    float value = 0.0f;
                    std::memcpy(&value, src + i * sizeof(float), sizeof(float));
                    if (!std::isfinite(value))
                    {
                        return false;
                    }
                }
                return true;
            };
            switch (f.type)
            {
            case FieldType::Float:
                return finiteFloats(1);
            case FieldType::Vector2:
                return finiteFloats(2);
            case FieldType::Vector3:
                return finiteFloats(3);
            case FieldType::Vector4:
                return finiteFloats(4);
            case FieldType::Double:
            {
                double value = 0.0;
                std::memcpy(&value, src, sizeof(double));
                return std::isfinite(value);
            }
            default:
                return true;
            }
        }

        /// The first field the writer emitted that the reader would refuse.
        struct UnreadableField
        {
            uint32_t entity = 0;
            std::string entityName;
            std::string type;
            std::string field;
            std::string value;
        };

        /// Where SerializeComponentFields reports an unreadable field, if anywhere.
        struct FieldReportContext
        {
            std::optional<UnreadableField>* unreadable = nullptr;
            uint32_t entity = 0;
            const std::string* entityName = nullptr;
        };

        // Emit one component's fields via reflection, recording in @p report the
        // first field (if any) whose value the reader would reject.
        json SerializeComponentFields(const std::string& typeName, const void* comp, const FieldReportContext& report)
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
                    if (report.unreadable && !report.unreadable->has_value() && !FieldValueIsReadable(comp, f))
                    {
                        *report.unreadable = UnreadableField{report.entity, *report.entityName, typeName, f.fieldName,
                                                             fields[f.fieldName].get<std::string>()};
                    }
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

        /// The order entities are written in: roots by ascending id, each
        /// followed depth-first by its children in Transform::children order;
        /// entities no root reaches (a broken parent link) follow by ascending id.
        /// DeserializeInto creates entities and re-links children in document
        /// order, so this order survives a snapshot restore unchanged: an editor
        /// undo/redo reproduces the prior document byte for byte, sibling order
        /// included. ECS storage order would not (a restore reverses it).
        std::vector<entt::entity> EntitiesInDocumentOrder(const entt::registry& reg)
        {
            std::vector<entt::entity> alive;
            for (auto&& [entity] : reg.storage<entt::entity>()->each())
            {
                alive.push_back(entity);
            }
            std::sort(alive.begin(), alive.end());

            auto parentOf = [&reg](entt::entity entity) -> entt::entity
            {
                const Transform* transform = reg.try_get<Transform>(entity);
                if (!transform || transform->parent == entity || !reg.valid(transform->parent))
                {
                    return entt::null;
                }
                return transform->parent;
            };

            std::vector<entt::entity> order;
            order.reserve(alive.size());
            std::unordered_set<entt::entity> written;
            std::vector<entt::entity> pending;
            for (const entt::entity root : alive)
            {
                if (parentOf(root) != entt::null)
                {
                    continue;
                }
                pending.push_back(root);
                while (!pending.empty())
                {
                    const entt::entity entity = pending.back();
                    pending.pop_back();
                    if (!written.insert(entity).second)
                    {
                        continue;
                    }
                    order.push_back(entity);
                    if (const Transform* transform = reg.try_get<Transform>(entity))
                    {
                        // Push in reverse so the first child is written first.
                        for (auto child : std::ranges::reverse_view(transform->children))
                        {
                            if (reg.valid(child) && parentOf(child) == entity && !written.contains(child))
                            {
                                pending.push_back(child);
                            }
                        }
                    }
                }
            }
            for (const entt::entity entity : alive)
            {
                if (written.insert(entity).second)
                {
                    order.push_back(entity);
                }
            }
            return order;
        }
    } // namespace

    /// Build the scene document. When @p unreadable is non-null it receives the
    /// first field whose written value the reader would refuse.
    static json BuildSceneDocument(const World& world, std::optional<UnreadableField>* unreadable)
    {
        json root;
        root["version"] = kCurrentSceneVersion;
        json entities = json::array();

        auto& factory = ComponentFactory::Get();
        // GetRegisteredNames() follows unordered_map iteration order, which differs
        // between standard libraries (MSVC vs libstdc++). Sort so the same world
        // writes its components in the same order on every platform.
        std::vector<std::string> names = factory.GetRegisteredNames();
        std::sort(names.begin(), names.end());
        const entt::registry& reg = world.GetRegistry();
        // Non-const World handle for the factory (its ops take void* world, uint32 entity).
        World& mutWorld = const_cast<World&>(world);

        for (const entt::entity entity : EntitiesInDocumentOrder(reg))
        {
            json ent;
            ent["id"] = EntityIdToJson(entity);
            const NameComponent* nameComponent = world.GetComponent<NameComponent>(entity);
            const std::string entityName = nameComponent ? nameComponent->name : std::string();
            ent["name"] = entityName;
            ent["parent"] = -1;
            if (const Transform* t = world.GetComponent<Transform>(entity))
                if (t->parent != entt::null)
                    ent["parent"] = EntityIdToJson(t->parent);

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
                c["fields"] = SerializeComponentFields(
                    type, comp, FieldReportContext{unreadable, static_cast<uint32_t>(entity), &entityName});
                comps.push_back(std::move(c));
            }
            ent["components"] = std::move(comps);
            entities.push_back(std::move(ent));
        }

        root["entities"] = std::move(entities);
        return root;
    }

    std::string SerializeWorld(const World& world)
    {
        return BuildSceneDocument(world, nullptr).dump(2);
    }

    bool TrySerializeWorld(const World& world, std::string& out, std::string* error)
    {
        if (error)
        {
            error->clear();
        }
        try
        {
            std::optional<UnreadableField> unreadable;
            std::string text = BuildSceneDocument(world, &unreadable).dump(2);
            if (unreadable)
            {
                return Reject(error, std::format("entity {} ('{}') field '{}.{}' is {}; scenes store only finite "
                                                 "numbers, so this world would not load back",
                                                 unreadable->entity, unreadable->entityName, unreadable->type,
                                                 unreadable->field, unreadable->value));
            }
            if (static_cast<uint64_t>(text.size()) > kMaxSceneDocumentBytes)
            {
                return Reject(error, std::format("scene document is {} bytes; the limit is {} bytes", text.size(),
                                                 kMaxSceneDocumentBytes));
            }
            const size_t values = json::count_values_upper_bound(text);
            if (static_cast<uint64_t>(values) > kMaxSceneDocumentValues)
            {
                return Reject(error, std::format("scene document holds {} JSON values; the limit is {}", values,
                                                 kMaxSceneDocumentValues));
            }
            out = std::move(text);
            return true;
        }
        catch (const std::exception& ex)
        {
            return Reject(error, std::format("scene could not be serialized: {}", ex.what()));
        }
    }

    bool DeserializeInto(World& world, const std::string& jsonText, SceneDeserializeMode mode, std::string* error)
    {
        if (error)
            error->clear();
        // In-process snapshots are this process's own SerializeWorld output, so
        // only untrusted text (files, recovery records) is held to the caps.
        const bool trustedSnapshot = mode == SceneDeserializeMode::TrustedSnapshot;
        if (!trustedSnapshot && static_cast<uint64_t>(jsonText.size()) > kMaxSceneDocumentBytes)
        {
            return Reject(error, std::format("scene document is {} bytes; the limit is {} bytes", jsonText.size(),
                                             kMaxSceneDocumentBytes));
        }
        try
        {
            json root;
            // Nesting depth is bounded inside json::parse (max_parse_depth), so a
            // deeply nested document throws here instead of overflowing the stack.
            // The value budget bounds breadth before any node is allocated.
            const size_t valueBudget =
                trustedSnapshot ? std::numeric_limits<size_t>::max() : static_cast<size_t>(kMaxSceneDocumentValues);
            root = json::parse(jsonText, valueBudget);
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
            std::vector<std::optional<uint32_t>> serializedParents(entities.size());
            std::unordered_set<uint32_t> reservedIds;
            for (size_t index = 0; index < entities.size(); ++index)
            {
                const json& ent = entities[index];
                if (!ent.is_object())
                {
                    return Reject(error, std::format("{} must be a JSON object, found {}", DescribeEntity(index, ent),
                                                     JsonTypeName(ent)));
                }
                if (ent.contains("parent"))
                {
                    const json& parentValue = ent["parent"];
                    uint32_t parentId = 0;
                    if (ReadSerializedEntityId(parentValue, parentId))
                    {
                        serializedParents[index] = parentId;
                    }
                    else if (!parentValue.is_number_integer() || parentValue.get<int64_t>() != -1)
                    {
                        return Reject(error, std::format("{} has parent {}; parent must be an integer entity id or -1",
                                                         DescribeEntity(index, ent), parentValue.dump()));
                    }
                }
                if (!ent.contains("id"))
                    continue;
                const json& idValue = ent["id"];
                uint32_t id = 0;
                if (!ReadSerializedEntityId(idValue, id))
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
                            // The writer emits every round-trippable field of a
                            // current document as a string. A present field of the
                            // wrong type, or one that does not parse, is damage:
                            // reject it (in every mode) so LoadWorld recovers the
                            // .bak image instead of installing a default that the
                            // next save would make permanent. Only legacy inline
                            // values keep lenient conversion.
                            // A trusted snapshot keeps the default for a value it
                            // cannot apply (a NaN the live world held), as before.
                            const bool rejectBadField =
                                strictRecovery || (!legacyScene && !trustedSnapshot && IsRoundTrippableField(f));
                            if (!legacyScene && !fieldValue->is_string())
                            {
                                if (rejectBadField)
                                {
                                    return Reject(error, std::format("{} field '{}.{}' must be a string, found {}",
                                                                     DescribeEntity(entityIndex - 1, ent), type,
                                                                     f.fieldName, JsonTypeName(*fieldValue)));
                                }
                                continue;
                            }
                            if (!SetFieldFromString(comp, f, JsonFieldValueToString(*fieldValue)) && rejectBadField)
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
                if (const std::optional<uint32_t>& parentId = serializedParents[entityIndex - 1])
                {
                    pending.push_back({e, *parentId});
                }
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
