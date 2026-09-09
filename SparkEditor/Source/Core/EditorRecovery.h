/**
 * @file EditorRecovery.h
 * @brief Durable, UI-thread-owned recovery snapshots for SparkEditor.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace SparkEditor
{
    inline constexpr uint32_t kEditorRecoverySchemaVersion = 1;
    inline constexpr size_t kEditorRecoveryMaxBytes = 16u * 1024u * 1024u;

    struct EditorRecoverySnapshot
    {
        uint32_t schemaVersion = kEditorRecoverySchemaVersion;
        std::string projectIdentity;
        std::string projectRelativeScene;
        std::string sceneDisplayName;
        std::string serializedWorld;
        std::string layoutIniPath;
        std::vector<std::string> recentOperations;
        uint64_t dirtySequence = 0;
        int64_t capturedUnixMilliseconds = 0;
    };

    enum class EditorRecoveryLoadState : uint8_t
    {
        None,
        Primary,
        Backup,
        Invalid
    };

    struct EditorRecoveryLoadResult
    {
        EditorRecoveryLoadState state = EditorRecoveryLoadState::None;
        std::optional<EditorRecoverySnapshot> snapshot;
        std::string error;
    };

    class EditorRecoveryStore
    {
      public:
        explicit EditorRecoveryStore(std::filesystem::path directory);

        bool Save(const EditorRecoverySnapshot& snapshot, std::string& error);
        bool Clear(std::string& error) const;
        EditorRecoveryLoadResult LoadForProject(std::string_view projectIdentity) const;

      private:
        std::filesystem::path m_directory;
    };
} // namespace SparkEditor
