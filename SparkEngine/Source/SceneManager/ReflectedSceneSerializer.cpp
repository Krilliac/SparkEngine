#include "SceneManager/ReflectedSceneSerializer.h"
#include "Engine/ECS/Components.h"
#include "Core/Reflection.h"
#include "Utils/LogMacros.h"

#include <nlohmann_json.h>
#include <fstream>
#include <unordered_map>

using nlohmann::json;

namespace Spark {

namespace {
// Components handled specially at the entity level, not in the generic "components" list.
bool IsEntityLevel(const std::string& type) { return type == "NameComponent"; }

// Emit one component's fields via reflection.
json SerializeComponentFields(const std::string& typeName, const void* comp)
{
    json fields = json::object();
    const TypeInfo* ti = TypeRegistry::Get().FindTypeByName(typeName);
    if (!ti) return fields;
    for (const FieldInfo& f : ti->fields) {
        if (!f.serialized) continue;
        switch (f.type) {
            case FieldType::Bool: case FieldType::Int: case FieldType::Float:
            case FieldType::Double: case FieldType::String:
            case FieldType::Vector2: case FieldType::Vector3: case FieldType::Vector4:
                fields[f.fieldName] = GetFieldAsString(comp, f);
                break;
            default:
                SPARK_LOG_WARN(Spark::LogCategory::Core,
                    "[ReflectedScene] skip unsupported field %s.%s (type %d)",
                    typeName.c_str(), f.fieldName.c_str(), (int)f.type);
                break;
        }
    }
    return fields;
}
} // namespace

std::string SerializeWorld(const World& world)
{
    json root;
    root["version"] = 1;
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
            if (t->parent != entt::null) parentId = static_cast<int>(static_cast<uint32_t>(t->parent));
        ent["parent"] = parentId;

        json comps = json::array();
        for (const std::string& type : names) {
            if (IsEntityLevel(type)) continue; // name handled above
            if (!factory.HasComponent(type, &mutWorld, static_cast<uint32_t>(entity))) continue;
            void* comp = factory.GetComponentRaw(type, &mutWorld, static_cast<uint32_t>(entity));
            if (!comp) continue;
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

} // namespace Spark
