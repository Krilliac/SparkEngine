// CrashArtifactSandboxProbe.cpp - the crash handler's private artifact root must
// be creatable, pinnable and writable from a sandboxed Windows process, with the
// same owner-only protected DACL it has in an ordinary process.
//
// Run without arguments, this executable is the launcher. It checks the
// ordinary (medium-integrity) process in place, then relaunches itself twice:
//   * inside a real AppContainer (CreateAppContainerProfile + SECURITY_CAPABILITIES,
//     no capabilities), from a copy placed in the container's own folder;
//   * at low integrity (a duplicate of its own token with a Low mandatory label).
// Each child runs the checks with "--inside <kind>" and first proves it really is
// sandboxed, so a launch that silently ran unsandboxed cannot pass.
//
// Header-only on purpose: it exercises the exact functions InstallCrashHandler()
// uses (ResolveCrashArtifactBaseDirectory, PruneStaleCrashArtifactDirectories,
// CreatePrivateCrashArtifactDirectory) without linking the engine, so the
// sandboxed copy needs nothing beyond system DLLs.

#include "Utils/CrashArtifactDirectory.h"
#include "Utils/CrashArtifactRetention.h"

#include <aclapi.h>
#include <sddl.h>
#include <userenv.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    namespace Detail = Spark::CrashHandlerDetail;

    // Exit codes: one per failed step, so a failing CTest log names the step.
    enum ProbeExit : int
    {
        kPass = 0,
        kNotSandboxed = 20,
        kNoBaseDirectory = 21,
        kCreateFailed = 22,
        kPinFailed = 23,
        kWriteFailed = 24,
        kReadBackFailed = 25,
        kDaclNotProtected = 26,
        kDaclForeignAce = 27,
        kDaclMissingOwnerAce = 28,
        kCleanupFailed = 29,
        kLauncherSetupFailed = 40,
        kChildLaunchFailed = 41,
        kChildTimedOut = 42,
    };

    int Fail(int code, const char* step)
    {
        std::fprintf(stderr, "[CrashArtifactSandboxProbe] FAIL %s (exit %d, GetLastError=%lu)\n", step, code,
                     GetLastError());
        return code;
    }

    struct TokenFacts
    {
        bool tokenQueryValid = false;
        bool appContainerQueryValid = false;
        bool integrityQueryValid = false;
        bool appContainer = false;
        DWORD integrityRid = 0;
        std::wstring appContainerSid;
    };

    TokenFacts ReadTokenFacts()
    {
        TokenFacts facts;
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        {
            return facts;
        }
        facts.tokenQueryValid = true;
        DWORD length = 0;
        DWORD isAppContainer = 0;
        if (GetTokenInformation(token, TokenIsAppContainer, &isAppContainer, sizeof(isAppContainer), &length))
        {
            facts.appContainerQueryValid = true;
            facts.appContainer = isAppContainer != 0;
        }
        if (facts.appContainer)
        {
            alignas(TOKEN_APPCONTAINER_INFORMATION) unsigned char
                buffer[sizeof(TOKEN_APPCONTAINER_INFORMATION) + SECURITY_MAX_SID_SIZE]{};
            LPWSTR text = nullptr;
            if (GetTokenInformation(token, TokenAppContainerSid, buffer, sizeof(buffer), &length))
            {
                const PSID appContainerSid =
                    reinterpret_cast<TOKEN_APPCONTAINER_INFORMATION*>(buffer)->TokenAppContainer;
                if (appContainerSid != nullptr && IsValidSid(appContainerSid) &&
                    ConvertSidToStringSidW(appContainerSid, &text))
                {
                    facts.appContainerSid = text;
                    LocalFree(text);
                }
            }
        }
        alignas(TOKEN_MANDATORY_LABEL) unsigned char label[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE]{};
        if (GetTokenInformation(token, TokenIntegrityLevel, label, sizeof(label), &length))
        {
            PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(label)->Label.Sid;
            if (sid != nullptr && IsValidSid(sid))
            {
                const PUCHAR subAuthorityCount = GetSidSubAuthorityCount(sid);
                if (subAuthorityCount != nullptr && *subAuthorityCount != 0)
                {
                    const DWORD index = static_cast<DWORD>(*subAuthorityCount - 1);
                    if (PDWORD subAuthority = GetSidSubAuthority(sid, index); subAuthority != nullptr)
                    {
                        facts.integrityRid = *subAuthority;
                        facts.integrityQueryValid = true;
                    }
                }
            }
        }
        CloseHandle(token);
        return facts;
    }

    bool SidStringEquals(PSID sid, const wchar_t* expected)
    {
        LPWSTR text = nullptr;
        if (sid == nullptr || !IsValidSid(sid) || !ConvertSidToStringSidW(sid, &text))
        {
            return false;
        }
        const bool equal = _wcsicmp(text, expected) == 0;
        LocalFree(text);
        return equal;
    }

    bool IsPathWithin(const fs::path& parent, const fs::path& candidate)
    {
        const std::wstring parentText = parent.lexically_normal().wstring();
        const std::wstring candidateText = candidate.lexically_normal().wstring();
        if (_wcsicmp(parentText.c_str(), candidateText.c_str()) == 0)
        {
            return true;
        }
        if (candidateText.size() <= parentText.size() ||
            _wcsnicmp(parentText.c_str(), candidateText.c_str(), parentText.size()) != 0)
        {
            return false;
        }
        const wchar_t separator = candidateText[parentText.size()];
        return separator == L'\\' || separator == L'/';
    }

    /// The DACL must be protected, hold an OWNER RIGHTS full-access ACE, and grant
    /// nobody but OWNER RIGHTS, SYSTEM and (in an AppContainer) that container.
    int CheckDacl(const fs::path& directory, const TokenFacts& facts)
    {
        PACL dacl = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (GetNamedSecurityInfoW(directory.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl,
                                  nullptr, &descriptor) != ERROR_SUCCESS ||
            dacl == nullptr)
        {
            return Fail(kDaclNotProtected, "read DACL");
        }
        SECURITY_DESCRIPTOR_CONTROL control = 0;
        DWORD revision = 0;
        int result = kPass;
        if (!GetSecurityDescriptorControl(descriptor, &control, &revision) || (control & SE_DACL_PROTECTED) == 0)
        {
            result = Fail(kDaclNotProtected, "DACL is not protected from inheritance");
        }

        bool ownerAce = false;
        bool systemAce = false;
        bool appContainerAce = !facts.appContainer;
        for (DWORD index = 0; result == kPass && index < dacl->AceCount; ++index)
        {
            void* rawAce = nullptr;
            if (!GetAce(dacl, index, &rawAce))
            {
                result = Fail(kDaclForeignAce, "read ACE");
                break;
            }
            const auto* header = static_cast<ACE_HEADER*>(rawAce);
            if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
            {
                result = Fail(kDaclForeignAce, "non access-allowed ACE");
                break;
            }
            auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(rawAce);
            PSID sid = &ace->SidStart;
            if (SidStringEquals(sid, L"S-1-3-4"))
            {
                if (ace->Mask != FILE_ALL_ACCESS)
                {
                    result = Fail(kDaclMissingOwnerAce, "OWNER RIGHTS ACE is not full access");
                }
                else
                {
                    ownerAce = true;
                }
                continue;
            }
            if (SidStringEquals(sid, L"S-1-5-18"))
            {
                if (ace->Mask != FILE_ALL_ACCESS)
                {
                    result = Fail(kDaclForeignAce, "SYSTEM ACE is not full access");
                }
                else
                {
                    systemAce = true;
                }
                continue;
            }
            if (facts.appContainer && !facts.appContainerSid.empty() &&
                SidStringEquals(sid, facts.appContainerSid.c_str()))
            {
                if (ace->Mask != FILE_ALL_ACCESS)
                {
                    result = Fail(kDaclForeignAce, "own AppContainer SID ACE is not full access");
                }
                else
                {
                    appContainerAce = true;
                }
                continue;
            }
            LPWSTR text = nullptr;
            ConvertSidToStringSidW(sid, &text);
            std::fwprintf(stderr, L"[CrashArtifactSandboxProbe] foreign ACE for %ls\n", text ? text : L"?");
            if (text)
            {
                LocalFree(text);
            }
            result = Fail(kDaclForeignAce, "DACL grants a principal other than owner/SYSTEM/own container");
        }
        if (result == kPass && !ownerAce)
        {
            result = Fail(kDaclMissingOwnerAce, "no OWNER RIGHTS full-access ACE");
        }
        if (result == kPass && !systemAce)
        {
            result = Fail(kDaclForeignAce, "no SYSTEM full-access ACE");
        }
        if (result == kPass && !appContainerAce)
        {
            result = Fail(kDaclForeignAce, "no own AppContainer SID full-access ACE");
        }
        LocalFree(descriptor);
        return result;
    }

    /// What InstallCrashHandler() does, then the operations the crash path needs.
    int RunChecks(const char* kind, bool requireSandbox)
    {
        const TokenFacts facts = ReadTokenFacts();
        std::fprintf(stdout, "[CrashArtifactSandboxProbe] %s: appContainer=%d integrity=0x%lx\n", kind,
                     facts.appContainer ? 1 : 0, facts.integrityRid);
        if (requireSandbox && (!facts.tokenQueryValid || !facts.appContainerQueryValid || !facts.integrityQueryValid))
        {
            return Fail(kNotSandboxed, "child token identity queries did not complete");
        }
        if (requireSandbox && !facts.appContainer && facts.integrityRid >= SECURITY_MANDATORY_MEDIUM_RID)
        {
            return Fail(kNotSandboxed, "child is not sandboxed");
        }
        if (std::strcmp(kind, "appcontainer") == 0 && !facts.appContainer)
        {
            return Fail(kNotSandboxed, "child is not in an AppContainer");
        }
        if (std::strcmp(kind, "lowil") == 0 && facts.integrityRid >= SECURITY_MANDATORY_MEDIUM_RID)
        {
            return Fail(kNotSandboxed, "child is not below medium integrity");
        }

        if (facts.appContainer)
        {
            PWSTR folderText = nullptr;
            if (facts.appContainerSid.empty() ||
                FAILED(GetAppContainerFolderPath(facts.appContainerSid.c_str(), &folderText)) || folderText == nullptr)
            {
                CoTaskMemFree(folderText);
                return Fail(kNoBaseDirectory, "GetAppContainerFolderPath for temp adversarial check");
            }
            const fs::path containerFolder(folderText);
            CoTaskMemFree(folderText);

            std::wstring windowsDirectory(32768, L'\0');
            const DWORD windowsLength =
                GetWindowsDirectoryW(windowsDirectory.data(), static_cast<UINT>(windowsDirectory.size()));
            if (windowsLength == 0 || windowsLength >= windowsDirectory.size())
            {
                return Fail(kNoBaseDirectory, "GetWindowsDirectory for temp adversarial check");
            }
            windowsDirectory.resize(windowsLength);
            const fs::path missingTemp = fs::path(windowsDirectory) / (L"SparkCrashArtifactSandboxProbe.Missing." +
                                                                       std::to_wstring(GetCurrentProcessId()) + L"." +
                                                                       std::to_wstring(GetTickCount64()));
            if (GetFileAttributesW(missingTemp.c_str()) != INVALID_FILE_ATTRIBUTES ||
                !SetEnvironmentVariableW(L"TEMP", missingTemp.c_str()) ||
                !SetEnvironmentVariableW(L"TMP", missingTemp.c_str()))
            {
                return Fail(kNoBaseDirectory, "set deliberately unusable AppContainer TEMP/TMP");
            }

            std::error_code tempError;
            const fs::path processTemp = fs::temp_directory_path(tempError);
            if (!tempError && IsPathWithin(containerFolder, processTemp))
            {
                return Fail(kNoBaseDirectory, "temp_directory_path was silently rerouted into AppContainer");
            }
        }

        const fs::path base = Detail::ResolveCrashArtifactBaseDirectory();
        std::fwprintf(stdout, L"[CrashArtifactSandboxProbe] base=%ls\n", base.c_str());
        if (base.empty())
        {
            return Fail(kNoBaseDirectory, "ResolveCrashArtifactBaseDirectory");
        }
        if (facts.appContainer)
        {
            PWSTR folderText = nullptr;
            if (facts.appContainerSid.empty())
            {
                return Fail(kNoBaseDirectory, "GetAppContainerFolderPath for selected base");
            }
            if (FAILED(GetAppContainerFolderPath(facts.appContainerSid.c_str(), &folderText)) || folderText == nullptr)
            {
                CoTaskMemFree(folderText);
                return Fail(kNoBaseDirectory, "GetAppContainerFolderPath for selected base");
            }
            const fs::path containerFolder(folderText);
            CoTaskMemFree(folderText);
            const fs::path containerTemp = containerFolder / L"Temp";
            const auto normalizedBase = base.lexically_normal().wstring();
            const auto normalizedContainerFolder = containerFolder.lexically_normal().wstring();
            const auto normalizedContainerTemp = containerTemp.lexically_normal().wstring();
            const bool isContainerFolder = _wcsicmp(normalizedBase.c_str(), normalizedContainerFolder.c_str()) == 0;
            const bool isContainerTemp = _wcsicmp(normalizedBase.c_str(), normalizedContainerTemp.c_str()) == 0;
            if ((!isContainerFolder && !isContainerTemp) || !Detail::Private::IsOrdinaryDirectory(containerFolder) ||
                (isContainerTemp && !Detail::Private::IsOrdinaryDirectory(containerTemp)))
            {
                return Fail(kNoBaseDirectory, "AppContainer base is outside its private folder");
            }
        }
        (void)Detail::PruneStaleCrashArtifactDirectories(base);

        const fs::path root = Detail::CreatePrivateCrashArtifactDirectory(base, GetCurrentProcessId());
        if (root.empty())
        {
            return Fail(kCreateFailed, "CreatePrivateCrashArtifactDirectory");
        }
        std::fwprintf(stdout, L"[CrashArtifactSandboxProbe] root=%ls\n", root.c_str());

        // PinArtifactRoot's open, with its exact access and flags.
        HANDLE pinned =
            CreateFileW(root.c_str(), FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        int result = pinned == INVALID_HANDLE_VALUE ? Fail(kPinFailed, "pin artifact root") : kPass;

        // The crash path creates each artifact exclusively inside the root.
        const fs::path artifact = root / L"GameEngineCrash_probe.log";
        if (result == kPass)
        {
            HANDLE file = CreateFileW(artifact.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                      CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            constexpr char payload[] = "crash artifact probe";
            DWORD written = 0;
            if (file == INVALID_HANDLE_VALUE ||
                !WriteFile(file, payload, static_cast<DWORD>(sizeof(payload) - 1), &written, nullptr) ||
                written != sizeof(payload) - 1)
            {
                result = Fail(kWriteFailed, "create and write an artifact in the root");
            }
            else
            {
                char readBack[sizeof(payload)]{};
                DWORD read = 0;
                SetFilePointer(file, 0, nullptr, FILE_BEGIN);
                if (!ReadFile(file, readBack, static_cast<DWORD>(sizeof(readBack) - 1), &read, nullptr) ||
                    read != written || std::memcmp(readBack, payload, read) != 0)
                {
                    result = Fail(kReadBackFailed, "read the artifact back");
                }
            }
            if (file != INVALID_HANDLE_VALUE)
            {
                CloseHandle(file);
            }
        }
        if (result == kPass)
        {
            result = CheckDacl(root, facts);
        }

        if (pinned != INVALID_HANDLE_VALUE)
        {
            CloseHandle(pinned);
        }
        std::error_code error;
        fs::remove(artifact, error);
        if (!RemoveDirectoryW(root.c_str()) && result == kPass)
        {
            result = Fail(kCleanupFailed, "remove the artifact root");
        }
        if (result == kPass)
        {
            std::fprintf(stdout, "[CrashArtifactSandboxProbe] %s: PASS\n", kind);
        }
        return result;
    }

    int WaitForChild(PROCESS_INFORMATION& process, const char* kind)
    {
        CloseHandle(process.hThread);
        const DWORD wait = WaitForSingleObject(process.hProcess, 60000);
        DWORD exitCode = kChildTimedOut;
        if (wait != WAIT_OBJECT_0)
        {
            TerminateProcess(process.hProcess, kChildTimedOut);
            WaitForSingleObject(process.hProcess, 5000);
        }
        else if (!GetExitCodeProcess(process.hProcess, &exitCode))
        {
            exitCode = kChildLaunchFailed;
        }
        CloseHandle(process.hProcess);
        std::fprintf(stdout, "[CrashArtifactSandboxProbe] %s child exit=%lu\n", kind, exitCode);
        return static_cast<int>(exitCode);
    }

    int RunInAppContainer(const fs::path& self)
    {
        const std::wstring containerName = L"Spark.Tests.CrashArtifactSandboxProbe." +
                                           std::to_wstring(GetCurrentProcessId()) + L"." +
                                           std::to_wstring(GetTickCount64());
        PSID containerSid = nullptr;
        if (FAILED(CreateAppContainerProfile(containerName.c_str(), L"Spark crash-artifact sandbox probe",
                                             L"SparkEngine test: crash artifacts under an AppContainer", nullptr, 0,
                                             &containerSid)))
        {
            return Fail(kLauncherSetupFailed, "CreateAppContainerProfile");
        }

        int result = kPass;
        LPWSTR sidText = nullptr;
        PWSTR folder = nullptr;
        fs::path copy;
        if (!ConvertSidToStringSidW(containerSid, &sidText) || FAILED(GetAppContainerFolderPath(sidText, &folder)) ||
            folder == nullptr)
        {
            result = Fail(kLauncherSetupFailed, "GetAppContainerFolderPath");
        }
        else
        {
            // The container cannot read the build tree; its own folder it can.
            copy = fs::path(folder) / self.filename();
            if (!CopyFileW(self.c_str(), copy.c_str(), FALSE))
            {
                result = Fail(kLauncherSetupFailed, "copy probe into the container folder");
            }
        }

        if (result == kPass)
        {
            SECURITY_CAPABILITIES capabilities{};
            capabilities.AppContainerSid = containerSid;
            SIZE_T attributeBytes = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
            std::vector<unsigned char> attributeStorage(attributeBytes);
            auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = sizeof(startup);
            startup.lpAttributeList = attributes;
            PROCESS_INFORMATION process{};
            std::wstring commandLine = L"\"" + copy.wstring() + L"\" --inside appcontainer";
            bool attributeListInitialized = false;
            if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes))
            {
                result = Fail(kLauncherSetupFailed, "initialize SECURITY_CAPABILITIES attribute");
            }
            else
            {
                attributeListInitialized = true;
                if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
                                               &capabilities, sizeof(capabilities), nullptr, nullptr))
                {
                    result = Fail(kLauncherSetupFailed, "SECURITY_CAPABILITIES attribute");
                }
                else if (!CreateProcessW(copy.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                                         EXTENDED_STARTUPINFO_PRESENT, nullptr, folder, &startup.StartupInfo, &process))
                {
                    result = Fail(kChildLaunchFailed, "launch AppContainer child");
                }
                else
                {
                    result = WaitForChild(process, "appcontainer");
                }
            }
            if (attributeListInitialized)
            {
                DeleteProcThreadAttributeList(attributes);
            }
        }

        if (!copy.empty())
        {
            DeleteFileW(copy.c_str());
        }
        if (folder)
        {
            CoTaskMemFree(folder);
        }
        if (sidText)
        {
            LocalFree(sidText);
        }
        FreeSid(containerSid);
        DeleteAppContainerProfile(containerName.c_str());
        return result;
    }

    int RunAtLowIntegrity(const fs::path& self)
    {
        HANDLE token = nullptr;
        HANDLE lowToken = nullptr;
        PSID lowSid = nullptr;
        int result = kPass;
        if (!OpenProcessToken(GetCurrentProcess(),
                              TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT | TOKEN_ASSIGN_PRIMARY, &token) ||
            !DuplicateTokenEx(token, 0, nullptr, SecurityImpersonation, TokenPrimary, &lowToken) ||
            !ConvertStringSidToSidW(L"S-1-16-4096", &lowSid))
        {
            result = Fail(kLauncherSetupFailed, "duplicate token for low integrity");
        }
        else
        {
            TOKEN_MANDATORY_LABEL label{};
            label.Label.Attributes = SE_GROUP_INTEGRITY;
            label.Label.Sid = lowSid;
            if (!SetTokenInformation(lowToken, TokenIntegrityLevel, &label, sizeof(label) + GetLengthSid(lowSid)))
            {
                result = Fail(kLauncherSetupFailed, "lower token integrity");
            }
        }

        if (result == kPass)
        {
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            std::wstring commandLine = L"\"" + self.wstring() + L"\" --inside lowil";
            if (CreateProcessAsUserW(lowToken, self.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                     nullptr, &startup, &process))
            {
                result = WaitForChild(process, "lowil");
            }
            else
            {
                commandLine = L"\"" + self.wstring() + L"\" --inside lowil";
                if (CreateProcessWithTokenW(lowToken, LOGON_WITH_PROFILE, self.c_str(), commandLine.data(), 0, nullptr,
                                            nullptr, &startup, &process))
                {
                    result = WaitForChild(process, "lowil");
                }
                else
                {
                    result = Fail(kChildLaunchFailed, "launch low-integrity child with primary-token fallbacks");
                }
            }
        }

        if (lowSid)
        {
            LocalFree(lowSid);
        }
        if (lowToken)
        {
            CloseHandle(lowToken);
        }
        if (token)
        {
            CloseHandle(token);
        }
        return result;
    }

    fs::path SelfPath()
    {
        std::wstring buffer(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0 || length >= buffer.size())
        {
            return {};
        }
        buffer.resize(length);
        return fs::path(buffer);
    }
} // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && std::strcmp(argv[1], "--inside") == 0)
    {
        return RunChecks(argv[2], true);
    }
    if (argc != 1)
    {
        std::fprintf(stderr, "usage: %s [--inside appcontainer|lowil]\n", argv[0]);
        return 2;
    }

    const fs::path self = SelfPath();
    if (self.empty())
    {
        return Fail(kLauncherSetupFailed, "GetModuleFileNameW");
    }

    int failures = 0;
    if (RunChecks("medium", false) != kPass)
    {
        ++failures;
    }
    if (RunInAppContainer(self) != kPass)
    {
        ++failures;
    }
    if (RunAtLowIntegrity(self) != kPass)
    {
        ++failures;
    }
    std::fprintf(stdout, "[CrashArtifactSandboxProbe] %d of 3 scenarios failed\n", failures);
    return failures == 0 ? 0 : 1;
}
