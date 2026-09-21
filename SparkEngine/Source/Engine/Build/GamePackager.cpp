#include "GamePackager.h"

#include <cstdio>
#include <format>

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

        bool IsLegacyBinary(const fs::path& path, PackagePlatform platform, bool debugBuild)
        {
            const auto extension = path.extension().string();
            const auto moduleExtension = ModuleExtension(platform);
            const auto executableExtension = ExecutableExtension(platform);
            return extension == moduleExtension || extension == executableExtension ||
                   (debugBuild && extension == ".pdb");
        }

        bool IsCountedLegacyBinary(const fs::path& path, PackagePlatform platform)
        {
            const auto extension = path.extension().string();
            return extension == ModuleExtension(platform) || extension == ExecutableExtension(platform);
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
            fs::absolute(fs::path(config.outputDirectory) /
                         std::format("{}_{}_{}", config.projectName, PlatformName(config.platform), configName));
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
                const auto relative = fs::relative(entry.path(), assetsSource, ec).string();
                if (!config.includeEditor && relative.starts_with("Editor"))
                    continue;

                const fs::path destination = assetsDestination / relative;
                fs::create_directories(destination.parent_path(), ec);
                fs::copy_file(entry.path(), destination, fs::copy_options::overwrite_existing, ec);
                if (ec)
                {
                    result.warnings.push_back(std::format("Failed to copy asset '{}': {}", relative, ec.message()));
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

            const auto filename = entry.path().filename().string();
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
                        std::format("Could not remove debug file '{}'", entry.path().filename().string()));
                    ec.clear();
                }
            }
        }

        const fs::path manifestPath = outputRoot / "manifest.txt";
        if (FILE* manifest = std::fopen(manifestPath.string().c_str(), "w"))
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
            for (const auto& entry : fs::recursive_directory_iterator(outputRoot, ec))
            {
                if (entry.is_regular_file(ec))
                {
                    const auto relative = fs::relative(entry.path(), outputRoot, ec);
                    std::fprintf(manifest, "%s %llu\n", relative.string().c_str(),
                                 static_cast<unsigned long long>(entry.file_size(ec)));
                }
            }
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
        result.outputPath = outputRoot.string();
        result.totalSizeMB = static_cast<float>(totalBytes) / (1024.0f * 1024.0f);
        result.success = result.errors.empty();
        return publish(result, result.success);
    }

} // namespace Spark::Build
