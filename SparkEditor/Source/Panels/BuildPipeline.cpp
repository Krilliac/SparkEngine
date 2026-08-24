/**
 * @file BuildPipeline.cpp
 * @brief Async CMake subprocess management for build orchestration
 *
 * Contains: lifecycle, public API (StartBuild, StartCookOnly, Cancel,
 * DrainLogLines, GetStatusText), worker threads, and settings mapping.
 * Subprocess execution and output parsing live in BuildPipelineProcess.cpp.
 */

#include "BuildPipeline.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <csignal>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif
#endif

namespace SparkEditor
{
    namespace
    {
        namespace fs = std::filesystem;

        std::string FormatCommandForLog(const std::string& executable, const std::vector<std::string>& arguments)
        {
            std::ostringstream stream;
            stream << executable;
            for (const auto& argument : arguments)
            {
                stream << ' ';
                const bool needsQuotes = argument.find_first_of(" \t\"") != std::string::npos;
                if (!needsQuotes)
                {
                    stream << argument;
                    continue;
                }

                stream << '"';
                for (char c : argument)
                {
                    if (c == '"')
                        stream << '\\';
                    stream << c;
                }
                stream << '"';
            }
            return stream.str();
        }

        fs::path GetExecutablePath()
        {
#ifdef _WIN32
            std::wstring buffer(32768, L'\0');
            const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0 || length >= buffer.size())
                return {};
            buffer.resize(length);
            return fs::path(buffer);
#elif defined(__APPLE__)
            uint32_t size = 0;
            _NSGetExecutablePath(nullptr, &size);
            std::string buffer(size, '\0');
            if (_NSGetExecutablePath(buffer.data(), &size) != 0)
                return {};
            buffer.resize(std::char_traits<char>::length(buffer.c_str()));
            return fs::path(buffer);
#else
            std::error_code ec;
            return fs::read_symlink("/proc/self/exe", ec);
#endif
        }

        void AddPrefixCandidates(std::vector<fs::path>& candidates, const fs::path& prefix)
        {
            if (prefix.empty())
                return;
            candidates.push_back(prefix);
            candidates.push_back(prefix / "lib" / "cmake" / "SparkEngine");
            candidates.push_back(prefix / "share" / "cmake" / "SparkEngine");
        }

        std::vector<fs::path> GetExecutableAncestors()
        {
            std::vector<fs::path> ancestors;
            fs::path current = GetExecutablePath().parent_path();
            for (int depth = 0; depth < 10 && !current.empty(); ++depth)
            {
                ancestors.push_back(current);
                const fs::path parent = current.parent_path();
                if (parent == current)
                    break;
                current = parent;
            }
            return ancestors;
        }

        fs::path FindEngineBuildDirectory()
        {
            for (const fs::path& ancestor : GetExecutableAncestors())
            {
                if (fs::is_regular_file(ancestor / "cmake_install.cmake") &&
                    fs::is_regular_file(ancestor / "SparkEngineConfig.cmake"))
                    return ancestor;
            }
            return {};
        }

        bool PackageSupportsConfiguration(const fs::path& directory, const std::string& configuration)
        {
            if (configuration.empty())
                return true;
            std::string configName = configuration;
            for (char& c : configName)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return fs::is_regular_file(directory / ("SparkEngineTargets-" + configName + ".cmake"));
        }

        std::string Lowercase(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        }

        bool IsSafeExecutableName(const std::string& requestedName, std::string& executableName)
        {
            fs::path requested(requestedName);
            if (requestedName.empty() || requested.has_parent_path() || requested.filename().string() != requestedName)
                return false;

            for (unsigned char c : requestedName)
            {
                if (c < 0x20 || std::string_view("<>:\"/\\|?*%^").find(static_cast<char>(c)) != std::string_view::npos)
                    return false;
            }
            if (requestedName.back() == ' ' || requestedName.back() == '.')
                return false;

            std::string stem = requested.stem().string();
            const std::string extension = Lowercase(requested.extension().string());
            if (stem.empty() || (!extension.empty() && extension != ".exe"))
                return false;
            const std::string reserved = Lowercase(stem);
            if (reserved == "con" || reserved == "prn" || reserved == "aux" || reserved == "nul" ||
                (reserved.size() == 4 && (reserved.starts_with("com") || reserved.starts_with("lpt")) &&
                 reserved[3] >= '1' && reserved[3] <= '9'))
                return false;

            executableName = extension == ".exe" ? requestedName : requestedName + ".exe";
            return true;
        }

