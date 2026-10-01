/**
 * @file LauncherTemplates.cpp
 * @brief SparkLauncher project templates: the template.json reader.
 *
 * LauncherApp::LoadTemplates lists every template directory and reads its template.json here.
 * Keeping the reader out of LauncherApp.cpp lets the SEC-120 fuzz target
 * (FuzzerTests/FuzzLauncherTemplate.cpp) link it without ImGui or the editor's ProjectManager.
 */

#include "LauncherTemplates.h"

#include "LauncherProcess.h"

#include <string_view>
#include <system_error>

namespace SparkLauncher
{
    namespace
    {
        // Very small JSON string-field extractor — matches ProjectManager.cpp's approach.
        std::string ExtractJsonString(std::string_view json, std::string_view key)
        {
            const std::string search = "\"" + std::string(key) + "\"";
            size_t pos = json.find(search);
            if (pos == std::string_view::npos)
            {
                return {};
            }
            pos = json.find(':', pos);
            if (pos == std::string_view::npos)
            {
                return {};
            }
            pos = json.find('"', pos + 1);
            if (pos == std::string_view::npos)
            {
                return {};
            }
            const size_t end = json.find('"', pos + 1);
            if (end == std::string_view::npos)
            {
                return {};
            }
            return std::string(json.substr(pos + 1, end - pos - 1));
        }
    } // namespace

    std::optional<TemplateEntry> ReadTemplateEntry(const std::filesystem::path& templateDirectory)
    {
        const std::filesystem::path manifest = templateDirectory / "template.json";
        bool opened = false;
        bool missing = false;
        const auto json = ReadBoundedRegularFile(manifest, kMaxTemplateManifestBytes, &opened, &missing);
        if (!opened && missing)
        {
            return std::nullopt;
        }

        // The read is decided on the opened handle: an ifstream opened by name blocked forever
        // on a FIFO named template.json, and read any file whole. A refused file reads as
        // empty text, as an unreadable one always did, so the template is still listed.
        const std::string_view text = json ? std::string_view(*json) : std::string_view();

        TemplateEntry entry;
        entry.directoryName = PathToUtf8(templateDirectory.filename());
        entry.displayName = ExtractJsonString(text, "name");
        if (entry.displayName.empty())
        {
            entry.displayName = entry.directoryName;
        }
        entry.description = ExtractJsonString(text, "description");
        entry.genre = ExtractJsonString(text, "genre");
        entry.gameModule = ExtractJsonString(text, "gameModule");
        return entry;
    }
} // namespace SparkLauncher
