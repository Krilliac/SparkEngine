/**
 * @file LauncherProcess.cpp
 * @brief Cross-platform SparkLauncher child-process construction and launch.
 */

#include "LauncherProcess.h"
#include "Utils/JsonUtils.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>
#include <unordered_set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <unistd.h>
#endif

namespace SparkLauncher
{
    namespace
    {
        std::string PathToUtf8(const std::filesystem::path& path)
        {
            const std::u8string utf8 = path.generic_u8string();
            return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
        }

        std::filesystem::path ExecutablePath(const std::filesystem::path& directory, const char* name)
        {
#ifdef _WIN32
            return directory / (std::string(name) + ".exe");
#else
            return directory / name;
#endif
        }

        std::string Lowercase(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
            return value;
        }

        std::string NativeModuleExtension()
        {
#ifdef _WIN32
            return ".dll";
#elif defined(__APPLE__)
            return ".dylib";
#else
            return ".so";
#endif
        }

        std::filesystem::path CanonicalPath(const std::filesystem::path& path)
        {
            std::error_code error;
            auto canonical = std::filesystem::weakly_canonical(path, error);
            return (error ? std::filesystem::absolute(path, error) : canonical).lexically_normal();
        }

        std::string PathKey(const std::filesystem::path& path)
        {
            std::string key = PathToUtf8(CanonicalPath(path));
#ifdef _WIN32
            key = Lowercase(std::move(key));
#endif
            return key;
        }

        bool IsPathWithin(const std::filesystem::path& candidate, const std::filesystem::path& directory)
        {
            std::string candidateKey = PathKey(candidate);
            std::string directoryKey = PathKey(directory);
            if (candidateKey == directoryKey)
                return true;
            if (!directoryKey.ends_with('/'))
                directoryKey.push_back('/');
            return candidateKey.starts_with(directoryKey);
        }

        bool HasAbiSidecar(const std::filesystem::path& module)
        {
            std::error_code error;
            auto sidecar = module;
            sidecar += ".sparkabi";
            return std::filesystem::is_regular_file(sidecar, error) && !error;
        }

        bool IsSymlink(const std::filesystem::path& path)
        {
            std::error_code error;
            return std::filesystem::is_symlink(std::filesystem::symlink_status(path, error)) && !error;
        }

        bool IsSharedLibraryExtension(const std::string& extension)
        {
            return extension == ".dll" || extension == ".so" || extension == ".dylib";
        }

        std::string ModuleStem(const std::filesystem::path& path)
        {
            std::string stem = Lowercase(PathToUtf8(path.stem()));
            const std::string extension = Lowercase(PathToUtf8(path.extension()));
            if ((extension == ".so" || extension == ".dylib") && stem.starts_with("lib") && stem.size() > 3)
                stem.erase(0, 3);
            return stem;
        }

        std::filesystem::path ManifestPath(std::string path)
        {
#ifndef _WIN32
            std::replace(path.begin(), path.end(), '\\', '/');
#endif
            return std::filesystem::u8path(path);
        }

        void AppendUniqueDirectory(std::vector<std::filesystem::path>& directories,
                                   std::unordered_set<std::string>& seen, const std::filesystem::path& directory)
        {
            if (!directory.empty() && seen.insert(PathKey(directory)).second)
                directories.push_back(CanonicalPath(directory));
        }

        std::vector<std::filesystem::path> DevelopmentModuleDirectories(
            const std::filesystem::path& binaryDirectory, const std::filesystem::path& projectRoot)
        {
            std::vector<std::filesystem::path> directories;
            std::unordered_set<std::string> seen;
            const auto buildRoot = projectRoot / "build";
            AppendUniqueDirectory(directories, seen, buildRoot);

            constexpr const char* configurations[] = {"Debug", "RelWithDebInfo", "Release", "MinSizeRel"};
            const std::string binaryLeaf = Lowercase(PathToUtf8(binaryDirectory.filename()));
            bool selectedConfiguration = false;
            for (const char* configuration : configurations)
            {
                if (binaryLeaf != Lowercase(configuration))
                    continue;
                AppendUniqueDirectory(directories, seen, buildRoot / configuration);
                AppendUniqueDirectory(directories, seen, buildRoot / configuration / configuration);
                selectedConfiguration = true;
            }
            if (!selectedConfiguration)
                for (const char* configuration : configurations)
                {
                    AppendUniqueDirectory(directories, seen, buildRoot / configuration);
                    AppendUniqueDirectory(directories, seen, buildRoot / configuration / configuration);
                }
            AppendUniqueDirectory(directories, seen, binaryDirectory);
            return directories;
        }

