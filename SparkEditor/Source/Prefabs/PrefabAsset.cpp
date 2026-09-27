/**
 * @file PrefabAsset.cpp
 * @brief Implementation of the PrefabAsset class
 * @author Spark Engine Team
 * @date 2025
 *
 * The `.sparkprefab` grammar lives in PrefabTextFormat.cpp; this file owns the file I/O, the
 * durable save and the `.bak` recovery.
 */

#include "PrefabAsset.h"
#include "PrefabTextFormat.h"
#include "Engine/SaveSystem/SaveFileDurability.h"
#include "Utils/LogMacros.h"
#include "Utils/Validate.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace SparkEditor
{
    namespace
    {
        std::filesystem::path PathFromUtf8(const std::string& path)
        {
            return std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size());
        }

        PrefabTextFormat::ParseResult ReadPrefabFile(const std::filesystem::path& path,
                                                     PrefabTextFormat::ParsedPrefab& out, std::string& reason)
        {
            std::error_code existsError;
            if (!std::filesystem::is_regular_file(path, existsError))
            {
                reason = "the file does not exist";
                return PrefabTextFormat::ParseResult::Rejected;
            }
            std::ifstream input(path, std::ios::binary);
            if (!input.is_open())
            {
                reason = "the file could not be opened";
                return PrefabTextFormat::ParseResult::Rejected;
            }
            const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            if (input.bad())
            {
                reason = "the file could not be read";
                return PrefabTextFormat::ParseResult::Rejected;
            }
            return PrefabTextFormat::Parse(text, out, reason);
        }
    } // namespace

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

        // Refuse what TryLoad would reject, so a save never replaces a loadable prefab with one
        // that cannot be read back. Version 2 quotes every name, so only empty names remain.
        if (m_name.empty())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Refusing to save prefab to '%s': the prefab name is empty",
                            path.c_str());
            return false;
        }
        for (const auto& comp : m_components)
        {
            const bool emptyPropertyName = std::any_of(comp.properties.begin(), comp.properties.end(),
                                                       [](const auto& property) { return property.first.empty(); });
            if (comp.typeName.empty() || emptyPropertyName)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Editor,
                                "Refusing to save prefab '%s': component type '%s' or one of its property names is "
                                "empty",
                                m_name.c_str(), comp.typeName.c_str());
                return false;
            }
        }

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Saving prefab '%s' with %zu components to: %s", m_name.c_str(),
                       m_components.size(), path.c_str());
        // Refreshing the .bak from a primary TryLoad rejected would replace the only good copy
        // with the damaged file before the rename that repairs the primary can fail.
        const bool retainBackup = !(m_recoveredFromBackup && path == m_filePath);
        std::error_code writeError;
        if (!Spark::SaveFileDurability::WriteFileAtomically(
                PathFromUtf8(path), PrefabTextFormat::Render(m_name, m_components), retainBackup, writeError))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor,
                            "Failed to save prefab '%s' to '%s': %s. The previous file is unchanged", m_name.c_str(),
                            path.c_str(), writeError.message().c_str());
            return false;
        }

        m_filePath = path;
        m_isModified = false;
        m_recoveredFromBackup = false;
        return true;
    }

    bool PrefabAsset::TryLoad(const std::string& path, PrefabAsset& out, std::string& error)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        error.clear();
        const std::filesystem::path primary = PathFromUtf8(path);

        PrefabTextFormat::ParsedPrefab parsed;
        std::string primaryReason;
        const PrefabTextFormat::ParseResult primaryResult = ReadPrefabFile(primary, parsed, primaryReason);
        if (primaryResult == PrefabTextFormat::ParseResult::NewerVersion)
        {
            error = "Prefab '" + path + "': " + primaryReason + ".";
            return false;
        }

        if (primaryResult != PrefabTextFormat::ParseResult::Ok)
        {
            const std::string backupName = path + ".bak";
            std::string backupReason;
            parsed = PrefabTextFormat::ParsedPrefab{};
            if (ReadPrefabFile(Spark::SaveFileDurability::BackupPathFor(primary), parsed, backupReason) !=
                PrefabTextFormat::ParseResult::Ok)
            {
                error = "Prefab '" + path + "' was rejected: " + primaryReason + ". Previous-good backup '" +
                        backupName + "' was not usable: " + backupReason + ".";
                return false;
            }
            error = "Prefab '" + path + "' was rejected: " + primaryReason + ". Loaded the previous-good backup '" +
                    backupName + "' instead.";
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "%s", error.c_str());
        }

        PrefabAsset loaded;
        loaded.m_name = std::move(parsed.name);
        loaded.m_components = std::move(parsed.components);
        // A recovered prefab keeps the primary path, so the next save repairs the primary.
        loaded.m_filePath = path;
        loaded.m_isModified = false;
        loaded.m_recoveredFromBackup = primaryResult != PrefabTextFormat::ParseResult::Ok;
        out = std::move(loaded);
        return true;
    }

} // namespace SparkEditor
