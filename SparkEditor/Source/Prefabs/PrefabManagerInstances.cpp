/**
 * @file PrefabManagerInstances.cpp
 * @brief Prefab instance tracking, template application, and overrides
 */

#include "PrefabManager.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <map>
#include <utility>

namespace SparkEditor
{

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

} // namespace SparkEditor
