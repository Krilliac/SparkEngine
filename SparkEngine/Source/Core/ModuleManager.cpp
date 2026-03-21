/**
 * @file ModuleManager.cpp
 * @brief Multi-module loader and lifecycle manager implementation
 */

#include "ModuleManager.h"
#include "IGameModule.h"
#include "Spark/Version.h"
#include "Utils/SparkConsole.h"
#include "Utils/LocalFileCache.h"
#include "Utils/Validate.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <unordered_map>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifdef SPARK_PLATFORM_WINDOWS
#include <Windows.h>
#endif // SPARK_PLATFORM_WINDOWS
#else
#include <dlfcn.h>
#endif

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

    ~LegacyModuleAdapter() override = default;

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
}

bool ModuleManager::LoadModule(const std::string& path)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Core);
    SPARK_VALIDATE_RET(Spark::LogCategory::Core, !path.empty(), false);
    SPARK_LOG_INFO(Spark::LogCategory::Core, "Loading module: %s", path.c_str());

    auto& console = Spark::SimpleConsole::GetInstance();

    // Security: reject path traversal sequences
    if (path.contains(".."))
    {
        console.LogError("Module path rejected — contains '..' traversal: " + path);
        return false;
    }

    // Load the shared library
    void* handle = nullptr;
#ifdef _WIN32
    handle = LoadLibraryA(path.c_str());
    if (!handle)
    {
        DWORD err = GetLastError();
        console.LogError(std::format("Failed to load module '{}' (error {})", path, err));
        return false;
    }
#else
    handle = dlopen(path.c_str(), RTLD_NOW);
    if (!handle)
    {
        const char* err = dlerror();
        console.LogError(std::format("Failed to load module '{}': {}", path, err ? err : "unknown"));
        return false;
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
            console.LogError(std::format("CreateModule() returned null for '{}'", path));
#ifdef _WIN32
            FreeLibrary(static_cast<HMODULE>(handle));
#else
            dlclose(handle);
#endif
            return false;
        }

        auto info = instance->GetModuleInfo();

        // SDK version compatibility check
        if (!Spark::IsSDKCompatible(info.sdkVersion))
        {
            console.LogError(std::format("Module '{}' SDK version mismatch (module={}, engine={})", info.name,
                                         info.sdkVersion, SPARK_SDK_VERSION));
            destroyFn(instance);
#ifdef _WIN32
            FreeLibrary(static_cast<HMODULE>(handle));
#else
            dlclose(handle);
#endif
            return false;
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

        console.LogSuccess(std::format("Loaded module: {} v{}", info.name, info.version));
        m_modules.push_back(std::move(entry));
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
            console.LogError("CreateGameModule() returned null for '" + path + "'");
#ifdef _WIN32
            FreeLibrary(static_cast<HMODULE>(handle));
#else
            dlclose(handle);
#endif
            return false;
        }

        // Wrap in adapter (unique_ptr for exception safety)
        auto adapterOwner = std::make_unique<LegacyModuleAdapter>(legacyModule, legacyDestroyFn);

        // For legacy modules, we use adapter's destroy which handles cleanup
        auto info = adapterOwner->GetModuleInfo();

        LoadedModule entry{};
        entry.name = info.name;
        entry.path = path;
        entry.libraryHandle = handle;
        entry.instance = adapterOwner.release(); // transfer ownership to entry
        entry.createFn = nullptr;                // managed by adapter
        entry.destroyFn = [](Spark::IModule* mod) { delete mod; };
        entry.loadOrder = info.loadOrder;
        entry.isLegacyAdapter = true;

        console.LogSuccess(std::format("Loaded legacy module: {} v{}", info.name, info.version));
        m_modules.push_back(std::move(entry));
        SortModules();
        return true;
    }

    // No recognized exports
    console.LogError(std::format("Module '{}' has no recognized exports (CreateModule or CreateGameModule)", path));
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
    return false;
}

bool ModuleManager::LoadModulesFromManifest(const std::string& manifestPath)
{
    auto& console = Spark::SimpleConsole::GetInstance();

    std::string content;

    if (m_fileCache)
    {
        auto result = m_fileCache->ReadText(manifestPath);
        if (result.IsOk())
        {
            content = result.Value();
        }
    }

    if (content.empty())
    {
        std::ifstream file(manifestPath);
        if (!file.is_open())
        {
            console.LogWarning("Could not open module manifest: " + manifestPath);
            return false;
        }
        content.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();
    }

    // Simple JSON parsing for the modules array
    // Format: { "modules": [ { "name": "...", "path": "...", "loadOrder": N }, ... ] }
    std::filesystem::path manifestDir = std::filesystem::path(manifestPath).parent_path();
    bool anyLoaded = false;

    // Find each module entry by looking for "path" keys
    size_t pos = 0;
    while ((pos = content.find("\"path\"", pos)) != std::string::npos)
    {
        // Extract the path value
        size_t colon = content.find(':', pos);
        if (colon == std::string::npos)
            break;
        size_t quote1 = content.find('\"', colon + 1);
        if (quote1 == std::string::npos)
            break;
        size_t quote2 = content.find('\"', quote1 + 1);
        if (quote2 == std::string::npos)
            break;

        std::string modulePath = content.substr(quote1 + 1, quote2 - quote1 - 1);

        // Resolve relative paths against manifest directory
        std::filesystem::path fullPath(modulePath);
        if (fullPath.is_relative())
            fullPath = manifestDir / fullPath;

        if (std::filesystem::exists(fullPath))
        {
            if (LoadModule(fullPath.string()))
                anyLoaded = true;
        }
        else
        {
            console.LogWarning("Module not found: " + fullPath.string());
        }

        pos = quote2 + 1;
    }

    return anyLoaded;
}

