/**
 * @file ModuleManager.cpp
 * @brief Multi-module loader and lifecycle manager implementation
 */

#include "ModuleManager.h"
#include "ModuleSidecar.h"
#include "Contracts.h"
#include "EngineContext.h"
#include "FaultIsolation.h"
#include "IGameModule.h"
#include "Spark/ModuleABI.h"
#include "Spark/Version.h"
#include "Utils/CrashHandler.h"
#include "Engine/SaveSystem/SaveSystem.h"
#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif
#include "Utils/SparkConsole.h"
#include "Utils/InvalidStateDetector.h"
#include "Utils/JsonUtils.h"
#include "Utils/Validate.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <exception>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#endif // SPARK_PLATFORM_WINDOWS
#else
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// =============================================================================
// ImGui cross-DLL injection payload (set by the engine exe before module load)
// =============================================================================
namespace
{
    void* s_imguiContext = nullptr;
    void* s_imguiAllocFn = nullptr;
    void* s_imguiFreeFn = nullptr;
    void* s_imguiUserData = nullptr;
    std::mutex s_teardownLifecycleEvidenceMutex;
    ModuleManager::LifecycleEvidence s_lastTeardownLifecycleEvidence;
    std::atomic<uint64_t> s_moduleRegistrationSerial{0};

    std::string MakeModuleRegistrationOwner(std::string_view moduleName)
    {
        return std::string(moduleName) + "#" +
               std::to_string(s_moduleRegistrationSerial.fetch_add(1, std::memory_order_relaxed));
    }

    /// Whether a registration scope wraps a module's OnLoad or its OnUnload.
    enum class ModuleRegistrationPhase : uint8_t
    {
        Load,
        Teardown,
    };

    /// Attributes every host registry write and name-based removal to one module image.
    /// Module teardown removes registrations by shared names; this scope keeps an
    /// outgoing image from removing what its hot-reload replacement registered.
    /// A Teardown scope also stops the outgoing image from overwriting a network
    /// handler the replacement installed (see NetworkManager::ScopedRegistrationOwner).
    struct ModuleRegistrationScope final
    {
        ModuleRegistrationScope(const std::string& ownerId, ModuleRegistrationPhase phase)
            : console(Spark::SimpleConsole::GetInstance(), ownerId),
              detector(Spark::InvalidStateDetector::GetInstance(), ownerId),
              serializers(Spark::ComponentSerializerRegistry::GetInstance(), ownerId)
#ifdef ENABLE_NETWORKING
              ,
              network(Spark::Net::NetworkManager::GetInstance(), ownerId, phase == ModuleRegistrationPhase::Teardown)
#endif
        {
            (void)phase;
        }

        Spark::SimpleConsole::ScopedRegistrationOwner console;
        Spark::InvalidStateDetector::ScopedRegistrationOwner detector;
        Spark::ComponentSerializerRegistry::ScopedRegistrationOwner serializers;
#ifdef ENABLE_NETWORKING
        Spark::Net::NetworkManager::ScopedRegistrationOwner network;
#endif
    };

    void AccumulateLifecycleEvidence(ModuleManager::LifecycleEvidence& target,
                                     const ModuleManager::LifecycleEvidence& source)
    {
        target.initialized += source.initialized;
        target.updated += source.updated;
        target.fixedUpdated += source.fixedUpdated;
        target.rendered += source.rendered;
        target.unloaded += source.unloaded;
        target.faults += source.faults;
        for (const auto& sourceRecord : source.modules)
        {
            auto targetRecord = std::find_if(target.modules.begin(), target.modules.end(),
                                             [&sourceRecord](const ModuleManager::ModuleLifecycleRecord& record)
                                             { return record.module == sourceRecord.module; });
            if (targetRecord == target.modules.end())
            {
                target.modules.push_back(sourceRecord);
                continue;
            }

            targetRecord->createModule += sourceRecord.createModule;
            targetRecord->onLoad += sourceRecord.onLoad;
            targetRecord->onUpdate += sourceRecord.onUpdate;
            targetRecord->onFixedUpdate += sourceRecord.onFixedUpdate;
            targetRecord->onRender += sourceRecord.onRender;
            targetRecord->onUnload += sourceRecord.onUnload;
            targetRecord->destroyModule += sourceRecord.destroyModule;
            targetRecord->faults += sourceRecord.faults;
        }
    }

    void PublishTeardownLifecycleEvidence(const ModuleManager::LifecycleEvidence& evidence)
    {
        const std::scoped_lock lock(s_teardownLifecycleEvidenceMutex);
        s_lastTeardownLifecycleEvidence = evidence;
    }

    std::filesystem::path PathFromUtf8(std::string_view path)
    {
        return std::filesystem::u8path(path.begin(), path.end());
    }

    using Spark::ModuleSidecar::SidecarPath;
    using Spark::ModuleSidecar::ValidateModuleSidecar;

    std::string PathToUtf8(const std::filesystem::path& path)
    {
        const std::u8string utf8 = path.generic_u8string();
        return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    }


    /**
     * @brief Rewrite a module path into the shared-library form this host builds.
     *
     * spark.modules.json ships one path per module and every generated manifest
     * writes the Windows form ("Blank3D.dll"), so a manifest launch resolved
     * nothing on Linux/macOS where the artifact is "libBlank3D.so"/".dylib".
     * The prefix/suffix pair is the same one DiscoverModuleCandidates applies.
     *
     * @return The host-native sibling path, or an empty path when @p modulePath
     *         already carries the host's form or has no recognised module suffix.
     */
    std::filesystem::path HostNativeModulePath(const std::filesystem::path& modulePath)
    {
#ifdef _WIN32
        constexpr std::string_view hostPrefix = "";
        constexpr std::string_view hostSuffix = ".dll";
#elif defined(__APPLE__)
        constexpr std::string_view hostPrefix = "lib";
        constexpr std::string_view hostSuffix = ".dylib";
#else
        constexpr std::string_view hostPrefix = "lib";
        constexpr std::string_view hostSuffix = ".so";
#endif
        std::string extension = PathToUtf8(modulePath.extension());
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (extension != ".dll" && extension != ".so" && extension != ".dylib")
            return {};

        std::string stem = PathToUtf8(modulePath.stem());
        if (stem.empty())
            return {};
        // A POSIX manifest carries the "lib" prefix; strip it before re-applying
        // the host's own prefix so the stem round-trips exactly. Windows names
        // never carry it, so a module genuinely called "libraryModule.dll" is
        // left alone.
        if (extension != ".dll" && stem.starts_with("lib") && stem.size() > 3)
            stem.erase(0, 3);

        std::filesystem::path candidate = modulePath.parent_path();
        candidate /= PathFromUtf8(std::string(hostPrefix) + stem + std::string(hostSuffix));
        if (candidate == modulePath)
            return {};
        return candidate;
    }

    template <typename FunctionType> FunctionType ResolveModuleExport(void* handle, const char* name)
    {
#ifdef _WIN32
        return reinterpret_cast<FunctionType>(GetProcAddress(static_cast<HMODULE>(handle), name));
#else
        return reinterpret_cast<FunctionType>(dlsym(handle, name));
#endif
    }

    void CloseModuleLibrary(void* handle)
    {
        if (!handle)
            return;
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(handle));
#else
        dlclose(handle);
#endif
    }

    /// Upper bound for spark.modules.json; real manifests are well under 1 KiB.
    constexpr std::uintmax_t kMaxManifestBytes = std::uintmax_t{1024} * 1024;

    /// One module's declared dependencies, as the graph check sees them.
    struct ModuleDependencyNode
    {
        std::string name;
        std::vector<std::string> dependencies;
    };

    /**
     * @brief Describe why a module dependency graph cannot be initialized.
     *
     * SortModules orders what it can and appends the rest, so a declared
     * dependency that is not loaded, or a dependency cycle, would otherwise
     * start a module before (or without) the modules it said it needs.
     *
     * @return An empty string when every dependency names a node and the graph
     *         is acyclic; otherwise the first problem found.
     */
    std::string DescribeDependencyGraphError(const std::vector<ModuleDependencyNode>& nodes)
    {
        std::unordered_map<std::string, size_t> nameToIndex;
        for (size_t i = 0; i < nodes.size(); ++i)
            nameToIndex.emplace(nodes[i].name, i);

        std::vector<std::vector<size_t>> dependents(nodes.size());
        std::vector<size_t> unmet(nodes.size(), 0);
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            for (const std::string& dependency : nodes[i].dependencies)
            {
                const auto it = nameToIndex.find(dependency);
                if (it == nameToIndex.end())
                {
                    return "Module '" + nodes[i].name + "' depends on '" + dependency + "', which is not loaded";
                }
                dependents[it->second].push_back(i);
                ++unmet[i];
            }
        }

        std::vector<size_t> ready;
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            if (unmet[i] == 0)
            {
                ready.push_back(i);
            }
        }
        size_t resolved = 0;
        while (!ready.empty())
        {
            const size_t node = ready.back();
            ready.pop_back();
            ++resolved;
            for (const size_t dependent : dependents[node])
            {
                if (--unmet[dependent] == 0)
                {
                    ready.push_back(dependent);
                }
            }
        }
        if (resolved == nodes.size())
        {
            return {};
        }

        std::string cycle;
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            if (unmet[i] != 0)
            {
                cycle += (cycle.empty() ? "'" : ", '") + nodes[i].name + "'";
            }
        }
        return "Circular module dependency involving " + cycle;
    }

    ModuleDependencyNode MakeDependencyNode(const std::string& name, const Spark::IModule& instance)
    {
        ModuleDependencyNode node{name, {}};
        const Spark::ModuleInfo info = instance.GetModuleInfo();
        for (int d = 0; d < info.dependencyCount && info.dependencies; ++d)
        {
            if (info.dependencies[d])
            {
                node.dependencies.emplace_back(info.dependencies[d]);
            }
        }
        return node;
    }