        std::expected<std::vector<std::filesystem::path>, std::string> FindModuleMatches(
            const std::vector<std::filesystem::path>& directories, const std::string& wantedStem)
        {
            std::vector<std::filesystem::path> matches;
            std::unordered_set<std::string> seen;
            for (const auto& directory : directories)
            {
                std::error_code error;
                std::filesystem::directory_iterator iterator(
                    directory, std::filesystem::directory_options::skip_permission_denied, error);
                const std::filesystem::directory_iterator end;
                while (!error && iterator != end)
                {
                    const auto& entry = *iterator;
                    std::error_code entryError;
                    const auto candidate = entry.path();
                    if (entry.is_regular_file(entryError) && !entryError &&
                        Lowercase(PathToUtf8(candidate.extension())) == NativeModuleExtension() &&
                        ModuleStem(candidate) == wantedStem && HasAbiSidecar(candidate))
                    {
                        const auto canonical = CanonicalPath(candidate);
                        if (!IsPathWithin(canonical, directory))
                            return std::unexpected("Module symlink escapes its discovery directory: " +
                                                   PathToUtf8(candidate));
                        if (seen.insert(PathKey(canonical)).second)
                            matches.push_back(canonical);
                    }
                    iterator.increment(error);
                }
            }
            std::sort(matches.begin(), matches.end());
            return matches;
        }

        std::expected<std::filesystem::path, std::string> ResolveModule(
            const std::filesystem::path& manifestDirectory, const std::string& declaredPath,
            const std::vector<std::filesystem::path>& searchDirectories)
        {
            const auto declared = ManifestPath(declaredPath);
            const std::string extension = Lowercase(PathToUtf8(declared.extension()));
            if (!IsSharedLibraryExtension(extension))
                return std::unexpected("Manifest uses an unsupported shared-library extension: " + declaredPath);

            if (declared.is_relative())
            {
                for (const auto& part : declared)
                    if (part == "..")
                        return std::unexpected("Project module path must not escape its manifest directory: " +
                                               declaredPath);
            }

            std::error_code error;
            const auto direct = declared.is_absolute() ? declared : manifestDirectory / declared;
            if (extension == NativeModuleExtension() && std::filesystem::is_regular_file(direct, error) && !error &&
                HasAbiSidecar(direct))
            {
                const auto canonical = CanonicalPath(direct);
                if (declared.is_relative() && !IsPathWithin(canonical, manifestDirectory))
                    return std::unexpected("Project module symlink escapes its manifest directory: " + declaredPath);
                return canonical;
            }
            if (declared.is_absolute())
                return std::unexpected("Absolute project module or ABI sidecar not found: " + declaredPath);

            auto matches = FindModuleMatches(searchDirectories, ModuleStem(declared));
            if (!matches)
                return std::unexpected(matches.error());
            if (matches->empty())
                return std::unexpected("No native built module with an ABI sidecar matches '" + declaredPath + "'");
            if (matches->size() != 1)
                return std::unexpected("Multiple built modules match '" + declaredPath +
                                       "' across the active launch context; remove stale outputs");
            return matches->front();
        }

        std::expected<Spark::Json::Value, std::string> ReadAndResolveManifest(
            const std::filesystem::path& manifest, const std::vector<std::filesystem::path>& searchDirectories)
        {
            std::ifstream input(manifest, std::ios::binary);
            if (!input)
                return std::unexpected("Could not open project module manifest: " + PathToUtf8(manifest));
            const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            Spark::Json::Value root;
            std::string parseError;
            if (!Spark::Json::ParseStrict(content, &root, &parseError) || !root.IsObject())
                return std::unexpected("Project module manifest is not valid JSON: " + PathToUtf8(manifest) +
                                       (parseError.empty() ? std::string{} : " (" + parseError + ")"));

            auto& modules = root["modules"];
            if (!modules.IsArray() || modules.Size() == 0)
                return std::unexpected("Project module manifest must contain a non-empty modules array: " +
                                       PathToUtf8(manifest));
            for (size_t index = 0; index < modules.Size(); ++index)
            {
                auto& entry = modules[index];
                if (!entry.IsObject() || !entry["path"].IsString() || entry["path"].AsString().empty())
                    return std::unexpected("Project module manifest entry " + std::to_string(index) +
                                           " must contain a non-empty string path");
                auto resolved = ResolveModule(manifest.parent_path(), entry["path"].AsString(), searchDirectories);
                if (!resolved)
                    return std::unexpected(resolved.error());
                entry["path"] = Spark::Json::Value(PathToUtf8(*resolved));
            }
            return root;
        }

