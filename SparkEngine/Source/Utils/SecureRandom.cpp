/**
 * @file SecureRandom.cpp
 * @brief Operating-system-backed cryptographic random byte generation.
 */
#include "SecureRandom.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#elif defined(__linux__)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/random.h>
#include <unistd.h>
#endif

namespace
{
    void SetError(std::string* error, std::string message) noexcept
    {
        if (!error)
            return;
        try
        {
            *error = std::move(message);
        }
        catch (...)
        {
        }
    }

#if defined(_WIN32)
    constexpr DWORD PrivateFileRights = FILE_GENERIC_READ | FILE_GENERIC_WRITE | DELETE | WRITE_DAC;

    struct LocalAllocation
    {
        HLOCAL value = nullptr;

        ~LocalAllocation()
        {
            if (value)
                LocalFree(value);
        }
    };

    class CreatedFileHandle
    {
      public:
        explicit CreatedFileHandle(HANDLE handle) noexcept : m_handle(handle) {}
        ~CreatedFileHandle() { (void)Discard(); }

        HANDLE Get() const noexcept { return m_handle; }

        DWORD Discard() noexcept
        {
            if (m_handle == INVALID_HANDLE_VALUE)
                return ERROR_SUCCESS;
            FILE_DISPOSITION_INFO disposition{TRUE};
            DWORD status = ERROR_SUCCESS;
            if (!SetFileInformationByHandle(m_handle, FileDispositionInfo, &disposition, sizeof(disposition)))
            {
                status = GetLastError();
                LARGE_INTEGER beginning{};
                if (SetFilePointerEx(m_handle, beginning, nullptr, FILE_BEGIN) && SetEndOfFile(m_handle))
                    (void)FlushFileBuffers(m_handle);
            }
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
            return status;
        }

        void PreserveAndClose() noexcept
        {
            if (m_handle != INVALID_HANDLE_VALUE)
                CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }

      private:
        HANDLE m_handle = INVALID_HANDLE_VALUE;
    };

    std::string WindowsError(const char* operation, DWORD code)
    {
        return std::string(operation) + " failed with Windows error " + std::to_string(code);
    }

    bool BoundedAllowedAceTrustee(const ACCESS_ALLOWED_ACE* ace, PSID& trustee)
    {
        constexpr size_t sidOffset = offsetof(ACCESS_ALLOWED_ACE, SidStart);
        const size_t aceBytes = ace->Header.AceSize;
        const size_t minimumSidBytes = GetSidLengthRequired(0);
        if (aceBytes < sidOffset + minimumSidBytes)
            return false;
        const auto* sid = reinterpret_cast<const SID*>(&ace->SidStart);
        const size_t sidBytes = GetSidLengthRequired(sid->SubAuthorityCount);
        if (sidBytes > aceBytes - sidOffset)
            return false;
        trustee = const_cast<SID*>(sid);
        return IsValidSid(trustee) != FALSE && GetLengthSid(trustee) == sidBytes;
    }

    bool HasProtectedOwnerOnlyAcl(HANDLE file, PSID expectedOwner, std::string& failure)
    {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        PSID owner = nullptr;
        PACL dacl = nullptr;
        const DWORD status = GetSecurityInfo(file, SE_FILE_OBJECT,
                                             OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                             &owner, nullptr, &dacl, nullptr, &descriptor);
        LocalAllocation descriptorAllocation{descriptor};
        if (status != ERROR_SUCCESS || !descriptor || !owner || !IsValidSid(owner) || !dacl || !IsValidAcl(dacl))
        {
            failure = status == ERROR_SUCCESS
                ? "private file ACL readback is incomplete"
                : WindowsError("GetSecurityInfo", status);
            return false;
        }

        bool valid = EqualSid(owner, expectedOwner) != FALSE;
        if (!valid)
            failure = "private file owner changed during creation";

        SECURITY_DESCRIPTOR_CONTROL control{};
        DWORD revision = 0;
        if (valid && (!GetSecurityDescriptorControl(descriptor, &control, &revision) ||
                      (control & (SE_DACL_PRESENT | SE_DACL_PROTECTED)) !=
                          (SE_DACL_PRESENT | SE_DACL_PROTECTED)))
        {
            valid = false;
            failure = "private file DACL is not protected from inheritance";
        }

        ACL_SIZE_INFORMATION information{};
        if (valid && !GetAclInformation(dacl, &information, sizeof(information), AclSizeInformation))
        {
            valid = false;
            failure = WindowsError("GetAclInformation", GetLastError());
        }

        if (valid && information.AceCount != 1)
        {
            valid = false;
            failure = "private file DACL is not the canonical single-owner ACL";
        }

        for (DWORD index = 0; valid && index < information.AceCount; ++index)
        {
            void* rawAce = nullptr;
            if (!GetAce(dacl, index, &rawAce))
            {
                valid = false;
                failure = WindowsError("GetAce", GetLastError());
                break;
            }
            const auto* header = static_cast<const ACE_HEADER*>(rawAce);
            constexpr BYTE inheritanceFlags = INHERITED_ACE | INHERIT_ONLY_ACE | OBJECT_INHERIT_ACE |
                                              CONTAINER_INHERIT_ACE | NO_PROPAGATE_INHERIT_ACE;
            if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || header->AceSize < sizeof(ACCESS_ALLOWED_ACE) ||
                (header->AceFlags & inheritanceFlags) != 0)
            {
                valid = false;
                failure = "private file DACL contains a non-canonical ACE";
                break;
            }
            const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(rawAce);
            PSID trustee = nullptr;
            if (!BoundedAllowedAceTrustee(ace, trustee) || !EqualSid(owner, trustee) ||
                (ace->Mask & PrivateFileRights) != PrivateFileRights)
            {
                valid = false;
                failure = "private file DACL does not grant the canonical owner rights";
                break;
            }
        }