#ifndef _WIN32
    class ScopedStagedModuleImage
    {
      public:
        ~ScopedStagedModuleImage() { Cleanup(); }

        ScopedStagedModuleImage(const ScopedStagedModuleImage&) = delete;
        ScopedStagedModuleImage& operator=(const ScopedStagedModuleImage&) = delete;
        ScopedStagedModuleImage() = default;

        void Set(std::filesystem::path path) { m_path = std::move(path); }
        [[nodiscard]] const std::filesystem::path& Get() const { return m_path; }

        void Disarm() { m_path.clear(); }

      private:
        void Cleanup()
        {
            if (m_path.empty())
                return;
            std::error_code ignored;
            std::filesystem::remove_all(m_path.parent_path(), ignored);
            m_path.clear();
        }

        std::filesystem::path m_path;
    };

    bool IsSiblingSharedLibrary(const std::filesystem::path& path)
    {
        const std::string filename = path.filename().string();
        // ABI metadata is named `<module>.so.sparkabi` on Linux.  The
        // versioned-library check below intentionally accepts names such as
        // `libfoo.so.1`, but must not classify the sidecar as a loadable
        // sibling.  In particular, the source module's sidecar has already
        // been copied into the private stage, so trying to symlink it again
        // fails with `file_exists` and prevents every Linux module load.
        if (filename.ends_with(".sparkabi"))
            return false;
#if defined(__APPLE__)
        return filename.ends_with(".dylib");
#else
        return filename.ends_with(".so") || filename.find(".so.") != std::string::npos;
#endif
    }

    bool StageSiblingSharedLibraries(const std::filesystem::path& source, const std::filesystem::path& stagingDirectory,
                                     std::string& error)
    {
        constexpr size_t kMaximumSiblingLibraries = 256;
        size_t stagedCount = 0;
        std::error_code iteratorError;
        for (std::filesystem::directory_iterator it(source.parent_path(), iteratorError), end;
             !iteratorError && it != end; it.increment(iteratorError))
        {
            const std::filesystem::path sibling = it->path();
            if (sibling.filename() == source.filename() || !IsSiblingSharedLibrary(sibling))
                continue;

            std::error_code typeError;
            if (!std::filesystem::is_regular_file(sibling, typeError) || typeError)
                continue;
            if (++stagedCount > kMaximumSiblingLibraries)
            {
                error = "module directory exceeds the 256 sibling shared-library staging limit";
                return false;
            }

            std::error_code linkError;
            const std::filesystem::path target = std::filesystem::weakly_canonical(sibling, linkError);
            if (linkError)
            {
                error = "failed to resolve sibling module dependency: " + linkError.message();
                return false;
            }
            std::filesystem::create_symlink(target, stagingDirectory / sibling.filename(), linkError);
            if (linkError)
            {
                error = "failed to stage sibling module dependency: " + linkError.message();
                return false;
            }
        }
        if (iteratorError)
        {
            error = "failed to enumerate sibling module dependencies: " + iteratorError.message();
            return false;
        }
        return true;
    }

    bool StageModuleForPosixLoad(const std::filesystem::path& source, ScopedStagedModuleImage& staged,
                                 std::string& error)
    {
        static std::atomic<uint64_t> stageSerial{0};
        const uint64_t timestamp = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());

        for (uint32_t attempt = 0; attempt < 32; ++attempt)
        {
            const uint64_t serial = stageSerial.fetch_add(1, std::memory_order_relaxed);
            const std::filesystem::path stagingDirectory =
                std::filesystem::temp_directory_path() /
                ("spark-module-stage-" + std::to_string(static_cast<uint64_t>(::getpid())) + "-" +
                 std::to_string(timestamp) + "-" + std::to_string(serial));
            std::error_code directoryError;
            if (!std::filesystem::create_directory(stagingDirectory, directoryError))
            {
                if (directoryError == std::errc::file_exists)
                    continue;
                error = "failed to create private module staging directory: " + directoryError.message();
                return false;
            }
            if (::chmod(stagingDirectory.c_str(), S_IRWXU) != 0)
            {
                std::error_code ignored;
                std::filesystem::remove(stagingDirectory, ignored);
                error = "failed to secure private module staging directory";
                return false;
            }

            const std::filesystem::path candidate = stagingDirectory / source.filename();
            const std::filesystem::path candidateSidecar = SidecarPath(candidate);

            std::error_code copyError;
            if (!std::filesystem::copy_file(source, candidate, std::filesystem::copy_options::none, copyError))
            {
                if (copyError == std::errc::file_exists)
                {
                    std::error_code ignored;
                    std::filesystem::remove_all(stagingDirectory, ignored);
                    continue;
                }
                // remove_all: a copy that failed part-way (EFBIG, ENOSPC) leaves the
                // partial image behind, and remove() of a non-empty directory fails.
                std::error_code ignored;
                std::filesystem::remove_all(stagingDirectory, ignored);
                error = "failed to stage module image: " + copyError.message();
                return false;
            }

            copyError.clear();
            if (!std::filesystem::copy_file(SidecarPath(source), candidateSidecar, std::filesystem::copy_options::none,
                                            copyError))
            {
                std::error_code ignored;
                std::filesystem::remove(candidate, ignored);
                ignored.clear();
                std::filesystem::remove(stagingDirectory, ignored);
                if (copyError == std::errc::file_exists)
                    continue;
                error = "failed to stage module ABI sidecar: " + copyError.message();
                return false;
            }

            if (!StageSiblingSharedLibraries(source, stagingDirectory, error))
            {
                std::error_code ignored;
                std::filesystem::remove_all(stagingDirectory, ignored);
                return false;
            }

            staged.Set(candidate);
            return true;
        }

        error = "failed to reserve a unique staged module image";
        return false;
    }
#endif
} // namespace

void ModuleManager::SetImGuiInjection(void* context, void* allocFn, void* freeFn, void* userData)
{
    s_imguiContext = context;
    s_imguiAllocFn = allocFn;
    s_imguiFreeFn = freeFn;
    s_imguiUserData = userData;
}

// =============================================================================
// Legacy IGameModule -> IModule adapter
// =============================================================================

/**
 * @brief Wraps a legacy IGameModule implementation behind the new IModule interface
 *
 * This allows existing game DLLs that export CreateGameModule/DestroyGameModule
 * to work with the new ModuleManager without any changes.
 */
class LegacyModuleAdapter : public Spark::IModule
{
  public:
    LegacyModuleAdapter(IGameModule* legacy, DestroyGameModuleFn destroyFn)
        : m_legacy(legacy), m_legacyDestroyFn(destroyFn)
    {
    }

    ~LegacyModuleAdapter() override
    {
        if (m_legacy && m_legacyDestroyFn)
            m_legacyDestroyFn(m_legacy);
        m_legacy = nullptr;
    }

    Spark::ModuleInfo GetModuleInfo() const override
    {
        Spark::ModuleInfo info{};
        info.name = m_legacy ? m_legacy->GetGameName() : "Unknown";
        info.version = m_legacy ? m_legacy->GetGameVersion() : "0.0.0";
        info.sdkVersion = SPARK_SDK_VERSION;
        info.loadOrder = 1000;
        return info;
    }

    bool OnLoad(Spark::IEngineContext* context) override
    {
        if (!m_legacy)
            return false;
        return m_legacy->Initialize(context->GetGraphics(), context->GetInput());
    }

    void OnUnload() override
    {
        if (m_legacy)
            m_legacy->Shutdown();
    }

    void OnUpdate(float deltaTime) override
    {
        if (m_legacy && !m_legacy->IsPaused())
            m_legacy->Update(deltaTime);
    }

    void OnRender() override
    {
        if (m_legacy)
            m_legacy->Render();
    }

    void OnResize(int width, int height) override
    {
        if (m_legacy)
            m_legacy->OnResize(width, height);
    }

    /** @brief Access the underlying legacy module (for backward-compat console commands) */
    IGameModule* GetLegacyModule() const { return m_legacy; }

  private:
    IGameModule* m_legacy = nullptr;
    DestroyGameModuleFn m_legacyDestroyFn = nullptr;
};

// =============================================================================
// ModuleManager implementation
// =============================================================================

ModuleManager::~ModuleManager()
{
    UnloadAll();
    if (m_publishTeardownLifecycleEvidence)
        PublishTeardownLifecycleEvidence(m_lifecycleEvidence);
}

ModuleManager::LifecycleEvidence ModuleManager::GetLastTeardownLifecycleEvidence()
{
    const std::scoped_lock lock(s_teardownLifecycleEvidenceMutex);
    return s_lastTeardownLifecycleEvidence;
}

void ModuleManager::PublishLifecycleEvidence() const
{
    PublishTeardownLifecycleEvidence(m_lifecycleEvidence);
}

std::string ModuleManager::LibraryTargetName(std::string_view libraryPath)
{
    // Split on either separator so a Windows-style path is handled on POSIX too.
    const size_t separator = libraryPath.find_last_of("/\\");
    std::string_view filename = separator == std::string_view::npos ? libraryPath : libraryPath.substr(separator + 1);
    const size_t extension = filename.rfind('.');
    if (extension != std::string_view::npos && extension > 0)
        filename = filename.substr(0, extension);
#ifndef _WIN32
    // CMAKE_SHARED_LIBRARY_PREFIX is "lib" on Linux and macOS; the Windows
    // record names the bare target, so strip it to keep one identity.
    if (filename.size() > 3 && filename.starts_with("lib"))
        filename.remove_prefix(3);
#endif
    return std::string(filename);
}

