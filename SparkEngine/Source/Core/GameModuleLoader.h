/**
 * @file GameModuleLoader.h
 * @brief Loads game modules (DLLs/shared libraries) at runtime
 *
 * The GameModuleLoader is the engine-side mechanism for discovering and loading
 * game DLLs. It handles platform-specific dynamic library loading (LoadLibrary
 * on Windows, dlopen on Linux/macOS) and resolves the CreateGameModule and
 * DestroyGameModule exports.
 *
 * This is analogous to how Unreal Engine loads game modules or how Unity
 * loads game assemblies into its player runtime.
 */

#pragma once

#include "IGameModule.h"
#include <string>

class GameModuleLoader
{
  public:
    GameModuleLoader() = default;
    ~GameModuleLoader();

    GameModuleLoader(const GameModuleLoader&) = delete;
    GameModuleLoader& operator=(const GameModuleLoader&) = delete;

    /**
     * @brief Load a game module from a DLL/shared library
     * @param path Path to the game DLL (e.g. "SparkGame.dll" or "./libSparkGame.so")
     * @return true if the module was loaded and exports were resolved
     */
    bool Load(const std::string& path);

    /**
     * @brief Unload the current game module and free the library
     *
     * Calls DestroyGameModule on the current module instance before unloading.
     */
    void Unload();

    /**
     * @brief Reload the game module (for hot-reload during development)
     *
     * Shuts down the current module, unloads the DLL, reloads it, and
     * creates a new module instance. The caller must re-initialize the
     * new module after this call.
     *
     * @return true if reload succeeded
     */
    bool Reload();

    /** @brief Check if a game module is currently loaded */
    bool IsLoaded() const { return m_module != nullptr; }

    /** @brief Get the loaded game module instance */
    IGameModule* GetModule() const { return m_module; }

    /** @brief Get the path the module was loaded from */
    const std::string& GetModulePath() const { return m_modulePath; }

  private:
    void* m_libraryHandle{nullptr};
    IGameModule* m_module{nullptr};
    CreateGameModuleFn m_createFn{nullptr};
    DestroyGameModuleFn m_destroyFn{nullptr};
    std::string m_modulePath;
};
