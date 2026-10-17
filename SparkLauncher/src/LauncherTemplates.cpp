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
#include "Utils/JsonUtils.h"

#include <string_view>
#include <system_error>

namespace SparkLauncher
{
    namespace
    {
        /// A top-level string member of the manifest object, or empty when absent or not a string.
        std::string TopLevelString(const Spark::Json::Value& document, const std::string& key)
        {
            const Spark::Json::Value& value = document[key];
            return value.IsString() ? value.AsString() : std::string{};
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

        // Parse the manifest instead of scanning for the first "key": the old substring scan
        // returned a nested member ("meta": {"name": ...}) as the template's name, never
        // unescaped strings, and read keys out of malformed text. Text that is not one strict
        // JSON object reads like a refused file: the template is listed under its directory.
        Spark::Json::Value document;
        if (Spark::Json::ParseStrict(text, &document) && document.IsObject())
        {
            entry.displayName = TopLevelString(document, "name");
            entry.description = TopLevelString(document, "description");
            entry.genre = TopLevelString(document, "genre");
            entry.gameModule = TopLevelString(document, "gameModule");
        }
        if (entry.displayName.empty())
        {
            entry.displayName = entry.directoryName;
        }
        return entry;
    }
} // namespace SparkLauncher