std::string ModuleManager::FormatLifecycleRecord(const ModuleLifecycleRecord& record)
{
    return std::format("SPARK_MODULE_LIFECYCLE module={} create={} load={} update={} fixed={} render={} unload={} "
                       "destroy={} faults={}",
                       LibraryTargetName(record.libraryPath), record.createModule, record.onLoad, record.onUpdate,
                       record.onFixedUpdate, record.onRender, record.onUnload, record.destroyModule, record.faults);
}

bool ModuleManager::LoadModule(const std::string& path)
{
    auto& console = Spark::SimpleConsole::GetInstance();
    m_lastLoadError.clear();

    const auto failLoad = [&](std::string message)
    {
        // A rejection after dlopen has already unmapped the image; drop its
        // recorded range so a later crash never attributes frames to it.
        RefreshCrashModuleIdentities();
        m_lastLoadError = std::move(message);
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "%s", m_lastLoadError.c_str());
        console.LogError(m_lastLoadError);
        return false;
    };

    if (path.empty())
        return failLoad("Module path must not be empty");

    SPARK_EXPECTS(!path.empty());
    SPARK_TRACE_ENTER(Spark::LogCategory::Core);
    SPARK_LOG_INFO(Spark::LogCategory::Core, "Loading module: %s", path.c_str());

    // Security: reject path traversal sequences
    if (path.contains(".."))
    {
        return failLoad("Module path rejected — contains '..' traversal: " + path);
    }

#ifdef _WIN32
    // The image the sidecar hashes must be the image the loader maps. A bare
    // or relative name would be hashed relative to the working directory but
    // resolved by LoadLibrary through the DLL search order, so pin, validate
    // and load one absolute path. LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR below also
    // requires an absolute path.
    std::error_code absolutePathError;
    const std::filesystem::path modulePath = std::filesystem::absolute(PathFromUtf8(path), absolutePathError);
    if (absolutePathError)
    {
        return failLoad(
            std::format("Failed to resolve module path '{}' for validation: {}", path, absolutePathError.message()));
    }
#else
    const std::filesystem::path modulePath = PathFromUtf8(path);
#endif

#ifndef _WIN32
    // A compiler or build system may atomically replace the source .so/.dylib
    // after its sidecar/hash check but before dlopen executes constructors.
    // Load a unique snapshot instead; normal rebuilds only ever replace the
    // declared source path, while the verified snapshot remains stable until
    // this module is unloaded.
    ScopedStagedModuleImage stagedImage;
    std::string stagingError;
    if (!StageModuleForPosixLoad(modulePath, stagedImage, stagingError))
    {
        return failLoad(std::format("Failed to stage module '{}' for validation: {}", path, stagingError));
    }
    const std::filesystem::path& validatedModulePath = stagedImage.Get();
#else
    const std::filesystem::path& validatedModulePath = modulePath;
#endif

