/**
 * @file GameModuleLoader.cpp
 * @brief Platform-specific implementation of dynamic game module loading
 */

#include "GameModuleLoader.h"
#include "Utils/SparkConsole.h"

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <Windows.h>
#else
    #include <dlfcn.h>
#endif

GameModuleLoader::~GameModuleLoader()
{
    Unload();
}

bool GameModuleLoader::Load(const std::string& path)
{
    if (m_libraryHandle)
    {
        Spark::SimpleConsole::GetInstance().LogWarning("Game module already loaded. Unload first.");
        return false;
    }

    m_modulePath = path;

    // Load the shared library
#ifdef _WIN32
    m_libraryHandle = LoadLibraryA(path.c_str());
    if (!m_libraryHandle)
    {
        DWORD err = GetLastError();
        Spark::SimpleConsole::GetInstance().LogError(
            "Failed to load game module '" + path + "' (error " + std::to_string(err) + ")");
        return false;
    }

    m_createFn = reinterpret_cast<CreateGameModuleFn>(
        GetProcAddress(static_cast<HMODULE>(m_libraryHandle), "CreateGameModule"));
    m_destroyFn = reinterpret_cast<DestroyGameModuleFn>(
        GetProcAddress(static_cast<HMODULE>(m_libraryHandle), "DestroyGameModule"));
#else
    m_libraryHandle = dlopen(path.c_str(), RTLD_NOW);
    if (!m_libraryHandle)
    {
        const char* err = dlerror();
        Spark::SimpleConsole::GetInstance().LogError(
            std::string("Failed to load game module '") + path + "': " + (err ? err : "unknown error"));
        return false;
    }

    m_createFn = reinterpret_cast<CreateGameModuleFn>(dlsym(m_libraryHandle, "CreateGameModule"));
    m_destroyFn = reinterpret_cast<DestroyGameModuleFn>(dlsym(m_libraryHandle, "DestroyGameModule"));
#endif

    if (!m_createFn || !m_destroyFn)
    {
        Spark::SimpleConsole::GetInstance().LogError(
            "Game module '" + path + "' missing required exports (CreateGameModule/DestroyGameModule)");
        Unload();
        return false;
    }

    // Create the game module instance
    m_module = m_createFn();
    if (!m_module)
    {
        Spark::SimpleConsole::GetInstance().LogError("CreateGameModule() returned null");
        Unload();
        return false;
    }

    Spark::SimpleConsole::GetInstance().LogSuccess(
        std::string("Loaded game module: ") + m_module->GetGameName() +
        " v" + m_module->GetGameVersion());

    return true;
}

void GameModuleLoader::Unload()
{
    if (m_module && m_destroyFn)
    {
        m_destroyFn(m_module);
    }
    m_module = nullptr;
    m_createFn = nullptr;
    m_destroyFn = nullptr;

    if (m_libraryHandle)
    {
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(m_libraryHandle));
#else
        dlclose(m_libraryHandle);
#endif
        m_libraryHandle = nullptr;
    }

    m_modulePath.clear();
}

bool GameModuleLoader::Reload()
{
    if (m_modulePath.empty())
    {
        Spark::SimpleConsole::GetInstance().LogError("No module path to reload from");
        return false;
    }

    std::string savedPath = m_modulePath;

    // Shut down and destroy existing module
    if (m_module)
    {
        m_module->Shutdown();
    }
    Unload();

    Spark::SimpleConsole::GetInstance().LogInfo("Reloading game module: " + savedPath);

    return Load(savedPath);
}
