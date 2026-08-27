/**
 * @file SecureRandom.cpp
 * @brief Operating-system-backed cryptographic random byte generation.
 */
#include "SecureRandom.h"

#include <algorithm>
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
    std::string WindowsError(const char* operation, DWORD code)
    {
        return std::string(operation) + " failed with Windows error " + std::to_string(code);
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
            EXPLICIT_ACCESSW access{};
            access.grfAccessPermissions = GENERIC_READ | GENERIC_WRITE | DELETE | READ_CONTROL | WRITE_DAC;
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

            SECURITY_DESCRIPTOR descriptor{};
            if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
                !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE))
            {
                const DWORD code = GetLastError();
                LocalFree(acl);
                SetError(error, WindowsError("SetSecurityDescriptorDacl", code));
                return false;
            }

            SECURITY_ATTRIBUTES attributes{};
            attributes.nLength = sizeof(attributes);
            attributes.lpSecurityDescriptor = &descriptor;
            const HANDLE file =
                CreateFileW(path.c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            LocalFree(acl);
            if (file == INVALID_HANDLE_VALUE)
            {
                SetError(error, WindowsError("CreateFileW", GetLastError()));
                return false;
            }

            bool success = true;
            size_t written = 0;
            while (written < contents.size())
            {
                const DWORD chunk = static_cast<DWORD>(
                    std::min(contents.size() - written, static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
                DWORD chunkWritten = 0;
                if (!WriteFile(file, contents.data() + written, chunk, &chunkWritten, nullptr) || chunkWritten == 0)
                {
                    SetError(error, WindowsError("WriteFile", GetLastError()));
                    success = false;
                    break;
                }
                written += chunkWritten;
            }
            if (success && !FlushFileBuffers(file))
            {
                SetError(error, WindowsError("FlushFileBuffers", GetLastError()));
                success = false;
            }
            CloseHandle(file);
            if (!success)
                DeleteFileW(path.c_str());
            return success;
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