#ifdef _WIN32
    // Prevent a concurrent rebuild, rename, or delete from changing the image
    // between the sidecar hash check and LoadLibraryExW. The loader may still
    // acquire its own read handle, while writers and delete/replace operations
    // remain excluded until the mapped image has been opened.
    HANDLE pinnedModule = CreateFileW(modulePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (pinnedModule == INVALID_HANDLE_VALUE)
    {
        return failLoad(std::format("Failed to pin module '{}' for validation (error {})", path, GetLastError()));
    }
#endif

    // Preflight the sidecar before asking the OS loader to map the image. The
    // SHA-256 binds the descriptor to this exact binary, so an incompatible or
    // stale candidate is rejected before DllMain/static constructors execute.
    std::string sidecarError;
    if (!ValidateModuleSidecar(validatedModulePath, sidecarError))
    {
#ifdef _WIN32
        CloseHandle(pinnedModule);
#endif
        return failLoad(std::format("Module '{}' rejected before OS load: {}", path, sidecarError));
    }

    // Load the shared library only after the non-executing compatibility gate.
    void* handle = nullptr;
#ifdef _WIN32
    // The sidecar hash covers only this image. Its static imports are resolved
    // from the module's own directory, then the application directory,
    // AddDllDirectory entries and System32 -- never the current directory or
    // PATH, where a planted dependency would run DllMain before the in-image
    // descriptor is read. DynamicPluginHost uses the same search set.
    handle = LoadLibraryExW(modulePath.c_str(), nullptr,
                            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    CloseHandle(pinnedModule);
    if (!handle)
    {
        DWORD err = GetLastError();
        return failLoad(std::format("Failed to load module '{}' with LoadLibraryExW (error {})", path, err));
    }
#else
    const std::string loadPath = PathToUtf8(validatedModulePath);
    handle = dlopen(loadPath.c_str(), RTLD_NOW);
    if (!handle)
    {
        const char* err = dlerror();
        return failLoad(std::format("Failed to load module '{}' (staged as '{}') with dlopen(RTLD_NOW): {}", path,
                                    loadPath, err ? err : "unknown dynamic-loader error"));
    }
    // Record the module's build-id before any export is called, so a crash in
    // it symbolicates from the symbol store (tools/ops/symbolicate_crash.py).
    RefreshCrashModuleIdentities();
#endif

    // Re-read the in-image descriptor as defense in depth after the sidecar
    // has established compatibility. It has a fixed C ABI and returns an
    // integer-only POD; no C++ object, allocator, vtable, or engine pointer
    // crosses before both compatibility checks have passed.
    auto compatibilityFn =
        ResolveModuleExport<SparkGetModuleCompatibilityFn>(handle, SPARK_MODULE_COMPATIBILITY_EXPORT_NAME);
    if (!compatibilityFn)
    {
        const std::string message = std::format(
            "Module '{}' rejected before injection/factory: missing mandatory {} export. "
            "Rebuild the module with the current Spark SDK (SPARK_IMPLEMENT_MODULE exports it automatically).",
            path, SPARK_MODULE_COMPATIBILITY_EXPORT_NAME);
        CloseModuleLibrary(handle);
        return failLoad(message);
    }

    const std::string compatibilityRejection = DescribeModuleCompatibilityRejection(compatibilityFn());
    if (!compatibilityRejection.empty())
    {
        const std::string message =
            std::format("Module '{}' in-image compatibility descriptor rejected before injection/factory: {}", path,
                        compatibilityRejection);
        CloseModuleLibrary(handle);
        return failLoad(message);
    }

#ifdef _WIN32
    // Compatibility is established before any Spark injection or factory.
    // Module DLLs
    // statically link SparkEngineLib, so without this their console-command
    // registrations land in a DLL-private SimpleConsole the engine never
    // reads (module commands were silently dead on Windows).
    using InjectConsoleFn = void (*)(void*);
    if (auto inject =
            reinterpret_cast<InjectConsoleFn>(GetProcAddress(static_cast<HMODULE>(handle), "SparkModuleInjectConsole")))
    {
        inject(&Spark::SimpleConsole::GetInstance());
    }

    using InjectInvalidStateDetectorFn = void (*)(void*);
    if (auto injectDetector = reinterpret_cast<InjectInvalidStateDetectorFn>(
            GetProcAddress(static_cast<HMODULE>(handle), "SparkModuleInjectInvalidStateDetector")))
    {
        injectDetector(&Spark::InvalidStateDetector::GetInstance());
    }

    // Inject the host EngineContext the same way. SparkEngineLib is a static lib
    // linked into every module DLL, so the module's g_engineContext global is a
    // per-image copy that is null inside the module — EngineContext::Get() there
    // returns nullptr and service-locator lookups (e.g. NetworkManager) fall back to
    // dead per-module singletons. Hand the module our live context through a
    // NON-owning setter so module teardown/FreeLibrary never frees the host context.
    // (No-op until the module exports the hook via SparkSDK ModuleDllMain.h.)
    using InjectContextFn = void (*)(void*);
    if (auto injectCtx = reinterpret_cast<InjectContextFn>(
            GetProcAddress(static_cast<HMODULE>(handle), "SparkModuleInjectEngineContext")))
    {
        injectCtx(EngineContext::Get());
    }

    // Inject the host ImGui context/allocators the same way: the module's
    // statically linked ImGui copy has a per-image GImGui that must point at
    // the exe-owned context or every module ImGui call draws nothing/crashes.
    if (s_imguiContext)
    {
        using InjectImGuiFn = void (*)(void*, void*, void*, void*);
        if (auto injectImGui =
                reinterpret_cast<InjectImGuiFn>(GetProcAddress(static_cast<HMODULE>(handle), "SparkModuleInjectImGui")))
        {
            injectImGui(s_imguiContext, s_imguiAllocFn, s_imguiFreeFn, s_imguiUserData);
        }
    }
#endif

    // Try new API first: CreateModule / DestroyModule
    CreateModuleFn createFn = nullptr;
    DestroyModuleFn destroyFn = nullptr;

#ifdef _WIN32
    createFn = reinterpret_cast<CreateModuleFn>(GetProcAddress(static_cast<HMODULE>(handle), "CreateModule"));
    destroyFn = reinterpret_cast<DestroyModuleFn>(GetProcAddress(static_cast<HMODULE>(handle), "DestroyModule"));
#else
    createFn = reinterpret_cast<CreateModuleFn>(dlsym(handle, "CreateModule"));
    destroyFn = reinterpret_cast<DestroyModuleFn>(dlsym(handle, "DestroyModule"));
#endif

    if (createFn && destroyFn)
    {
        // New-style module
        Spark::IModule* instance = createFn();
        if (!instance)
        {
            const std::string message = std::format("CreateModule() returned null for '{}'", path);
#ifdef _WIN32
            FreeLibrary(static_cast<HMODULE>(handle));
#else
            dlclose(handle);
#endif
            return failLoad(message);
        }

        auto info = instance->GetModuleInfo();

        // Defense in depth: the sidecar and in-image descriptor already pinned
        // sdk_version before OS load, so this only fires for a module whose
        // hand-written ModuleInfo contradicts its own compatibility descriptor.
        if (!Spark::IsSDKCompatible(info.sdkVersion))
        {
            const std::string message =
                std::format("Module '{}' ('{}') rejected: ModuleInfo field 'sdkVersion' host expects {}, module "
                            "declares {}; stable-v1 module ABI is exact-match only (N-1 modules are not loaded)",
                            info.name, path, SPARK_SDK_VERSION, info.sdkVersion);
            destroyFn(instance);
#ifdef _WIN32
            FreeLibrary(static_cast<HMODULE>(handle));
#else
            dlclose(handle);
#endif
            return failLoad(message);
        }

        // Single-game-module policy: game modules own the simulation (physics
        // stepping, world, net sim) — two of them double-step physics and
        // corrupt each other. Addon-kind modules coexist freely.
        if (info.kind == Spark::ModuleKind::Game)
        {
            const std::string existing = GetGameModuleName();
            if (!existing.empty())
            {
                const std::string message =
                    std::format("REFUSED to load game module '{}': game module '{}' is already loaded. "
                                "One game module per process; mark libraries/extensions with "
                                "ModuleKind::Addon in their ModuleInfo.",
                                info.name, existing);
                destroyFn(instance);
#ifdef _WIN32
                FreeLibrary(static_cast<HMODULE>(handle));
#else
                dlclose(handle);
#endif
                return failLoad(message);
            }
        }

        LoadedModule entry{};
        entry.name = info.name;
        entry.path = path;
        entry.libraryHandle = handle;
        entry.instance = instance;
        entry.createFn = createFn;
        entry.destroyFn = destroyFn;
        entry.loadOrder = info.loadOrder;
        entry.isLegacyAdapter = false;
        entry.kind = info.kind;
        entry.registrationOwner = MakeModuleRegistrationOwner(info.name);
#ifndef _WIN32
        entry.transientImagePath = PathToUtf8(stagedImage.Get());
#endif

        console.LogSuccess(std::format("Loaded module: {} v{}", info.name, info.version));
        m_modules.push_back(std::move(entry));
        if (!m_modules.back().isLegacyAdapter)
        {
            ModuleLifecycleRecord& record = FindOrCreateLifecycleRecord(m_modules.back().name);
            ++record.createModule;
            // A hot-reload replacement is created by a staged manager from a
            // shadow copy; merging its evidence keeps this live-manager path.
            record.libraryPath = path;
            record.kind = info.kind;
        }
#ifndef _WIN32
        stagedImage.Disarm();
#endif
        SortModules();
        return true;
    }

    // Fall back to legacy API: CreateGameModule / DestroyGameModule
    CreateGameModuleFn legacyCreateFn = nullptr;
    DestroyGameModuleFn legacyDestroyFn = nullptr;

#ifdef _WIN32
    legacyCreateFn =
        reinterpret_cast<CreateGameModuleFn>(GetProcAddress(static_cast<HMODULE>(handle), "CreateGameModule"));
    legacyDestroyFn =
        reinterpret_cast<DestroyGameModuleFn>(GetProcAddress(static_cast<HMODULE>(handle), "DestroyGameModule"));
#else
    legacyCreateFn = reinterpret_cast<CreateGameModuleFn>(dlsym(handle, "CreateGameModule"));
    legacyDestroyFn = reinterpret_cast<DestroyGameModuleFn>(dlsym(handle, "DestroyGameModule"));
#endif

    if (legacyCreateFn && legacyDestroyFn)
    {
        IGameModule* legacyModule = legacyCreateFn();
        if (!legacyModule)
        {
            const std::string message = "CreateGameModule() returned null for '" + path + "'";
#ifdef _WIN32
            FreeLibrary(static_cast<HMODULE>(handle));
#else
            dlclose(handle);
#endif
            return failLoad(message);
        }

        // Wrap in adapter (unique_ptr for exception safety)
        auto adapterOwner = std::make_unique<LegacyModuleAdapter>(legacyModule, legacyDestroyFn);

        // For legacy modules, we use adapter's destroy which handles cleanup
        auto info = adapterOwner->GetModuleInfo();

        // Legacy CreateGameModule exports are game modules by definition —
        // the single-game-module policy applies to them too.
        {
            const std::string existing = GetGameModuleName();
            if (!existing.empty())
            {
                const std::string message =
                    std::format("REFUSED to load legacy game module '{}': game module '{}' is already "
                                "loaded (one game module per process).",
                                info.name, existing);
                // Destroy the adapter (and therefore the legacy object through
                // its DLL export) while the library code is still resident.
                adapterOwner.reset();
#ifdef _WIN32
                FreeLibrary(static_cast<HMODULE>(handle));
#else
                dlclose(handle);
#endif
                return failLoad(message);
            }
        }

        LoadedModule entry{};
        entry.name = info.name;
        entry.path = path;
        entry.libraryHandle = handle;
        entry.createFn = nullptr; // managed by adapter
        entry.destroyFn = [](Spark::IModule* mod) { delete mod; };
        entry.loadOrder = info.loadOrder;
        entry.isLegacyAdapter = true;
        entry.kind = Spark::ModuleKind::Game;
        entry.registrationOwner = MakeModuleRegistrationOwner(info.name);
#ifndef _WIN32
        entry.transientImagePath = PathToUtf8(stagedImage.Get());
#endif

        // Transfer ownership last to avoid leak if any prior line throws
        entry.instance = adapterOwner.release();

        console.LogSuccess(std::format("Loaded legacy module: {} v{}", info.name, info.version));
        m_modules.push_back(std::move(entry));
#ifndef _WIN32
        stagedImage.Disarm();
#endif
        SortModules();
        return true;
    }

    // No recognized exports
    const std::string message =
        std::format("Module '{}' has no recognized exports (CreateModule or CreateGameModule)", path);
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
    return failLoad(message);
}

bool ModuleManager::LoadModulesFromManifest(const std::string& manifestPath)
{
    auto& console = Spark::SimpleConsole::GetInstance();
    const std::filesystem::path manifestFile = PathFromUtf8(manifestPath);
    m_lastLoadError.clear();

    const auto failManifest = [&](std::string message)
    {
        m_lastLoadError = std::move(message);
        console.LogError(m_lastLoadError);
        return false;
    };

    // A manifest is a small regular file. A FIFO, a device or a symlink to one
    // (/dev/zero, /dev/urandom) has no usable size and could stream forever or
    // block the open, so refuse anything that is not a regular file before
    // opening it, and refuse a regular file past the budget before reading it.
    std::error_code statusError;
    const bool isRegularFile = std::filesystem::is_regular_file(manifestFile, statusError);
    if (statusError || !isRegularFile)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "ModuleManager: cannot open manifest '%s' as a regular file",
                        manifestPath.c_str());
        return failManifest("Could not open module manifest as a regular file: " + manifestPath);
    }
    std::error_code sizeError;
    const std::uintmax_t manifestBytes = std::filesystem::file_size(manifestFile, sizeError);
    if (sizeError)
    {
        return failManifest("Could not read module manifest size: " + manifestPath);
    }
    if (manifestBytes > kMaxManifestBytes)
    {
        return failManifest(std::format("Module manifest exceeds {} bytes: {}", kMaxManifestBytes, manifestPath));
    }

    // Always read the manifest straight from disk, bounded to one byte past the
    // budget: the file can still grow (or be swapped) after the checks above.
    // LocalFileCache is deliberately not used here; its whole-file read has no
    // limit and a cached copy would outlive an edited manifest.
    std::ifstream file(manifestFile, std::ios::binary);
    if (!file.is_open())
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "ModuleManager: cannot open manifest '%s' (errno=%d)",
                        manifestPath.c_str(), errno);
        m_lastLoadError = "Could not open module manifest: " + manifestPath;
        console.LogWarning(m_lastLoadError);
        return false;
    }
    std::string content(static_cast<std::size_t>(kMaxManifestBytes) + 1, '\0');
    file.read(content.data(), static_cast<std::streamsize>(content.size()));
    content.resize(static_cast<std::size_t>(std::max<std::streamsize>(file.gcount(), 0)));
    if (content.size() > kMaxManifestBytes)
    {
        return failManifest(std::format("Module manifest exceeds {} bytes: {}", kMaxManifestBytes, manifestPath));
    }

    const std::filesystem::path manifestDir = manifestFile.parent_path();

    Spark::Json::Value manifest;
    std::string parseError;
    if (!Spark::Json::ParseStrict(content, &manifest, &parseError) || !manifest.IsObject())
    {
        return failManifest("Module manifest is not valid JSON: " + manifestPath +
                            (parseError.empty() ? std::string{} : " (" + parseError + ")"));
    }

    const Spark::Json::Value& modules = manifest["modules"];
    if (!modules.IsArray() || modules.Size() == 0)
        return failManifest("Module manifest must contain a non-empty modules array: " + manifestPath);

    // The manifest names the exact module set this process runs. Resolve every
    // entry before loading any of them, so a malformed or missing entry fails
    // the manifest instead of starting a partial module set.
    std::vector<std::filesystem::path> resolvedPaths;
    resolvedPaths.reserve(modules.Size());
    for (size_t index = 0; index < modules.Size(); ++index)
    {
        const Spark::Json::Value& module = modules[index];
        if (!module.IsObject() || !module["path"].IsString())
        {
            return failManifest(std::format("Module manifest entry {} has no string path: {}", index, manifestPath));
        }

        const std::string modulePath = module["path"].AsString();
        if (modulePath.empty())
            return failManifest(std::format("Module manifest entry {} has an empty path: {}", index, manifestPath));

        // Resolve relative paths against manifest directory
        std::filesystem::path fullPath = PathFromUtf8(modulePath);
        if (fullPath.is_relative())
            fullPath = manifestDir / fullPath;

        // Manifests ship a single module path — every generated one writes the
        // Windows ".dll" form — so retry the host's own shared-library naming
        // before declaring the module missing. Without this, a template's own
        // spark.modules.json resolves nothing on Linux/macOS.
        if (!std::filesystem::exists(fullPath))
        {
            const std::filesystem::path hostPath = HostNativeModulePath(fullPath);
            if (!hostPath.empty() && std::filesystem::exists(hostPath))
            {
                console.LogInfo("Module manifest path '" + PathToUtf8(fullPath) + "' resolved to host image '" +
                                PathToUtf8(hostPath) + "'");
                fullPath = hostPath;
            }
        }

        if (!std::filesystem::exists(fullPath))
        {
            return failManifest("Module not found: " + PathToUtf8(fullPath));
        }
        resolvedPaths.push_back(std::move(fullPath));
    }

    // Modules loaded before this call are not part of this manifest's rollback.
    std::vector<std::string> preexistingOwners;
    preexistingOwners.reserve(m_modules.size());
    for (const auto& entry : m_modules)
    {
        preexistingOwners.push_back(entry.registrationOwner);
    }

    for (const auto& fullPath : resolvedPaths)
    {
        if (LoadModule(PathToUtf8(fullPath)))
        {
            continue;
        }

        // A rejected entry (ABI, hash, identity or policy) fails the manifest.
        // Every module this call loaded is still uninitialized; unload them so
        // the host never initializes a partial set.
        const std::string rejection = m_lastLoadError;
        for (auto entry = m_modules.begin(); entry != m_modules.end();)
        {
            if (std::find(preexistingOwners.begin(), preexistingOwners.end(), entry->registrationOwner) !=
                preexistingOwners.end())
            {
                ++entry;
                continue;
            }
            UnregisterModuleRegistrations(*entry);
            UnloadEntry(*entry);
            entry = m_modules.erase(entry);
        }
        return failManifest(std::format("Module manifest {} rejected: {}", manifestPath, rejection));
    }

    m_lastLoadError.clear();
    return true;
}

