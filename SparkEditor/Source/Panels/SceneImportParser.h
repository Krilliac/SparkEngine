/**
 * @file SceneImportParser.h
 * @brief Reader for the game INI .scene dialect the Scene Import panel imports
 *
 * The dialect is the one SceneManager::LoadCustom (engine) and TFWorldCollision::ParseScene
 * (game module) read: '#'/';' comments, [Section] headers, key=value pairs, and
 * comma-separated float triples parsed with strtof. SceneImportPanel::ParseSceneFile reads
 * the file and hands its text here; the panel then resolves model paths on disk. The reader
 * lives in its own translation unit so the SEC-120 fuzz target links it without the panel,
 * ImGui or the editor UI.
 *
 * Thread affinity: any thread; no shared state.
 * Ownership: @p text is borrowed; the document is returned by value.
 * Allocation: one record per section and one string per field, bounded by the caller's
 * file-size limit.
 */

#pragma once

#include "../Gizmos/SceneEditTools.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace SparkEditor
{

    /// @brief Largest .scene file the Scene Import panel reads (shipped scenes are under 64 KB).
    inline constexpr std::uint64_t kMaxGameSceneBytes = std::uint64_t{4} * 1024 * 1024;

    /// @brief Everything the INI text says, before any filesystem lookup.
    struct GameSceneIniDocument
    {
        std::string sceneName;                                  ///< [Scene] name= (may be empty)
        std::vector<SceneEditTools::SceneObjectRecord> objects; ///< importable objects, file order
        std::vector<std::string> skippedTypes;                  ///< one entry per skipped node ("spawnpoint", ...)
    };

    /**
     * @brief Parse game INI .scene text.
     *
     * Only nodes of type cube/model become objects; every other non-[Scene] section is
     * recorded as a skip. In a float triple, as in the game's reader, a malformed component
     * reads as 0 and the rest keep their prior values; a component that is not finite keeps
     * its prior value too, so no NaN or infinity reaches an imported Transform.
     */
    GameSceneIniDocument ParseGameSceneIni(std::string_view text);

} // namespace SparkEditor