        bool CopyDirectoryContents(const fs::path& source, const fs::path& destination, std::string& error)
        {
            if (!fs::is_directory(source))
                return true;

            std::error_code ec;
            fs::create_directories(destination, ec);
            if (ec)
            {
                error = "Failed to create '" + destination.string() + "': " + ec.message();
                return false;
            }

            for (fs::recursive_directory_iterator it(source, ec), end; it != end && !ec; it.increment(ec))
            {
                const fs::path relative = fs::relative(it->path(), source, ec);
                if (ec)
                    break;
                const fs::path target = destination / relative;
                if (it->is_directory(ec))
                {
                    fs::create_directories(target, ec);
                }
                else if (it->is_regular_file(ec))
                {
                    fs::create_directories(target.parent_path(), ec);
                    if (!ec)
                        fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing, ec);
                }
            }
            if (ec)
            {
                error = "Failed to copy '" + source.string() + "': " + ec.message();
                return false;
            }
            return true;
        }

        bool CopyProjectContent(const fs::path& projectRoot, const fs::path& outputDirectory, bool includeAssets,
                                std::string& error)
        {
            if (includeAssets && !CopyDirectoryContents(projectRoot / "Assets", outputDirectory / "Assets", error))
                return false;
            if (!CopyDirectoryContents(projectRoot / "Scenes", outputDirectory / "Scenes", error) ||
                !CopyDirectoryContents(projectRoot / "Config", outputDirectory / "Config", error))
                return false;

            std::error_code ec;
            for (fs::directory_iterator it(projectRoot, ec), end; it != end && !ec; it.increment(ec))
            {
                if (!it->is_regular_file(ec))
                    continue;
                const std::string filename = it->path().filename().string();
                if (it->path().extension() != ".sparkproject" && filename != "spark.project.json" &&
                    filename != "spark.modules.json")
                    continue;
                fs::copy_file(it->path(), outputDirectory / it->path().filename(), fs::copy_options::overwrite_existing,
                              ec);
            }
            if (ec)
            {
                error = "Failed to package project metadata: " + ec.message();
                return false;
            }
            return true;
        }

        bool WriteTextFile(const fs::path& path, const std::string& contents, std::string& error)
        {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file || !(file << contents))
            {
                error = "Failed to write '" + path.string() + "'";
                return false;
            }
            return true;
        }

        std::string EscapeJson(const std::string& value)
        {
            std::string escaped;
            escaped.reserve(value.size() + 8);
            for (char c : value)
            {
                if (c == '\\' || c == '"')
                    escaped.push_back('\\');
                escaped.push_back(c);
            }
            return escaped;
        }

        bool IsPathWithin(const fs::path& candidate, const fs::path& parent)
        {
            const fs::path normalizedCandidate = candidate.lexically_normal();
            const fs::path normalizedParent = parent.lexically_normal();
            auto candidateIt = normalizedCandidate.begin();
            auto parentIt = normalizedParent.begin();
            const auto candidateEnd = normalizedCandidate.end();
            const auto parentEnd = normalizedParent.end();
            for (; parentIt != parentEnd; ++parentIt, ++candidateIt)
            {
                if (candidateIt == candidateEnd)
                    return false;
#ifdef _WIN32
                if (Lowercase(candidateIt->string()) != Lowercase(parentIt->string()))
#else
                if (*candidateIt != *parentIt)
#endif
                    return false;
            }
            return true;
        }

        fs::path FindFirstReflectedScene(const fs::path& scenesDirectory)
        {
            const fs::path preferred = scenesDirectory / "Default.sparkscene";
            if (fs::is_regular_file(preferred))
                return preferred;

            std::vector<fs::path> scenes;
            std::error_code ec;
            for (fs::recursive_directory_iterator it(scenesDirectory, ec), end; it != end && !ec; it.increment(ec))
            {
                if (it->is_regular_file(ec) && it->path().extension() == ".sparkscene")
                    scenes.push_back(it->path());
            }
            std::sort(scenes.begin(), scenes.end());
            return scenes.empty() ? fs::path{} : scenes.front();
        }

        std::string ManifestModuleFilename(const fs::path& projectRoot)
        {
            std::ifstream file(projectRoot / "spark.modules.json", std::ios::binary);
            if (!file)
                return {};
            const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            const size_t key = content.find("\"path\"");
            if (key == std::string::npos)
                return {};
            const size_t colon = content.find(':', key);
            const size_t begin = colon == std::string::npos ? std::string::npos : content.find('"', colon + 1);
            const size_t end = begin == std::string::npos ? std::string::npos : content.find('"', begin + 1);
            if (end == std::string::npos)
                return {};
            return fs::path(content.substr(begin + 1, end - begin - 1)).filename().string();
        }

        fs::path FindBuiltModuleBinary(const fs::path& buildDirectory, const fs::path& projectRoot)
        {
            std::vector<fs::path> candidates;
            std::error_code ec;
            for (fs::recursive_directory_iterator it(buildDirectory, ec), end; it != end && !ec; it.increment(ec))
            {
                if (!it->is_regular_file(ec) || it->path().extension() != ".sparkabi")
                    continue;
                fs::path binary = it->path();
                binary.replace_extension();
                if (binary.extension() == ".dll" && fs::is_regular_file(binary))
                    candidates.push_back(binary);
            }
            const std::string wanted = Lowercase(ManifestModuleFilename(projectRoot));
            if (!wanted.empty())
            {
                const auto match = std::find_if(candidates.begin(), candidates.end(), [&](const fs::path& candidate)
                                                { return Lowercase(candidate.filename().string()) == wanted; });
                if (match != candidates.end())
                    return *match;
            }
            return candidates.size() == 1 ? candidates.front() : fs::path{};
        }

        fs::path FindRuntimeHostExecutable(const std::string& configuration)
        {
            std::vector<fs::path> candidates;
            if (const char* explicitHost = std::getenv("SPARKENGINE_RUNTIME_HOST"))
                candidates.emplace_back(explicitHost);

            const fs::path executableDirectory = GetExecutablePath().parent_path();
            const bool runningDebug = Lowercase(executableDirectory.filename().string()) == "debug";
            if ((configuration == "Debug") == runningDebug)
                candidates.push_back(executableDirectory / "SparkEngine.exe");
            for (const fs::path& ancestor : GetExecutableAncestors())
            {
                candidates.push_back(ancestor / "bin" / configuration / "SparkEngine.exe");
                candidates.push_back(ancestor / configuration / "SparkEngine.exe");
            }
            for (const fs::path& candidate : candidates)
            {
                if (fs::is_regular_file(candidate))
                    return candidate;
            }
            return {};
        }
    } // namespace

    BuildPipeline::BuildPipeline() = default;

    BuildPipeline::~BuildPipeline()
    {
        Cancel();
        if (m_worker.joinable())
            m_worker.join();
    }

    // ========================================================================
    // Public API
    // ========================================================================

    bool BuildPipeline::StartBuild(const BuildSettings& settings, const std::string& projectRoot)
    {
        if (m_running.load())
            return false;

#ifdef _WIN32
        // The packaged host is the native x64 SparkEngine executable. Other
        // target choices require a real cross/native toolchain and matching
        // runtime host; pretending otherwise produces an unloadable module.
        if (settings.platform != BuildCookPanel::TargetPlatform::WindowsX64)
            return false;
#endif

        const std::filesystem::path sourceRoot = std::filesystem::absolute(projectRoot).lexically_normal();
        if (!std::filesystem::is_regular_file(sourceRoot / "CMakeLists.txt"))
            return false;

        if (m_worker.joinable())
            m_worker.join();

        m_running.store(true);
        m_cancelRequested.store(false);
        m_progress.store(0.0f);
        m_result.store(BuildResult::None);
        {
            std::lock_guard lk(m_logMutex);
            m_logQueue.clear();
        }

        auto buildType = SettingsToBuildType(settings);
        auto defines = SettingsToDefines(settings);
        std::string buildDir = (sourceRoot / "build" / buildType).string();

        PushLog(BuildLogLine::Level::Info, "Starting " + buildType + " build for " + sourceRoot.string());

        m_worker = std::thread(&BuildPipeline::WorkerThread, this, settings, std::move(buildType), std::move(buildDir),
                               sourceRoot.string(), std::move(defines));
        return true;
    }

    bool BuildPipeline::StartCookOnly(const BuildSettings& settings, const std::string& projectRoot)
    {
        if (m_running.load())
            return false;

        const std::filesystem::path sourceRoot = std::filesystem::absolute(projectRoot).lexically_normal();
        if (!std::filesystem::is_directory(sourceRoot))
            return false;

        if (m_worker.joinable())
            m_worker.join();

        m_running.store(true);
        m_cancelRequested.store(false);
        m_progress.store(0.0f);
        m_result.store(BuildResult::None);
        {
            std::lock_guard lk(m_logMutex);
            m_logQueue.clear();
        }

        std::string outputDir = (sourceRoot / settings.outputDirectory).lexically_normal().string();
        m_worker =
            std::thread(&BuildPipeline::CookWorkerThread, this, settings, std::move(outputDir), sourceRoot.string());
        return true;
    }

    void BuildPipeline::Cancel()
    {
        if (!m_running.load())
            return;
        m_cancelRequested.store(true);

#ifdef _WIN32
        if (m_processHandle)
            TerminateProcess(m_processHandle, 1);
#else
        if (m_childPid > 0)
            kill(m_childPid, SIGTERM);
#endif
    }

    std::vector<BuildLogLine> BuildPipeline::DrainLogLines()
    {
        std::lock_guard lk(m_logMutex);
        std::vector<BuildLogLine> out;
        out.swap(m_logQueue);
        return out;
    }

    std::string BuildPipeline::GetStatusText() const
    {
        std::lock_guard lk(m_statusMutex);
        return m_statusText;
    }

    // ========================================================================
    // Worker threads
    // ========================================================================

    void BuildPipeline::WorkerThread(BuildSettings settings, std::string buildType, std::string buildDir,
                                     std::string projectRoot, std::vector<std::string> extraDefines)
    {
        // Phase 1 — Configure
        {
            std::lock_guard lk(m_statusMutex);
            m_statusText = "Configuring...";
        }
        m_progress.store(0.05f);

        for (const auto& def : extraDefines)
        {
            // Arguments are passed directly to CreateProcess/execvp, but keep
            // persisted build settings free of command-language metacharacters.
            if (def.find_first_of(";|&$`\n\r") != std::string::npos)
            {
                PushLog(BuildLogLine::Level::Error, "Unsafe characters in define: " + def);
                m_result.store(BuildResult::Failed);
                m_running.store(false);
                return;
            }
        }

        const std::string installConfig = buildType == "Debug" ? "Debug" : "Release";
        std::string sparkEngineDirectory = FindSparkEnginePackageDirectory(installConfig);
        if (sparkEngineDirectory.empty())
        {
            // A CMake build-tree SparkEngineConfig.cmake is intentionally not
            // sufficient: install(EXPORT) creates SparkEngineTargets.cmake and
            // the module helpers only in an SDK install. Materialize that SDK
            // once in a deterministic directory beside the running engine build.
            const std::filesystem::path engineBuild = FindEngineBuildDirectory();
            if (!engineBuild.empty())
            {
                const std::filesystem::path sdkPrefix = engineBuild / "spark-sdk" / installConfig;
                const std::filesystem::path sdkConfig = sdkPrefix / "lib" / "cmake" / "SparkEngine";
                {
                    std::lock_guard lk(m_statusMutex);
                    m_statusText = "Installing SparkEngine SDK...";
                }
                std::vector<std::string> installArgs = {"--install", engineBuild.string(), "--config",    installConfig,
                                                        "--prefix",  sdkPrefix.string(),   "--component", "sdk"};
                PushLog(BuildLogLine::Level::Info, "> " + FormatCommandForLog("cmake", installArgs));
                const int installExit = RunCommand("cmake", installArgs);
                if (installExit == 0 && IsSparkEnginePackageDirectory(sdkConfig.string()) &&
                    PackageSupportsConfiguration(sdkConfig, installConfig))
                {
                    sparkEngineDirectory = sdkConfig.string();
                }
            }

            if (m_cancelRequested.load())
            {
                m_result.store(BuildResult::Cancelled);
                m_running.store(false);
                return;
            }

            if (sparkEngineDirectory.empty())
            {
                PushLog(BuildLogLine::Level::Error,
                        "No complete SparkEngine SDK was found. Set SparkEngine_DIR to an installed "
                        "lib/cmake/SparkEngine directory or install the engine SDK first.");
                m_result.store(BuildResult::Failed);
                m_running.store(false);
                return;
            }
        }

        std::vector<std::string> configArgs =
            CreateConfigureArguments(settings, projectRoot, buildDir, sparkEngineDirectory, extraDefines);

        PushLog(BuildLogLine::Level::Info, "> " + FormatCommandForLog("cmake", configArgs));
        int configExit = RunCommand("cmake", configArgs);

        if (m_cancelRequested.load())
        {
            m_result.store(BuildResult::Cancelled);
            m_running.store(false);
            return;
        }
        if (configExit != 0)
        {
            PushLog(BuildLogLine::Level::Error, "CMake configure failed (exit " + std::to_string(configExit) + ")");
            m_result.store(BuildResult::Failed);
            m_running.store(false);
            return;
        }

        m_progress.store(0.15f);

        // Phase 2 — Build
        {
            std::lock_guard lk(m_statusMutex);
            m_statusText = "Compiling...";
        }

        std::vector<std::string> buildArgs = {"--build", buildDir, "--config", buildType};

        PushLog(BuildLogLine::Level::Info, "> " + FormatCommandForLog("cmake", buildArgs));
        int buildExit = RunCommand("cmake", buildArgs);

        if (m_cancelRequested.load())
        {
            m_result.store(BuildResult::Cancelled);
            m_running.store(false);
            return;
        }

        if (buildExit != 0)
        {
            PushLog(BuildLogLine::Level::Error, "Build failed (exit " + std::to_string(buildExit) + ")");
            {
                std::lock_guard lk(m_statusMutex);
                m_statusText = "Build Failed";
            }
            m_result.store(BuildResult::Failed);
        }
        else
        {
#ifdef _WIN32
            {
                std::lock_guard lk(m_statusMutex);
                m_statusText = "Packaging...";
            }
            m_progress.store(0.9f);

            const std::filesystem::path runtimeHost = FindRuntimeHostExecutable(installConfig);
            const std::filesystem::path moduleBinary = FindBuiltModuleBinary(buildDir, projectRoot);
            std::filesystem::path outputDirectory(settings.outputDirectory);
            if (outputDirectory.is_relative())
                outputDirectory = std::filesystem::path(projectRoot) / outputDirectory;

            std::string packageError;
            if (runtimeHost.empty())
                packageError = "No SparkEngine.exe runtime host matching configuration " + installConfig +
                               " was found beside the editor/build tree";
            else if (moduleBinary.empty())
                packageError = "Could not uniquely identify the freshly built module DLL and .sparkabi sidecar";
            else if (!AssembleWindowsPackage(settings, projectRoot, runtimeHost.string(), moduleBinary.string(),
                                             outputDirectory.string(), &packageError))
            {
                // AssembleWindowsPackage provides the actionable detail.
            }

            if (!packageError.empty())
            {
                PushLog(BuildLogLine::Level::Error, "Package assembly failed: " + packageError);
                {
                    std::lock_guard lk(m_statusMutex);
                    m_statusText = "Packaging Failed";
                }
                m_result.store(BuildResult::Failed);
                m_running.store(false);
                return;
            }
            PushLog(BuildLogLine::Level::Info, "Runnable package assembled at " + outputDirectory.string());
#endif
            m_progress.store(1.0f);
            PushLog(BuildLogLine::Level::Info, "Build completed successfully!");
            {
                std::lock_guard lk(m_statusMutex);
                m_statusText = "Build Complete";
            }
            m_result.store(BuildResult::Success);
        }

        m_running.store(false);
    }

    void BuildPipeline::CookWorkerThread(BuildSettings settings, std::string outputDir, std::string projectRoot)
    {
        {
            std::lock_guard lk(m_statusMutex);
            m_statusText = "Cooking assets...";
        }
        PushLog(BuildLogLine::Level::Info, "Cooking assets to " + outputDir);

        namespace fs = std::filesystem;
        const fs::path sourceRoot = fs::absolute(projectRoot).lexically_normal();
        const fs::path destination = fs::absolute(outputDir).lexically_normal();
        if (IsPathWithin(destination, sourceRoot / "Assets") || IsPathWithin(destination, sourceRoot / "Scenes") ||
            IsPathWithin(destination, sourceRoot / "Config"))
        {
            PushLog(BuildLogLine::Level::Error, "Cook output cannot be inside Assets, Scenes, or Config");
            m_result.store(BuildResult::Failed);
            m_running.store(false);
            return;
        }

        std::error_code ec;
        fs::create_directories(destination, ec);
        if (ec)
        {
            PushLog(BuildLogLine::Level::Error, "Failed to create output directory: " + ec.message());
            m_result.store(BuildResult::Failed);
            m_running.store(false);
            return;
        }

        m_progress.store(0.25f);
        std::string copyError;
        if (!CopyProjectContent(sourceRoot, destination, settings.cookAssets, copyError))
        {
            PushLog(BuildLogLine::Level::Error, copyError);
            m_result.store(BuildResult::Failed);
            m_running.store(false);
            return;
        }

        m_progress.store(1.0f);
        PushLog(BuildLogLine::Level::Info, settings.cookAssets ? "Content packaging complete"
                                                               : "Scenes/config packaging complete (Assets skipped)");
        {
            std::lock_guard lk(m_statusMutex);
            m_statusText = "Cook Complete";
        }
        m_result.store(BuildResult::Success);
        m_running.store(false);
    }

    bool BuildPipeline::AssembleWindowsPackage(const BuildSettings& settings, const std::string& projectRoot,
                                               const std::string& runtimeHost, const std::string& moduleBinary,
                                               const std::string& outputDirectory, std::string* error)
    {
        namespace fs = std::filesystem;
        std::string detail;
        auto fail = [&](std::string message)
        {
            if (error)
                *error = std::move(message);
            return false;
        };

        std::string executableName;
        if (!IsSafeExecutableName(settings.executableName, executableName))
            return fail("Executable name must be a safe Windows filename without a path");

        const fs::path sourceRoot = fs::absolute(projectRoot).lexically_normal();
        const fs::path host = fs::absolute(runtimeHost).lexically_normal();
        const fs::path module = fs::absolute(moduleBinary).lexically_normal();
        const fs::path sidecar = module.string() + ".sparkabi";
        const fs::path destination = fs::absolute(outputDirectory).lexically_normal();
        if (!fs::is_directory(sourceRoot) || !fs::is_regular_file(host) || !fs::is_regular_file(module) ||
            !fs::is_regular_file(sidecar))
            return fail("Project root, runtime host, module DLL, or module ABI sidecar is missing");
        if (destination == sourceRoot || IsPathWithin(destination, sourceRoot / "Assets") ||
            IsPathWithin(destination, sourceRoot / "Scenes") || IsPathWithin(destination, sourceRoot / "Config"))
            return fail("Package output cannot replace the project root or live inside packaged content");

        std::error_code ec;
        fs::create_directories(destination, ec);
        if (ec)
            return fail("Failed to create package output: " + ec.message());

        const fs::path packagedHost = destination / executableName;
        fs::copy_file(host, packagedHost, fs::copy_options::overwrite_existing, ec);
        if (ec)
            return fail("Failed to copy SparkEngine runtime host: " + ec.message());
        fs::copy_file(module, destination / module.filename(), fs::copy_options::overwrite_existing, ec);
        if (ec)
            return fail("Failed to copy game module: " + ec.message());
        fs::copy_file(sidecar, destination / sidecar.filename(), fs::copy_options::overwrite_existing, ec);
        if (ec)
            return fail("Failed to copy module ABI sidecar: " + ec.message());

        const fs::path runtimeShaders = host.parent_path() / "Shaders";
        if (!fs::is_directory(runtimeShaders))
            return fail("SparkEngine runtime Shaders directory is missing beside the host executable");
        if (!CopyDirectoryContents(runtimeShaders, destination / "Shaders", detail))
            return fail(detail);
        if (!CopyDirectoryContents(host.parent_path() / "Resources", destination / "Resources", detail))
            return fail(detail);
        if (!CopyProjectContent(sourceRoot, destination, settings.cookAssets, detail))
            return fail(detail);

        std::ostringstream manifest;
        manifest << "{\n  \"modules\": [\n    {\n"
                 << "      \"name\": \"" << EscapeJson(module.stem().string()) << "\",\n"
                 << "      \"path\": \"" << EscapeJson(module.filename().string()) << "\",\n"
                 << "      \"loadOrder\": 1000\n"
                 << "    }\n  ]\n}\n";
        if (!WriteTextFile(destination / "spark.modules.json", manifest.str(), detail))
            return fail(detail);

        const std::string hostStem = fs::path(executableName).stem().string();
        std::ostringstream gameLauncher;
        gameLauncher << "@echo off\r\nsetlocal\r\npushd \"%~dp0\"\r\n"
                     << "\"" << executableName << "\"\r\n"
                     << "set \"spark_exit=%ERRORLEVEL%\"\r\npopd\r\nexit /b %spark_exit%\r\n";
        if (!WriteTextFile(destination / "LaunchGame.cmd", gameLauncher.str(), detail))
            return fail(detail);

        const fs::path reflectedScene = FindFirstReflectedScene(destination / "Scenes");
        bool hasScenePreview = false;
        if (!reflectedScene.empty())
        {
            const fs::path startupScene = destination / "Startup.sparkscene";
            fs::copy_file(reflectedScene, startupScene, fs::copy_options::overwrite_existing, ec);
            if (ec)
                return fail("Failed to stage reflected startup scene: " + ec.message());

            const fs::path previewDirectory = destination / "ScenePreview";
            fs::create_directories(previewDirectory, ec);
            if (ec)
                return fail("Failed to create isolated scene-preview directory: " + ec.message());
            const std::string previewExecutable = hostStem + " Scene.exe";
            fs::copy_file(host, previewDirectory / previewExecutable, fs::copy_options::overwrite_existing, ec);
            if (ec)
                return fail("Failed to copy isolated scene-preview host: " + ec.message());

            std::ostringstream sceneLauncher;
            sceneLauncher << "@echo off\r\nsetlocal\r\npushd \"%~dp0\"\r\n"
                          << "\"ScenePreview\\" << previewExecutable << "\" -scene Startup.sparkscene\r\n"
                          << "set \"spark_exit=%ERRORLEVEL%\"\r\npopd\r\nexit /b %spark_exit%\r\n";
            if (!WriteTextFile(destination / "LaunchScene.cmd", sceneLauncher.str(), detail))
                return fail(detail);
            hasScenePreview = true;
        }

        if (settings.includeDebugSymbols && !settings.stripDebugSymbols)
        {
            const fs::path modulePdb = module.parent_path() / (module.stem().string() + ".pdb");
            if (fs::is_regular_file(modulePdb))
            {
                fs::copy_file(modulePdb, destination / modulePdb.filename(), fs::copy_options::overwrite_existing, ec);
                if (ec)
                    return fail("Failed to copy module debug symbols: " + ec.message());
            }
        }

        std::ostringstream readme;
        readme << "SparkEngine runnable package\r\n\r\n"
               << "LaunchGame.cmd runs " << executableName
               << " with spark.modules.json. This executes the compiled game module; -scene is intentionally not "
                  "passed because the runtime ignores reflected-scene rendering while a module is loaded.\r\n";
        if (hasScenePreview)
            readme << "LaunchScene.cmd runs an isolated host with no module manifest and safely passes the staged "
                      "Startup.sparkscene to -scene. This is a reflected-scene preview, separate from module "
                      "execution.\r\n";
        else
            readme << "No .sparkscene file was present, so no reflected-scene preview launcher was generated.\r\n";
        if (!WriteTextFile(destination / "PACKAGE_README.txt", readme.str(), detail))
            return fail(detail);

        if (error)
            error->clear();
        return true;
    }

    // ========================================================================
    // Settings mapping
    // ========================================================================

    bool BuildPipeline::IsSparkEnginePackageDirectory(const std::string& directory)
    {
        const std::filesystem::path root(directory);
        if (!std::filesystem::is_regular_file(root / "SparkEngineConfig.cmake") ||
            !std::filesystem::is_regular_file(root / "SparkEngineTargets.cmake") ||
            !std::filesystem::is_regular_file(root / "SparkGameModule.cmake") ||
            !std::filesystem::is_regular_file(root / "WriteSparkModuleABI.cmake"))
            return false;

        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(root, ec))
        {
            const std::string filename = entry.path().filename().string();
            if (entry.is_regular_file() && filename.starts_with("SparkEngineTargets-") &&
                entry.path().extension() == ".cmake")
                return true;
        }
        return false;
    }

    std::string BuildPipeline::FindSparkEnginePackageDirectory(const std::string& configuration)
    {
        namespace fs = std::filesystem;
        std::vector<fs::path> candidates;

        if (const char* explicitDirectory = std::getenv("SparkEngine_DIR"))
            AddPrefixCandidates(candidates, explicitDirectory);
        if (const char* sdkRoot = std::getenv("SPARKENGINE_SDK_ROOT"))
            AddPrefixCandidates(candidates, sdkRoot);
        if (const char* prefixPath = std::getenv("CMAKE_PREFIX_PATH"))
        {
            std::stringstream stream(prefixPath);
            std::string prefix;
#ifdef _WIN32
            constexpr char separator = ';';
#else
            constexpr char separator = ':';
#endif
            while (std::getline(stream, prefix, separator))
                AddPrefixCandidates(candidates, prefix);
        }

        for (const fs::path& ancestor : GetExecutableAncestors())
        {
            AddPrefixCandidates(candidates, ancestor);
            AddPrefixCandidates(candidates, ancestor / "spark-sdk" / "Debug");
            AddPrefixCandidates(candidates, ancestor / "spark-sdk" / "Release");
            AddPrefixCandidates(candidates, ancestor / "sdk");
            AddPrefixCandidates(candidates, ancestor / "install");
        }

        for (const fs::path& candidate : candidates)
        {
            if (!IsSparkEnginePackageDirectory(candidate.string()) ||
                !PackageSupportsConfiguration(candidate, configuration))
                continue;
            std::error_code ec;
            const fs::path canonical = fs::weakly_canonical(candidate, ec);
            return (ec ? candidate.lexically_normal() : canonical).string();
        }
        return {};
    }

    std::vector<std::string> BuildPipeline::CreateConfigureArguments(const BuildSettings& settings,
                                                                     const std::string& projectRoot,
                                                                     const std::string& buildDir,
                                                                     const std::string& sparkEngineDirectory,
                                                                     const std::vector<std::string>& extraDefines)
    {
        std::vector<std::string> arguments = {"-S", std::filesystem::absolute(projectRoot).lexically_normal().string(),
                                              "-B", std::filesystem::absolute(buildDir).lexically_normal().string()};

        if (settings.platform == BuildCookPanel::TargetPlatform::WindowsX64 ||
            settings.platform == BuildCookPanel::TargetPlatform::WindowsX86)
        {
            arguments.insert(arguments.end(),
                             {"-G", "Visual Studio 17 2022", "-A",
                              settings.platform == BuildCookPanel::TargetPlatform::WindowsX86 ? "Win32" : "x64"});
        }
        else
        {
            arguments.insert(arguments.end(), {"-G", "Ninja", "-DCMAKE_BUILD_TYPE=" + SettingsToBuildType(settings)});
        }

        arguments.push_back("-DSparkEngine_DIR=" +
                            std::filesystem::absolute(sparkEngineDirectory).lexically_normal().string());
        for (const std::string& define : extraDefines)
            arguments.push_back("-D" + define);
        return arguments;
    }

    std::string BuildPipeline::SettingsToBuildType(const BuildSettings& settings)
    {
        switch (settings.profile)
        {
        case BuildCookPanel::BuildProfile::Debug:
            return "Debug";
        case BuildCookPanel::BuildProfile::Development:
            return "RelWithDebInfo";
        case BuildCookPanel::BuildProfile::Release:
        case BuildCookPanel::BuildProfile::Shipping:
            return "Release";
        default:
            return "Release";
        }
    }

    std::vector<std::string> BuildPipeline::SettingsToDefines(const BuildSettings& settings)
    {
        std::vector<std::string> defs;
        if (settings.enableLTO)
            defs.push_back("CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON");
        return defs;
    }

} // namespace SparkEditor