        struct GameLaunchContext
        {
            std::filesystem::path executable;
            std::filesystem::path workingDirectory;
            std::filesystem::path projectFile;
            std::filesystem::path manifest;
        };

        std::expected<GameLaunchContext, std::string> ResolveGameLaunchContext(
            const std::filesystem::path& binaryDirectory, const std::filesystem::path& projectFile)
        {
            const auto projectRoot = CanonicalPath(projectFile.parent_path());
            const auto sourceManifest = projectRoot / "spark.modules.json";
            std::error_code error;
            if (!std::filesystem::is_regular_file(sourceManifest, error) || error)
                return std::unexpected("Project module manifest not found: " + PathToUtf8(sourceManifest));

            const auto packageContext = [&](const std::filesystem::path& packageRoot)
                -> std::expected<GameLaunchContext, std::string>
            {
                const auto canonicalPackageRoot = CanonicalPath(packageRoot);
                const auto packageManifest = canonicalPackageRoot / "spark.modules.json";
                const auto packageExecutable = ExecutablePath(canonicalPackageRoot, "SparkEngine");
                if (!std::filesystem::is_regular_file(packageManifest, error) || error ||
                    !std::filesystem::is_regular_file(packageExecutable, error) || error)
                    return std::unexpected("Packaged runtime is incomplete under " + PathToUtf8(canonicalPackageRoot));
                auto resolved = ReadAndResolveManifest(packageManifest, {canonicalPackageRoot});
                if (!resolved)
                    return std::unexpected(resolved.error());
                auto packagedProject = canonicalPackageRoot / projectFile.filename();
                if (!std::filesystem::is_regular_file(packagedProject, error) || error)
                    packagedProject = projectFile;
                return GameLaunchContext{CanonicalPath(packageExecutable), canonicalPackageRoot,
                                         CanonicalPath(packagedProject),
                                         CanonicalPath(packageManifest)};
            };

            if (std::filesystem::is_regular_file(projectRoot / "manifest.json", error) && !error)
                return packageContext(projectRoot);

            const auto buildRoot = projectRoot / "build";
            if (IsSymlink(buildRoot) ||
                (std::filesystem::exists(buildRoot, error) && !error && !IsPathWithin(buildRoot, projectRoot)))
                return std::unexpected("Project build directory escapes the project through a symlink: " +
                                       PathToUtf8(buildRoot));

            auto resolved = ReadAndResolveManifest(sourceManifest,
                                                   DevelopmentModuleDirectories(binaryDirectory, projectRoot));
            if (!resolved && resolved.error().starts_with("No native built module"))
            {
                const auto packageRoot = projectRoot / "Build" / "Output";
                auto package = packageContext(packageRoot);
                if (package)
                    return package;
            }
            if (!resolved)
                return std::unexpected(resolved.error());

            const auto generatedDirectory = buildRoot / ".spark-launcher";
            if (IsSymlink(generatedDirectory) ||
                (std::filesystem::exists(generatedDirectory, error) && !error &&
                 !IsPathWithin(generatedDirectory, projectRoot)))
                return std::unexpected("Launcher manifest output path escapes the project through a symlink: " +
                                       PathToUtf8(generatedDirectory));
            error.clear();
            std::filesystem::create_directories(generatedDirectory, error);
            if (error || !IsPathWithin(generatedDirectory, projectRoot))
                return std::unexpected("Could not create a project-contained launcher manifest directory: " +
                                       PathToUtf8(generatedDirectory));
            const auto generatedManifest = generatedDirectory / "spark.modules.json";
            if (IsSymlink(generatedManifest) ||
                (std::filesystem::exists(generatedManifest, error) && !error &&
                 !IsPathWithin(generatedManifest, generatedDirectory)))
                return std::unexpected("Resolved launcher manifest is a symlink outside its output directory: " +
                                       PathToUtf8(generatedManifest));
            std::ofstream output(generatedManifest, std::ios::binary | std::ios::trunc);
            if (!output)
                return std::unexpected("Could not write resolved launcher manifest: " + PathToUtf8(generatedManifest));
            output << Spark::Json::StringifyPretty(*resolved) << '\n';
            output.close();
            if (!output)
                return std::unexpected("Could not finish resolved launcher manifest: " + PathToUtf8(generatedManifest));
            return GameLaunchContext{ExecutablePath(binaryDirectory, "SparkEngine"), projectRoot,
                                     CanonicalPath(projectFile), CanonicalPath(generatedManifest)};
        }

#ifdef _WIN32
        void AppendQuotedArgument(std::wstring& commandLine, const std::wstring& argument)
        {
            commandLine.push_back(L'"');
            size_t backslashes = 0;
            for (const wchar_t character : argument)
            {
                if (character == L'\\')
                {
                    ++backslashes;
                    continue;
                }
                if (character == L'"')
                {
                    commandLine.append(backslashes * 2 + 1, L'\\');
                    commandLine.push_back(L'"');
                    backslashes = 0;
                    continue;
                }
                commandLine.append(backslashes, L'\\');
                backslashes = 0;
                commandLine.push_back(character);
            }
            commandLine.append(backslashes * 2, L'\\');
            commandLine.push_back(L'"');
        }
#endif
    } // namespace

