#include "GamePackager.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>

namespace Spark
{

    GamePackager& GamePackager::GetInstance()
    {
        static GamePackager instance;
        return instance;
    }

    void GamePackager::Initialize()
    {
        m_initialized = true;
        m_lastResult = {};

        m_supportedPlatforms.clear();
#if defined(_WIN32)
        m_supportedPlatforms.push_back(TargetPlatform::Windows);
#elif defined(__linux__)
        m_supportedPlatforms.push_back(TargetPlatform::Linux);
#elif defined(__APPLE__)
        m_supportedPlatforms.push_back(TargetPlatform::macOS);
#endif

        for (auto p : {TargetPlatform::Windows, TargetPlatform::Linux, TargetPlatform::macOS})
        {
            if (std::find(m_supportedPlatforms.begin(), m_supportedPlatforms.end(), p) == m_supportedPlatforms.end())
            {
                m_supportedPlatforms.push_back(p);
            }
        }
    }

    void GamePackager::Shutdown()
    {
        m_initialized = false;
        m_supportedPlatforms.clear();
    }

    PackageResult GamePackager::Package(const PackageConfig& config)
    {
        PackageResult result;

        auto validationErrors = ValidateConfig(config);
        if (!validationErrors.empty())
        {
            result.errors = std::move(validationErrors);
            result.success = false;
            m_lastResult = result;
            return result;
        }

        namespace fs = std::filesystem;

        std::string platformStr{PlatformToString(config.platform)};
        std::string configStr = (config.buildConfig == PackageBuildConfig::Debug) ? "Debug" : "Release";
        fs::path outputRoot =
            fs::path(config.outputDir) / std::format("{}_{}_{}", config.projectName, platformStr, configStr);

        std::error_code ec;
        fs::create_directories(outputRoot / "Bin", ec);
        if (ec)
        {
            result.errors.push_back(std::format("Failed to create output directory: {}", ec.message()));
            m_lastResult = result;
            return result;
        }
        fs::create_directories(outputRoot / "Assets", ec);
        fs::create_directories(outputRoot / "Config", ec);

        auto [assetCount, assetWarnings] = CookAssets(config, outputRoot);
        result.assetCount = assetCount;
        result.warnings.insert(result.warnings.end(), assetWarnings.begin(), assetWarnings.end());

        auto [dllCount, binErrors] = CopyBinaries(config, outputRoot);
        result.dllCount = dllCount;
        if (!binErrors.empty())
        {
            result.errors.insert(result.errors.end(), binErrors.begin(), binErrors.end());
            m_lastResult = result;
            return result;
        }

        if (config.stripDebugSymbols && config.buildConfig == PackageBuildConfig::Release)
        {
            auto stripWarnings = StripSymbols(outputRoot);
            result.warnings.insert(result.warnings.end(), stripWarnings.begin(), stripWarnings.end());
        }

        if (!CreateManifest(config, outputRoot, result))
        {
            result.warnings.push_back("Failed to write package manifest");
        }

        if (config.compressAssets)
        {
            auto compressWarnings = CompressOutput(outputRoot);
            result.warnings.insert(result.warnings.end(), compressWarnings.begin(), compressWarnings.end());
        }

        uint64_t totalBytes = 0;
        for (const auto& entry : fs::recursive_directory_iterator(outputRoot, ec))
        {
            if (entry.is_regular_file())
            {
                totalBytes += entry.file_size();
            }
        }
        result.totalSizeMB = static_cast<float>(totalBytes) / (1024.0f * 1024.0f);

        result.outputPath = fs::absolute(outputRoot).string();
        result.success = result.errors.empty();
        m_lastResult = result;
        return result;
    }

    std::vector<std::string> GamePackager::ValidateConfig(const PackageConfig& config) const
    {
        std::vector<std::string> errors;

        if (config.outputDir.empty())
        {
            errors.push_back("Output directory must not be empty");
        }
        if (config.projectName.empty())
        {
            errors.push_back("Project name must not be empty");
        }
        if (config.projectName.find_first_of("/\\:*?\"<>|") != std::string::npos)
        {
            errors.push_back("Project name contains invalid filesystem characters");
        }
        if (!m_initialized)
        {
            errors.push_back("GamePackager has not been initialized");
        }

        return errors;
    }