bool ModuleManager::LoadModulesFromDirectory(const std::string& directory)
{
    auto& console = Spark::SimpleConsole::GetInstance();
    bool anyLoaded = false;

    if (!std::filesystem::exists(directory))
    {
        console.LogWarning("Module directory does not exist: " + directory);
        return false;
    }

    // Determine platform-specific DLL extension
#ifdef _WIN32
    const std::string ext = ".dll";
#elif defined(__APPLE__)
    const std::string ext = ".dylib";
#else
    const std::string ext = ".so";
#endif

    for (auto& entry : std::filesystem::directory_iterator(directory))
    {
        if (!entry.is_regular_file())
            continue;

        auto filePath = entry.path();
        if (filePath.extension() != ext)
            continue;

        auto filename = filePath.filename().string();

        // Skip the engine executable's own DLLs that aren't modules
        // Look for common module naming patterns
        bool isCandidate = (filename.contains("Game") || filename.contains("Module") || filename.contains("Plugin"));

        // Skip system/runtime DLLs
        bool isSystem =
            (filename.find("d3d") == 0 || filename.find("vcruntime") == 0 || filename.find("msvcp") == 0 ||
             filename.find("ucrtbase") == 0 || filename.contains("SparkConsole") || filename.contains("SparkEngine"));

        if (isCandidate && !isSystem)
        {
            console.LogInfo("Found candidate module: " + filename);
            if (LoadModule(filePath.string()))
                anyLoaded = true;
        }
    }

    return anyLoaded;
}

void ModuleManager::InitializeAll(Spark::IEngineContext* context)
{
    auto& console = Spark::SimpleConsole::GetInstance();

    for (auto& entry : m_modules)
    {
        if (entry.initialized)
            continue;

        console.LogInfo("Initializing module: " + entry.name);
        if (entry.instance->OnLoad(context))
        {
            entry.initialized = true;
            console.LogSuccess("Module initialized: " + entry.name);
        }
        else
        {
            console.LogError("Module initialization failed: " + entry.name);
        }
    }
}

void ModuleManager::UpdateAll(float deltaTime)
{
    for (auto& entry : m_modules)
    {
        if (entry.initialized && entry.instance)
            entry.instance->OnUpdate(deltaTime);
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
            entry.instance->OnRender();
    }
}

void ModuleManager::ResizeAll(int width, int height)
{
    for (auto& entry : m_modules)
    {
        if (entry.initialized && entry.instance)
            entry.instance->OnResize(width, height);
    }
}

void ModuleManager::ShutdownAll()
{
    auto& console = Spark::SimpleConsole::GetInstance();

    // Shut down in reverse load order
    for (auto it = m_modules.rbegin(); it != m_modules.rend(); ++it)
    {
        if (it->initialized && it->instance)
        {
            console.LogInfo("Shutting down module: " + it->name);
            it->instance->OnUnload();
            it->initialized = false;
        }
    }
}

bool ModuleManager::ReloadModule(const std::string& name, Spark::IEngineContext* context)
{
    auto& console = Spark::SimpleConsole::GetInstance();

    for (auto& entry : m_modules)
    {
        if (entry.name != name)
            continue;

        std::string savedPath = entry.path;
        int savedOrder = entry.loadOrder;

        // Shut down and unload
        if (entry.initialized && entry.instance)
        {
            entry.instance->OnUnload();
            entry.initialized = false;
        }
        UnloadEntry(entry);

        // Remove from list
        m_modules.erase(std::remove_if(m_modules.begin(), m_modules.end(),
                                       [&name](const LoadedModule& m) { return m.name == name; }),
                        m_modules.end());

        // Reload
        console.LogInfo("Reloading module: " + savedPath);
        if (!LoadModule(savedPath))
        {
            console.LogError("Failed to reload module: " + name);
            return false;
        }

        // Re-initialize
        for (auto& m : m_modules)
        {
            if (m.path == savedPath && !m.initialized)
            {
                if (m.instance->OnLoad(context))
                {
                    m.initialized = true;
                    console.LogSuccess("Module reloaded and initialized: " + m.name);
                    return true;
                }
                else
                {
                    console.LogError("Module reload init failed: " + m.name);
                    return false;
                }
            }
        }

        return false;
    }

    console.LogError("Module not found for reload: " + name);
    return false;
}

void ModuleManager::UnloadAll()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Core);
    SPARK_LOG_INFO(Spark::LogCategory::Core, "Unloading all modules (%zu loaded)", m_modules.size());

    for (auto& entry : m_modules)
        UnloadEntry(entry);
    m_modules.clear();
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

Spark::IModule* ModuleManager::GetPrimaryModule() const
{
    return m_modules.empty() ? nullptr : m_modules.front().instance;
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

void ModuleManager::UnloadEntry(LoadedModule& entry)
{
    if (entry.instance && entry.destroyFn)
    {
        entry.destroyFn(entry.instance);
    }
    entry.instance = nullptr;
    entry.createFn = nullptr;
    entry.destroyFn = nullptr;

    if (entry.libraryHandle)
    {
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(entry.libraryHandle));
#else
        dlclose(entry.libraryHandle);
#endif
        entry.libraryHandle = nullptr;
    }
}
