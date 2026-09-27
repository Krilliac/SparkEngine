/**
 * @file LauncherProcess.h
 * @brief Validated launch requests for SparkLauncher project actions.
 */

#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace SparkLauncher
{
    enum class LaunchTarget
    {
        Editor,
        Game,
        DedicatedServer,
        ServiceTopology
    };

    /**
     * @brief Largest spark.modules.json the Game target will read (1 MiB).
     *
     * A real manifest lists a handful of modules and is a few hundred bytes; the
     * cap keeps a hostile project's manifest from being read into memory whole.
     */
    inline constexpr std::size_t kMaxModuleManifestBytes = std::size_t{1024} * 1024;

    struct LaunchRequest
    {
        std::filesystem::path executable;
        std::filesystem::path workingDirectory;
        std::vector<std::string> arguments; ///< UTF-8 text; LaunchDetached refuses anything else.
    };

    /**
     * @brief UTF-8 spelling of @p path with generic ('/') separators.
     *
     * This is the encoding of every LaunchRequest argument. Never use path::string()
     * for them: on Windows it yields active-code-page bytes (or throws for characters
     * outside that code page).
     */
    [[nodiscard]] std::string PathToUtf8(const std::filesystem::path& path);

    /**
     * @brief Decode UTF-8 text (ImGui input, ProjectManager's recent-project paths) into a path.
     *
     * Never throws for bad text. Fails closed on an embedded NUL on every platform,
     * and on invalid UTF-8 on Windows, where the text must become UTF-16.
     */
    [[nodiscard]] std::expected<std::filesystem::path, std::string> PathFromUtf8(std::string_view text);

    /** Build and validate the executable, project, and target-specific inputs. */
    [[nodiscard]] std::expected<LaunchRequest, std::string> BuildLaunchRequest(
        const std::filesystem::path& binaryDirectory, const std::filesystem::path& projectFile, LaunchTarget target);

    /**
     * Start an independent child and return after the operating system accepts it.
     * An argument that is not valid UTF-8 (Windows) or holds an embedded NUL is
     * returned as an error before anything is started; bad text never throws.
     */
    [[nodiscard]] std::expected<void, std::string> LaunchDetached(const LaunchRequest& request);

    [[nodiscard]] const char* LaunchTargetName(LaunchTarget target);
} // namespace SparkLauncher
