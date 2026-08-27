/**
 * @file ModuleManager.h
 * @brief Multi-module loader and lifecycle manager
 *
 * ModuleManager replaces GameModuleLoader with support for loading multiple
 * game/gameplay modules simultaneously. Each module is a DLL exporting the
 * mandatory SparkGetModuleCompatibility C descriptor followed by
 * CreateModule()/DestroyModule() (new API) or CreateGameModule()/
 * DestroyGameModule() (legacy API — wrapped via adapter).
 *
 * Modules are loaded from:
 *   1. A spark.modules.json manifest file
 *   2. A directory scan (fallback)
 *   3. Individual paths via LoadModule()
 *
 * Modules are initialized in load-order and shut down in reverse order.
 */

#pragma once

#include "Spark/IModule.h"
#include "Spark/IEngineContext.h"
#include "Core/Contracts.h"
#include <string>
#include <vector>
#include <memory>

namespace Spark
{
    class LocalFileCache;
}

// Forward declaration for legacy adapter
class IGameModule;
using CreateGameModuleFn = IGameModule* (*)();
using DestroyGameModuleFn = void (*)(IGameModule*);

/**
 * @brief Metadata about a module DLL discovered on disk (before loading)
 *
 * Used by the editor's GameModuleSelectorPanel to show available modules
 * without actually loading them. Populated by DiscoverModules().
 */
struct DiscoveredModule
{
    std::string name;      ///< Module name (from ModuleInfo or filename)
    std::string path;      ///< Full path to the DLL/SO
    std::string version;   ///< Module version string
    bool isLoaded = false; ///< Whether this module is currently loaded

    /// Load-policy class from ModuleInfo (single-game-module policy applies to
    /// ModuleKind::Game). Only meaningful when kindKnown is true — legacy
    /// CreateGameModule()-only DLLs cannot be probed for a kind.
    Spark::ModuleKind kind = Spark::ModuleKind::Game;
    bool kindKnown = false; ///< True when kind came from an actual ModuleInfo probe
};

/**
 * @brief Manages the lifecycle of multiple dynamically loaded modules
 */
class ModuleManager
{
  public:
    enum class DiscoveryMode
    {
        ConservativeNameHints,
        CompatibleSidecars,
    };

    ModuleManager() = default;
    ~ModuleManager();

    ModuleManager(const ModuleManager&) = delete;
    ModuleManager& operator=(const ModuleManager&) = delete;

    /**
     * @brief Load a single module from a DLL path
     *
     * Tries the new API (CreateModule/DestroyModule) first, then falls back
     * to the legacy API (CreateGameModule/DestroyGameModule) with an adapter.
     *
     * @param path Path to the DLL/shared library
     * @return true if the module was loaded successfully
     */
    bool LoadModule(const std::string& path);

    /**
     * @brief Load modules listed in a spark.modules.json manifest
     * @param manifestPath Path to the JSON manifest file
     * @return true if at least one module was loaded
     */
    bool LoadModulesFromManifest(const std::string& manifestPath);

    /**
     * @brief Scan a directory for module DLLs and load them
     *
     * Looks for DLLs matching common naming patterns (*Module*.dll, *Game*.dll).
     * This is the fallback when no manifest file is found.
     *
     * NOTE: the single-game-module policy still applies — the second Game-kind
     * module in the directory fails to load (loudly). Prefer the project
     * selector / -game / manifest paths; this bulk loader suits addon packs.
     *
     * @param directory Directory to scan
     * @return true if at least one module was loaded
     */
    bool LoadModulesFromDirectory(const std::string& directory);

    /**
     * @brief Enumerate module-DLL candidates in a directory WITHOUT loading them
     *
     * ConservativeNameHints applies the fallback engine-directory name/system
     * filters. CompatibleSidecars accepts any native library whose ABI sidecar
     * validates without mapping the image; this is intended for bounded project
     * build directories where targets commonly have names such as FPSStarter.
     *
     * @return Absolute paths of probable module DLLs, sorted by filename.
     */
    static std::vector<std::string> DiscoverModuleCandidates(const std::string& directory,
                                                             DiscoveryMode mode = DiscoveryMode::ConservativeNameHints);

    /** @brief Name of the loaded Game-kind module, or empty when none. */
    std::string GetGameModuleName() const;

    /** @brief Name of the initialized Game-kind module, or empty when none is usable. */
    std::string GetInitializedGameModuleName() const;

    /**
     * @brief Initialize all loaded modules (sorted by loadOrder)
     * @param context Engine context passed to each module's OnLoad()
     */
    void InitializeAll(Spark::IEngineContext* context);

    /** @brief Call OnUpdate() on all modules in load order */
    void UpdateAll(float deltaTime);

    /** @brief Call OnFixedUpdate() on all modules in load order.
     *  Driven from the main loop via FixedTimestepAccumulator — before this
     *  existed, IModule::OnFixedUpdate was declared but NEVER invoked, so any
     *  module fixed-step simulation was silently dead. */
    void FixedUpdateAll(float fixedDeltaTime);

    /** @brief Call OnRender() on all modules in load order */
    void RenderAll();

    /** @brief Call OnImGui() on all modules in load order (between ImGui NewFrame/Render) */
    void ImGuiAll();

