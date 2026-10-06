/**
 * @file CrashArtifactDirectory.h
 * @brief Private crash-artifact directory creation with platform-specific security.
 */
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <objbase.h>
#include <sddl.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Spark::CrashHandlerDetail
{
    namespace Private
    {
        inline bool FillRandomBytes(std::array<std::uint8_t, 16>& bytes)
        {
#ifdef _WIN32
            return BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                                   BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
            const int fd = open("/dev/urandom", O_RDONLY
#ifdef O_CLOEXEC
                                                    | O_CLOEXEC
#endif
            );
            if (fd < 0)
                return false;

            size_t offset = 0;
            while (offset < bytes.size())
            {
                const ssize_t count = read(fd, bytes.data() + offset, bytes.size() - offset);
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0)
                    break;
                offset += static_cast<size_t>(count);
            }
            close(fd);
            return offset == bytes.size();
#endif
        }

        inline std::string RandomDirectorySuffix()
        {
            std::array<std::uint8_t, 16> bytes{};
            if (!FillRandomBytes(bytes))
                return {};

            constexpr char hex[] = "0123456789abcdef";
            std::string suffix;
            suffix.resize(bytes.size() * 2);
            for (size_t index = 0; index < bytes.size(); ++index)
            {
                suffix[index * 2] = hex[bytes[index] >> 4];
                suffix[index * 2 + 1] = hex[bytes[index] & 0x0F];
            }
            return suffix;
        }

#ifdef _WIN32
        /// What the process token says about the sandbox this process runs in.
        struct SandboxFacts
        {
            bool valid = false;
            bool appContainer = false;
            bool belowMediumIntegrity = false;
            std::wstring appContainerSid; ///< String SID; empty outside an AppContainer.
        };

        inline SandboxFacts ReadSandboxFacts()
        {
            SandboxFacts facts;
            HANDLE token = nullptr;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            {
                return facts;
            }

            DWORD length = 0;
            DWORD isAppContainer = 0;
            const bool hasAppContainerStatus =
                GetTokenInformation(token, TokenIsAppContainer, &isAppContainer, sizeof(isAppContainer), &length) != 0;
            if (hasAppContainerStatus)
            {
                facts.appContainer = isAppContainer != 0;
            }
            if (facts.appContainer)
            {
                alignas(TOKEN_APPCONTAINER_INFORMATION)
                    std::array<unsigned char, sizeof(TOKEN_APPCONTAINER_INFORMATION) + SECURITY_MAX_SID_SIZE>
                        buffer{};
                LPWSTR text = nullptr;
                if (GetTokenInformation(token, TokenAppContainerSid, buffer.data(), static_cast<DWORD>(buffer.size()),
                                        &length) &&
                    ConvertSidToStringSidW(
                        reinterpret_cast<TOKEN_APPCONTAINER_INFORMATION*>(buffer.data())->TokenAppContainer, &text))
                {
                    facts.appContainerSid = text;
                    LocalFree(text);
                }
            }

            alignas(TOKEN_MANDATORY_LABEL)
                std::array<unsigned char, sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE>
                    label{};
            if (GetTokenInformation(token, TokenIntegrityLevel, label.data(), static_cast<DWORD>(label.size()),
                                    &length))
            {
                PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(label.data())->Label.Sid;
                if (IsValidSid(sid) && *GetSidSubAuthorityCount(sid) != 0)
                {
                    const DWORD rid = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
                    facts.belowMediumIntegrity = rid < SECURITY_MANDATORY_MEDIUM_RID;
                    facts.valid = hasAppContainerStatus;
                }
            }
            CloseHandle(token);
            return facts;
        }

        /// Resolve an export of a System32 DLL at run time, so this header adds no
        /// userenv or shell32 import-library requirement. Callers cache the result;
        /// the loaded module stays pinned while the function pointer is in use.
        template <typename Function> Function LoadSystemFunction(const wchar_t* moduleName, const char* functionName)
        {
            HMODULE module = LoadLibraryExW(moduleName, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (module == nullptr)
            {
                return nullptr;
            }
            FARPROC function = GetProcAddress(module, functionName);
            if (function == nullptr)
            {
                FreeLibrary(module);
                return nullptr;
            }
            return reinterpret_cast<Function>(function);
        }

        /// A shell/userenv string result, freed with CoTaskMemFree.
        inline std::filesystem::path TakeCoTaskMemPath(PWSTR text)
        {
            if (text == nullptr)
            {
                return {};
            }
            std::filesystem::path result(text);
            CoTaskMemFree(text);
            return result;
        }

        inline bool IsOrdinaryDirectory(const std::filesystem::path& candidate)
        {
            const DWORD attributes = GetFileAttributesW(candidate.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                   (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
        }

        /// The per-container folder (%LOCALAPPDATA%\\Packages\\<name>\\AC) an AppContainer can write.
        inline std::filesystem::path AppContainerFolder(const std::wstring& appContainerSid)
        {
            using GetFolderFunction = HRESULT(WINAPI*)(PCWSTR, PWSTR*);
            static const auto getFolder =
                LoadSystemFunction<GetFolderFunction>(L"userenv.dll", "GetAppContainerFolderPath");
            PWSTR folder = nullptr;
            if (getFolder == nullptr || appContainerSid.empty() || FAILED(getFolder(appContainerSid.c_str(), &folder)))
            {
                CoTaskMemFree(folder);
                return {};
            }
            return TakeCoTaskMemPath(folder);
        }

        /// FOLDERID_LocalAppDataLow, the per-user folder a low-integrity process can write.
        inline std::filesystem::path LocalAppDataLowFolder()
        {
            static constexpr GUID kLocalAppDataLow = {
                0xA520A1A4, 0x1780, 0x4FF6, {0xBD, 0x18, 0x16, 0x73, 0x43, 0xC5, 0xAF, 0x16}};
            using GetKnownFolderFunction = HRESULT(WINAPI*)(const GUID&, DWORD, HANDLE, PWSTR*);
            static const auto getKnownFolder =
                LoadSystemFunction<GetKnownFolderFunction>(L"shell32.dll", "SHGetKnownFolderPath");
            const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
            {
                return {};
            }
            PWSTR folder = nullptr;
            const bool found =
                getKnownFolder != nullptr && SUCCEEDED(getKnownFolder(kLocalAppDataLow, 0, nullptr, &folder));
            const std::filesystem::path result = found ? TakeCoTaskMemPath(folder) : std::filesystem::path{};
            if (!found)
            {
                CoTaskMemFree(folder); // the API may allocate even on failure
            }
            if (SUCCEEDED(initialized))
            {
                CoUninitialize();
            }
            return result;
        }
#endif
    } // namespace Private

    /**
     * @brief Atomically create one owner-only crash-artifact directory.
     *
     * Existing paths, symlinks, and reparse points are never accepted. This
     * exact-candidate entry point is intentionally exposed for deterministic
     * security tests; normal callers should use CreatePrivateCrashArtifactDirectory.
     */
    inline bool TryCreatePrivateCrashArtifactDirectory(const std::filesystem::path& candidate)
    {
        if (candidate.empty())
            return false;

#ifdef _WIN32
        // Protected DACL: the owner and SYSTEM only. An AppContainer access check
        // also needs a grant to the container's own SID (OWNER RIGHTS alone fails
        // its second pass with ERROR_ACCESS_DENIED), so a sandboxed process adds
        // exactly that SID - never ALL APPLICATION PACKAGES or a capability.
        std::wstring sddl = L"D:P(A;;FA;;;OW)(A;;FA;;;SY)";
        const Private::SandboxFacts sandbox = Private::ReadSandboxFacts();
        if (!sandbox.valid)
        {
            return false;
        }
        if (sandbox.appContainer)
        {
            if (sandbox.appContainerSid.empty())
            {
                return false;
            }
            sddl += L"(A;;FA;;;" + sandbox.appContainerSid + L")";
        }

        PSECURITY_DESCRIPTOR securityDescriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &securityDescriptor,
                                                                  nullptr))
        {
            return false;
        }

        SECURITY_ATTRIBUTES securityAttributes{};
        securityAttributes.nLength = sizeof(securityAttributes);
        securityAttributes.lpSecurityDescriptor = securityDescriptor;
        const BOOL created = CreateDirectoryW(candidate.c_str(), &securityAttributes);
        LocalFree(securityDescriptor);
        if (!created)
            return false;

        const DWORD attributes = GetFileAttributesW(candidate.c_str());
        const bool valid = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
        if (!valid)
            RemoveDirectoryW(candidate.c_str());
        return valid;
#else
        if (mkdir(candidate.c_str(), S_IRWXU) != 0)
            return false;

        int flags = O_RDONLY;
#ifdef O_DIRECTORY
        flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        const int fd = open(candidate.c_str(), flags);
        struct stat info
        {
        };
        const bool valid = fd >= 0 && fstat(fd, &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == geteuid() &&
                           (info.st_mode & (S_IRWXG | S_IRWXO)) == 0;
        if (fd >= 0)
            close(fd);
        if (!valid)
            rmdir(candidate.c_str());
        return valid;
#endif
    }

    /**
     * @brief The directory that holds this process's spark_crash_* artifact roots.
     *
     * InstallCrashHandler() prunes stale roots here and creates its own private
     * root under it. Empty when no usable location could be resolved.
     *
     * On Windows a sandboxed process cannot write the user's %TEMP%:
     *   - in an AppContainer, the container's own folder (its AC\\Temp when present)
     *     from GetAppContainerFolderPath, which does not depend on the inherited
     *     TEMP variable a launcher may have left pointing at the user's temp;
     *   - below medium integrity, FOLDERID_LocalAppDataLow.
     * Sandbox lookup failures return an empty path. Everyone else uses the
     * temp directory.
     */
    inline std::filesystem::path ResolveCrashArtifactBaseDirectory()
    {
#ifdef _WIN32
        const Private::SandboxFacts sandbox = Private::ReadSandboxFacts();
        if (!sandbox.valid)
        {
            return {};
        }
        if (sandbox.appContainer)
        {
            const std::filesystem::path folder = Private::AppContainerFolder(sandbox.appContainerSid);
            if (Private::IsOrdinaryDirectory(folder))
            {
                const std::filesystem::path containerTemp = folder / L"Temp";
                return Private::IsOrdinaryDirectory(containerTemp) ? containerTemp : folder;
            }
            return {};
        }
        else if (sandbox.belowMediumIntegrity)
        {
            const std::filesystem::path folder = Private::LocalAppDataLowFolder();
            if (Private::IsOrdinaryDirectory(folder))
            {
                return folder;
            }
            return {};
        }
#endif
        std::error_code error;
        const std::filesystem::path tempDirectory = std::filesystem::temp_directory_path(error);
        return error ? std::filesystem::path{} : tempDirectory;
    }

    /** @brief Create a randomized, exclusive, owner-only crash-artifact directory. */
    inline std::filesystem::path CreatePrivateCrashArtifactDirectory(const std::filesystem::path& baseDirectory,
                                                                     unsigned long processId)
    {
        if (baseDirectory.empty())
            return {};

        std::error_code error;
        const std::filesystem::path absoluteBase = std::filesystem::absolute(baseDirectory, error).lexically_normal();
        if (error)
            return {};

        for (int attempt = 0; attempt < 64; ++attempt)
        {
            const std::string suffix = Private::RandomDirectorySuffix();
            if (suffix.empty())
                return {};
            const std::filesystem::path candidate =
                absoluteBase / ("spark_crash_" + std::to_string(processId) + "_" + suffix);
            if (TryCreatePrivateCrashArtifactDirectory(candidate))
                return candidate;
        }
        return {};
    }
} // namespace Spark::CrashHandlerDetail
