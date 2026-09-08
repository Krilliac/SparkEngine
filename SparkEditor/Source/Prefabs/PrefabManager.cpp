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
