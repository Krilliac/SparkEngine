/**
 * @file ModuleManager.cpp
 * @brief Multi-module loader and lifecycle manager implementation
 */

#include "ModuleManager.h"
#include "Contracts.h"
#include "EngineContext.h"
#include "FaultIsolation.h"
#include "IGameModule.h"
#include "Spark/ModuleABI.h"
#include "Spark/Version.h"
#include "Utils/SparkConsole.h"
#include "Utils/LocalFileCache.h"
#include "Utils/JsonUtils.h"
#include "Utils/Validate.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#endif // SPARK_PLATFORM_WINDOWS
#else
#include <dlfcn.h>
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

    std::filesystem::path PathFromUtf8(std::string_view path)
    {
        return std::filesystem::u8path(path.begin(), path.end());
    }

    std::string PathToUtf8(const std::filesystem::path& path)
    {
        const std::u8string utf8 = path.generic_u8string();
        return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    }

    std::filesystem::path SidecarPath(const std::filesystem::path& modulePath)
    {
        std::filesystem::path sidecar = modulePath;
        sidecar += ".sparkabi";
        return sidecar;
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

    constexpr uint32_t RotateRight(uint32_t value, uint32_t bits)
    {
        return (value >> bits) | (value << (32u - bits));
    }

    class ModuleSha256
    {
      public:
        void Update(const uint8_t* data, size_t size)
        {
            for (size_t i = 0; i < size; ++i)
            {
                m_buffer[m_bufferSize++] = data[i];
                if (m_bufferSize == m_buffer.size())
                {
                    Transform(m_buffer.data());
                    m_bitLength += 512;
                    m_bufferSize = 0;
                }
            }
        }

        std::string Finalize()
        {
            m_bitLength += static_cast<uint64_t>(m_bufferSize) * 8u;
            m_buffer[m_bufferSize++] = 0x80u;

            if (m_bufferSize > 56)
            {
                while (m_bufferSize < 64)
                    m_buffer[m_bufferSize++] = 0;
                Transform(m_buffer.data());
                m_bufferSize = 0;
            }

            while (m_bufferSize < 56)
                m_buffer[m_bufferSize++] = 0;
            for (size_t i = 0; i < 8; ++i)
                m_buffer[63 - i] = static_cast<uint8_t>(m_bitLength >> (i * 8u));
            Transform(m_buffer.data());

            static constexpr char kHex[] = "0123456789abcdef";
            std::string result;
            result.resize(64);
            for (size_t i = 0; i < m_state.size(); ++i)
            {
                for (size_t byte = 0; byte < 4; ++byte)
                {
                    const uint8_t value = static_cast<uint8_t>(m_state[i] >> ((3u - byte) * 8u));
                    const size_t offset = (i * 8u) + (byte * 2u);
                    result[offset] = kHex[value >> 4u];
                    result[offset + 1] = kHex[value & 0x0fu];
                }
            }
            return result;
        }

      private:
        void Transform(const uint8_t* block)
        {
            static constexpr std::array<uint32_t, 64> kRoundConstants = {
                0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
                0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
                0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
                0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
                0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
                0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
                0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
                0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
            };

            std::array<uint32_t, 64> words{};
            for (size_t i = 0; i < 16; ++i)
            {
                const size_t offset = i * 4;
                words[i] = (static_cast<uint32_t>(block[offset]) << 24u) |
                           (static_cast<uint32_t>(block[offset + 1]) << 16u) |
                           (static_cast<uint32_t>(block[offset + 2]) << 8u) | static_cast<uint32_t>(block[offset + 3]);
            }
            for (size_t i = 16; i < words.size(); ++i)
            {
                const uint32_t s0 =
                    RotateRight(words[i - 15], 7) ^ RotateRight(words[i - 15], 18) ^ (words[i - 15] >> 3u);
                const uint32_t s1 =
                    RotateRight(words[i - 2], 17) ^ RotateRight(words[i - 2], 19) ^ (words[i - 2] >> 10u);
                words[i] = words[i - 16] + s0 + words[i - 7] + s1;
            }

            uint32_t a = m_state[0];
            uint32_t b = m_state[1];
            uint32_t c = m_state[2];
            uint32_t d = m_state[3];
            uint32_t e = m_state[4];
            uint32_t f = m_state[5];
            uint32_t g = m_state[6];
            uint32_t h = m_state[7];
            for (size_t i = 0; i < words.size(); ++i)
            {
                const uint32_t sum1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
                const uint32_t choose = (e & f) ^ (~e & g);
                const uint32_t temp1 = h + sum1 + choose + kRoundConstants[i] + words[i];
                const uint32_t sum0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
                const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
                const uint32_t temp2 = sum0 + majority;
                h = g;
                g = f;
                f = e;
                e = d + temp1;
                d = c;
                c = b;
                b = a;
                a = temp1 + temp2;
            }

            m_state[0] += a;
            m_state[1] += b;
            m_state[2] += c;
            m_state[3] += d;
            m_state[4] += e;
            m_state[5] += f;
            m_state[6] += g;
            m_state[7] += h;
        }

        std::array<uint32_t, 8> m_state = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                           0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
        std::array<uint8_t, 64> m_buffer{};
        size_t m_bufferSize = 0;
        uint64_t m_bitLength = 0;
    };

    std::optional<std::string> ComputeModuleSha256(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return std::nullopt;

        ModuleSha256 sha;
        std::array<uint8_t, 64 * 1024> buffer{};
        while (file)
        {
            file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = file.gcount();
            if (count > 0)
                sha.Update(buffer.data(), static_cast<size_t>(count));
        }
        if (!file.eof())
            return std::nullopt;
        return sha.Finalize();
    }

    bool ParseSidecarUInt(const std::unordered_map<std::string, std::string>& values, std::string_view key,
                          uint32_t& output, std::string& error)
    {
        const auto it = values.find(std::string(key));
        if (it == values.end())
        {
            error = "missing field '" + std::string(key) + "'";
            return false;
        }

        const char* begin = it->second.data();
        const char* end = begin + it->second.size();
        const auto [parsedEnd, parseError] = std::from_chars(begin, end, output);
        if (parseError != std::errc{} || parsedEnd != end)
        {
            error = "invalid integer field '" + std::string(key) + "'";
            return false;
        }
        return true;
    }

    bool ValidateModuleSidecar(const std::filesystem::path& modulePath, std::string& error)
    {
        const std::filesystem::path sidecarPath = SidecarPath(modulePath);
        std::ifstream sidecar(sidecarPath, std::ios::binary);
        if (!sidecar)
        {
            error = "missing mandatory ABI sidecar '" + PathToUtf8(sidecarPath) + "'";
            return false;
        }

        std::unordered_map<std::string, std::string> values;
        std::string line;
        while (std::getline(sidecar, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            const size_t separator = line.find('=');
            if (separator == std::string::npos || separator == 0 || separator + 1 >= line.size())
            {
                error = "malformed ABI sidecar line";
                return false;
            }
            if (!values.emplace(line.substr(0, separator), line.substr(separator + 1)).second)
            {
                error = "duplicate ABI sidecar field";
                return false;
            }
        }

        SparkModuleCompatibilityDescriptor descriptor{};
        if (!ParseSidecarUInt(values, "struct_size", descriptor.structSize, error) ||
            !ParseSidecarUInt(values, "magic", descriptor.magic, error) ||
            !ParseSidecarUInt(values, "format", descriptor.descriptorVersion, error) ||
            !ParseSidecarUInt(values, "sdk_version", descriptor.sdkVersion, error) ||
            !ParseSidecarUInt(values, "runtime_abi_version", descriptor.runtimeABIVersion, error) ||
            !ParseSidecarUInt(values, "compiler_family", descriptor.compilerFamily, error) ||
            !ParseSidecarUInt(values, "compiler_abi_version", descriptor.compilerABIVersion, error) ||
            !ParseSidecarUInt(values, "cxx_language_level", descriptor.cxxLanguageLevel, error) ||
            !ParseSidecarUInt(values, "runtime_library", descriptor.runtimeLibrary, error) ||
            !ParseSidecarUInt(values, "iterator_debug_level", descriptor.iteratorDebugLevel, error) ||
            !ParseSidecarUInt(values, "pointer_size", descriptor.pointerSize, error))
        {
            return false;
        }

        const auto hashIt = values.find("binary_sha256");
        if (hashIt == values.end() || hashIt->second.size() != 64)
        {
            error = "missing or invalid binary_sha256 field";
            return false;
        }
        if (values.size() != 12)
        {
            error = "unexpected ABI sidecar fields";
            return false;
        }

        const Spark::ModuleCompatibilityStatus status = Spark::CheckModuleCompatibility(&descriptor);
        if (status != Spark::ModuleCompatibilityStatus::Compatible)
        {
            error = Spark::ModuleCompatibilityStatusName(status);
            return false;
        }

        const std::optional<std::string> actualHash = ComputeModuleSha256(modulePath);
        if (!actualHash)
        {
            error = "failed to hash module binary";
            return false;
        }
        if (*actualHash != hashIt->second)
        {
            error = "ABI sidecar binary hash mismatch";
            return false;
        }
        return true;
    }
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
}

bool ModuleManager::LoadModule(const std::string& path)
{
    SPARK_EXPECTS(!path.empty());
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

    const std::filesystem::path modulePath = PathFromUtf8(path);

#ifdef _WIN32
    // Prevent a concurrent rebuild, rename, or delete from changing the image
    // between the sidecar hash check and LoadLibraryW. The loader may still
    // acquire its own read handle, while writers and delete/replace operations
    // remain excluded until the mapped image has been opened.
    HANDLE pinnedModule = CreateFileW(modulePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (pinnedModule == INVALID_HANDLE_VALUE)
    {
        console.LogError(std::format("Failed to pin module '{}' for validation (error {})", path, GetLastError()));
        return false;
    }
#endif

    // Preflight the sidecar before asking the OS loader to map the image. The
    // SHA-256 binds the descriptor to this exact binary, so an incompatible or
    // stale candidate is rejected before DllMain/static constructors execute.
    std::string sidecarError;
    if (!ValidateModuleSidecar(modulePath, sidecarError))
    {
#ifdef _WIN32
        CloseHandle(pinnedModule);
#endif
        console.LogError(std::format("Module '{}' rejected before OS load: {}", path, sidecarError));
        return false;
    }

    // Load the shared library only after the non-executing compatibility gate.
    void* handle = nullptr;
#ifdef _WIN32
    handle = LoadLibraryW(modulePath.c_str());
    CloseHandle(pinnedModule);
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

    // Re-read the in-image descriptor as defense in depth after the sidecar
    // has established compatibility. It has a fixed C ABI and returns an
    // integer-only POD; no C++ object, allocator, vtable, or engine pointer
    // crosses before both compatibility checks have passed.
    auto compatibilityFn =
        ResolveModuleExport<SparkGetModuleCompatibilityFn>(handle, SPARK_MODULE_COMPATIBILITY_EXPORT_NAME);
    if (!compatibilityFn)
    {
        console.LogError(std::format(
            "Module '{}' rejected before injection/factory: missing mandatory {} export. "
            "Rebuild the module with the current Spark SDK (SPARK_IMPLEMENT_MODULE exports it automatically).",
            path, SPARK_MODULE_COMPATIBILITY_EXPORT_NAME));
        CloseModuleLibrary(handle);
        return false;
    }

    const SparkModuleCompatibilityDescriptor* compatibility = compatibilityFn();
    const Spark::ModuleCompatibilityStatus compatibilityStatus = Spark::CheckModuleCompatibility(compatibility);
    if (compatibilityStatus != Spark::ModuleCompatibilityStatus::Compatible)
    {
        console.LogError(std::format("Module '{}' rejected before injection/factory: {}. Rebuild it with the same "
                                     "Spark SDK, compiler ABI, C++ mode, architecture, and runtime configuration.",
                                     path, Spark::ModuleCompatibilityStatusName(compatibilityStatus)));
        CloseModuleLibrary(handle);
        return false;
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

        // Single-game-module policy: game modules own the simulation (physics
        // stepping, world, net sim) — two of them double-step physics and
        // corrupt each other. Addon-kind modules coexist freely.
        if (info.kind == Spark::ModuleKind::Game)
        {
            const std::string existing = GetGameModuleName();
            if (!existing.empty())
            {
                console.LogError(std::format("REFUSED to load game module '{}': game module '{}' is already loaded. "
                                             "One game module per process; mark libraries/extensions with "
                                             "ModuleKind::Addon in their ModuleInfo.",
                                             info.name, existing));
                destroyFn(instance);
#ifdef _WIN32
                FreeLibrary(static_cast<HMODULE>(handle));
#else
                dlclose(handle);
#endif
                return false;
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

        // Legacy CreateGameModule exports are game modules by definition —
        // the single-game-module policy applies to them too.
        {
            const std::string existing = GetGameModuleName();
            if (!existing.empty())
            {
                console.LogError(std::format("REFUSED to load legacy game module '{}': game module '{}' is already "
                                             "loaded (one game module per process).",
                                             info.name, existing));
                // Destroy the adapter (and therefore the legacy object through
                // its DLL export) while the library code is still resident.
                adapterOwner.reset();
#ifdef _WIN32
                FreeLibrary(static_cast<HMODULE>(handle));
#else
                dlclose(handle);
#endif
                return false;
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

        // Transfer ownership last to avoid leak if any prior line throws
        entry.instance = adapterOwner.release();

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
    const std::filesystem::path manifestFile = PathFromUtf8(manifestPath);

    std::string content;

#ifndef _WIN32
    // LocalFileCache's string-only FileUtils backend is UTF-8-safe on POSIX,
    // but cannot open arbitrary Unicode paths on Windows. Use the native path
    // stream below there so the manifest stays wide end-to-end.
    if (m_fileCache)
    {
        auto result = m_fileCache->ReadText(manifestPath);
        if (result.IsOk())
        {
            content = result.Value();
        }
    }
#endif

    if (content.empty())
    {
        std::ifstream file(manifestFile);
        if (!file.is_open())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "ModuleManager: cannot open manifest '%s' (errno=%d)",
                            manifestPath.c_str(), errno);
            console.LogWarning("Could not open module manifest: " + manifestPath);
            return false;
        }
        content.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();
    }

    const std::filesystem::path manifestDir = manifestFile.parent_path();
    bool anyLoaded = false;

    Spark::Json::Value manifest;
    std::string parseError;
    if (!Spark::Json::ParseStrict(content, &manifest, &parseError) || !manifest.IsObject())
    {
        console.LogError("Module manifest is not valid JSON: " + manifestPath +
                         (parseError.empty() ? std::string{} : " (" + parseError + ")"));
        return false;
    }

    const Spark::Json::Value& modules = manifest["modules"];
    if (!modules.IsArray() || modules.Size() == 0)
    {
        console.LogError("Module manifest must contain a non-empty modules array: " + manifestPath);
        return false;
    }

    for (size_t index = 0; index < modules.Size(); ++index)
    {
        const Spark::Json::Value& module = modules[index];
        if (!module.IsObject())
        {
            console.LogWarning(std::format("Module manifest entry {} has no string path", index));
            continue;
        }
        const Spark::Json::Value& path = module["path"];
        if (!path.IsString())
        {
            console.LogWarning(std::format("Module manifest entry {} has no string path", index));
            continue;
        }

        const std::string modulePath = path.AsString();
        if (modulePath.empty())
        {
            console.LogWarning(std::format("Module manifest entry {} has an empty path", index));
            continue;
        }

        // Resolve relative paths against manifest directory
        std::filesystem::path fullPath = PathFromUtf8(modulePath);
        if (fullPath.is_relative())
            fullPath = manifestDir / fullPath;

        if (std::filesystem::exists(fullPath))
        {
            if (LoadModule(PathToUtf8(fullPath)))
                anyLoaded = true;
        }
        else
        {
            console.LogWarning("Module not found: " + PathToUtf8(fullPath));
        }
    }

    return anyLoaded;
}

bool ModuleManager::LoadModulesFromDirectory(const std::string& directory)
{
    auto& console = Spark::SimpleConsole::GetInstance();
    bool anyLoaded = false;

    if (!std::filesystem::exists(PathFromUtf8(directory)))
    {
        console.LogWarning("Module directory does not exist: " + directory);
        return false;
    }

    for (const auto& candidate : DiscoverModuleCandidates(directory))
    {
        console.LogInfo("Found candidate module: " + PathToUtf8(PathFromUtf8(candidate).filename()));
        if (LoadModule(candidate))
            anyLoaded = true;
    }

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
        if (m.kind == Spark::ModuleKind::Game)
            return m.name;
    }
    return {};
}

void ModuleManager::InitializeAll(Spark::IEngineContext* context)
{
    SPARK_EXPECTS(context != nullptr);
    auto& console = Spark::SimpleConsole::GetInstance();

    if (!context)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "InitializeAll called with null context");
        return;
    }

    for (auto& entry : m_modules)
    {
        if (entry.initialized)
            continue;
        if (!entry.instance)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "Module '%s' has null instance — skipped", entry.name.c_str());
            continue;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Core, "Initializing module: %s", entry.name.c_str());
        console.LogInfo("Initializing module: " + entry.name);
        if (entry.instance->OnLoad(context))
        {
            entry.initialized = true;
            console.LogSuccess("Module initialized: " + entry.name);
        }
        else
        {
            console.LogError("Module initialization failed: " + entry.name);

            // Failed-boot teardown ordering (W10 exit AV): a module that fails
            // OnLoad never gets OnUnload from ShutdownAll (initialized stays
            // false), so its instance used to survive until UnloadAll — which
            // runs AFTER engine physics teardown. Its destructor then released
            // shared_ptr<PhysicsBody> handles into a destroyed PhysicsSystem
            // (dangling EngineContext/raw pointers → AV at exit). Destroy the
            // instance NOW, while every engine service it may reference
            // (physics, ECS world, event bus) is still alive. OnUnload() runs
            // first so the module can deregister anything its partial OnLoad
            // installed. The DLL itself stays mapped until the normal
            // UnloadAll so any registrations that survive (console commands,
            // event channels) never point at unmapped code.
            SPARK_LOG_WARN(Spark::LogCategory::Core,
                           "Module '%s' failed OnLoad — destroying its instance immediately "
                           "(DLL stays mapped until engine shutdown)",
                           entry.name.c_str());
            entry.instance->OnUnload();
            if (entry.destroyFn)
            {
                entry.destroyFn(entry.instance);
            }
            entry.instance = nullptr;
            entry.destroyFn = nullptr;
        }
    }
}