    std::expected<LaunchRequest, std::string> BuildLaunchRequest(const std::filesystem::path& binaryDirectory,
                                                                 const std::filesystem::path& projectFile,
                                                                 LaunchTarget target)
    {
        std::error_code error;
        if (!std::filesystem::is_regular_file(projectFile, error))
            return std::unexpected("Project file not found: " + projectFile.string());
        if (projectFile.extension() != ".sparkproject")
            return std::unexpected("Expected a .sparkproject file: " + projectFile.string());

        LaunchRequest request;
        request.workingDirectory = projectFile.parent_path();
        switch (target)
        {
        case LaunchTarget::Editor:
            request.executable = ExecutablePath(binaryDirectory, "SparkEditor");
            request.arguments = {"--project", projectFile.string()};
            break;
        case LaunchTarget::Game:
        {
            auto context = ResolveGameLaunchContext(binaryDirectory, projectFile);
            if (!context)
                return std::unexpected(context.error());
            request.executable = context->executable;
            request.workingDirectory = context->workingDirectory;
            request.arguments = {"-manifest", PathToUtf8(context->manifest), "--project",
                                 PathToUtf8(context->projectFile)};
            break;
        }
        case LaunchTarget::DedicatedServer:
        {
            const std::filesystem::path config = request.workingDirectory / "Config" / "server.ini";
            if (!std::filesystem::is_regular_file(config, error))
                return std::unexpected("Dedicated server config not found: " + config.string());
            request.executable = ExecutablePath(binaryDirectory, "SparkServer");
            request.arguments = {"--config", config.string()};
            break;
        }
        case LaunchTarget::ServiceTopology:
            request.executable = ExecutablePath(binaryDirectory, "SparkEditor");
            request.arguments = {"--project", projectFile.string(), "--open-panel", "ServiceTopology"};
            break;
        }

        if (!std::filesystem::is_regular_file(request.executable, error))
            return std::unexpected(std::string(LaunchTargetName(target)) +
                                   " executable not found: " + request.executable.string());
        return request;
    }

    std::expected<void, std::string> LaunchDetached(const LaunchRequest& request)
    {
#ifdef _WIN32
        std::wstring commandLine;
        AppendQuotedArgument(commandLine, request.executable.wstring());
        for (const auto& argument : request.arguments)
        {
            commandLine.push_back(L' ');
            AppendQuotedArgument(commandLine, std::filesystem::u8path(argument).wstring());
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const auto executable = request.executable.wstring();
        const auto workingDirectory = request.workingDirectory.wstring();
        const BOOL started = CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                                            DETACHED_PROCESS, nullptr, workingDirectory.c_str(), &startup, &process);
        if (!started)
            return std::unexpected("CreateProcess failed (error " + std::to_string(GetLastError()) + ")");
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
#else
        const pid_t child = fork();
        if (child < 0)
            return std::unexpected(std::string("fork failed: ") + std::strerror(errno));
        if (child == 0)
        {
            (void)setsid();
            if (!request.workingDirectory.empty() && chdir(request.workingDirectory.c_str()) != 0)
                _exit(126);

            std::vector<char*> arguments;
            std::string executable = request.executable.string();
            arguments.reserve(request.arguments.size() + 2);
            arguments.push_back(executable.data());
            for (const auto& argument : request.arguments)
                arguments.push_back(const_cast<char*>(argument.c_str()));
            arguments.push_back(nullptr);
            execv(executable.c_str(), arguments.data());
            _exit(127);
        }
#endif
        return {};
    }

    const char* LaunchTargetName(LaunchTarget target)
    {
        switch (target)
        {
        case LaunchTarget::Editor:
            return "Editor";
        case LaunchTarget::Game:
            return "Game";
        case LaunchTarget::DedicatedServer:
            return "Dedicated server";
        case LaunchTarget::ServiceTopology:
            return "Service topology";
        }
        return "Spark process";
    }
} // namespace SparkLauncher
