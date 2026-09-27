#include "GamePackager.h"

#include "Utils/FileUtils.h"

#include <cstdio>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace Spark::Build
{
    namespace
    {
        namespace fs = std::filesystem;

        std::string_view PlatformName(PackagePlatform platform)
        {
            switch (platform)
            {
            case PackagePlatform::WindowsX64:
                return "Windows";
            case PackagePlatform::LinuxX64:
                return "Linux";
            case PackagePlatform::MacOSX64:
            case PackagePlatform::MacOSARM64:
                return "macOS";
            }
            return "Unknown";
        }

        std::string_view ModuleExtension(PackagePlatform platform)
        {
            switch (platform)
            {
            case PackagePlatform::WindowsX64:
                return ".dll";
            case PackagePlatform::LinuxX64:
                return ".so";
            case PackagePlatform::MacOSX64:
            case PackagePlatform::MacOSARM64:
                return ".dylib";
            }
            return {};
        }

        std::string_view ExecutableExtension(PackagePlatform platform)
        {
            return platform == PackagePlatform::WindowsX64 ? ".exe" : std::string_view{};
        }

        /// UTF-8 text for diagnostic messages only. path::string() goes through the
        /// Windows ANSI code page and throws for a name it cannot spell. Never feed
        /// the result into a packaging decision or the manifest: the placeholder
        /// matches no filter, so a policy check on it fails open.
        std::string DisplayName(const fs::path& path)
        {
            return FileUtils::TryPathToUtf8(path).value_or("<unrepresentable name>");
        }

        bool IsLegacyBinary(const fs::path& path, PackagePlatform platform, bool debugBuild)
        {
            const std::optional<std::string> extension = FileUtils::TryPathToUtf8(path.extension());
            if (!extension)
                return false;
            const auto moduleExtension = ModuleExtension(platform);
            const auto executableExtension = ExecutableExtension(platform);
            return *extension == moduleExtension || *extension == executableExtension ||
                   (debugBuild && *extension == ".pdb");
        }

        bool IsCountedLegacyBinary(const fs::path& path, PackagePlatform platform)
        {
            const std::optional<std::string> extension = FileUtils::TryPathToUtf8(path.extension());
            return extension &&
                   (*extension == ModuleExtension(platform) || *extension == ExecutableExtension(platform));
        }

    } // namespace

    LegacyPackageResult GamePackager::PackageLegacy(const LegacyPackageConfig& config)
    {
        LegacyPackageResult result;
        const auto publish = [this](const LegacyPackageResult& legacy, bool countPackage)
        {
            m_lastResult = {};
            m_lastResult.success = legacy.success;
            m_lastResult.outputPath = legacy.outputPath;
            m_lastResult.filesCopied = legacy.filesCopied;
            m_lastResult.totalSizeBytes = static_cast<uint64_t>(legacy.totalSizeMB * 1024.0 * 1024.0);
            m_lastResult.warnings = legacy.warnings;
            if (!legacy.errors.empty())
                m_lastResult.errorMessage = legacy.errors.front();
            if (countPackage)
                ++m_packageCount;
            return legacy;
        };
        if (!m_initialized)
        {
            result.errors.emplace_back("GamePackager has not been initialized");
            return publish(result, false);
        }

        if (config.outputDirectory.empty())
            result.errors.emplace_back("Output directory must not be empty");
        if (config.projectName.empty())
            result.errors.emplace_back("Project name must not be empty");
        if (config.projectName.find_first_of("/\\:*?\"<>|") != std::string::npos)
            result.errors.emplace_back("Project name contains invalid filesystem characters");
        if (!result.errors.empty())
            return publish(result, false);

        const auto configName = config.debugBuild ? "Debug" : "Release";
        const fs::path outputRoot =
            fs::absolute(FileUtils::PathFromUtf8(config.outputDirectory) /
                         FileUtils::PathFromUtf8(
                             std::format("{}_{}_{}", config.projectName, PlatformName(config.platform), configName)));
        const fs::path binDestination = outputRoot / "Bin";
        const fs::path assetsDestination = outputRoot / "Assets";
        const fs::path configDestination = outputRoot / "Config";

        std::error_code ec;
        fs::create_directories(binDestination, ec);
        if (ec)
        {
            result.errors.push_back(std::format("Failed to create output directory: {}", ec.message()));
            return publish(result, false);
        }
        fs::create_directories(assetsDestination, ec);
        fs::create_directories(configDestination, ec);

        // Legacy behavior searches the configuration-specific build directory,
        // then falls back to the generic build directory.
        fs::path binSource = fs::path("build") / configName;
        if (!fs::is_directory(binSource, ec))
            binSource = "build";
        if (!fs::is_directory(binSource, ec))
        {
            result.errors.push_back(std::format("Binary source directory '{}' not found", binSource.string()));
            return publish(result, false);
        }

        const fs::path assetsSource = "Assets";
        if (!fs::is_directory(assetsSource, ec))
        {
            result.warnings.emplace_back("Assets directory not found; skipping asset cooking");
        }
        else
        {
            for (const auto& entry : fs::recursive_directory_iterator(assetsSource, ec))
            {
                if (!entry.is_regular_file(ec))
                    continue;
                const fs::path relativePath = fs::relative(entry.path(), assetsSource, ec);
                // The editor filter must see the real name. A Windows name that is not
                // well-formed UTF-16 has no UTF-8 spelling to filter on, so the package
                // fails instead of shipping an entry the filter never examined.
                if (ec || relativePath.empty())
                {
                    result.errors.push_back(std::format("Failed to resolve asset path: {}", ec.message()));
                    ec.clear();
                    continue;
                }
                const std::optional<std::string> relativeUtf8 = FileUtils::TryPathToUtf8(relativePath);
                if (!relativeUtf8)
                {
                    result.errors.emplace_back("Asset name is not valid Unicode; refusing to package it unfiltered");
                    continue;
                }
                const std::string& relative = *relativeUtf8;
                if (!config.includeEditor && relative.starts_with("Editor"))
                    continue;

                const fs::path destination = assetsDestination / relativePath;
                fs::create_directories(destination.parent_path(), ec);
                fs::copy_file(entry.path(), destination, fs::copy_options::overwrite_existing, ec);
                if (ec)
                {
                    // A missing asset makes the package incomplete; report it as
                    // an error so the aggregate failure path below rejects it.
                    result.errors.push_back(std::format("Failed to copy asset '{}': {}", relative, ec.message()));
                    ec.clear();
                }
                else
                {
                    ++result.assetCount;
                    ++result.filesCopied;
                }
            }
        }

        // Preserve the historical Core ordering: assets are staged before binary
        // validation, so a binary copy failure still reports the assets copied.
        for (const auto& entry : fs::directory_iterator(binSource, ec))
        {
            if (!entry.is_regular_file(ec) || !IsLegacyBinary(entry.path(), config.platform, config.debugBuild))
                continue;

            // As for assets: filter on the real name, or fail rather than copy an
            // unexamined binary into a shipping package.
            const std::optional<std::string> filenameUtf8 = FileUtils::TryPathToUtf8(entry.path().filename());
            if (!filenameUtf8)
            {
                result.errors.emplace_back("Binary name is not valid Unicode; refusing to package it unfiltered");
                continue;
            }
            const std::string& filename = *filenameUtf8;
            if (!config.includeEditor && filename.find("Editor") != std::string::npos)
                continue;

            fs::copy_file(entry.path(), binDestination / entry.path().filename(), fs::copy_options::overwrite_existing,
                          ec);
            if (ec)
            {
                // Collect every copy failure. The legacy implementation returned
                // the complete error vector rather than stopping at the first
                // blocked destination.
                result.errors.push_back(std::format("Failed to copy binary '{}': {}", filename, ec.message()));
                ec.clear();
                continue;
            }
            if (IsCountedLegacyBinary(entry.path(), config.platform))
                ++result.dllCount;
            ++result.filesCopied;
        }

        if (!result.errors.empty())
        {
            // A failed legacy package has no publishable output, but retains the
            // asset/binary counts gathered before the failure for diagnostics.
            result.outputPath.clear();
            result.totalSizeMB = 0.0f;
            result.success = false;
            return publish(result, false);
        }

        if (config.stripDebugSymbols && !config.debugBuild)
        {
            for (const auto& entry : fs::directory_iterator(binDestination, ec))
            {
                if (!entry.is_regular_file(ec) || entry.path().extension() != ".pdb")
                    continue;
                fs::remove(entry.path(), ec);
                if (ec)
                {
                    result.warnings.push_back(
                        std::format("Could not remove debug file '{}'", DisplayName(entry.path().filename())));
                    ec.clear();
                }
            }
        }

        // The output directory is reused between runs, so it can hold files this run
        // did not copy. The manifest must name every one of them truthfully; a name
        // with no UTF-8 spelling would be recorded as a placeholder, so fail instead.
        const std::optional<std::string> outputRootUtf8 = FileUtils::TryPathToUtf8(outputRoot);
        if (!outputRootUtf8)
            result.errors.emplace_back("Output directory name is not valid Unicode");
        struct ManifestLine
        {
            std::string relativePath;
            unsigned long long sizeBytes = 0;
        };
        std::vector<ManifestLine> manifestLines;
        for (const auto& entry : fs::recursive_directory_iterator(outputRoot, ec))
        {
            if (!entry.is_regular_file(ec))
                continue;
            const auto relative = fs::relative(entry.path(), outputRoot, ec);
            std::optional<std::string> relativeUtf8 = ec ? std::nullopt : FileUtils::TryPathToUtf8(relative);
            if (!relativeUtf8)
            {
                result.errors.emplace_back("Package output contains a file whose name is not valid Unicode");
                ec.clear();
                continue;
            }
            // A manifest left by an earlier run is rewritten below; it does not list itself.
            if (*relativeUtf8 == "manifest.txt")
                continue;
            manifestLines.push_back({std::move(*relativeUtf8), static_cast<unsigned long long>(entry.file_size(ec))});
        }
        if (!result.errors.empty())
        {
            result.outputPath.clear();
            result.totalSizeMB = 0.0f;
            result.success = false;
            return publish(result, false);
        }

        const fs::path manifestPath = outputRoot / "manifest.txt";
#ifdef _WIN32
        FILE* manifest = _wfopen(manifestPath.c_str(), L"w");
#else
        FILE* manifest = std::fopen(manifestPath.c_str(), "w");
#endif
        if (manifest)
        {
            const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::fprintf(manifest, "# SparkEngine Package Manifest\n");
            std::fprintf(manifest, "# Project: %s\n", config.projectName.c_str());
            std::fprintf(manifest, "# Platform: %.*s\n", static_cast<int>(PlatformName(config.platform).size()),
                         PlatformName(config.platform).data());
            std::fprintf(manifest, "# Config: %s\n", configName);
            std::fprintf(manifest, "# Timestamp: %lld\n", static_cast<long long>(now));
            std::fprintf(manifest, "# Assets: %u\n", result.assetCount);
            std::fprintf(manifest, "# DLLs: %u\n\n", result.dllCount);
            for (const ManifestLine& line : manifestLines)
                std::fprintf(manifest, "%s %llu\n", line.relativePath.c_str(), line.sizeBytes);
            std::fclose(manifest);
        }
        else
        {
            result.warnings.emplace_back("Failed to write package manifest");
        }

        if (config.compressAssets)
        {
            uint32_t fileCount = 0;
            if (!fs::is_directory(assetsDestination, ec) || fs::is_empty(assetsDestination, ec))
            {
                result.warnings.emplace_back("No assets to compress");
            }
            else
            {
                for (const auto& entry : fs::recursive_directory_iterator(assetsDestination, ec))
                {
                    if (entry.is_regular_file(ec))
                        ++fileCount;
                }
                if (fileCount == 0)
                    result.warnings.emplace_back("Assets directory is empty; nothing to compress");
            }
        }

        uint64_t totalBytes = 0;
        for (const auto& entry : fs::recursive_directory_iterator(outputRoot, ec))
        {
            if (entry.is_regular_file(ec))
                totalBytes += entry.file_size(ec);
        }
        result.outputPath = *outputRootUtf8;
        result.totalSizeMB = static_cast<float>(totalBytes) / (1024.0f * 1024.0f);
        result.success = result.errors.empty();
        return publish(result, result.success);
    }

} // namespace Spark::Build
