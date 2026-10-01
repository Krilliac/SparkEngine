/**
 * @file LauncherTemplates.h
 * @brief SparkLauncher project templates: the template.json reader.
 */

#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

namespace SparkLauncher
{
    struct TemplateEntry
    {
        std::string directoryName;
        std::string displayName;
        std::string description;
        std::string genre;
        std::string gameModule;
    };

    /**
     * @brief Largest template.json the launcher reads (64 KiB).
     *
     * A shipped template.json is a few hundred bytes; the templates directory can come from
     * the working directory of a development build, so the cap keeps a planted file from
     * being read into memory whole.
     */
    inline constexpr std::size_t kMaxTemplateManifestBytes = std::size_t{64} * 1024;

    /**
     * @brief Read @p templateDirectory / "template.json" into a TemplateEntry.
     *
     * The file is read as bytes through ReadBoundedRegularFile (LauncherProcess.h), so a FIFO,
     * a directory or anything else that is not a regular file of at most
     * kMaxTemplateManifestBytes is refused without blocking, and then reads as empty text.
     * Each field is the first quoted string after the first `"<key>"` and the ':' that follows
     * it, without escape processing (name, description, genre, gameModule); an empty name
     * falls back to the directory name.
     *
     * Thread affinity: none (it only reads the file).
     * Allocation: the file's bytes and the extracted strings.
     *
     * @return std::nullopt when the directory holds no template.json.
     */
    [[nodiscard]] std::optional<TemplateEntry> ReadTemplateEntry(const std::filesystem::path& templateDirectory);
} // namespace SparkLauncher