    /**
     * @brief Provide the host ImGui context + allocators for module injection.
     *
     * Set by the engine executable (game-mode ImGui layer) BEFORE modules are
     * loaded. On Windows each module DLL statically links its own ImGui object
     * code with a DLL-private GImGui; LoadModule passes this payload to the
     * module's exported SparkModuleInjectImGui() so all images share the one
     * exe-owned context (same pattern as SparkModuleInjectConsole).
     */
    static void SetImGuiInjection(void* context, void* allocFn, void* freeFn, void* userData);

    /** @brief Call OnResize() on all modules */
    void ResizeAll(int width, int height);

    /**
     * @brief Non-destructively ask every initialized module whether shutdown is safe.
     * @return false without changing module lifecycle state when any module vetoes.
     */
    bool CanShutdownAll();

    /**
     * @brief Shut down modules in reverse load order.
     * @return true when every initialized module passed CanUnload and shut down.
     * A false result leaves vetoing modules initialized and usable for retry.
     */
    bool ShutdownAll();

    /**
     * @brief Commit module shutdown after a successful CanShutdownAll preflight.
     *
     * This is the non-fallible phase of the two-phase shutdown contract. The
     * caller must not tick modules or otherwise mutate persistent state
     * between preflight and this call.
     */
    void ShutdownAllAfterPreflight();

    /**
     * @brief Roll back modules initialized by a host startup that cannot commit.
     *
     * A startup failure has no usable running state to preserve, so this
     * explicitly bypasses CanUnload vetoes while still invoking OnUnload in
     * reverse dependency order. Do not use this for a live host shutdown.
     */
    void RollbackStartup();

    /**
     * @brief Reload a specific module by name (for hot-reload)
     *
     * Shuts down the module, unloads the DLL, reloads it, and re-initializes.
     * The replacement is first copied to a unique shadow image, fully loaded,
     * and initialized. The working module is committed away only after that
     * staging succeeds, so validation/load/init failures preserve its instance
     * and state. The caller must provide the IEngineContext for staging.
     *
     * @param name Module name to reload
     * @param context Engine context for re-initialization
     * @return true if reload succeeded
     */
    bool ReloadModule(const std::string& name, Spark::IEngineContext* context);

    /** @brief Unload shut-down modules and free their DLLs. Active modules are retained. */
    void UnloadAll();

    /** @brief Get a module by name, or nullptr if not found */
    Spark::IModule* GetModule(const std::string& name) const;

    /** @brief Get the first loaded module (convenience for single-module setups) */
    Spark::IModule* GetPrimaryModule() const;

    /** @brief Get the number of loaded modules */
    size_t GetModuleCount() const { return m_modules.size(); }

    /** @brief Get the number of modules whose OnLoad completed successfully. */
    size_t GetInitializedModuleCount() const;

    /**
     * @brief Detailed reason from the most recent load operation.
     *
     * Empty after a successful LoadModule/LoadModulesFromManifest/
     * LoadModulesFromDirectory/ReloadModule call. The borrowed reference
     * remains valid only until the next load operation; ModuleManager load
     * calls are serialized.
     */
    const std::string& GetLastLoadError() const { return m_lastLoadError; }

    /** @brief Check if any modules are loaded */
    bool HasModules() const { return !m_modules.empty(); }

    /** @brief Get paths and names of all loaded modules for hot-reload watching */
    std::vector<std::pair<std::string, std::string>> GetModulePathsAndNames() const;

    /** @brief Set the file cache for manifest loading (non-owning). */
    void SetFileCache(Spark::LocalFileCache* cache) { m_fileCache = cache; }

    /**
     * @brief Scan a directory for module DLLs without executing them
     *
     * Unloaded candidates use filename/unknown metadata. This method never
     * invokes DllMain, compatibility hooks, injection hooks, or factories.
     * Modules already loaded by this manager report their copied ModuleInfo.
     *
     * @param directory Directory to scan (defaults to executable directory)
     * @return List of discovered modules
     */
    std::vector<DiscoveredModule> DiscoverModules(const std::string& directory) const;

    /**
     * @brief Get info about all currently loaded modules
     * @return List of loaded module names, paths, and versions
     */
    std::vector<DiscoveredModule> GetLoadedModuleInfo() const;

  private:
    struct LoadedModule
    {
        std::string name;
        std::string path;
        void* libraryHandle = nullptr;
        Spark::IModule* instance = nullptr;
        CreateModuleFn createFn = nullptr;
        DestroyModuleFn destroyFn = nullptr;
        int loadOrder = 1000;
        bool initialized = false;
        bool isLegacyAdapter = false;                     ///< True if wrapping IGameModule
        Spark::ModuleKind kind = Spark::ModuleKind::Game; ///< Load-policy class (one Game per process)
        std::string transientImagePath;                   ///< Shadow image removed after the module library is closed
    };

    /** @brief Sort modules by loadOrder */
    void SortModules();

    /** @brief Unload a single module entry */
    void UnloadEntry(LoadedModule& entry);

    std::vector<LoadedModule> m_modules;
    Spark::LocalFileCache* m_fileCache = nullptr;
    std::string m_lastLoadError;
};
