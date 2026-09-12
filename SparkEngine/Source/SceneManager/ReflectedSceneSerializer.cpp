#include "SceneManager/ReflectedSceneSerializer.h"
#include "Engine/ECS/Components.h"
#include "Core/Reflection.h"
#include "Utils/LogMacros.h"

#include <nlohmann_json.h>
#include <fstream>
#include <filesystem>
#include <limits>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

using nlohmann::json;

namespace Spark
{

    namespace
    {
        constexpr int kCurrentSceneVersion = 1;

        bool ReadTextFile(const std::filesystem::path& path, std::string& text)
        {
            std::ifstream input(path, std::ios::binary);
            if (!input.is_open())
                return false;
            text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
            return input.good() || input.eof();
        }

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

        bool WriteDurableText(const std::filesystem::path& path, const std::string& text, std::error_code& error)
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output.is_open())
                return false;
            output.write(text.data(), static_cast<std::streamsize>(text.size()));
            output.close();
            if (output.fail())
                return false;
            return FlushFileDurably(path, error);
        }

        void RemoveFileNoThrow(const std::filesystem::path& path)
        {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }

        // Components handled specially at the entity level, not in the generic "components" list.
        bool IsEntityLevel(const std::string& type)
        {
            return type == "NameComponent";
        }

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

        bool IsRoundTrippableField(const FieldInfo& field)
        {
            if (!field.serialized)
                return false;
            switch (field.type)
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
                return true;
            default:
                return false;
            }
        }

        const FieldInfo* FindFieldBySerializedName(const TypeInfo& type, const std::string& name)
        {
            for (const FieldInfo& field : type.fields)
            {
                if (field.fieldName == name)
                    return &field;
            }
            return nullptr;
        }

        bool ReadStrictEntityId(const json& value, uint32_t& id)
        {
            if (value.is_number_unsigned())
            {
                const uint64_t raw = value.get<uint64_t>();
                if (raw > std::numeric_limits<uint32_t>::max())
                    return false;
                id = static_cast<uint32_t>(raw);
                return static_cast<entt::entity>(id) != entt::null;
            }
            if (!value.is_number_integer())
                return false;
            const int64_t raw = value.get<int64_t>();
            if (raw < 0 || static_cast<uint64_t>(raw) > std::numeric_limits<uint32_t>::max())
                return false;
            id = static_cast<uint32_t>(raw);
            return static_cast<entt::entity>(id) != entt::null;
        }

        bool ReadStrictParentId(const json& value, int64_t& parentId)
        {
            if (value.is_number_integer())
            {
                parentId = value.get<int64_t>();
                return parentId >= -1;
            }
            if (!value.is_number_unsigned())
                return false;
            const uint64_t raw = value.get<uint64_t>();
            if (raw > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                return false;
            parentId = static_cast<int64_t>(raw);
            return true;
        }

        bool ValidateStrictRecoveryDocument(const json& root, ComponentFactory& factory)
        {
            if (!root.is_object() || root.contains("sceneVersion") || !root.contains("version") ||
                !root["version"].is_number_integer() || root["version"].get<int64_t>() != kCurrentSceneVersion ||
                !root.contains("entities") || !root["entities"].is_array())
            {
                return false;
            }

            std::unordered_set<uint32_t> entityIds;
            std::vector<int64_t> parentIds;
            for (const json& entity : root["entities"])
            {
                if (!entity.is_object() || !entity.contains("id") || !entity.contains("name") ||
                    !entity.contains("parent") || !entity.contains("components") || !entity["name"].is_string() ||
                    !entity["components"].is_array())
                {
                    return false;
                }

                uint32_t entityId = 0;
                int64_t parentId = -1;
                if (!ReadStrictEntityId(entity["id"], entityId) || !entityIds.insert(entityId).second ||
                    !ReadStrictParentId(entity["parent"], parentId))
                {
                    return false;
                }
                parentIds.push_back(parentId);

                std::unordered_set<std::string> componentTypes;
                for (const json& component : entity["components"])
                {
                    if (!component.is_object() || !component.contains("type") || !component["type"].is_string() ||
                        !component.contains("fields") || !component["fields"].is_object())
                    {
                        return false;
                    }

                    const std::string type = component["type"].get<std::string>();
                    if (type.empty() || IsEntityLevel(type) || !factory.IsRegistered(type) ||
                        !componentTypes.insert(type).second)
                    {
                        return false;
                    }

                    const TypeInfo* typeInfo = TypeRegistry::Get().FindTypeByName(type);
                    if (!typeInfo)
                        return false;

                    const json& fields = component["fields"];
                    for (const auto& field : fields.items())
                    {
                        const FieldInfo* fieldInfo = FindFieldBySerializedName(*typeInfo, field.key);
                        if (!fieldInfo || !IsRoundTrippableField(*fieldInfo) || !field.value.is_string())
                            return false;
                    }
                    for (const FieldInfo& field : typeInfo->fields)
                    {
                        if (IsRoundTrippableField(field) && !fields.contains(field.fieldName))
                            return false;
                    }
                }
            }

            for (const int64_t parentId : parentIds)
            {
                if (parentId >= 0 && (static_cast<uint64_t>(parentId) > std::numeric_limits<uint32_t>::max() ||
                                      !entityIds.contains(static_cast<uint32_t>(parentId))))
                    return false;
            }
            return true;
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

    bool DeserializeInto(World& world, const std::string& jsonText, SceneDeserializeMode mode)
    {
        try
        {
            json root;
            root = json::parse(jsonText);
            if (!root.is_object() || !root.contains("entities") || !root["entities"].is_array())
                return false;

            // SparkEditor versions before the reflected serializer shipped
            // `sceneVersion` plus inline component values. Keep those projects
            // loadable and migrate naturally on their next explicit save.
            const bool hasCurrentVersion = root.contains("version");
            const bool hasLegacyVersion = root.contains("sceneVersion");
            if (hasCurrentVersion == hasLegacyVersion)
                return false;

            const char* versionField = hasCurrentVersion ? "version" : "sceneVersion";
            if (!root[versionField].is_number_integer() || root[versionField].get<int64_t>() != kCurrentSceneVersion)
                return false;
            const bool legacyScene = hasLegacyVersion;

            auto& factory = ComponentFactory::Get();
            const bool strictRecovery = mode == SceneDeserializeMode::StrictRecovery;
            if (strictRecovery && (!hasCurrentVersion || !ValidateStrictRecoveryDocument(root, factory)))
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
                if (!ent.contains("id"))
                    continue;
                if (!ent["id"].is_number_integer() && !ent["id"].is_number_unsigned())
                    return false;

                const int64_t rawId = ent.value<int64_t>("id", -1);
                if (rawId < 0 || static_cast<uint64_t>(rawId) > std::numeric_limits<uint32_t>::max())
                    return false;
                const uint32_t id = static_cast<uint32_t>(rawId);
                if (static_cast<entt::entity>(id) == entt::null || !reservedIds.insert(id).second)
                    return false;
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
                        return false;
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
                    return false;
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
                                return false;
                            continue;
                        }
                        if (!factory.HasComponent(type, &world, (uint32_t)e))
                            factory.AddComponent(type, &world, (uint32_t)e);
                        void* comp = factory.GetComponentRaw(type, &world, (uint32_t)e);
                        if (!comp)
                        {
                            if (strictRecovery)
                                return false;
                            continue;
                        }
                        const TypeInfo* ti = TypeRegistry::Get().FindTypeByName(type);
                        if (!ti)
                        {
                            if (strictRecovery)
                                return false;
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
                                    return false;
                                continue;
                            }
                            if (!legacyScene && !fieldValue->is_string())
                            {
                                if (strictRecovery)
                                    return false;
                                continue;
                            }
                            if (!SetFieldFromString(comp, f, JsonFieldValueToString(*fieldValue)) && strictRecovery)
                                return false;
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
                        return false;
                    continue;
                }
                if (!world.SetParent(p.child, it->second) && strictRecovery)
                    return false;
            }
            return true;
        }
        catch (const std::exception& ex)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "[ReflectedScene] deserialization error: %s", ex.what());
            return false;
        }
    }

    bool SaveWorld(const World& world, const std::string& path)
    {
        const std::filesystem::path destination = std::filesystem::u8path(path);
        std::filesystem::path temporary = destination;
        temporary += ".tmp";
        std::filesystem::path backup = destination;
        backup += ".bak";
        std::filesystem::path backupTemporary = backup;
        backupTemporary += ".tmp";

        RemoveFileNoThrow(temporary);
        RemoveFileNoThrow(backupTemporary);

        const std::string serialized = SerializeWorld(world);
        std::error_code error;
        if (!WriteDurableText(temporary, serialized, error))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "[ReflectedScene] durable staging write failed for %s: %s",
                           path.c_str(), error.message().c_str());
            RemoveFileNoThrow(temporary);
            return false;
        }

        // Preserve the previous image only when it is a loadable scene. A
        // corrupt destination must never displace the last known-good backup.
        std::string previous;
        if (ReadTextFile(destination, previous))
        {
            World validationWorld(World::EntityEventCleanupMode::Suppressed);
            if (DeserializeInto(validationWorld, previous))
            {
                if (!WriteDurableText(backupTemporary, previous, error) ||
                    !ReplaceFileAtomically(backupTemporary, backup, error))
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Core, "[ReflectedScene] previous-good backup failed for %s: %s",
                                   path.c_str(), error.message().c_str());
                    RemoveFileNoThrow(temporary);
                    RemoveFileNoThrow(backupTemporary);
                    return false;
                }
            }
        }

        if (!ReplaceFileAtomically(temporary, destination, error))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "[ReflectedScene] atomic replace failed for %s: %s", path.c_str(),
                           error.message().c_str());
            RemoveFileNoThrow(temporary);
            return false;
        }
        return true;
    }

    bool LoadWorld(World& world, const std::string& path)
    {
        const std::filesystem::path primary = std::filesystem::u8path(path);
        std::filesystem::path backup = primary;
        backup += ".bak";

        std::string text;
        if (ReadTextFile(primary, text) && DeserializeInto(world, text))
            return true;

        if (!ReadTextFile(backup, text))
            return false;

        SPARK_LOG_WARN(Spark::LogCategory::Core, "[ReflectedScene] recovering %s from previous-good backup",
                       path.c_str());
        return DeserializeInto(world, text);
    }

} // namespace Spark
