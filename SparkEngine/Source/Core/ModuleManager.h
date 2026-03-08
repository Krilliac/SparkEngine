/**
 * @file ModuleManager.h
 * @brief Multi-module loader and lifecycle manager
 *
 * ModuleManager replaces GameModuleLoader with support for loading multiple
 * game/gameplay modules simultaneously. Each module is a DLL exporting
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
#include <string>
#include <vector>
#include <memory>

// Forward declaration for legacy adapter
class IGameModule;
using CreateGameModuleFn = IGameModule* (*)();
using DestroyGameModuleFn = void (*)(IGameModule*);

/**
 * @brief Manages the lifecycle of multiple dynamically loaded modules
 */
class ModuleManager
{
  public:
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
     * @param directory Directory to scan
     * @return true if at least one module was loaded
     */
    bool LoadModulesFromDirectory(const std::string& directory);

    /**
     * @brief Initialize all loaded modules (sorted by loadOrder)
     * @param context Engine context passed to each module's OnLoad()
     */
    void InitializeAll(Spark::IEngineContext* context);

    /** @brief Call OnUpdate() on all modules in load order */
    void UpdateAll(float deltaTime);

    /** @brief Call OnRender() on all modules in load order */
    void RenderAll();

    /** @brief Call OnResize() on all modules */
    void ResizeAll(int width, int height);

    /** @brief Shut down all modules in reverse load order, then unload DLLs */
    void ShutdownAll();

    /**
     * @brief Reload a specific module by name (for hot-reload)
     *
     * Shuts down the module, unloads the DLL, reloads it, and re-initializes.
     * The caller must have stored the IEngineContext to pass for re-init.
     *
     * @param name Module name to reload
     * @param context Engine context for re-initialization
     * @return true if reload succeeded
     */
    bool ReloadModule(const std::string& name, Spark::IEngineContext* context);

    /** @brief Unload all modules and free all DLLs */
    void UnloadAll();

    /** @brief Get a module by name, or nullptr if not found */
    Spark::IModule* GetModule(const std::string& name) const;

    /** @brief Get the first loaded module (convenience for single-module setups) */
    Spark::IModule* GetPrimaryModule() const;

    /** @brief Get the number of loaded modules */
    size_t GetModuleCount() const { return m_modules.size(); }

    /** @brief Check if any modules are loaded */
    bool HasModules() const { return !m_modules.empty(); }

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
        bool isLegacyAdapter = false; ///< True if wrapping IGameModule
    };

    /** @brief Sort modules by loadOrder */
    void SortModules();

    /** @brief Unload a single module entry */
    void UnloadEntry(LoadedModule& entry);

    std::vector<LoadedModule> m_modules;
};
