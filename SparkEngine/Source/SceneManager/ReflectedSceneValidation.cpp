#include "SceneManager/ReflectedSceneValidation.h"

#include "Engine/ECS/Components.h"

#include <format>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

using nlohmann::json;

namespace Spark::ReflectedSceneDetail
{
    namespace
    {
        std::string SupportedWindowText()
        {
            return std::format("this build reads reflected scene version {} and the legacy editor "
                               "'sceneVersion': {} dialect, and writes version {}",
                               kCurrentSceneVersion, kLegacyEditorSceneVersion, kCurrentSceneVersion);
        }

        const FieldInfo* FindFieldBySerializedName(const TypeInfo& type, const std::string& name)
        {
            for (const FieldInfo& field : type.fields)
            {
                if (field.fieldName == name)
                {
                    return &field;
                }
            }
            return nullptr;
        }

        bool ReadStrictEntityId(const json& value, uint32_t& id)
        {
            if (value.is_number_unsigned())
            {
                const uint64_t raw = value.get<uint64_t>();
                if (raw > std::numeric_limits<uint32_t>::max())
                {
                    return false;
                }
                id = static_cast<uint32_t>(raw);
                return static_cast<entt::entity>(id) != entt::null;
            }
            if (!value.is_number_integer())
            {
                return false;
            }
            const int64_t raw = value.get<int64_t>();
            if (raw < 0 || static_cast<uint64_t>(raw) > std::numeric_limits<uint32_t>::max())
            {
                return false;
            }
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
            {
                return false;
            }
            const uint64_t raw = value.get<uint64_t>();
            if (raw > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            {
                return false;
            }
            parentId = static_cast<int64_t>(raw);
            return true;
        }
    } // namespace

    bool Reject(std::string* error, std::string message)
    {
        if (error)
        {
            *error = std::move(message);
        }
        return false;
    }

    const char* JsonTypeName(const json& value)
    {
        switch (value.type())
        {
        case json::value_t::null:
            return "null";
        case json::value_t::boolean:
            return "boolean";
        case json::value_t::number_integer:
        case json::value_t::number_unsigned:
        case json::value_t::number_float:
            return "number";
        case json::value_t::string:
            return "string";
        case json::value_t::array:
            return "array";
        case json::value_t::object:
            return "object";
        }
        return "unknown";
    }

    std::string DescribeEntity(size_t index, const json& entity)
    {
        if (entity.is_object() && entity.contains("name") && entity["name"].is_string() &&
            !entity["name"].get_ref().empty())
        {
            return std::format("entity #{} ('{}')", index, entity["name"].get_ref());
        }
        return std::format("entity #{}", index);
    }

    bool CheckSceneVersion(const json& root, bool& legacyScene, std::string* error)
    {
        const bool hasCurrentVersion = root.contains("version");
        const bool hasLegacyVersion = root.contains("sceneVersion");
        if (hasCurrentVersion && hasLegacyVersion)
        {
            return Reject(error, "scene declares both 'version' and 'sceneVersion', so its dialect is ambiguous; "
                                 "keep exactly one (" +
                                     SupportedWindowText() + ")");
        }
        if (!hasCurrentVersion && !hasLegacyVersion)
        {
            return Reject(error, "scene declares no 'version' field, so it is not a reflected SparkEngine scene (" +
                                     SupportedWindowText() + ")");
        }

        legacyScene = hasLegacyVersion;
        const char* versionField = hasCurrentVersion ? "version" : "sceneVersion";
        const json& versionValue = root[versionField];
        if (!versionValue.is_number_integer())
        {
            return Reject(error, std::format("scene '{}' must be an integer, found {} {}; {}", versionField,
                                             JsonTypeName(versionValue), versionValue.dump(), SupportedWindowText()));
        }

        const int64_t fileVersion = versionValue.get<int64_t>();
        const int64_t supportedVersion = legacyScene ? kLegacyEditorSceneVersion : kCurrentSceneVersion;
        if (fileVersion == supportedVersion)
        {
            return true;
        }

        const std::string action = fileVersion > supportedVersion
                                       ? "open it with the newer SparkEngine build that wrote it"
                                       : "no migration exists for that version; re-export it from the tool "
                                         "that wrote it";
        return Reject(error, std::format("scene '{}' {} is unsupported: {}; {}", versionField, fileVersion,
                                         SupportedWindowText(), action));
    }