bool ModuleManager::LoadModulesFromDirectory(const std::string& directory)
{
    auto& console = Spark::SimpleConsole::GetInstance();
    m_lastLoadError.clear();
    bool anyLoaded = false;

    if (!std::filesystem::exists(PathFromUtf8(directory)))
    {
        m_lastLoadError = "Module directory does not exist: " + directory;
        console.LogWarning(m_lastLoadError);
        return false;
    }

    for (const auto& candidate : DiscoverModuleCandidates(directory))
    {
        console.LogInfo("Found candidate module: " + PathToUtf8(PathFromUtf8(candidate).filename()));
        if (LoadModule(candidate))
            anyLoaded = true;
    }

    if (anyLoaded)
        m_lastLoadError.clear();
    else if (m_lastLoadError.empty())
        m_lastLoadError = "Module directory did not contain a loadable module: " + directory;
    return anyLoaded;
}

std::vector<std::string> ModuleManager::DiscoverModuleCandidates(const std::string& directory, DiscoveryMode mode)
{
    std::vector<std::string> candidates;
    const std::filesystem::path directoryPath = PathFromUtf8(directory);

    std::error_code ec;
    if (!std::filesystem::is_directory(directoryPath, ec) || ec)
        return candidates;

#ifdef _WIN32
    const std::string ext = ".dll";
#elif defined(__APPLE__)
    const std::string ext = ".dylib";
#else
    const std::string ext = ".so";
#endif

    std::filesystem::directory_iterator iterator(directoryPath, ec);
    const std::filesystem::directory_iterator end;
    while (!ec && iterator != end)
    {
        const std::filesystem::directory_entry entry = *iterator;
        iterator.increment(ec);

        std::error_code entryError;
        if (!entry.is_regular_file(entryError) || entryError)
            continue;

        auto filePath = entry.path();
        std::string fileExtension = PathToUtf8(filePath.extension());
#ifdef _WIN32
        std::transform(fileExtension.begin(), fileExtension.end(), fileExtension.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
#endif
        if (fileExtension != ext)
            continue;

        const std::string filename = PathToUtf8(filePath.filename());

        // Common module naming patterns only — everything else is skipped.
        bool isCandidate = (filename.contains("Game") || filename.contains("Module") || filename.contains("Plugin"));

        // Skip system/runtime DLLs
        bool isSystem =
            (filename.find("d3d") == 0 || filename.find("vcruntime") == 0 || filename.find("msvcp") == 0 ||
             filename.find("ucrtbase") == 0 || filename.contains("SparkConsole") || filename.contains("SparkEngine"));

        if (mode == DiscoveryMode::ConservativeNameHints && (!isCandidate || isSystem))
            continue;

        // A supported candidate has build-generated compatibility metadata.
        // Do not map the image even with platform-specific probe flags: the
        // editor's discovery path must remain portable and non-executing.
        std::error_code sidecarStatusError;
        if (!std::filesystem::is_regular_file(SidecarPath(filePath), sidecarStatusError) || sidecarStatusError)
            continue;
        if (mode == DiscoveryMode::CompatibleSidecars)
        {
            std::string sidecarError;
            if (!ValidateModuleSidecar(filePath, sidecarError))
                continue;
        }
        candidates.push_back(PathToUtf8(filePath));
    }

    std::sort(candidates.begin(), candidates.end(), [](const std::string& a, const std::string& b)
              { return PathFromUtf8(a).filename() < PathFromUtf8(b).filename(); });
    return candidates;
}

std::string ModuleManager::GetGameModuleName() const
{
    for (const auto& m : m_modules)
    {
        // An entry whose OnLoad failed has already had its instance destroyed
        // (see InitializeAll) and only survives to keep the DLL mapped. Counting
        // it as "the loaded game module" made the single-game-module policy
        // refuse every replacement for the rest of the process lifetime.
        if (m.kind == Spark::ModuleKind::Game && m.instance)
            return m.name;
    }
    return {};
}

std::string ModuleManager::GetInitializedGameModuleName() const
{
    for (const auto& module : m_modules)
    {
        if (module.kind == Spark::ModuleKind::Game && module.initialized && module.instance)
            return module.name;
    }
    return {};
}

bool ModuleManager::InitializeAll(Spark::IEngineContext* context)
{
    SPARK_EXPECTS(context != nullptr);
    auto& console = Spark::SimpleConsole::GetInstance();

    if (!context)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "InitializeAll called with null context");
        return false;
    }

    if (m_validateDependencyGraph)
    {
        std::vector<ModuleDependencyNode> graph;
        graph.reserve(m_modules.size());
        for (const auto& entry : m_modules)
        {
            if (entry.instance)
            {
                graph.push_back(MakeDependencyNode(entry.name, *entry.instance));
            }
        }
        const std::string graphError = DescribeDependencyGraphError(graph);
        if (!graphError.empty())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "Refusing to initialize modules: %s", graphError.c_str());
            console.LogError("Refusing to initialize modules: " + graphError);
            return false;
        }
    }

    bool allInitialized = true;
    for (auto& entry : m_modules)
    {
        if (entry.initialized)
            continue;
        if (!entry.instance)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "Module '%s' has null instance — skipped", entry.name.c_str());
            allInitialized = false;
            continue;
        }

        // The graph check above only proves every dependency is loaded. One can
        // still fail (or throw from) its own OnLoad earlier in this pass, which
        // destroys its instance; the modules sorted after it must then not start
        // without it. Require every declared dependency to be initialized right
        // now. A skipped module stays uninitialized, so its own dependents are
        // skipped in turn (the order is topological). The reload staging
        // manager holds only the replacement and checks the live graph instead.
        if (m_validateDependencyGraph)
        {
            const ModuleDependencyNode node = MakeDependencyNode(entry.name, *entry.instance);
            const auto unmetDependency =
                std::find_if(node.dependencies.begin(), node.dependencies.end(),
                             [this](const std::string& dependency)
                             {
                                 return std::none_of(m_modules.begin(), m_modules.end(),
                                                     [&dependency](const LoadedModule& provider) {
                                                         return provider.name == dependency && provider.initialized &&
                                                                provider.instance != nullptr;
                                                     });
                             });
            if (unmetDependency != node.dependencies.end())
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Core,
                                "Module '%s' not initialized: its dependency '%s' is not initialized",
                                entry.name.c_str(), unmetDependency->c_str());
                console.LogError("Module initialization skipped: " + entry.name + " (dependency '" + *unmetDependency +
                                 "' is not initialized)");
                allInitialized = false;
                continue;
            }
        }

        SPARK_LOG_INFO(Spark::LogCategory::Core, "Initializing module: %s", entry.name.c_str());
        console.LogInfo("Initializing module: " + entry.name);
        ModuleRegistrationScope registrationScope(entry.registrationOwner, ModuleRegistrationPhase::Load);
        bool loadSucceeded = false;
        try
        {
            loadSucceeded = entry.instance->OnLoad(context);
        }
        catch (const std::exception& exception)
        {
            allInitialized = false;
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "Module '%s' OnLoad threw: %s", entry.name.c_str(),
                            exception.what());
        }
        catch (...)
        {
            allInitialized = false;
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "Module '%s' OnLoad threw an unknown exception",
                            entry.name.c_str());
        }

        if (loadSucceeded)
        {
            // A manager lifetime owns fresh evidence. Clear same-name records
            // left by a previous manager so an otherwise healthy replacement
            // is not born disabled; any faults already seen by this manager
            // remain preserved in m_lifecycleEvidence.
            if (m_publishTeardownLifecycleEvidence)
            {
                auto& faultIsolator = Spark::SubsystemFaultIsolator::GetInstance();
                faultIsolator.ResetSubsystem("Module:" + entry.name);
                faultIsolator.ResetSubsystem("ModuleFixed:" + entry.name);
            }
            ++m_lifecycleEvidence.initialized;
            if (!entry.isLegacyAdapter)
                ++FindOrCreateLifecycleRecord(entry.name).onLoad;
            entry.initialized = true;
            console.LogSuccess("Module initialized: " + entry.name);
        }
        else
        {
            console.LogError("Module initialization failed: " + entry.name);
            allInitialized = false;

            // Failed-boot teardown ordering (W10 exit AV): a module that fails
            // OnLoad never gets OnUnload from ShutdownAll (initialized stays
            // false), so its instance used to survive until UnloadAll — which
            // runs AFTER engine physics teardown. Its destructor then released
            // shared_ptr<PhysicsBody> handles into a destroyed PhysicsSystem
            // (dangling EngineContext/raw pointers → AV at exit). Destroy the
            // instance NOW, while every engine service it may reference
            // (physics, ECS world, event bus) is still alive. OnUnload() runs
            // first so the module can deregister anything its partial OnLoad
            // installed. The host removes only owner-scoped registrations
            // (console, invalid-state rules, serializers); an EventBus,
            // network or timer callback the partial OnLoad left behind can
            // still point into the image, so it is never unmapped (see
            // LoadedModule::retainImage), including a failed hot-reload
            // replacement whose staging manager unloads it immediately.
            SPARK_LOG_WARN(Spark::LogCategory::Core,
                           "Module '%s' failed OnLoad — destroying its instance immediately "
                           "(its image stays mapped for the rest of the process)",
                           entry.name.c_str());
            entry.retainImage = true;
            bool unloadCompleted = false;
            try
            {
                entry.instance->OnUnload();
                unloadCompleted = true;
            }
            catch (const std::exception& exception)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Core, "Module '%s' partial OnUnload threw: %s", entry.name.c_str(),
                                exception.what());
            }
            catch (...)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Core, "Module '%s' partial OnUnload threw an unknown exception",
                                entry.name.c_str());
            }
            if (unloadCompleted)
            {
                ++m_lifecycleEvidence.unloaded;
                if (!entry.isLegacyAdapter)
                    ++FindOrCreateLifecycleRecord(entry.name).onUnload;
            }
            if (unloadCompleted && entry.destroyFn)
            {
                entry.destroyFn(entry.instance);
                if (!entry.isLegacyAdapter)
                    ++FindOrCreateLifecycleRecord(entry.name).destroyModule;
            }
            else if (!unloadCompleted)
            {
                // The partial OnUnload did not finish, so callbacks it should
                // have removed may still capture this instance. Leak it rather
                // than turn their next dispatch into a use-after-free.
                SPARK_LOG_ERROR(Spark::LogCategory::Core,
                                "Module '%s' instance quarantined, not destroyed: its partial OnUnload did not "
                                "complete",
                                entry.name.c_str());
            }
            entry.instance = nullptr;
            entry.destroyFn = nullptr;
            UnregisterModuleRegistrations(entry);
        }
    }

    return allInitialized;
}