    std::vector<TargetPlatform> GamePackager::GetSupportedPlatforms() const
    {
        return m_supportedPlatforms;
    }

    std::string GamePackager::Console_GetStatus() const
    {
        if (!m_initialized)
        {
            return "GamePackager: not initialized";
        }

        std::string status =
            std::format("GamePackager: initialized, {} supported platform(s)", m_supportedPlatforms.size());

        if (!m_lastResult.outputPath.empty())
        {
            status += std::format("\n  Last package: {} ({})", m_lastResult.outputPath,
                                  m_lastResult.success ? "success" : "failed");
            status += std::format("\n  Assets: {}, DLLs: {}, Size: {:.1f} MB", m_lastResult.assetCount,
                                  m_lastResult.dllCount, m_lastResult.totalSizeMB);
            if (!m_lastResult.errors.empty())
            {
                status += std::format("\n  Errors: {}", m_lastResult.errors.size());
            }
        }

        return status;
    }

    std::string_view GamePackager::PlatformToString(TargetPlatform p)
    {
        switch (p)
        {
        case TargetPlatform::Windows:
            return "Windows";
        case TargetPlatform::Linux:
            return "Linux";
        case TargetPlatform::macOS:
            return "macOS";
        }
        return "Unknown";
    }

    std::string_view GamePackager::GetDllExtension(TargetPlatform p)
    {
        switch (p)
        {
        case TargetPlatform::Windows:
            return ".dll";
        case TargetPlatform::Linux:
            return ".so";
        case TargetPlatform::macOS:
            return ".dylib";
        }
        return "";
    }

    std::string_view GamePackager::GetExeExtension(TargetPlatform p)
    {
        switch (p)
        {
        case TargetPlatform::Windows:
            return ".exe";
        default:
            return "";
        }
    }

    std::pair<uint32_t, std::vector<std::string>> GamePackager::CookAssets(
        const PackageConfig& config, const std::filesystem::path& outputRoot) const
    {
        namespace fs = std::filesystem;
        uint32_t count = 0;
        std::vector<std::string> warnings;

        fs::path assetsSource = "Assets";
        std::error_code ec;
        if (!fs::exists(assetsSource, ec))
        {
            warnings.push_back("Assets directory not found; skipping asset cooking");
            return {count, warnings};
        }

        fs::path assetsDest = outputRoot / "Assets";
        for (const auto& entry : fs::recursive_directory_iterator(assetsSource, ec))
        {
            if (!entry.is_regular_file())
                continue;

            std::string relPath = fs::relative(entry.path(), assetsSource).string();
            if (!config.includeEditor && relPath.starts_with("Editor"))
                continue;

            fs::path destFile = assetsDest / relPath;
            fs::create_directories(destFile.parent_path(), ec);
            fs::copy_file(entry.path(), destFile, fs::copy_options::overwrite_existing, ec);
            if (ec)
            {
                warnings.push_back(std::format("Failed to copy asset '{}': {}", relPath, ec.message()));
                ec.clear();
            }
            else
            {
                ++count;
            }
        }

        return {count, warnings};
    }

