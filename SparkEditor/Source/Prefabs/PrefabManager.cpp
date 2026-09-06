/**
 * @file PrefabManager.cpp
 * @brief Implementation of the PrefabManager
 * @author Spark Engine Team
 * @date 2025
 */

#include "PrefabManager.h"
#include "../SceneSystem/SceneComponentCodec.h"
#include "Utils/ContainerUtils.h"
#include "Utils/LogMacros.h"
#include "Utils/Validate.h"
#include <algorithm>
#include <cinttypes>
#include <filesystem>
#include <iostream>
#include <map>

namespace SparkEditor
{

    bool PrefabManager::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        CreateSamplePrefabs();
        return true;
    }

    void PrefabManager::Shutdown()
    {
        m_prefabs.clear();
        m_instances.clear();
    }

    PrefabAsset* PrefabManager::CreatePrefabFromEntity(uint64_t entityId, const std::string& prefabName)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        PrefabAsset prefab(prefabName);

        // Query the scene for the entity's transform and components
        if (m_scene)
        {
            auto objectId = static_cast<ObjectID>(entityId);
            SceneObject* obj = m_scene->FindObject(objectId);
            if (obj)
            {
                // Serialize the transform
                SerializedComponent transform;
                transform.typeName = "Transform";
                transform.properties["position"] = obj->transform.position;
                transform.properties["rotation"] = obj->transform.rotation;
                transform.properties["scale"] = obj->transform.scale;
                prefab.AddComponent(transform);

                // Serialize each component attached to the entity
                auto comps = m_scene->GetObjectComponents(objectId);
                for (auto* comp : comps)
                {
                    SerializedComponent sc;
                    sc.typeName = std::to_string(static_cast<uint32_t>(comp->type));
                    sc.properties["enabled"] = comp->enabled;
                    prefab.AddComponent(sc);
                }
            }
            else
            {
                SPARK_LOG_WARN(Spark::LogCategory::Editor, "Entity %llu not found in scene for prefab creation",
                               static_cast<unsigned long long>(entityId));
            }
        }

        // Fallback: always include a default transform if we have nothing
        if (prefab.GetComponents().empty())
        {
            SerializedComponent transform;
            transform.typeName = "Transform";
            transform.properties["position"] = XMFLOAT3{0.0f, 0.0f, 0.0f};
            transform.properties["rotation"] = XMFLOAT4{0.0f, 0.0f, 0.0f, 1.0f};
            transform.properties["scale"] = XMFLOAT3{1.0f, 1.0f, 1.0f};
            prefab.AddComponent(transform);
        }

        m_prefabs[prefabName] = std::move(prefab);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Created prefab from entity %" PRIu64 ": '%s'", entityId,
                       prefabName.c_str());
        NotifyPrefabsChanged();
        return &m_prefabs[prefabName];
    }

    PrefabAsset* PrefabManager::CreateEmptyPrefab(const std::string& name)
    {
        PrefabAsset prefab(name);

        // Add a default Transform component
        SerializedComponent transform;
        transform.typeName = "Transform";
        transform.properties["position"] = XMFLOAT3{0.0f, 0.0f, 0.0f};
        transform.properties["rotation"] = XMFLOAT4{0.0f, 0.0f, 0.0f, 1.0f};
        transform.properties["scale"] = XMFLOAT3{1.0f, 1.0f, 1.0f};
        prefab.AddComponent(transform);

        m_prefabs[name] = std::move(prefab);
        NotifyPrefabsChanged();
        return &m_prefabs[name];
    }

    uint64_t PrefabManager::InstantiatePrefab(const std::string& prefabName)
    {
        auto it = m_prefabs.find(prefabName);
        if (it == m_prefabs.end())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "Cannot instantiate unknown prefab: '%s'", prefabName.c_str());
            return 0;
        }

        // Create a scene object from the prefab data
        if (m_scene)
        {
            ObjectID newId = m_scene->GetNextObjectID();
            SceneObject obj;
            obj.id = newId;
            obj.name = prefabName;
            obj.active = true;

            // Materialize the template: full transform (rotation included) plus the component type list, so
            // an instance of a prefab carrying Camera/RigidBody/Collider is not a bare SceneObject.
            for (const auto& comp : it->second.GetComponents())
            {
                if (comp.typeName == "Transform")
                {
                    auto posIt = comp.properties.find("position");
                    if (posIt != comp.properties.end())
                    {
                        if (auto* val = std::get_if<XMFLOAT3>(&posIt->second))
                            obj.transform.position = *val;
                    }
                    auto rotIt = comp.properties.find("rotation");
                    if (rotIt != comp.properties.end())
                    {
                        if (auto* val = std::get_if<XMFLOAT4>(&rotIt->second))
                            obj.transform.rotation = *val;
                    }
                    auto scaleIt = comp.properties.find("scale");
                    if (scaleIt != comp.properties.end())
                    {
                        if (auto* val = std::get_if<XMFLOAT3>(&scaleIt->second))
                            obj.transform.scale = *val;
                    }
                    continue;
                }

                ComponentType componentType = ComponentType::CUSTOM;
                if (!TryParseSceneComponentTypeName(comp.typeName, componentType))
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Editor,
                                   "Prefab '%s' component '%s' has no scene component type; not attached",
                                   prefabName.c_str(), comp.typeName.c_str());
                    continue;
                }
                if (!Spark::ContainerUtils::Contains(obj.componentTypes, componentType))
                    obj.componentTypes.push_back(componentType);
            }

            m_scene->objects.push_back(std::move(obj));

            SPARK_LOG_INFO(Spark::LogCategory::Editor, "Instantiated prefab '%s' as entity %llu", prefabName.c_str(),
                           (unsigned long long)newId);
            RegisterInstance(newId, prefabName);
            return newId;
        }

        // Fallback if no scene is set: use a counter
        static uint64_t nextFallbackId = 1000;
        uint64_t entityId = nextFallbackId++;
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Instantiated prefab '%s' as entity %llu (no scene)",
                       prefabName.c_str(), static_cast<unsigned long long>(entityId));
        RegisterInstance(entityId, prefabName);
        return entityId;
    }

    bool PrefabManager::SavePrefab(const std::string& name, const std::string& directory)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        auto it = m_prefabs.find(name);
        if (it == m_prefabs.end())
        {
            return false;
        }

        std::string dir = directory.empty() ? "." : directory;
        std::string path = dir + "/" + name + ".sparkprefab";
        return it->second.Save(path);
    }

    PrefabAsset* PrefabManager::LoadPrefab(const std::string& filePath)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        PrefabAsset prefab = PrefabAsset::Load(filePath);
        if (prefab.GetName().empty())
        {
            return nullptr;
        }

        std::string name = prefab.GetName();
        m_prefabs[name] = std::move(prefab);
        NotifyPrefabsChanged();
        return &m_prefabs[name];
    }

    bool PrefabManager::DeletePrefab(const std::string& name)
    {
        auto it = m_prefabs.find(name);
        if (it == m_prefabs.end())
        {
            return false;
        }

        m_prefabs.erase(it);

        // Remove all instances of this prefab
        m_instances.erase(std::remove_if(m_instances.begin(), m_instances.end(),
                                         [&](const PrefabInstance& inst) { return inst.sourcePrefabName == name; }),
                          m_instances.end());

        NotifyPrefabsChanged();
        return true;
    }

    PrefabAsset* PrefabManager::GetPrefab(const std::string& name)
    {
        auto it = m_prefabs.find(name);
        return (it != m_prefabs.end()) ? &it->second : nullptr;
    }

    const PrefabAsset* PrefabManager::GetPrefab(const std::string& name) const
    {
        auto it = m_prefabs.find(name);
        return (it != m_prefabs.end()) ? &it->second : nullptr;
    }

    std::vector<std::string> PrefabManager::GetPrefabNames() const
    {
        std::vector<std::string> names;
        names.reserve(m_prefabs.size());
        for (const auto& [name, prefab] : m_prefabs)
        {
            names.push_back(name);
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    void PrefabManager::RegisterInstance(uint64_t entityId, const std::string& prefabName)
    {
        PrefabInstance instance;
        instance.entityId = entityId;
        instance.sourcePrefabName = prefabName;
        m_instances.push_back(std::move(instance));
    }

    void PrefabManager::UnregisterInstance(uint64_t entityId)
    {
        m_instances.erase(std::remove_if(m_instances.begin(), m_instances.end(),
                                         [entityId](const PrefabInstance& inst) { return inst.entityId == entityId; }),
                          m_instances.end());
    }

    std::vector<PrefabInstance> PrefabManager::GetInstances(const std::string& prefabName) const
    {
        std::vector<PrefabInstance> result;
        for (const auto& inst : m_instances)
        {
            if (inst.sourcePrefabName == prefabName)
            {
                result.push_back(inst);
            }
        }
        return result;
    }

    bool PrefabManager::ApplyTemplateTransform(const PrefabAsset& prefab, const PrefabInstance& instance)
    {
        if (!m_scene)
            return false;

        auto objIt = std::find_if(m_scene->objects.begin(), m_scene->objects.end(),
                                  [&](const SceneObject& obj) { return obj.id == instance.entityId; });
        if (objIt == m_scene->objects.end())
            return false;

        const SerializedComponent* transform = prefab.GetComponent("Transform");
        if (!transform)
            return false;

        // An override is the value the instance is supposed to keep, so it is the value that gets
        // written. Treating the override as nothing more than a skip-list left PrefabOverride::value
        // recorded and never read: the instance kept whatever the SceneObject happened to hold, which
        // is not what SetInstanceOverride() promised to store.
        std::map<std::string, const PrefabPropertyValue*> transformOverrides;
        for (const auto& instanceOverride : instance.overrides)
        {
            if (instanceOverride.componentType == "Transform")
                transformOverrides.emplace(instanceOverride.propertyName, &instanceOverride.value);
        }

        const auto findOverride = [&transformOverrides](const std::string& propertyName) -> const PrefabPropertyValue*
        {
            const auto it = transformOverrides.find(propertyName);
            return it == transformOverrides.end() ? nullptr : it->second;
        };

        // Writes the override when one is recorded, otherwise the template value. A recorded override
        // holding the wrong alternative writes nothing at all rather than falling back to the template,
        // because the instance deliberately does not track the template for that property.
        const auto resolve = [&transform, &findOverride]<typename T>(const std::string& propertyName,
                                                                     T& destination) -> bool
        {
            if (const PrefabPropertyValue* overrideValue = findOverride(propertyName))
            {
                if (const auto* typed = std::get_if<T>(overrideValue))
                {
                    destination = *typed;
                    return true;
                }
                return false;
            }

            const auto templateIt = transform->properties.find(propertyName);
            if (templateIt == transform->properties.end())
                return false;
            if (const auto* typed = std::get_if<T>(&templateIt->second))
            {
                destination = *typed;
                return true;
            }
            return false;
        };

        // Every property is resolved: `||` would short-circuit and leave the later ones unwritten.
        bool wrote = false;
        if (resolve("position", objIt->transform.position))
            wrote = true;
        if (resolve("rotation", objIt->transform.rotation))
            wrote = true;
        if (resolve("scale", objIt->transform.scale))
            wrote = true;
        return wrote;
    }

    int PrefabManager::ApplyPrefabToInstances(const std::string& prefabName)
    {
        const PrefabAsset* prefab = GetPrefab(prefabName);
        if (!prefab)
        {
            return 0;
        }

        if (!m_scene)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor,
                           "Cannot apply prefab '%s' to instances: no scene is set on the prefab manager",
                           prefabName.c_str());
            return 0;
        }

        int updatedCount = 0;
        for (const auto& instance : m_instances)
        {
            if (instance.sourcePrefabName != prefabName)
                continue;
            if (ApplyTemplateTransform(*prefab, instance))
                ++updatedCount;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Applied prefab '%s' to %d instance(s)", prefabName.c_str(),
                       updatedCount);
        return updatedCount;
    }

    bool PrefabManager::SetInstanceOverride(uint64_t entityId, const std::string& componentType,
                                            const std::string& propertyName, const PrefabPropertyValue& value)
    {
        auto instanceIt = std::find_if(m_instances.begin(), m_instances.end(),
                                       [entityId](const PrefabInstance& inst) { return inst.entityId == entityId; });
        if (instanceIt == m_instances.end())
            return false;

        for (auto& existing : instanceIt->overrides)
        {
            if (existing.componentType == componentType && existing.propertyName == propertyName)
            {
                existing.value = value;
                return true;
            }
        }

        PrefabOverride added;
        added.componentType = componentType;
        added.propertyName = propertyName;
        added.value = value;
        instanceIt->overrides.push_back(std::move(added));
        return true;
    }

    bool PrefabManager::RevertInstance(uint64_t entityId)
    {
        auto instanceIt = std::find_if(m_instances.begin(), m_instances.end(),
                                       [entityId](const PrefabInstance& inst) { return inst.entityId == entityId; });
        if (instanceIt == m_instances.end())
            return false;

        const PrefabAsset* prefab = GetPrefab(instanceIt->sourcePrefabName);
        if (!prefab)
            return false;

        instanceIt->overrides.clear();

        // The overrides are gone the moment the line above runs, so the revert HAS happened. Returning
        // ApplyTemplateTransform()'s result told the caller the revert failed — while the instance had
        // already been changed irreversibly — whenever the prefab has no Transform component, stores
        // none of position/rotation/scale, or the instance's SceneObject is gone.
        if (!ApplyTemplateTransform(*prefab, *instanceIt))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor,
                           "Reverted instance %llu of prefab '%s': overrides cleared, but no template transform "
                           "was written back",
                           static_cast<unsigned long long>(entityId), instanceIt->sourcePrefabName.c_str());
        }
        return true;
    }

    bool PrefabManager::RevertProperty(uint64_t entityId, const std::string& componentType,
                                       const std::string& propertyName)
    {
        auto instanceIt = std::find_if(m_instances.begin(), m_instances.end(),
                                       [entityId](const PrefabInstance& inst) { return inst.entityId == entityId; });
        if (instanceIt == m_instances.end())
            return false;

        auto& overrides = instanceIt->overrides;
        const auto removed =
            std::remove_if(overrides.begin(), overrides.end(), [&](const PrefabOverride& o)
                           { return o.componentType == componentType && o.propertyName == propertyName; });
        if (removed == overrides.end())
            return false;
        overrides.erase(removed, overrides.end());

        const PrefabAsset* prefab = GetPrefab(instanceIt->sourcePrefabName);
        if (prefab)
            ApplyTemplateTransform(*prefab, *instanceIt);
        return true;
    }

    void PrefabManager::NotifyPrefabsChanged()
    {
        if (m_onPrefabsChanged)
        {
            m_onPrefabsChanged();
        }
    }

    void PrefabManager::CreateSamplePrefabs()
    {
        // FPS Player prefab
        {
            PrefabAsset player("FPS Player");
            SerializedComponent transform;
            transform.typeName = "Transform";
            transform.properties["position"] = XMFLOAT3{0.0f, 1.8f, 0.0f};
            transform.properties["rotation"] = XMFLOAT4{0.0f, 0.0f, 0.0f, 1.0f};
            transform.properties["scale"] = XMFLOAT3{1.0f, 1.0f, 1.0f};
            player.AddComponent(transform);

            SerializedComponent camera;
            camera.typeName = "Camera";
            camera.properties["fov"] = 90.0f;
            camera.properties["nearClip"] = 0.1f;
            camera.properties["farClip"] = 1000.0f;
            player.AddComponent(camera);

            SerializedComponent rigidbody;
            rigidbody.typeName = "RigidBody";
            rigidbody.properties["mass"] = 80.0f;
            rigidbody.properties["isKinematic"] = false;
            player.AddComponent(rigidbody);

            SerializedComponent collider;
            collider.typeName = "Collider";
            collider.properties["type"] = std::string("Capsule");
            collider.properties["height"] = 1.8f;
            collider.properties["radius"] = 0.4f;
            player.AddComponent(collider);

            m_prefabs["FPS Player"] = std::move(player);
        }

        // Point Light prefab
        {
            PrefabAsset light("Point Light");
            SerializedComponent transform;
            transform.typeName = "Transform";
            transform.properties["position"] = XMFLOAT3{0.0f, 3.0f, 0.0f};
            transform.properties["rotation"] = XMFLOAT4{0.0f, 0.0f, 0.0f, 1.0f};
            transform.properties["scale"] = XMFLOAT3{1.0f, 1.0f, 1.0f};
            light.AddComponent(transform);

            SerializedComponent lightComp;
            lightComp.typeName = "Light";
            lightComp.properties["color"] = XMFLOAT3{1.0f, 0.95f, 0.8f};
            lightComp.properties["intensity"] = 1.0f;
            lightComp.properties["range"] = 10.0f;
            lightComp.properties["castShadows"] = true;
            light.AddComponent(lightComp);

            m_prefabs["Point Light"] = std::move(light);
        }

        // Weapon Pickup prefab
        {
            PrefabAsset pickup("Weapon Pickup");
            SerializedComponent transform;
            transform.typeName = "Transform";
            transform.properties["position"] = XMFLOAT3{0.0f, 0.5f, 0.0f};
            transform.properties["rotation"] = XMFLOAT4{0.0f, 0.0f, 0.0f, 1.0f};
            transform.properties["scale"] = XMFLOAT3{0.5f, 0.5f, 0.5f};
            pickup.AddComponent(transform);

            SerializedComponent mesh;
            mesh.typeName = "MeshRenderer";
            mesh.properties["meshPath"] = std::string("Models/WeaponPickup.fbx");
            mesh.properties["castShadows"] = true;
            pickup.AddComponent(mesh);

            SerializedComponent collider;
            collider.typeName = "Collider";
            collider.properties["type"] = std::string("Box");
            collider.properties["isTrigger"] = true;
            pickup.AddComponent(collider);

            m_prefabs["Weapon Pickup"] = std::move(pickup);
        }

        // Crate prefab
        {
            PrefabAsset crate("Crate");
            SerializedComponent transform;
            transform.typeName = "Transform";
            transform.properties["position"] = XMFLOAT3{0.0f, 0.5f, 0.0f};
            transform.properties["rotation"] = XMFLOAT4{0.0f, 0.0f, 0.0f, 1.0f};
            transform.properties["scale"] = XMFLOAT3{1.0f, 1.0f, 1.0f};
            crate.AddComponent(transform);

            SerializedComponent mesh;
            mesh.typeName = "MeshRenderer";
            mesh.properties["meshPath"] = std::string("Models/Crate.fbx");
            mesh.properties["castShadows"] = true;
            crate.AddComponent(mesh);

            SerializedComponent rigidbody;
            rigidbody.typeName = "RigidBody";
            rigidbody.properties["mass"] = 25.0f;
            rigidbody.properties["isKinematic"] = false;
            crate.AddComponent(rigidbody);

            SerializedComponent collider;
            collider.typeName = "Collider";
            collider.properties["type"] = std::string("Box");
            collider.properties["isTrigger"] = false;
            crate.AddComponent(collider);

            m_prefabs["Crate"] = std::move(crate);
        }
    }

} // namespace SparkEditor