        return valid;
    }
#endif
} // namespace

namespace Spark::SecureRandom
{
    bool Fill(void* buffer, size_t size) noexcept
    {
        if (size == 0)
            return true;
        if (!buffer)
            return false;

        auto* output = static_cast<uint8_t*>(buffer);
#if defined(_WIN32)
        while (size > 0)
        {
            const ULONG chunk =
                static_cast<ULONG>(std::min(size, static_cast<size_t>((std::numeric_limits<ULONG>::max)())));
            if (BCryptGenRandom(nullptr, output, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
                return false;
            output += chunk;
            size -= chunk;
        }
        return true;
#elif defined(__APPLE__)
        arc4random_buf(output, size);
        return true;
#elif defined(__linux__)
        while (size > 0)
        {
            const ssize_t count = getrandom(output, size, 0);
            if (count < 0)
            {
                if (errno == EINTR)
                    continue;
                return false;
            }
            if (count == 0)
                return false;
            output += static_cast<size_t>(count);
            size -= static_cast<size_t>(count);
        }
        return true;
#else
        (void)output;
        return false;
#endif
    }

    std::string HexToken(size_t byteCount)
    {
        if (byteCount == 0 || byteCount > 1024)
            return {};
        std::vector<uint8_t> bytes(byteCount);
        if (!Fill(bytes.data(), bytes.size()))
            return {};

        static constexpr char digits[] = "0123456789abcdef";
        std::string token(byteCount * 2, '\0');
        for (size_t i = 0; i < byteCount; ++i)
        {
            token[i * 2] = digits[bytes[i] >> 4];
            token[i * 2 + 1] = digits[bytes[i] & 0x0f];
        }
        return token;
    }

    bool CreatePrivateFile(const std::filesystem::path& path, std::string_view contents, std::string* error) noexcept
    {
        try
        {
            if (path.empty() || path.filename().empty())
            {
                SetError(error, "private file path is empty");
                return false;
            }

            std::error_code filesystemError;
            if (!path.parent_path().empty())
            {
                std::filesystem::create_directories(path.parent_path(), filesystemError);
                if (filesystemError)
                {
                    SetError(error, "failed to create private file directory: " + filesystemError.message());
                    return false;
                }
            }

#if defined(_WIN32)
            HANDLE token = nullptr;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            {
                SetError(error, WindowsError("OpenProcessToken", GetLastError()));
                return false;
            }

            DWORD tokenBytes = 0;
            GetTokenInformation(token, TokenUser, nullptr, 0, &tokenBytes);
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || tokenBytes == 0)
            {
                const DWORD code = GetLastError();
                CloseHandle(token);
                SetError(error, WindowsError("GetTokenInformation(size)", code));
                return false;
            }

            std::vector<unsigned char> tokenBuffer(tokenBytes);
            if (!GetTokenInformation(token, TokenUser, tokenBuffer.data(), tokenBytes, &tokenBytes))
            {
                const DWORD code = GetLastError();
                CloseHandle(token);
                SetError(error, WindowsError("GetTokenInformation", code));
                return false;
            }
            CloseHandle(token);

            const auto* tokenUser = reinterpret_cast<const TOKEN_USER*>(tokenBuffer.data());
            if (!tokenUser->User.Sid || !IsValidSid(tokenUser->User.Sid))
            {
                SetError(error, "current process user SID is invalid");
                return false;
            }
            EXPLICIT_ACCESSW access{};
            access.grfAccessPermissions = PrivateFileRights;
            access.grfAccessMode = SET_ACCESS;
            access.grfInheritance = NO_INHERITANCE;
            access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
            access.Trustee.TrusteeType = TRUSTEE_IS_USER;
            access.Trustee.ptstrName = static_cast<LPWSTR>(tokenUser->User.Sid);

            PACL acl = nullptr;
            const DWORD aclResult = SetEntriesInAclW(1, &access, nullptr, &acl);
            if (aclResult != ERROR_SUCCESS)
            {
                SetError(error, WindowsError("SetEntriesInAclW", aclResult));
                return false;
            }
            LocalAllocation aclAllocation{acl};

            SECURITY_DESCRIPTOR descriptor{};
            if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION))
            {
                const DWORD code = GetLastError();
                SetError(error, WindowsError("InitializeSecurityDescriptor", code));
                return false;
            }
            if (!SetSecurityDescriptorOwner(&descriptor, tokenUser->User.Sid, FALSE))
            {
                const DWORD code = GetLastError();
                SetError(error, WindowsError("SetSecurityDescriptorOwner", code));
                return false;
            }
            if (!SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE))
            {
                const DWORD code = GetLastError();
                SetError(error, WindowsError("SetSecurityDescriptorDacl", code));
                return false;
            }
            if (!SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
            {
                const DWORD code = GetLastError();
                SetError(error, WindowsError("SetSecurityDescriptorControl", code));
                return false;
            }

            SECURITY_ATTRIBUTES attributes{};
            attributes.nLength = sizeof(attributes);
            attributes.lpSecurityDescriptor = &descriptor;
            const HANDLE file =
                CreateFileW(path.c_str(), GENERIC_WRITE | READ_CONTROL | WRITE_DAC | DELETE, 0, &attributes, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                SetError(error, WindowsError("CreateFileW", GetLastError()));
                return false;
            }
            CreatedFileHandle createdFile(file);

            const auto discardCreatedFile = [&](std::string failure)
            {
                const DWORD dispositionError = createdFile.Discard();
                if (dispositionError != ERROR_SUCCESS)
                    failure += "; secure cleanup failed: " + WindowsError(
                        "SetFileInformationByHandle(FileDispositionInfo)", dispositionError);
                SetError(error, std::move(failure));
                return false;
            };

            // Defensively re-apply the owner-only DACL to the live exclusive
            // handle, then read it back before any secret bytes are written.
            const DWORD protectStatus = SetSecurityInfo(
                createdFile.Get(), SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                nullptr, nullptr, acl, nullptr);
            std::string aclFailure;
            const bool protectedOwnerOnly =
                protectStatus == ERROR_SUCCESS &&
                HasProtectedOwnerOnlyAcl(createdFile.Get(), tokenUser->User.Sid, aclFailure);
            if (!protectedOwnerOnly)
            {
                if (protectStatus != ERROR_SUCCESS)
                    aclFailure = WindowsError("SetSecurityInfo", protectStatus);
                return discardCreatedFile(std::move(aclFailure));
            }

            bool success = true;
            std::string writeFailure;
            size_t written = 0;
            while (written < contents.size())
            {
                const DWORD chunk = static_cast<DWORD>(
                    std::min(contents.size() - written, static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
                DWORD chunkWritten = 0;
                if (!WriteFile(createdFile.Get(), contents.data() + written, chunk, &chunkWritten, nullptr) ||
                    chunkWritten == 0)
                {
                    writeFailure = WindowsError("WriteFile", GetLastError());
                    success = false;
                    break;
                }
                written += chunkWritten;
            }
            if (success && !FlushFileBuffers(createdFile.Get()))
            {
                writeFailure = WindowsError("FlushFileBuffers", GetLastError());
                success = false;
            }
            if (!success)
                return discardCreatedFile(std::move(writeFailure));
            createdFile.PreserveAndClose();
            return true;
#elif defined(__APPLE__) || defined(__linux__)
            const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (descriptor < 0)
            {
                SetError(error, std::string("open failed: ") + std::strerror(errno));
                return false;
            }

            bool success = true;
            size_t written = 0;
            while (written < contents.size())
            {
                const ssize_t count = ::write(descriptor, contents.data() + written, contents.size() - written);
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0)
                {
                    SetError(error, std::string("write failed: ") + std::strerror(errno));
                    success = false;
                    break;
                }
                written += static_cast<size_t>(count);
            }
            if (success && ::fsync(descriptor) != 0)
            {
                SetError(error, std::string("fsync failed: ") + std::strerror(errno));
                success = false;
            }
            ::close(descriptor);
            if (!success)
                ::unlink(path.c_str());
            return success;
#else
            (void)contents;
            SetError(error, "owner-only private file creation is unsupported on this platform");
            return false;
#endif
        }
        catch (const std::exception& exception)
        {
            SetError(error, std::string("private file creation failed: ") + exception.what());
            return false;
        }
        catch (...)
        {
            SetError(error, "private file creation failed");
            return false;
        }
    }
} // namespace Spark::SecureRandom
