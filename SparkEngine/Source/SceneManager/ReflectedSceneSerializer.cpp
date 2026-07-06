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

bool DeserializeInto(World& world, const std::string& jsonText)
{
    try {
        json root;
        root = json::parse(jsonText);
        if (!root.contains("entities") || !root["entities"].is_array()) return false;

        auto& factory = ComponentFactory::Get();
        std::unordered_map<uint32_t, entt::entity> idMap; // serialized id -> live entity
        struct PendingParent { entt::entity child; uint32_t parentId; };
        std::vector<PendingParent> pending;

        for (const json& ent : root["entities"]) {
            const std::string name = ent.value("name", std::string());
            entt::entity e = world.CreateEntity(name); // emplaces NameComponent when name non-empty
            // Note: use int64_t (not "0u"/unsigned int) — the bundled json stub
            // (ThirdParty/Utils/json/nlohmann_json.h) only specializes get<T>() for
            // a fixed set of types and unsigned int isn't one of them, which is a
            // link error (LNK2001), not a compile error.
            const uint32_t sid = static_cast<uint32_t>(ent.value<int64_t>("id", 0));
            idMap[sid] = e;

            if (ent.contains("components") && ent["components"].is_array()) {
                for (const json& c : ent["components"]) {
                    const std::string type = c.value("type", std::string());
                    if (type.empty() || !factory.IsRegistered(type)) {
                        SPARK_LOG_WARN(Spark::LogCategory::Core,
                            "[ReflectedScene] unknown component type '%s' skipped", type.c_str());
                        continue;
                    }
                    if (!factory.HasComponent(type, &world, (uint32_t)e))
                        factory.AddComponent(type, &world, (uint32_t)e);
                    void* comp = factory.GetComponentRaw(type, &world, (uint32_t)e);
                    if (!comp) continue;
                    const TypeInfo* ti = TypeRegistry::Get().FindTypeByName(type);
                    if (!ti || !c.contains("fields")) continue;
                    for (const FieldInfo& f : ti->fields) {
                        if (!c["fields"].contains(f.fieldName)) continue;
                        const auto& fv = c["fields"][f.fieldName];
                        if (!fv.is_string()) continue;
                        SetFieldFromString(comp, f, fv.get<std::string>());
                    }
                }
            }
            const int parentId = ent.value("parent", -1);
            if (parentId >= 0) pending.push_back({ e, (uint32_t)parentId });
        }

        // Second pass: resolve parents now that all ids exist.
        for (const PendingParent& p : pending) {
            auto it = idMap.find(p.parentId);
            if (it == idMap.end()) continue;
            Transform* childT = world.GetComponent<Transform>(p.child);
            if (!childT) childT = &world.AddComponent<Transform>(p.child);
            childT->parent = it->second;
            if (Transform* parentT = world.GetComponent<Transform>(it->second))
                parentT->children.push_back(p.child);
        }
        return true;
    } catch (const std::exception& ex) {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "[ReflectedScene] deserialization error: %s", ex.what());
        return false;
    }
}

bool SaveWorld(const World& world, const std::string& path)
{
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    f << SerializeWorld(world);
    return f.good();
}

bool LoadWorld(World& world, const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return DeserializeInto(world, text);
}

} // namespace Spark