void ModuleManager::UpdateAll(float deltaTime)
{
    for (auto& entry : m_modules)
    {
        if (entry.initialized && entry.instance)
        {
            std::string guardName = "Module:" + entry.name;
            bool callbackCompleted = false;
            SPARK_GUARDED_UPDATE(guardName.c_str(), "Core", {
                entry.instance->OnUpdate(deltaTime);
                ++m_lifecycleEvidence.updated;
                if (!entry.isLegacyAdapter)
                    ++FindOrCreateLifecycleRecord(entry.name).onUpdate;
                callbackCompleted = true;
            });
            if (!callbackCompleted)
            {
                ++m_lifecycleEvidence.faults;
                if (!entry.isLegacyAdapter)
                    ++FindOrCreateLifecycleRecord(entry.name).faults;
            }
        }
    }
}

void ModuleManager::FixedUpdateAll(float fixedDeltaTime)
{
    for (auto& entry : m_modules)
    {
        if (entry.initialized && entry.instance)
        {
            std::string guardName = "ModuleFixed:" + entry.name;
            bool callbackCompleted = false;
            SPARK_GUARDED_UPDATE(guardName.c_str(), "Core", {
                entry.instance->OnFixedUpdate(fixedDeltaTime);
                ++m_lifecycleEvidence.fixedUpdated;
                if (!entry.isLegacyAdapter)
                    ++FindOrCreateLifecycleRecord(entry.name).onFixedUpdate;
                callbackCompleted = true;
            });
            if (!callbackCompleted)
            {
                ++m_lifecycleEvidence.faults;
                if (!entry.isLegacyAdapter)
                    ++FindOrCreateLifecycleRecord(entry.name).faults;
            }
        }
    }
}

void ModuleManager::RenderAll()
{
#ifdef SPARK_HEADLESS_SUPPORT
    extern bool g_headlessMode;
    if (g_headlessMode)
        return;
#endif

    for (auto& entry : m_modules)
    {
        if (entry.initialized && entry.instance)
        {
            std::string guardName = "Module:" + entry.name;
            bool callbackCompleted = false;
            SPARK_GUARDED_UPDATE(guardName.c_str(), "Core", {
                entry.instance->OnRender();
                ++m_lifecycleEvidence.rendered;
                if (!entry.isLegacyAdapter)
                    ++FindOrCreateLifecycleRecord(entry.name).onRender;
                callbackCompleted = true;
            });
            if (!callbackCompleted)
            {
                ++m_lifecycleEvidence.faults;
                if (!entry.isLegacyAdapter)
                    ++FindOrCreateLifecycleRecord(entry.name).faults;
            }
        }
    }
}

void ModuleManager::ImGuiAll()
{
#ifdef SPARK_HEADLESS_SUPPORT
    extern bool g_headlessMode;
    if (g_headlessMode)
        return;
#endif

    for (auto& entry : m_modules)
    {
        if (entry.initialized && entry.instance)
        {
            std::string guardName = "ModuleImGui:" + entry.name;
            SPARK_GUARDED_UPDATE(guardName.c_str(), "Core", { entry.instance->OnImGui(); });
        }
    }
}

void ModuleManager::ResizeAll(int width, int height)
{
    SPARK_EXPECTS(width > 0 && height > 0);
    for (auto& entry : m_modules)
    {
        if (entry.initialized && entry.instance)
        {
            std::string guardName = "Module:" + entry.name;
            SPARK_GUARDED_UPDATE(guardName.c_str(), "Core", { entry.instance->OnResize(width, height); });
        }
    }
}

bool ModuleManager::CanShutdownAll()
{
    auto& console = Spark::SimpleConsole::GetInstance();

    for (auto it = m_modules.rbegin(); it != m_modules.rend(); ++it)
    {
        if (it->initialized && it->instance && !it->instance->CanUnload())
        {
            console.LogError("Module refused shutdown and remains initialized: " + it->name);
            return false;
        }
    }
    return true;
}

bool ModuleManager::ShutdownAll()
{
    // Two-phase shutdown: never dismantle some modules before discovering a
    // later module cannot checkpoint. A veto leaves the complete dependency
    // graph initialized and usable for retry.
    if (!CanShutdownAll())
        return false;

    ShutdownAllAfterPreflight();
    return true;
}

void ModuleManager::ShutdownAllAfterPreflight()
{
    auto& console = Spark::SimpleConsole::GetInstance();

    // Shut down in reverse load order
    for (auto it = m_modules.rbegin(); it != m_modules.rend(); ++it)
    {
        if (it->initialized && it->instance)
        {
            console.LogInfo("Shutting down module: " + it->name);
            ModuleRegistrationScope registrationScope(it->registrationOwner, ModuleRegistrationPhase::Teardown);
            it->instance->OnUnload();
            ++m_lifecycleEvidence.unloaded;
            if (!it->isLegacyAdapter)
                ++FindOrCreateLifecycleRecord(it->name).onUnload;
            it->initialized = false;
            UnregisterModuleRegistrations(*it);
        }
    }
}

void ModuleManager::RollbackStartup()
{
    Spark::SimpleConsole::GetInstance().LogWarning(
        "Rolling back module initialization after host startup failed; unload vetoes do not apply");
    ShutdownAllAfterPreflight();
}

