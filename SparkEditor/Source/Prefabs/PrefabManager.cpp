/**
 * @file PrefabManager.cpp
 * @brief Implementation of the PrefabManager
 * @author Spark Engine Team
 * @date 2025
 */

#include "PrefabManager.h"
#include "Utils/Validate.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>

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
        // Create a new prefab and populate it with the entity's components
        PrefabAsset prefab(prefabName);

        // In a full implementation, this would query the ECS for the entity's components
        // and serialize them into the prefab. For now, create a basic template.
        SerializedComponent transform;
        transform.typeName = "Transform";
        transform.properties["position"] = XMFLOAT3{0.0f, 0.0f, 0.0f};
        transform.properties["rotation"] = XMFLOAT4{0.0f, 0.0f, 0.0f, 1.0f};
        transform.properties["scale"] = XMFLOAT3{1.0f, 1.0f, 1.0f};
        prefab.AddComponent(transform);

        m_prefabs[prefabName] = std::move(prefab);
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
            return 0;
        }

        // In a full implementation, this would create an entity in the ECS
        // and apply the prefab's component data to it.
        static uint64_t nextInstanceId = 1000;
        uint64_t entityId = nextInstanceId++;

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

    void PrefabManager::ApplyPrefabToInstances(const std::string& prefabName)
    {
        auto prefab = GetPrefab(prefabName);
        if (!prefab)
        {
            return;
        }

        const auto& templateComponents = prefab->GetComponents();
        int updatedCount = 0;

        for (auto& instance : m_instances)
        {
            if (instance.sourcePrefabName != prefabName)
            {
                continue;
            }

            // Build a set of overridden property keys for fast lookup
            std::set<std::string> overriddenKeys;
            for (const auto& override : instance.overrides)
            {
                overriddenKeys.insert(override.componentType + "." + override.propertyName);
            }

            // Sync non-overridden properties from the template.
            // When the engine runtime is connected, this would call
            // ECS::SetComponentProperty for each synced value.
            for (const auto& comp : templateComponents)
            {
                for (const auto& [propName, propValue] : comp.properties)
                {
                    std::string key = comp.typeName + "." + propName;
                    if (overriddenKeys.find(key) == overriddenKeys.end())
                    {
                        // Property is not overridden — inherits from template
                    }
                }
            }

            updatedCount++;
        }

        std::cout << "Applied prefab '" << prefabName << "' to " << updatedCount << " instance(s)\n";
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