    // Components handled specially at the entity level, not in the generic "components" list.
    bool IsEntityLevel(const std::string& type)
    {
        return type == "NameComponent";
    }

    bool IsRoundTrippableField(const FieldInfo& field)
    {
        if (!field.serialized)
        {
            return false;
        }
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

    bool ValidateStrictRecoveryDocument(const json& root, ComponentFactory& factory, std::string* error)
    {
        constexpr const char* kRecovery = "crash-recovery record";
        if (root.contains("sceneVersion"))
        {
            return Reject(error, std::format("{} uses the legacy 'sceneVersion' dialect; recovery records must "
                                             "declare 'version': {}",
                                             kRecovery, kCurrentSceneVersion));
        }

        std::unordered_set<uint32_t> entityIds;
        std::vector<std::pair<size_t, int64_t>> parentIds;
        const json& entities = root["entities"];
        for (size_t index = 0; index < entities.size(); ++index)
        {
            const json& entity = entities[index];
            const std::string where = DescribeEntity(index, entity);
            if (!entity.is_object() || !entity.contains("id") || !entity.contains("name") ||
                !entity.contains("parent") || !entity.contains("components") || !entity["name"].is_string() ||
                !entity["components"].is_array())
            {
                return Reject(error, std::format("{} {} must be an object with 'id', string 'name', 'parent' "
                                                 "and array 'components'",
                                                 kRecovery, where));
            }

            uint32_t entityId = 0;
            int64_t parentId = -1;
            if (!ReadStrictEntityId(entity["id"], entityId))
            {
                return Reject(error, std::format("{} {} has invalid id {}", kRecovery, where, entity["id"].dump()));
            }
            if (!entityIds.insert(entityId).second)
            {
                return Reject(error, std::format("{} {} repeats id {}", kRecovery, where, entityId));
            }
            if (!ReadStrictParentId(entity["parent"], parentId))
            {
                return Reject(error,
                              std::format("{} {} has invalid parent {}", kRecovery, where, entity["parent"].dump()));
            }
            parentIds.emplace_back(index, parentId);

            std::unordered_set<std::string> componentTypes;
            for (const json& component : entity["components"])
            {
                if (!component.is_object() || !component.contains("type") || !component["type"].is_string() ||
                    !component.contains("fields") || !component["fields"].is_object())
                {
                    return Reject(error, std::format("{} {} has a component without string 'type' and object "
                                                     "'fields'",
                                                     kRecovery, where));
                }

                const std::string type = component["type"].get<std::string>();
                if (type.empty() || IsEntityLevel(type) || !factory.IsRegistered(type))
                {
                    return Reject(error, std::format("{} {} has component type '{}', which this build does not "
                                                     "register",
                                                     kRecovery, where, type));
                }
                if (!componentTypes.insert(type).second)
                {
                    return Reject(error, std::format("{} {} repeats component type '{}'", kRecovery, where, type));
                }

                const TypeInfo* typeInfo = TypeRegistry::Get().FindTypeByName(type);
                if (!typeInfo)
                {
                    return Reject(error, std::format("{} {} component '{}' has no reflection data in this build",
                                                     kRecovery, where, type));
                }

                const json& fields = component["fields"];
                for (const auto& field : fields.items())
                {
                    const FieldInfo* fieldInfo = FindFieldBySerializedName(*typeInfo, field.key);
                    if (!fieldInfo || !IsRoundTrippableField(*fieldInfo) || !field.value.is_string())
                    {
                        return Reject(error, std::format("{} {} field '{}.{}' is not a string-encoded reflected "
                                                         "field of this build's schema",
                                                         kRecovery, where, type, field.key));
                    }
                }
                for (const FieldInfo& field : typeInfo->fields)
                {
                    if (IsRoundTrippableField(field) && !fields.contains(field.fieldName))
                    {
                        return Reject(error, std::format("{} {} is missing field '{}.{}' required by this "
                                                         "build's schema",
                                                         kRecovery, where, type, field.fieldName));
                    }
                }
            }
        }

        for (const auto& [index, parentId] : parentIds)
        {
            if (parentId >= 0 && (static_cast<uint64_t>(parentId) > std::numeric_limits<uint32_t>::max() ||
                                  !entityIds.contains(static_cast<uint32_t>(parentId))))
            {
                return Reject(error, std::format("{} {} references parent id {}, which is not in the record", kRecovery,
                                                 DescribeEntity(index, entities[index]), parentId));
            }
        }
        return true;
    }
} // namespace Spark::ReflectedSceneDetail