bool ModuleManager::ReloadModule(const std::string& name, Spark::IEngineContext* context)
{
    auto& console = Spark::SimpleConsole::GetInstance();
    m_lastLoadError.clear();

    const auto failReload = [&](std::string message)
    {
        m_lastLoadError = std::move(message);
        console.LogError(m_lastLoadError);
        return false;
    };

    if (!context)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "ReloadModule called with null context");
        return failReload("Cannot reload module '" + name + "' with a null engine context");
    }

    for (size_t index = 0; index < m_modules.size(); ++index)
    {
        auto& entry = m_modules[index];
        if (entry.name != name)
            continue;

        if (entry.instance && !entry.instance->SupportsHotReload())
        {
            return failReload("Module does not support transactional hot reload; perform a full restart: " + name);
        }

        // A stateful module may need to checkpoint before an image swap. Run
        // the non-destructive gate before staging a replacement so a veto
        // leaves the working instance and all of its dependencies untouched.
        if (entry.initialized && entry.instance && !entry.instance->CanUnload())
        {
            return failReload("Module refused hot reload and remains active: " + name);
        }

        const std::string savedPath = entry.path;
        const std::filesystem::path sourcePath = PathFromUtf8(savedPath);
#ifdef _WIN32
        const uint64_t reloadToken = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        static std::atomic<uint64_t> reloadSerial{0};
        std::filesystem::path shadowName = sourcePath.stem();
        shadowName += ".spark-reload-" + std::to_string(reloadToken) + "-" +
                      std::to_string(reloadSerial.fetch_add(1, std::memory_order_relaxed));
        shadowName += sourcePath.extension();
        const std::filesystem::path shadowPath = sourcePath.parent_path() / shadowName;
        const std::filesystem::path sourceSidecar = SidecarPath(sourcePath);
        const std::filesystem::path shadowSidecar = SidecarPath(shadowPath);

        // A replacement whose OnLoad failed keeps its shadow mapped until its
        // process exits (LoadedModule::retainImage), so the file outlives it.
        // Remove this module's earlier shadows; one still mapped by a live
        // process cannot be deleted and is skipped.
        {
            const std::string shadowPrefix = PathToUtf8(sourcePath.stem()) + ".spark-reload-";
            std::vector<std::filesystem::path> staleShadows;
            std::error_code sweepError;
            for (std::filesystem::directory_iterator it(sourcePath.parent_path(), sweepError), end;
                 !sweepError && it != end; it.increment(sweepError))
            {
                if (PathToUtf8(it->path().filename()).starts_with(shadowPrefix) &&
                    it->path().extension() == sourcePath.extension())
                {
                    staleShadows.push_back(it->path());
                }
            }
            for (const auto& staleShadow : staleShadows)
            {
                std::error_code removeError;
                std::filesystem::remove(staleShadow, removeError);
            }
        }

        auto removeShadowFiles = [&]()
        {
            std::error_code cleanupError;
            std::filesystem::remove(shadowSidecar, cleanupError);
            cleanupError.clear();
            std::filesystem::remove(shadowPath, cleanupError);
        };

        // Stage an immutable shadow image while the working module remains
        // loaded. A compiler may be replacing the source DLL and sidecar, so a
        // partial or mismatched copy is expected to fail the normal ABI/hash
        // gate without disturbing the active instance.
        std::error_code copyError;
        std::filesystem::copy_file(sourcePath, shadowPath, std::filesystem::copy_options::overwrite_existing,
                                   copyError);
        if (!copyError)
        {
            std::filesystem::copy_file(sourceSidecar, shadowSidecar, std::filesystem::copy_options::overwrite_existing,
                                       copyError);
        }
        if (copyError)
        {
            removeShadowFiles();
            return failReload("Failed to stage module reload for '" + name + "': " + copyError.message());
        }
#else
        ScopedStagedModuleImage reloadShadow;
        std::string reloadStagingError;
        if (!StageModuleForPosixLoad(sourcePath, reloadShadow, reloadStagingError))
        {
            return failReload("Failed to stage module reload for '" + name + "': " + reloadStagingError);
        }
        const std::filesystem::path shadowPath = reloadShadow.Get();
        const auto removeShadowFiles = []() {};
#endif

        ModuleManager stagedManager;
        stagedManager.m_publishTeardownLifecycleEvidence = false;
        if (!stagedManager.LoadModule(PathToUtf8(shadowPath)))
        {
            const std::string detail = stagedManager.GetLastLoadError();
            stagedManager.UnloadAll();
            removeShadowFiles();
            return failReload(detail.empty()
                                  ? "Failed to validate staged replacement for module: " + name
                                  : "Failed to validate staged replacement for module '" + name + "': " + detail);
        }

        if (stagedManager.m_modules.size() != 1 || stagedManager.m_modules.front().name != name)
        {
            stagedManager.UnloadAll();
            removeShadowFiles();
            return failReload("Staged replacement identity does not match module: " + name);
        }

        // The replacement image is the code that will own the live process
        // after the swap. It must make the same transactional-hot-reload
        // promise as the outgoing image; otherwise an updated module can
        // silently opt into a lifecycle it explicitly declared unsafe.
        if (!stagedManager.m_modules.front().instance->SupportsHotReload())
        {
            stagedManager.UnloadAll();
            removeShadowFiles();
            return failReload("Staged replacement does not support transactional hot reload: " + name);
        }

        const Spark::ModuleKind replacementKind = stagedManager.m_modules.front().kind;
        if (replacementKind == Spark::ModuleKind::Game)
        {
            for (size_t otherIndex = 0; otherIndex < m_modules.size(); ++otherIndex)
            {
                if (otherIndex != index && m_modules[otherIndex].kind == Spark::ModuleKind::Game)
                {
                    stagedManager.UnloadAll();
                    removeShadowFiles();
                    return failReload("Staged replacement would violate the one-game-module policy: " + name);
                }
            }
        }

        // The staged manager holds only the replacement, so check its declared
        // dependencies against the live graph with the replacement swapped in.
        // The replacement's OnLoad runs below, so only initialized modules can
        // satisfy a dependency: one that is loaded but never started (or was
        // skipped because its own dependency failed) must not count.
        std::vector<ModuleDependencyNode> graph;
        graph.reserve(m_modules.size());
        for (size_t otherIndex = 0; otherIndex < m_modules.size(); ++otherIndex)
        {
            if (otherIndex == index)
            {
                graph.push_back(MakeDependencyNode(name, *stagedManager.m_modules.front().instance));
            }
            else if (m_modules[otherIndex].initialized && m_modules[otherIndex].instance)
            {
                graph.push_back(MakeDependencyNode(m_modules[otherIndex].name, *m_modules[otherIndex].instance));
            }
        }
        if (std::string graphError = DescribeDependencyGraphError(graph); !graphError.empty())
        {
            stagedManager.UnloadAll();
            removeShadowFiles();
            return failReload(
                std::format("Staged replacement dependency graph is invalid for '{}': {}", name, graphError));
        }

        // Initialize the replacement before touching the working instance. A
        // failed OnLoad is cleaned up by InitializeAll and leaves the old
        // module, including its in-memory state and registry callbacks, intact.
        stagedManager.m_validateDependencyGraph = false;
        stagedManager.InitializeAll(context);
        AccumulateLifecycleEvidence(m_lifecycleEvidence, stagedManager.m_lifecycleEvidence);
        stagedManager.m_lifecycleEvidence = {};
        if (!stagedManager.m_modules.front().initialized || !stagedManager.m_modules.front().instance)
        {
            stagedManager.UnloadAll();
            removeShadowFiles();
            return failReload("Staged replacement initialization failed; preserving module: " + name);
        }

        LoadedModule replacement = std::move(stagedManager.m_modules.front());
        stagedManager.m_modules.clear();
        replacement.path = savedPath;
#ifdef _WIN32
        // Windows maps the outer reload shadow directly and keeps it until the
        // replacement image is unloaded.
        replacement.transientImagePath = PathToUtf8(shadowPath);
#else
        // POSIX LoadModule already created and tracked a private inner staging
        // image. Preserve that ownership and discard the now-unused outer
        // reload copy instead of overwriting the tracked cleanup path.
        removeShadowFiles();
#endif

        // Commit only after the replacement is fully usable.
        if (entry.initialized && entry.instance)
        {
            ModuleRegistrationScope registrationScope(entry.registrationOwner, ModuleRegistrationPhase::Teardown);
            entry.instance->OnUnload();
            ++m_lifecycleEvidence.unloaded;
            if (!entry.isLegacyAdapter)
                ++FindOrCreateLifecycleRecord(entry.name).onUnload;
        }
        UnregisterModuleRegistrations(entry);
        UnloadEntry(entry);
        m_modules[index] = std::move(replacement);
        auto& faultIsolator = Spark::SubsystemFaultIsolator::GetInstance();
        faultIsolator.ResetSubsystem("Module:" + name);
        faultIsolator.ResetSubsystem("ModuleFixed:" + name);
        SortModules();
        console.LogSuccess("Module transactionally reloaded and initialized: " + name);
        return true;
    }

    return failReload("Module not found for reload: " + name);
}

void ModuleManager::UnloadAll()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Core);
    if (m_modules.empty())
        return;

    SPARK_LOG_INFO(Spark::LogCategory::Core, "Unloading all modules (%zu loaded)", m_modules.size());

    auto entry = m_modules.begin();
    while (entry != m_modules.end())
    {
        if (entry->initialized && entry->instance)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core,
                            "Refusing to unload active module '%s'; call ShutdownAll and resolve any veto first",
                            entry->name.c_str());
            ++entry;
            continue;
        }
        // A module whose OnLoad failed after registering commands still has
        // handlers in the host console; the image is about to be unmapped, so
        // drop them before the code they point at disappears.
        UnregisterModuleRegistrations(*entry);
        UnloadEntry(*entry);
        entry = m_modules.erase(entry);
    }
}

Spark::IModule* ModuleManager::GetModule(const std::string& name) const
{
    for (auto& entry : m_modules)
    {
        if (entry.name == name)
            return entry.instance;
    }
    return nullptr;
}

std::vector<std::pair<std::string, std::string>> ModuleManager::GetModulePathsAndNames() const
{
    std::vector<std::pair<std::string, std::string>> result;
    for (const auto& entry : m_modules)
        result.emplace_back(entry.name, entry.path);
    return result;
}

size_t ModuleManager::GetInitializedModuleCount() const
{
    return static_cast<size_t>(std::count_if(m_modules.begin(), m_modules.end(), [](const LoadedModule& module)
                                             { return module.initialized && module.instance != nullptr; }));
}

Spark::IModule* ModuleManager::GetPrimaryModule() const
{
    // An entry whose OnLoad failed keeps its slot (so its DLL stays mapped) but
    // its instance was already destroyed. Returning that null front entry made
    // module_info drop its Primary line and module_reload answer "No primary
    // module to reload" while a perfectly usable module sat behind it.
    for (const auto& entry : m_modules)
    {
        if (entry.initialized && entry.instance)
            return entry.instance;
    }
    return nullptr;
}

