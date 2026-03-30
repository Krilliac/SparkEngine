/**
 * @file PrefabAsset.cpp
 * @brief Implementation of the PrefabAsset class
 * @author Spark Engine Team
 * @date 2025
 */

#include "PrefabAsset.h"
#include "Utils/LogMacros.h"
#include "Utils/Validate.h"
#include <algorithm>
#include <fstream>

namespace SparkEditor
{

    uint64_t PrefabAsset::s_nextId = 1;

    PrefabAsset::PrefabAsset(const std::string& name) : m_name(name), m_id(s_nextId++) {}

    void PrefabAsset::AddComponent(const SerializedComponent& component)
    {
        SPARK_VALIDATE_NOT_EMPTY(Spark::LogCategory::Editor, component.typeName);
        // Replace if component of same type already exists
        auto it = std::find_if(m_components.begin(), m_components.end(),
                               [&](const SerializedComponent& c) { return c.typeName == component.typeName; });

        if (it != m_components.end())
        {
            *it = component;
        }
        else
        {
            m_components.push_back(component);
        }
        m_isModified = true;
    }

    bool PrefabAsset::RemoveComponent(const std::string& typeName)
    {
        SPARK_VALIDATE_RET(Spark::LogCategory::Editor, !typeName.empty(), false);
        auto it = std::find_if(m_components.begin(), m_components.end(),
                               [&](const SerializedComponent& c) { return c.typeName == typeName; });

        if (it != m_components.end())
        {
            m_components.erase(it);
            m_isModified = true;
            return true;
        }
        return false;
    }

    bool PrefabAsset::HasComponent(const std::string& typeName) const
    {
        return std::any_of(m_components.begin(), m_components.end(),
                           [&](const SerializedComponent& c) { return c.typeName == typeName; });
    }

    const SerializedComponent* PrefabAsset::GetComponent(const std::string& typeName) const
    {
        auto it = std::find_if(m_components.begin(), m_components.end(),
                               [&](const SerializedComponent& c) { return c.typeName == typeName; });

        return (it != m_components.end()) ? &(*it) : nullptr;
    }

    bool PrefabAsset::Save(const std::string& path)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_VALIDATE_RET(Spark::LogCategory::Editor, !path.empty(), false);
        // Simple text-based serialization for the prefab
        std::ofstream file(path);
        if (!file.is_open())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to save prefab '%s' to: %s", m_name.c_str(),
                            path.c_str());
            return false;
        }
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Saving prefab '%s' with %zu components to: %s", m_name.c_str(),
                       m_components.size(), path.c_str());

        file << "SPARKPREFAB 1\n";
        file << "name " << m_name << "\n";
        file << "components " << m_components.size() << "\n";

        for (const auto& comp : m_components)
        {
            file << "component " << comp.typeName << "\n";
            file << "properties " << comp.properties.size() << "\n";
            for (const auto& [name, value] : comp.properties)
            {
                file << "  " << name << " ";
                std::visit(
                    [&file](auto&& val)
                    {
                        using T = std::decay_t<decltype(val)>;
                        if constexpr (std::is_same_v<T, bool>)
                        {
                            file << "bool " << (val ? "true" : "false");
                        }
                        else if constexpr (std::is_same_v<T, int>)
                        {
                            file << "int " << val;
                        }
                        else if constexpr (std::is_same_v<T, float>)
                        {
                            file << "float " << val;
                        }
                        else if constexpr (std::is_same_v<T, double>)
                        {
                            file << "double " << val;
                        }
                        else if constexpr (std::is_same_v<T, std::string>)
                        {
                            file << "string " << val;
                        }
                        else if constexpr (std::is_same_v<T, XMFLOAT3>)
                        {
                            file << "float3 " << val.x << " " << val.y << " " << val.z;
                        }
                        else if constexpr (std::is_same_v<T, XMFLOAT4>)
                        {
                            file << "float4 " << val.x << " " << val.y << " " << val.z << " " << val.w;
                        }
                    },
                    value);
                file << "\n";
            }
        }

        m_filePath = path;
        m_isModified = false;
        return true;
    }

    PrefabAsset PrefabAsset::Load(const std::string& path)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        PrefabAsset prefab;
        std::ifstream file(path);
        if (!file.is_open())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to load prefab from: %s", path.c_str());
            return prefab;
        }
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Loading prefab from: %s", path.c_str());

        std::string line;
        // Read header
        std::getline(file, line);
        if (!line.contains("SPARKPREFAB"))
        {
            return prefab;
        }

        // Read name
        std::string token;
        file >> token; // "name"
        std::getline(file, line);
        if (!line.empty() && line[0] == ' ')
        {
            line = line.substr(1);
        }
        prefab.m_name = line;

        // Read component count
        int componentCount = 0;
        file >> token >> componentCount;

        for (int i = 0; i < componentCount; ++i)
        {
            SerializedComponent comp;
            file >> token >> comp.typeName; // "component TypeName"

            int propCount = 0;
            file >> token >> propCount; // "properties N"

            for (int j = 0; j < propCount; ++j)
            {
                std::string propName;
                std::string propType;
                file >> propName >> propType;

                if (propType == "bool")
                {
                    std::string val;
                    file >> val;
                    comp.properties[propName] = (val == "true");
                }
                else if (propType == "int")
                {
                    int val = 0;
                    file >> val;
                    comp.properties[propName] = val;
                }
                else if (propType == "float")
                {
                    float val = 0.0f;
                    file >> val;
                    comp.properties[propName] = val;
                }
                else if (propType == "double")
                {
                    double val = 0.0;
                    file >> val;
                    comp.properties[propName] = val;
                }
                else if (propType == "string")
                {
                    std::string val;
                    std::getline(file, val);
                    if (!val.empty() && val[0] == ' ')
                    {
                        val = val.substr(1);
                    }
                    comp.properties[propName] = val;
                }
                else if (propType == "float3")
                {
                    XMFLOAT3 val;
                    file >> val.x >> val.y >> val.z;
                    comp.properties[propName] = val;
                }
                else if (propType == "float4")
                {
                    XMFLOAT4 val;
                    file >> val.x >> val.y >> val.z >> val.w;
                    comp.properties[propName] = val;
                }
            }

            prefab.m_components.push_back(std::move(comp));
        }

        prefab.m_filePath = path;
        prefab.m_isModified = false;
        return prefab;
    }

} // namespace SparkEditor
