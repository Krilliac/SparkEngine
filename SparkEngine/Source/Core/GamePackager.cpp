#include "GamePackager.h"

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
    auto& canonical = Build::GamePackager::GetInstance();
    canonical.Initialize();
    m_initialized = true;
    m_lastResult = {};
    m_supportedPlatforms = {TargetPlatform::Windows, TargetPlatform::Linux, TargetPlatform::macOS};
}

void GamePackager::Shutdown()
{
    Build::GamePackager::GetInstance().Shutdown();
    m_initialized = false;
    m_supportedPlatforms.clear();
}

PackageResult GamePackager::Package(const PackageConfig& config)
{
    if (!m_initialized)
    {
        m_lastResult = {false, {}, 0.0f, {"GamePackager has not been initialized"}, {}, 0, 0};
        return m_lastResult;
    }

    Build::LegacyPackageConfig canonicalConfig;
    canonicalConfig.outputDirectory = config.outputDir;
    canonicalConfig.projectName = config.projectName;
    canonicalConfig.platform = config.platform == TargetPlatform::Linux
                                   ? Build::PackagePlatform::LinuxX64
                                   : config.platform == TargetPlatform::macOS ? Build::PackagePlatform::MacOSX64
                                                                                : Build::PackagePlatform::WindowsX64;
    canonicalConfig.debugBuild = config.buildConfig == PackageBuildConfig::Debug;
    canonicalConfig.stripDebugSymbols = config.stripDebugSymbols;
    canonicalConfig.compressAssets = config.compressAssets;
    canonicalConfig.includeEditor = config.includeEditor;

    const auto canonicalResult = Build::GamePackager::GetInstance().PackageLegacy(canonicalConfig);
    m_lastResult.success = canonicalResult.success;
    m_lastResult.outputPath = canonicalResult.outputPath;
    m_lastResult.totalSizeMB = canonicalResult.totalSizeMB;
    m_lastResult.errors = canonicalResult.errors;
    m_lastResult.warnings = canonicalResult.warnings;
    m_lastResult.assetCount = canonicalResult.assetCount;
    m_lastResult.dllCount = canonicalResult.dllCount;
    return m_lastResult;
}

std::vector<std::string> GamePackager::ValidateConfig(const PackageConfig& config) const
{
    std::vector<std::string> errors;
    if (config.outputDir.empty())
        errors.emplace_back("Output directory must not be empty");
    if (config.projectName.empty())
        errors.emplace_back("Project name must not be empty");
    if (config.projectName.find_first_of("/\\:*?\"<>|") != std::string::npos)
        errors.emplace_back("Project name contains invalid filesystem characters");
    if (!m_initialized)
        errors.emplace_back("GamePackager has not been initialized");
    return errors;
}

std::vector<TargetPlatform> GamePackager::GetSupportedPlatforms() const
{
    return m_supportedPlatforms;
}

std::string GamePackager::Console_GetStatus() const
{
    if (!m_initialized)
        return "GamePackager: not initialized";

    std::string status =
        std::format("GamePackager: initialized, {} supported platform(s)", m_supportedPlatforms.size());
    if (!m_lastResult.outputPath.empty())
    {
        status += std::format("\n  Last package: {} ({})", m_lastResult.outputPath,
                              m_lastResult.success ? "success" : "failed");
        status += std::format("\n  Assets: {}, DLLs: {}, Size: {:.1f} MB", m_lastResult.assetCount,
                              m_lastResult.dllCount, m_lastResult.totalSizeMB);
        if (!m_lastResult.errors.empty())
            status += std::format("\n  Errors: {}", m_lastResult.errors.size());
    }
    return status;
}

} // namespace Spark