void ModuleManager::UpdateAll(float deltaTime)
{
    for (auto& entry : m_modules)
    {
        if (entry.initialized && entry.instance)
        {
            std::string guardName = "Module:" + entry.name;
            SPARK_GUARDED_UPDATE(guardName.c_str(), "Core", { entry.instance->OnUpdate(deltaTime); });
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
            SPARK_GUARDED_UPDATE(guardName.c_str(), "Core", { entry.instance->OnFixedUpdate(fixedDeltaTime); });
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
            SPARK_GUARDED_UPDATE(guardName.c_str(), "Core", { entry.instance->OnRender(); });
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
            it->instance->OnUnload();
            it->initialized = false;
        }
    }
}

bool ModuleManager::ReloadModule(const std::string& name, Spark::IEngineContext* context)
{
    auto& console = Spark::SimpleConsole::GetInstance();

    if (!context)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "ReloadModule called with null context");
        return false;
    }

    for (size_t index = 0; index < m_modules.size(); ++index)
    {
        auto& entry = m_modules[index];
        if (entry.name != name)
            continue;

        if (entry.instance && !entry.instance->SupportsHotReload())
        {
            console.LogError("Module does not support transactional hot reload; perform a full restart: " + name);
            return false;
        }

        // A stateful module may need to checkpoint before an image swap. Run
        // the non-destructive gate before staging a replacement so a veto
        // leaves the working instance and all of its dependencies untouched.
        if (entry.initialized && entry.instance && !entry.instance->CanUnload())
        {
            console.LogError("Module refused hot reload and remains active: " + name);
            return false;
        }

        const std::string savedPath = entry.path;
        const std::filesystem::path sourcePath = PathFromUtf8(savedPath);
        const uint64_t reloadToken = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        static std::atomic<uint64_t> reloadSerial{0};
        std::filesystem::path shadowName = sourcePath.stem();
        shadowName += ".spark-reload-" + std::to_string(reloadToken) + "-" +
                      std::to_string(reloadSerial.fetch_add(1, std::memory_order_relaxed));
        shadowName += sourcePath.extension();
        const std::filesystem::path shadowPath = sourcePath.parent_path() / shadowName;
        const std::filesystem::path sourceSidecar = SidecarPath(sourcePath);
        const std::filesystem::path shadowSidecar = SidecarPath(shadowPath);

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
            console.LogError("Failed to stage module reload for '" + name + "': " + copyError.message());
            removeShadowFiles();
            return false;
        }

        ModuleManager stagedManager;
        stagedManager.m_fileCache = m_fileCache;
        if (!stagedManager.LoadModule(PathToUtf8(shadowPath)))
        {
            console.LogError("Failed to validate staged replacement for module: " + name);
            stagedManager.UnloadAll();
            removeShadowFiles();
            return false;
        }

        if (stagedManager.m_modules.size() != 1 || stagedManager.m_modules.front().name != name)
        {
            console.LogError("Staged replacement identity does not match module: " + name);
            stagedManager.UnloadAll();
            removeShadowFiles();
            return false;
        }

        const Spark::ModuleKind replacementKind = stagedManager.m_modules.front().kind;
        if (replacementKind == Spark::ModuleKind::Game)
        {
            for (size_t otherIndex = 0; otherIndex < m_modules.size(); ++otherIndex)
            {
                if (otherIndex != index && m_modules[otherIndex].kind == Spark::ModuleKind::Game)
                {
                    console.LogError("Staged replacement would violate the one-game-module policy: " + name);
                    stagedManager.UnloadAll();
                    removeShadowFiles();
                    return false;
                }
            }
        }

        // Initialize the replacement before touching the working instance. A
        // failed OnLoad is cleaned up by InitializeAll and leaves the old
        // module, including its in-memory state, intact.
        stagedManager.InitializeAll(context);
        if (!stagedManager.m_modules.front().initialized || !stagedManager.m_modules.front().instance)
        {
            console.LogError("Staged replacement initialization failed; preserving module: " + name);
            stagedManager.UnloadAll();
            removeShadowFiles();
            return false;
        }

        LoadedModule replacement = std::move(stagedManager.m_modules.front());
        stagedManager.m_modules.clear();
        replacement.path = savedPath;
        replacement.transientImagePath = PathToUtf8(shadowPath);

        // Commit only after the replacement is fully usable.
        if (entry.initialized && entry.instance)
            entry.instance->OnUnload();
        UnloadEntry(entry);
        m_modules[index] = std::move(replacement);
        SortModules();
        console.LogSuccess("Module transactionally reloaded and initialized: " + name);
        return true;
    }

    console.LogError("Module not found for reload: " + name);
    return false;
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

    if (!entry.transientImagePath.empty())
    {
        std::error_code cleanupError;
        const std::filesystem::path transientPath = PathFromUtf8(entry.transientImagePath);
        std::filesystem::remove(SidecarPath(transientPath), cleanupError);
        cleanupError.clear();
        std::filesystem::remove(transientPath, cleanupError);
        entry.transientImagePath.clear();
    }
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