void ModuleManager::SortModules()
{
    auto& console = Spark::SimpleConsole::GetInstance();

    // Check if any module declares dependencies
    bool hasDependencies = false;
    for (const auto& entry : m_modules)
    {
        if (!entry.instance)
            continue;
        auto info = entry.instance->GetModuleInfo();
        if (info.dependencyCount > 0)
        {
            hasDependencies = true;
            break;
        }
    }

    if (!hasDependencies)
    {
        // Simple numeric sort when no dependencies declared
        std::stable_sort(m_modules.begin(), m_modules.end(),
                         [](const LoadedModule& a, const LoadedModule& b) { return a.loadOrder < b.loadOrder; });
        return;
    }

    // Topological sort (Kahn's algorithm) respecting declared dependencies.
    // Within the same dependency level, fall back to loadOrder.
    const size_t count = m_modules.size();

    // Build name → index map
    std::unordered_map<std::string, size_t> nameToIndex;
    for (size_t i = 0; i < count; ++i)
        nameToIndex[m_modules[i].name] = i;

    // Build adjacency list and compute in-degrees
    std::vector<std::vector<size_t>> dependents(count); // dependents[dep] = modules that depend on dep
    std::vector<int> inDegree(count, 0);

    for (size_t i = 0; i < count; ++i)
    {
        if (!m_modules[i].instance)
            continue;
        auto info = m_modules[i].instance->GetModuleInfo();
        for (int d = 0; d < info.dependencyCount; ++d)
        {
            auto it = nameToIndex.find(info.dependencies[d]);
            if (it != nameToIndex.end())
            {
                dependents[it->second].push_back(i);
                inDegree[i]++;
            }
            else
            {
                console.LogWarning("Module '" + m_modules[i].name + "' depends on '" +
                                   std::string(info.dependencies[d]) + "' which is not loaded");
            }
        }
    }

    // Kahn's algorithm: start with modules that have no unmet dependencies
    std::vector<LoadedModule> sorted;
    sorted.reserve(count);

    // Collect ready modules, sorted by loadOrder for determinism
    auto getReadyModules = [&]()
    {
        std::vector<size_t> ready;
        for (size_t i = 0; i < count; ++i)
        {
            if (inDegree[i] == 0)
                ready.push_back(i);
        }
        std::stable_sort(ready.begin(), ready.end(),
                         [this](size_t a, size_t b) { return m_modules[a].loadOrder < m_modules[b].loadOrder; });
        return ready;
    };

    std::vector<size_t> ready = getReadyModules();
    // Mark processed with -1
    while (!ready.empty())
    {
        for (size_t idx : ready)
        {
            sorted.push_back(std::move(m_modules[idx]));
            inDegree[idx] = -1;
            for (size_t dep : dependents[idx])
            {
                if (inDegree[dep] > 0)
                    inDegree[dep]--;
            }
        }
        ready = getReadyModules();
    }

    // Cycle detection: any remaining modules have circular dependencies
    for (size_t i = 0; i < count; ++i)
    {
        if (inDegree[i] > 0)
        {
            console.LogError("Circular dependency detected involving module: " + m_modules[i].name);
            sorted.push_back(std::move(m_modules[i]));
        }
    }

    m_modules = std::move(sorted);
}

void ModuleManager::UnregisterModuleRegistrations(const LoadedModule& entry)
{
    if (entry.registrationOwner.empty())
        return;

    auto& console = Spark::SimpleConsole::GetInstance();
    const size_t removedCommands = console.UnregisterCommandsByOwner(entry.registrationOwner);
    const size_t removedRules = Spark::InvalidStateDetector::GetInstance().RemoveRulesByOwner(entry.registrationOwner);
    const size_t removedSerializers =
        Spark::ComponentSerializerRegistry::GetInstance().UnregisterByOwner(entry.registrationOwner);
    size_t removedNetworkHandlers = 0;
#ifdef ENABLE_NETWORKING
    // Every caller runs this while the module image is still mapped: a network callback whose invoker or
    // destructor lives in that image must be destroyed now, not by a later packet, a later replacement or the
    // engine-shutdown ClearHandlers after FreeLibrary/dlclose.
    removedNetworkHandlers =
        Spark::Net::NetworkManager::GetInstance().UnregisterHandlersByOwner(entry.registrationOwner);
#endif
    if (removedCommands != 0 || removedRules != 0 || removedSerializers != 0 || removedNetworkHandlers != 0)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core,
                       "Removed %zu console command(s), %zu invalid-state rule(s), %zu save serializer(s) and %zu "
                       "network handler(s) owned by module '%s'",
                       removedCommands, removedRules, removedSerializers, removedNetworkHandlers, entry.name.c_str());
    }
}

void ModuleManager::UnloadEntry(LoadedModule& entry)
{
    // ModuleRuntimeInjection stores a non-owning host EngineContext pointer in
    // each static-library image. Clear that pointer while the image is still
    // mapped and before destroying the module object, so a module destructor
    // or its CRT teardown can never retain a pointer to the host context.
    if (entry.libraryHandle)
    {
        using InjectContextFn = void (*)(void*);
#ifdef _WIN32
        auto clearContext = reinterpret_cast<InjectContextFn>(
            GetProcAddress(static_cast<HMODULE>(entry.libraryHandle), "SparkModuleInjectEngineContext"));
#else
        auto clearContext =
            reinterpret_cast<InjectContextFn>(dlsym(entry.libraryHandle, "SparkModuleInjectEngineContext"));
#endif
        if (clearContext)
            clearContext(nullptr);
    }

    if (entry.instance && entry.destroyFn)
    {
        entry.destroyFn(entry.instance);
        if (entry.createFn && !entry.isLegacyAdapter)
            ++FindOrCreateLifecycleRecord(entry.name).destroyModule;
    }
    entry.instance = nullptr;
    entry.createFn = nullptr;
    entry.destroyFn = nullptr;

    if (entry.libraryHandle && entry.retainImage)
    {
        // Deliberately leak the mapping: see LoadedModule::retainImage.
        SPARK_LOG_WARN(Spark::LogCategory::Core, "Module '%s' failed OnLoad; its image stays mapped until process exit",
                       entry.name.c_str());
        entry.libraryHandle = nullptr;
    }
    else if (entry.libraryHandle)
    {
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(entry.libraryHandle));
#else
        dlclose(entry.libraryHandle);
#endif
        entry.libraryHandle = nullptr;
        // The unmapped range may be reused by a later mapping; forget it so
        // crash frames there are never resolved against this module.
        RefreshCrashModuleIdentities();
    }

    if (!entry.transientImagePath.empty())
    {
        std::error_code cleanupError;
        const std::filesystem::path transientPath = PathFromUtf8(entry.transientImagePath);
#ifdef _WIN32
        std::filesystem::remove(SidecarPath(transientPath), cleanupError);
        cleanupError.clear();
        std::filesystem::remove(transientPath, cleanupError);
#else
        std::filesystem::remove_all(transientPath.parent_path(), cleanupError);
#endif
        entry.transientImagePath.clear();
    }
}

ModuleManager::ModuleLifecycleRecord& ModuleManager::FindOrCreateLifecycleRecord(std::string_view module)
{
    for (auto& record : m_lifecycleEvidence.modules)
    {
        if (record.module == module)
            return record;
    }

    m_lifecycleEvidence.modules.push_back({std::string(module)});
    return m_lifecycleEvidence.modules.back();
}

std::vector<DiscoveredModule> ModuleManager::DiscoverModules(const std::string& directory) const
{
    std::vector<DiscoveredModule> result;

    if (!std::filesystem::exists(PathFromUtf8(directory)))
        return result;

    // Metadata discovery must remain non-executing. Candidate enumeration uses
    // only filename hints and mandatory sidecar presence; it never maps an
    // unloaded image or invokes DllMain, compatibility hooks, injections, or
    // factories. Unloaded modules intentionally retain filename/"unknown"
    // presentation metadata.
    for (const std::string& candidate : DiscoverModuleCandidates(directory))
    {
        const std::filesystem::path filePath = PathFromUtf8(candidate);

        DiscoveredModule discovered;
        discovered.path = candidate;
        discovered.name = PathToUtf8(filePath.stem());
        discovered.version = "unknown";
        discovered.isLoaded = false;

        // Check if already loaded
        for (const auto& loaded : m_modules)
        {
            std::error_code ec;
            const bool sameFile =
                loaded.path == discovered.path ||
                std::filesystem::equivalent(PathFromUtf8(loaded.path), PathFromUtf8(discovered.path), ec);
            if (sameFile)
            {
                discovered.isLoaded = true;
                discovered.name = loaded.name;
                discovered.kind = loaded.kind;
                discovered.kindKnown = true;
                if (loaded.instance)
                {
                    auto info = loaded.instance->GetModuleInfo();
                    discovered.version = info.version;
                }
                break;
            }
        }

        result.push_back(std::move(discovered));
    }

    return result;
}

std::vector<DiscoveredModule> ModuleManager::GetLoadedModuleInfo() const
{
    std::vector<DiscoveredModule> result;
    result.reserve(m_modules.size());

    for (const auto& entry : m_modules)
    {
        DiscoveredModule info;
        info.name = entry.name;
        info.path = entry.path;
        info.isLoaded = true;
        info.kind = entry.kind;
        info.kindKnown = true;

        if (entry.instance)
        {
            auto modInfo = entry.instance->GetModuleInfo();
            info.version = modInfo.version;
        }
        else
        {
            info.version = "unknown";
        }

        result.push_back(std::move(info));
    }

    return result;
}