    std::pair<uint32_t, std::vector<std::string>> GamePackager::CopyBinaries(
        const PackageConfig& config, const std::filesystem::path& outputRoot) const
    {
        namespace fs = std::filesystem;
        uint32_t count = 0;
        std::vector<std::string> errors;

        std::string configStr = (config.buildConfig == PackageBuildConfig::Debug) ? "Debug" : "Release";
        fs::path binSource = fs::path("build") / configStr;

        std::error_code ec;
        if (!fs::exists(binSource, ec))
        {
            binSource = "build";
        }
        if (!fs::exists(binSource, ec))
        {
            errors.push_back(std::format("Binary source directory '{}' not found", binSource.string()));
            return {count, errors};
        }

        fs::path binDest = outputRoot / "Bin";
        std::string_view dllExt = GetDllExtension(config.platform);
        std::string_view exeExt = GetExeExtension(config.platform);

        for (const auto& entry : fs::directory_iterator(binSource, ec))
        {
            if (!entry.is_regular_file())
                continue;

            std::string ext = entry.path().extension().string();
            bool isBinary = (ext == dllExt || ext == exeExt);
            if (config.buildConfig == PackageBuildConfig::Debug && ext == ".pdb")
                isBinary = true;

            if (!isBinary)
                continue;

            std::string filename = entry.path().filename().string();
            if (!config.includeEditor && filename.find("Editor") != std::string::npos)
                continue;

            fs::copy_file(entry.path(), binDest / filename, fs::copy_options::overwrite_existing, ec);
            if (ec)
            {
                errors.push_back(std::format("Failed to copy binary '{}': {}", filename, ec.message()));
                ec.clear();
            }
            else if (ext == dllExt || ext == exeExt)
            {
                ++count;
            }
        }

        return {count, errors};
    }

    std::vector<std::string> GamePackager::StripSymbols(const std::filesystem::path& outputRoot) const
    {
        namespace fs = std::filesystem;
        std::vector<std::string> warnings;
        std::error_code ec;

        fs::path binDir = outputRoot / "Bin";
        for (const auto& entry : fs::directory_iterator(binDir, ec))
        {
            if (!entry.is_regular_file())
                continue;

            std::string ext = entry.path().extension().string();
            if (ext == ".pdb")
            {
                fs::remove(entry.path(), ec);
                if (ec)
                {
                    warnings.push_back(
                        std::format("Could not remove debug file '{}'", entry.path().filename().string()));
                    ec.clear();
                }
            }
        }

        return warnings;
    }

    bool GamePackager::CreateManifest(const PackageConfig& config, const std::filesystem::path& outputRoot,
                                      const PackageResult& result) const
    {
        namespace fs = std::filesystem;

        fs::path manifestPath = outputRoot / "manifest.txt";
        FILE* f = std::fopen(manifestPath.string().c_str(), "w");
        if (!f)
            return false;

        auto now = std::chrono::system_clock::now();
        auto timeT = std::chrono::system_clock::to_time_t(now);

        std::fprintf(f, "# SparkEngine Package Manifest\n");
        std::fprintf(f, "# Project: %s\n", config.projectName.c_str());
        std::fprintf(f, "# Platform: %.*s\n", static_cast<int>(PlatformToString(config.platform).size()),
                     PlatformToString(config.platform).data());
        std::fprintf(f, "# Config: %s\n", config.buildConfig == PackageBuildConfig::Debug ? "Debug" : "Release");
        std::fprintf(f, "# Timestamp: %lld\n", static_cast<long long>(timeT));
        std::fprintf(f, "# Assets: %u\n", result.assetCount);
        std::fprintf(f, "# DLLs: %u\n", result.dllCount);
        std::fprintf(f, "\n");

        std::error_code ec;
        for (const auto& entry : fs::recursive_directory_iterator(outputRoot, ec))
        {
            if (!entry.is_regular_file())
                continue;
            auto relPath = fs::relative(entry.path(), outputRoot);
            std::fprintf(f, "%s %llu\n", relPath.string().c_str(), static_cast<unsigned long long>(entry.file_size()));
        }

        std::fclose(f);
        return true;
    }

    std::vector<std::string> GamePackager::CompressOutput(const std::filesystem::path& outputRoot) const
    {
        namespace fs = std::filesystem;
        std::vector<std::string> warnings;

        fs::path assetsDir = outputRoot / "Assets";
        std::error_code ec;
        if (!fs::exists(assetsDir, ec) || fs::is_empty(assetsDir, ec))
        {
            warnings.push_back("No assets to compress");
            return warnings;
        }

        uint32_t fileCount = 0;
        for (const auto& entry : fs::recursive_directory_iterator(assetsDir, ec))
        {
            if (entry.is_regular_file())
                ++fileCount;
        }

        if (fileCount == 0)
        {
            warnings.push_back("Assets directory is empty; nothing to compress");
        }

        return warnings;
    }

} // namespace Spark
