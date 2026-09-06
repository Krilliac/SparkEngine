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
#include <mutex>
#include <optional>
#include <sstream>
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

    void AccumulateLifecycleEvidence(ModuleManager::LifecycleEvidence& target,
                                     const ModuleManager::LifecycleEvidence& source)
    {
        target.initialized += source.initialized;
        target.updated += source.updated;
        target.fixedUpdated += source.fixedUpdated;
        target.rendered += source.rendered;
        target.unloaded += source.unloaded;
        target.faults += source.faults;
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
        std::vector<uint8_t> buffer(64 * 1024);
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
                std::error_code ignored;
                std::filesystem::remove(stagingDirectory, ignored);
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

bool ModuleManager::LoadModule(const std::string& path)
{
    auto& console = Spark::SimpleConsole::GetInstance();
    m_lastLoadError.clear();

    const auto failLoad = [&](std::string message)
    {
        m_lastLoadError = std::move(message);
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

    const std::filesystem::path modulePath = PathFromUtf8(path);

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
    // between the sidecar hash check and LoadLibraryW. The loader may still
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
    handle = LoadLibraryW(modulePath.c_str());
    CloseHandle(pinnedModule);
    if (!handle)
    {
        DWORD err = GetLastError();
        return failLoad(std::format("Failed to load module '{}' with LoadLibraryW (error {})", path, err));
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

    const SparkModuleCompatibilityDescriptor* compatibility = compatibilityFn();
    const Spark::ModuleCompatibilityStatus compatibilityStatus = Spark::CheckModuleCompatibility(compatibility);
    if (compatibilityStatus != Spark::ModuleCompatibilityStatus::Compatible)
    {
        const std::string message =
            std::format("Module '{}' rejected before injection/factory: {}. Rebuild it with the same "
                        "Spark SDK, compiler ABI, C++ mode, architecture, and runtime configuration.",
                        path, Spark::ModuleCompatibilityStatusName(compatibilityStatus));
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

        // SDK version compatibility check
        if (!Spark::IsSDKCompatible(info.sdkVersion))
        {
            const std::string message = std::format("Module '{}' SDK version mismatch (module={}, engine={})",
                                                    info.name, info.sdkVersion, SPARK_SDK_VERSION);
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
#ifndef _WIN32
        entry.transientImagePath = PathToUtf8(stagedImage.Get());
#endif

        console.LogSuccess(std::format("Loaded module: {} v{}", info.name, info.version));
        m_modules.push_back(std::move(entry));
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
            m_lastLoadError = "Could not open module manifest: " + manifestPath;
            console.LogWarning(m_lastLoadError);
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
        m_lastLoadError = "Module manifest is not valid JSON: " + manifestPath +
                          (parseError.empty() ? std::string{} : " (" + parseError + ")");
        console.LogError(m_lastLoadError);
        return false;
    }

    const Spark::Json::Value& modules = manifest["modules"];
    if (!modules.IsArray() || modules.Size() == 0)
    {
        m_lastLoadError = "Module manifest must contain a non-empty modules array: " + manifestPath;
        console.LogError(m_lastLoadError);
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

        if (std::filesystem::exists(fullPath))
        {
            if (LoadModule(PathToUtf8(fullPath)))
                anyLoaded = true;
        }
        else
        {
            m_lastLoadError = "Module not found: " + PathToUtf8(fullPath);
            console.LogWarning(m_lastLoadError);
        }
    }

    if (anyLoaded)
        m_lastLoadError.clear();
    else if (m_lastLoadError.empty())
        m_lastLoadError = "Module manifest did not contain a loadable module: " + manifestPath;
    return anyLoaded;
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
            ++m_lifecycleEvidence.unloaded;
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
            bool callbackCompleted = false;
            SPARK_GUARDED_UPDATE(guardName.c_str(), "Core", {
                entry.instance->OnUpdate(deltaTime);
                ++m_lifecycleEvidence.updated;
                callbackCompleted = true;
            });
            if (!callbackCompleted)
                ++m_lifecycleEvidence.faults;
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
                callbackCompleted = true;
            });
            if (!callbackCompleted)
                ++m_lifecycleEvidence.faults;
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
                callbackCompleted = true;
            });
            if (!callbackCompleted)
                ++m_lifecycleEvidence.faults;
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
            ++m_lifecycleEvidence.unloaded;
            it->initialized = false;
            // Console handlers a module registered under its own id live in the
            // host registry and outlive the DLL unless the host drops them. The
            // module id is the owner token every SDK module registers with.
            const size_t removed = console.UnregisterCommandsByOwner(it->name);
            if (removed != 0)
            {
                SPARK_LOG_INFO(Spark::LogCategory::Core, "Removed %zu console command(s) owned by module '%s'", removed,
                               it->name.c_str());
            }
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
        stagedManager.m_fileCache = m_fileCache;
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

        // Drop the outgoing module's console commands before the replacement's
        // OnLoad runs. They are host-registry entries pointing into an image that
        // the swap below unmaps, and doing it after the swap would delete the
        // replacement's own registrations instead: it re-registers under the same
        // owner token, so the two sets are indistinguishable by then.
        const size_t removedCommands = console.UnregisterCommandsByOwner(name);

        // Initialize the replacement before touching the working instance. A
        // failed OnLoad is cleaned up by InitializeAll and leaves the old
        // module, including its in-memory state, intact.
        stagedManager.InitializeAll(context);
        AccumulateLifecycleEvidence(m_lifecycleEvidence, stagedManager.m_lifecycleEvidence);
        stagedManager.m_lifecycleEvidence = {};
        if (!stagedManager.m_modules.front().initialized || !stagedManager.m_modules.front().instance)
        {
            stagedManager.UnloadAll();
            removeShadowFiles();
            if (removedCommands != 0)
            {
                console.LogWarning("Console commands owned by '" + name +
                                   "' were removed for the reload and the preserved module cannot re-register them; "
                                   "reload again once the module is fixed");
            }
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
            entry.instance->OnUnload();
            ++m_lifecycleEvidence.unloaded;
        }
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
        Spark::SimpleConsole::GetInstance().UnregisterCommandsByOwner(entry->name);
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
