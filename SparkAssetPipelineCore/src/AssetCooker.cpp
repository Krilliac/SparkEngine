#include "SparkAssetPipelineCore/AssetCooker.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Spark::AssetPipeline
{
    namespace
    {
        constexpr uint32_t RotateRight(uint32_t value, uint32_t bits)
        {
            return (value >> bits) | (value << (32u - bits));
        }

        class Sha256
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

            [[nodiscard]] std::string Finalize()
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
                constexpr char kHex[] = "0123456789abcdef";
                std::string result(64, '0');
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
                static constexpr std::array<uint32_t, 64> kRounds = {
                    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
                    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
                    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
                    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
                    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
                    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
                    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
                    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
                    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
                    0xc67178f2u,
                };
                std::array<uint32_t, 64> words{};
                for (size_t i = 0; i < 16; ++i)
                {
                    const size_t offset = i * 4;
                    words[i] = (static_cast<uint32_t>(block[offset]) << 24u) |
                               (static_cast<uint32_t>(block[offset + 1]) << 16u) |
                               (static_cast<uint32_t>(block[offset + 2]) << 8u) |
                               static_cast<uint32_t>(block[offset + 3]);
                }
                for (size_t i = 16; i < words.size(); ++i)
                {
                    const uint32_t s0 =
                        RotateRight(words[i - 15], 7) ^ RotateRight(words[i - 15], 18) ^ (words[i - 15] >> 3u);
                    const uint32_t s1 =
                        RotateRight(words[i - 2], 17) ^ RotateRight(words[i - 2], 19) ^ (words[i - 2] >> 10u);
                    words[i] = words[i - 16] + s0 + words[i - 7] + s1;
                }
                uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
                uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];
                for (size_t i = 0; i < words.size(); ++i)
                {
                    const uint32_t temp1 = h + (RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25)) +
                                           ((e & f) ^ (~e & g)) + kRounds[i] + words[i];
                    const uint32_t temp2 =
                        (RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
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

        /// Comparison form for containment. Normalizing is what makes
        /// lexically_relative trustworthy; case-folding is what makes it correct on
        /// Windows, where path equality is case-sensitive but the filesystem is not,
        /// so "C:/Build/Out" and "C:/build/out" would otherwise relativize to an
        /// escaping "../../build/out" form and reject a legitimate target.
        std::filesystem::path ComparisonForm(const std::filesystem::path& path)
        {
#if defined(_WIN32)
            std::wstring text = path.lexically_normal().wstring();
            std::transform(text.begin(), text.end(), text.begin(), [](wchar_t character)
                           { return static_cast<wchar_t>(std::towlower(static_cast<std::wint_t>(character))); });
            return std::filesystem::path(std::move(text));
#else
            return path.lexically_normal();
#endif
        }

        /// Internal spelling of the public IsPathContained (see AssetCooker.h).
        bool IsContained(const std::filesystem::path& child, const std::filesystem::path& parent)
        {
            const auto relative = ComparisonForm(child).lexically_relative(ComparisonForm(parent));
            if (relative.empty() || relative.is_absolute())
                return false;
            for (const auto& component : relative)
            {
                if (component == "..")
                    return false;
            }
            return true;
        }

        bool IsLinkLike(const std::filesystem::path& path)
        {
            std::error_code ec;
            const auto status = std::filesystem::symlink_status(path, ec);
            if (!ec && std::filesystem::is_symlink(status))
                return true;
#if defined(_WIN32)
            const DWORD attributes = ::GetFileAttributesW(path.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
            return false;
#endif
        }

        bool IsUnsafeOutputLink(const std::filesystem::path& path)
        {
            if (IsLinkLike(path))
                return true;
            std::error_code ec;
            return std::filesystem::is_regular_file(path, ec) && !ec &&
                   std::filesystem::hard_link_count(path, ec) > 1 && !ec;
        }

#if defined(_WIN32)
        /// Owns one Win32 handle (the pattern of SparkEngine's Engine/Modding/HeldHandles.h).
        class ScopedHandle
        {
          public:
            explicit ScopedHandle(HANDLE handle) : m_handle(handle) {}
            ~ScopedHandle()
            {
                if (IsValid())
                {
                    ::CloseHandle(m_handle);
                }
            }
            ScopedHandle(const ScopedHandle&) = delete;
            ScopedHandle& operator=(const ScopedHandle&) = delete;

            [[nodiscard]] HANDLE Get() const { return m_handle; }
            [[nodiscard]] bool IsValid() const { return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE; }

          private:
            HANDLE m_handle;
        };

        /// NT-namespace final path of an open handle, or empty when it cannot be queried.
        std::wstring FinalPathOf(HANDLE handle)
        {
            std::wstring buffer(512, L'\0');
            for (;;)
            {
                const DWORD length = ::GetFinalPathNameByHandleW(
                    handle, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_NT);
                if (length == 0)
                {
                    return {};
                }
                if (length < buffer.size())
                {
                    buffer.resize(length);
                    return buffer;
                }
                buffer.resize(static_cast<size_t>(length) + 1);
            }
        }

        /// True when @p child names an entry anywhere below @p parent (both final paths).
        bool IsDescendantFinalPath(const std::wstring& child, std::wstring parent)
        {
            while (!parent.empty() && parent.back() == L'\\')
            {
                parent.pop_back();
            }
            return !parent.empty() && child.size() > parent.size() + 1 &&
                   child.compare(0, parent.size(), parent) == 0 && child[parent.size()] == L'\\';
        }
#else
        /// Owns one POSIX file descriptor.
        class ScopedFd
        {
          public:
            explicit ScopedFd(int fd) : m_fd(fd) {}
            ~ScopedFd()
            {
                if (m_fd >= 0)
                {
                    ::close(m_fd);
                }
            }
            ScopedFd(const ScopedFd&) = delete;
            ScopedFd& operator=(const ScopedFd&) = delete;

            [[nodiscard]] int Get() const { return m_fd; }

            /// Close now and report whether the close succeeded (a deferred write error).
            bool Close()
            {
                const int fd = m_fd;
                m_fd = -1;
                return ::close(fd) == 0;
            }

          private:
            int m_fd;
        };

        /// Path the kernel reports for an open descriptor, or empty when it cannot say.
        std::filesystem::path PathOfDescriptor(int fd)
        {
#if defined(__APPLE__)
            char buffer[PATH_MAX] = {};
            if (::fcntl(fd, F_GETPATH, buffer) == -1)
            {
                return {};
            }
            return std::filesystem::path(buffer);
#else
            std::error_code ec;
            std::filesystem::path path = std::filesystem::read_symlink("/proc/self/fd/" + std::to_string(fd), ec);
            return ec ? std::filesystem::path() : path;
#endif
        }
#endif

        /// Copy one source asset into the new file @p destination through a handle that is opened
        /// first and checked afterwards. It must be a regular file (a FIFO or device is refused
        /// without blocking); with a @p sourceRoot, it must also not be a link and must still lie
        /// inside that root. The source walk checked a path, and copying by that name copied
        /// whatever replaced the entry in between, such as a link to a file outside the tree.
        /// A destination this call created is removed again when the copy fails.
        bool CopySourceAsset(const std::filesystem::path& source, const std::filesystem::path& sourceRoot,
                             const std::filesystem::path& destination, std::string& error)
        {
            const std::string changed = "source asset changed while cooking: '" + source.string() + "'";
#if defined(_WIN32)
            const DWORD openFlags = FILE_FLAG_SEQUENTIAL_SCAN | (sourceRoot.empty() ? 0 : FILE_FLAG_OPEN_REPARSE_POINT);
            const ScopedHandle input(::CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                   nullptr, OPEN_EXISTING, openFlags, nullptr));
            if (!input.IsValid())
            {
                error = "failed to open source asset '" + source.string() + "' (error " +
                        std::to_string(::GetLastError()) + ")";
                return false;
            }
            BY_HANDLE_FILE_INFORMATION info{};
            if (::GetFileType(input.Get()) != FILE_TYPE_DISK || !::GetFileInformationByHandle(input.Get(), &info) ||
                (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                error = "source asset is not a regular file: '" + source.string() + "'";
                return false;
            }
            if (!sourceRoot.empty())
            {
                const ScopedHandle root(::CreateFileW(sourceRoot.c_str(), FILE_READ_ATTRIBUTES,
                                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
                const std::wstring rootFinal = root.IsValid() ? FinalPathOf(root.Get()) : std::wstring();
                if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || rootFinal.empty() ||
                    !IsDescendantFinalPath(FinalPathOf(input.Get()), rootFinal))
                {
                    error = changed;
                    return false;
                }
            }
            const auto sameMetadata =
                [](const BY_HANDLE_FILE_INFORMATION& before, const BY_HANDLE_FILE_INFORMATION& after)
            {
                return before.dwVolumeSerialNumber == after.dwVolumeSerialNumber &&
                       before.nFileIndexHigh == after.nFileIndexHigh && before.nFileIndexLow == after.nFileIndexLow &&
                       before.nFileSizeHigh == after.nFileSizeHigh && before.nFileSizeLow == after.nFileSizeLow &&
                       before.ftLastWriteTime.dwHighDateTime == after.ftLastWriteTime.dwHighDateTime &&
                       before.ftLastWriteTime.dwLowDateTime == after.ftLastWriteTime.dwLowDateTime;
            };
            bool copied = false;
            {
                const ScopedHandle output(::CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                                        FILE_ATTRIBUTE_NORMAL, nullptr));
                if (!output.IsValid())
                {
                    error = "failed to create '" + destination.string() + "' (error " +
                            std::to_string(::GetLastError()) + ")";
                    return false;
                }
                std::vector<char> buffer(64 * 1024);
                for (;;)
                {
                    DWORD got = 0;
                    if (!::ReadFile(input.Get(), buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr))
                    {
                        error = "failed while reading '" + source.string() + "'";
                        break;
                    }
                    if (got == 0)
                    {
                        copied = true;
                        break;
                    }
                    DWORD written = 0;
                    if (!::WriteFile(output.Get(), buffer.data(), got, &written, nullptr) || written != got)
                    {
                        error = "failed while writing '" + destination.string() + "'";
                        break;
                    }
                }
                BY_HANDLE_FILE_INFORMATION after{};
                if (!::GetFileInformationByHandle(input.Get(), &after) || !sameMetadata(info, after))
                {
                    error = changed;
                    copied = false;
                }
            }
            if (!copied)
            {
                ::DeleteFileW(destination.c_str());
            }
            return copied;
#else
            int openFlags = O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC;
            if (!sourceRoot.empty())
            {
                openFlags |= O_NOFOLLOW;
            }
            const ScopedFd input(::open(source.c_str(), openFlags));
            if (input.Get() < 0)
            {
                error = (errno == ELOOP)
                            ? changed
                            : "failed to open source asset '" + source.string() + "': " + std::strerror(errno);
                return false;
            }
            struct stat info = {};
            if (::fstat(input.Get(), &info) != 0 || !S_ISREG(info.st_mode))
            {
                error = "source asset is not a regular file: '" + source.string() + "'";
                return false;
            }
            if (!sourceRoot.empty())
            {
                // The opened file must live inside the source root, and that path must still
                // name the very file that was opened.
                const std::filesystem::path opened = PathOfDescriptor(input.Get());
                struct stat named = {};
                if (opened.empty() || !IsContained(opened, sourceRoot) || ::stat(opened.c_str(), &named) != 0 ||
                    named.st_dev != info.st_dev || named.st_ino != info.st_ino)
                {
                    error = changed;
                    return false;
                }
            }
            ScopedFd output(::open(destination.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR));
            if (output.Get() < 0)
            {
                error = "failed to create '" + destination.string() + "': " + std::strerror(errno);
                return false;
            }
            const auto fail = [&](const std::string& what)
            {
                error = what + ": " + std::strerror(errno);
                output.Close();
                ::unlink(destination.c_str());
                return false;
            };
            std::vector<char> buffer(64 * 1024);
            for (;;)
            {
                const ssize_t got = ::read(input.Get(), buffer.data(), buffer.size());
                if (got < 0 && errno == EINTR)
                {
                    continue;
                }
                if (got < 0)
                {
                    return fail("failed while reading '" + source.string() + "'");
                }
                if (got == 0)
                {
                    break;
                }
                size_t written = 0;
                while (written < static_cast<size_t>(got))
                {
                    const ssize_t put =
                        ::write(output.Get(), buffer.data() + written, static_cast<size_t>(got) - written);
                    if (put < 0 && errno == EINTR)
                    {
                        continue;
                    }
                    if (put <= 0)
                    {
                        return fail("failed while writing '" + destination.string() + "'");
                    }
                    written += static_cast<size_t>(put);
                }
            }
            struct stat after = {};
            if (::fstat(input.Get(), &after) != 0)
            {
                error = changed;
                output.Close();
                ::unlink(destination.c_str());
                return false;
            }
#if defined(__APPLE__)
            const auto sameTimestamp = [](const timespec& before, const timespec& after)
            { return before.tv_sec == after.tv_sec && before.tv_nsec == after.tv_nsec; };
            const bool timestampsUnchanged = sameTimestamp(info.st_mtimespec, after.st_mtimespec) &&
                                             sameTimestamp(info.st_ctimespec, after.st_ctimespec);
#else
            const auto sameTimestamp = [](const timespec& before, const timespec& after)
            { return before.tv_sec == after.tv_sec && before.tv_nsec == after.tv_nsec; };
            const bool timestampsUnchanged =
                sameTimestamp(info.st_mtim, after.st_mtim) && sameTimestamp(info.st_ctim, after.st_ctim);
#endif
            if (after.st_dev != info.st_dev || after.st_ino != info.st_ino || after.st_size != info.st_size ||
                !timestampsUnchanged)
            {
                error = changed;
                output.Close();
                ::unlink(destination.c_str());
                return false;
            }
            // Keep the source's permission bits, as std::filesystem::copy_file did, without its
            // set-id and sticky bits.
            if (::fchmod(output.Get(), info.st_mode & 0777) != 0)
            {
                return fail("failed to set the mode of '" + destination.string() + "'");
            }
            if (!output.Close())
            {
                error = "failed to finish '" + destination.string() + "': " + std::strerror(errno);
                ::unlink(destination.c_str());
                return false;
            }
            return true;
#endif
        }

        bool ValidateOutputTarget(const std::filesystem::path& target, const std::filesystem::path& outputRoot,
                                  std::string& error, std::string_view label)
        {
            const auto normalizedRoot = outputRoot.lexically_normal();
            const auto relative = target.lexically_normal().lexically_relative(normalizedRoot);
            if (!IsContained(target, outputRoot))
            {
                error = std::string(label) + " escapes the output root";
                return false;
            }

            std::filesystem::path current = normalizedRoot;
            for (const auto& component : relative)
            {
                current /= component;
                if (IsUnsafeOutputLink(current))
                {
                    error = "refusing linked " + std::string(label) + " target '" + current.string() + "'";
                    return false;
                }
            }
            return true;
        }

        std::filesystem::path MakeStagePath(const std::filesystem::path& destination)
        {
            static std::atomic<uint64_t> sequence{0};
            const uint64_t nonce = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
            auto stage = destination;
            stage += ".spark-stage-" + std::to_string(nonce) + "-" +
                     std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
            return stage;
        }

        bool AtomicReplace(const std::filesystem::path& stage, const std::filesystem::path& destination,
                           std::string& error)
        {
            if (IsUnsafeOutputLink(destination))
            {
                error = "refusing to replace linked output '" + destination.string() + "'";
                return false;
            }
#if defined(_WIN32)
            if (!::MoveFileExW(stage.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                error = "failed to atomically replace '" + destination.string() + "' (error " +
                        std::to_string(::GetLastError()) + ")";
                return false;
            }
#else
            std::error_code ec;
            std::filesystem::rename(stage, destination, ec);
            if (ec)
            {
                error = "failed to atomically replace '" + destination.string() + "': " + ec.message();
                return false;
            }
#endif
            return true;
        }

        /// Strict UTF-8 (RFC 3629): no stray continuation bytes, overlong forms, surrogates or
        /// code points past U+10FFFF.
        bool IsValidUtf8(std::string_view text)
        {
            size_t index = 0;
            while (index < text.size())
            {
                const auto lead = static_cast<unsigned char>(text[index]);
                if (lead < 0x80u)
                {
                    ++index;
                    continue;
                }
                size_t length = 0;
                uint32_t codepoint = 0;
                uint32_t minimum = 0;
                if ((lead & 0xE0u) == 0xC0u)
                {
                    length = 2;
                    codepoint = lead & 0x1Fu;
                    minimum = 0x80u;
                }
                else if ((lead & 0xF0u) == 0xE0u)
                {
                    length = 3;
                    codepoint = lead & 0x0Fu;
                    minimum = 0x800u;
                }
                else if ((lead & 0xF8u) == 0xF0u)
                {
                    length = 4;
                    codepoint = lead & 0x07u;
                    minimum = 0x10000u;
                }
                else
                {
                    return false;
                }
                if (text.size() - index < length)
                {
                    return false;
                }
                for (size_t offset = 1; offset < length; ++offset)
                {
                    const auto next = static_cast<unsigned char>(text[index + offset]);
                    if ((next & 0xC0u) != 0x80u)
                    {
                        return false;
                    }
                    codepoint = (codepoint << 6u) | (next & 0x3Fu);
                }
                if (codepoint < minimum || codepoint > 0x10FFFFu || (codepoint >= 0xD800u && codepoint <= 0xDFFFu))
                {
                    return false;
                }
                index += length;
            }
            return true;
        }

        std::string EscapeJson(std::string_view input)
        {
            std::string output;
            output.reserve(input.size());
            constexpr char kHex[] = "0123456789abcdef";
            for (const unsigned char value : input)
            {
                switch (value)
                {
                case '"':
                    output += "\\\"";
                    break;
                case '\\':
                    output += "\\\\";
                    break;
                case '\b':
                    output += "\\b";
                    break;
                case '\f':
                    output += "\\f";
                    break;
                case '\n':
                    output += "\\n";
                    break;
                case '\r':
                    output += "\\r";
                    break;
                case '\t':
                    output += "\\t";
                    break;
                default:
                    if (value < 0x20u)
                    {
                        output += "\\u00";
                        output.push_back(kHex[value >> 4u]);
                        output.push_back(kHex[value & 0x0fu]);
                    }
                    else
                    {
                        output.push_back(static_cast<char>(value));
                    }
                    break;
                }
            }
            return output;
        }

        class CookOutputLock
        {
          public:
            CookOutputLock() = default;
            CookOutputLock(const CookOutputLock&) = delete;
            CookOutputLock& operator=(const CookOutputLock&) = delete;

            ~CookOutputLock()
            {
#if defined(_WIN32)
                if (m_handle != INVALID_HANDLE_VALUE)
                {
                    if (m_locked)
                    {
                        OVERLAPPED overlapped{};
                        ::UnlockFileEx(m_handle, 0, MAXDWORD, MAXDWORD, &overlapped);
                    }
                    ::CloseHandle(m_handle);
                }
#else
                if (m_fd >= 0)
                {
                    while (m_locked && ::flock(m_fd, LOCK_UN) != 0 && errno == EINTR)
                    {
                    }
                    ::close(m_fd);
                }
#endif
            }

            bool Acquire(const std::filesystem::path& outputRoot, std::string& error)
            {
                std::error_code ec;
                std::filesystem::create_directories(outputRoot.parent_path(), ec);
                if (ec)
                {
                    error = "failed to create output parent for cook lock: " + ec.message();
                    return false;
                }

                auto lockPath = outputRoot;
                lockPath += ".spark-cook.lock";
                if (IsLinkLike(lockPath))
                {
                    error = "cook lock must not be a link or reparse point";
                    return false;
                }

#if defined(_WIN32)
                m_handle =
                    ::CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                if (m_handle == INVALID_HANDLE_VALUE)
                {
                    error = "failed to open cook lock (error " + std::to_string(::GetLastError()) + ")";
                    return false;
                }
                BY_HANDLE_FILE_INFORMATION information{};
                if (!::GetFileInformationByHandle(m_handle, &information) ||
                    (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
                    information.nNumberOfLinks != 1)
                {
                    error = "cook lock is not a private regular file";
                    return false;
                }
                OVERLAPPED overlapped{};
                if (!::LockFileEx(m_handle, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &overlapped))
                {
                    error = "failed to acquire cook lock (error " + std::to_string(::GetLastError()) + ")";
                    return false;
                }
                m_locked = true;
#else
                int flags = O_CREAT | O_RDWR;
#ifdef O_CLOEXEC
                flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
                flags |= O_NOFOLLOW;
#endif
                m_fd = ::open(lockPath.c_str(), flags, S_IRUSR | S_IWUSR);
                if (m_fd < 0)
                {
                    error = "failed to open cook lock: " + std::string(std::strerror(errno));
                    return false;
                }
                if (::fchmod(m_fd, S_IRUSR | S_IWUSR) != 0)
                {
                    error = "failed to restrict cook lock permissions: " + std::string(std::strerror(errno));
                    return false;
                }
                struct stat information = {};
                if (::fstat(m_fd, &information) != 0 || !S_ISREG(information.st_mode) || information.st_nlink != 1 ||
                    information.st_uid != ::geteuid())
                {
                    error = "cook lock is not an owner-local regular file";
                    return false;
                }
                while (::flock(m_fd, LOCK_EX) != 0)
                {
                    if (errno == EINTR)
                        continue;
                    error = "failed to acquire cook lock: " + std::string(std::strerror(errno));
                    return false;
                }
                m_locked = true;
#endif
                return true;
            }

          private:
#if defined(_WIN32)
            HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
            int m_fd = -1;
#endif
            bool m_locked = false;
        };

        class ScopedDirectoryCleanup
        {
          public:
            explicit ScopedDirectoryCleanup(std::filesystem::path path) : m_path(std::move(path)) {}
            ScopedDirectoryCleanup(const ScopedDirectoryCleanup&) = delete;
            ScopedDirectoryCleanup& operator=(const ScopedDirectoryCleanup&) = delete;
            ~ScopedDirectoryCleanup()
            {
                if (m_active)
                {
                    std::error_code ignored;
                    std::filesystem::remove_all(m_path, ignored);
                }
            }
            void Release() noexcept { m_active = false; }

          private:
            std::filesystem::path m_path;
            bool m_active = true;
        };

        bool CreateGenerationDirectory(const std::filesystem::path& outputRoot, std::filesystem::path& generation,
                                       std::string& error)
        {
            for (size_t attempt = 0; attempt < 32; ++attempt)
            {
                generation = MakeStagePath(outputRoot);
                std::error_code ec;
                if (std::filesystem::create_directory(generation, ec))
                    return true;
                if (ec && ec != std::errc::file_exists)
                {
                    error = "failed to create cook generation directory: " + ec.message();
                    return false;
                }
            }
            error = "failed to allocate a unique cook generation directory";
            return false;
        }

        bool ComputeFileSha256Opened(const std::filesystem::path& path, const std::filesystem::path& containmentRoot,
                                     std::string& digest, std::string& error, bool& missing);

        bool CopyGenerationFile(const std::filesystem::path& source, const std::filesystem::path& sourceRoot,
                                const std::filesystem::path& generationOutput,
                                const std::filesystem::path& previousOutput,
                                const std::filesystem::path& previousOutputRoot, bool& updated,
                                std::string& actualSha256, std::uintmax_t& actualSize, std::string& error)
        {
            std::error_code ec;
            std::filesystem::create_directories(generationOutput.parent_path(), ec);
            if (ec)
            {
                error = "failed to create generation output directory: " + ec.message();
                return false;
            }
            if (!CopySourceAsset(source, sourceRoot, generationOutput, error))
            {
                return false;
            }
            actualSize = std::filesystem::file_size(generationOutput, ec);
            bool generationMissing = false;
            if (ec || !ComputeFileSha256Opened(generationOutput, generationOutput.parent_path(), actualSha256, error,
                                               generationMissing))
            {
                if (ec)
                    error = "failed to inspect generated asset: " + ec.message();
                return false;
            }

            updated = true;
            std::string validationError;
            const bool safePrevious =
                ValidateOutputTarget(previousOutput, previousOutputRoot, validationError, "previous cooked output");
            if (safePrevious)
            {
                std::string previousSha256;
                bool missing = false;
                if (!ComputeFileSha256Opened(previousOutput, previousOutputRoot, previousSha256, error, missing) &&
                    !missing)
                    return false;
                if (!missing)
                {
                    updated = previousSha256 != actualSha256;
                }
                else
                {
                    error.clear();
                }
            }
            return true;
        }

        bool WriteManifest(const std::filesystem::path& manifest, const std::vector<CookRecord>& records,
                           std::string_view manifestSha256, std::string& error)
        {
            std::error_code ec;
            std::filesystem::create_directories(manifest.parent_path(), ec);
            if (ec)
            {
                error = "failed to create cook manifest directory: " + ec.message();
                return false;
            }
            std::ofstream stream(manifest, std::ios::binary | std::ios::trunc);
            if (!stream)
            {
                error = "failed to create cook manifest";
                return false;
            }
            stream << "{\n  \"schemaVersion\": 1,\n  \"manifestSha256\": \"" << manifestSha256
                   << "\",\n  \"assets\": [\n";
            for (size_t i = 0; i < records.size(); ++i)
            {
                const auto& record = records[i];
                stream << "    {\"path\": \"" << EscapeJson(record.path) << "\", \"sha256\": \"" << record.sha256
                       << "\", \"size\": " << record.size << "}" << (i + 1 == records.size() ? "\n" : ",\n");
            }
            stream << "  ]\n}\n";
            stream.flush();
            if (!stream)
            {
                error = "failed to write cook manifest";
                return false;
            }
            return true;
        }

        bool PublishGeneration(const std::filesystem::path& generation, const std::filesystem::path& outputRoot,
                               std::string& error)
        {
            std::error_code ec;
            const auto outputStatus = std::filesystem::symlink_status(outputRoot, ec);
            const bool outputExists = !ec && std::filesystem::exists(outputStatus);
            if (ec && ec != std::errc::no_such_file_or_directory)
            {
                error = "failed to inspect current cook output: " + ec.message();
                return false;
            }
            if (outputExists && (!std::filesystem::is_directory(outputStatus) || IsLinkLike(outputRoot)))
            {
                error = "cook output must be a real directory";
                return false;
            }

            std::filesystem::path backup;
            if (outputExists)
            {
                for (size_t attempt = 0; attempt < 32; ++attempt)
                {
                    backup = MakeStagePath(outputRoot);
                    if (!std::filesystem::exists(backup, ec) && !ec)
                        break;
                    ec.clear();
                    backup.clear();
                }
                if (backup.empty())
                {
                    error = "failed to allocate a unique cook rollback directory";
                    return false;
                }
                std::filesystem::rename(outputRoot, backup, ec);
                if (ec)
                {
                    error = "failed to stage current cook output for replacement: " + ec.message();
                    return false;
                }
            }

            std::filesystem::rename(generation, outputRoot, ec);
            if (ec)
            {
                const std::string publishError = ec.message();
                if (outputExists)
                {
                    std::error_code rollbackError;
                    std::filesystem::rename(backup, outputRoot, rollbackError);
                    if (rollbackError)
                    {
                        error = "failed to publish cook generation (" + publishError + ") and rollback failed (" +
                                rollbackError.message() + ")";
                        return false;
                    }
                }
                error = "failed to publish cook generation: " + publishError;
                return false;
            }

            if (outputExists)
            {
                std::error_code ignored;
                std::filesystem::remove_all(backup, ignored);
            }
            return true;
        }

        std::string HashRecords(const std::vector<CookRecord>& records)
        {
            std::ostringstream body;
            for (const auto& record : records)
                body << record.path << '\0' << record.sha256 << '\0' << record.size << '\n';
            const std::string bytes = body.str();
            Sha256 sha;
            sha.Update(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
            return sha.Finalize();
        }

        bool StageAndCookFile(const std::filesystem::path& source, const std::filesystem::path& sourceRoot,
                              const std::filesystem::path& output, const std::string& expectedSha256, bool dryRun,
                              bool& updated, std::string& actualSha256, std::uintmax_t& actualSize, std::string& error)
        {
            updated = true;
            std::error_code ec;
            std::filesystem::path stage;
            if (dryRun)
            {
                stage = std::filesystem::temp_directory_path(ec) / MakeStagePath(output.filename());
                if (ec)
                {
                    error = "failed to resolve temporary staging directory: " + ec.message();
                    return false;
                }
            }
            else
            {
                const std::filesystem::path outputParent = output.parent_path();
                if (!outputParent.empty())
                {
                    std::filesystem::create_directories(outputParent, ec);
                    if (ec)
                    {
                        error = "failed to create output directory: " + ec.message();
                        return false;
                    }
                }
                stage = MakeStagePath(output);
            }

            if (!CopySourceAsset(source, sourceRoot, stage, error))
            {
                error = "failed to stage '" + source.string() + "': " + error;
                return false;
            }
            const auto removeStage = [&]
            {
                std::error_code ignored;
                std::filesystem::remove(stage, ignored);
            };

            actualSize = std::filesystem::file_size(stage, ec);
            bool stagedMissing = false;
            if (ec || !ComputeFileSha256Opened(stage, stage.parent_path(), actualSha256, error, stagedMissing))
            {
                if (ec)
                    error = "failed to inspect staged asset: " + ec.message();
                removeStage();
                return false;
            }
            if (!expectedSha256.empty() && actualSha256 != expectedSha256)
            {
                error = "staged asset SHA-256 does not match the requested digest";
                removeStage();
                return false;
            }

            std::string current;
            bool outputMissing = false;
            if (ComputeFileSha256Opened(output, output.parent_path(), current, error, outputMissing))
            {
                if (current == actualSha256)
                {
                    updated = false;
                    removeStage();
                    return true;
                }
            }
            else if (!outputMissing)
            {
                removeStage();
                return false;
            }
            else
            {
                error.clear();
            }
            if (dryRun)
            {
                removeStage();
                return true;
            }
            if (!AtomicReplace(stage, output, error))
            {
                removeStage();
                return false;
            }
            return true;
        }

        bool ComputeFileSha256Opened(const std::filesystem::path& path, const std::filesystem::path& containmentRoot,
                                     std::string& digest, std::string& error, bool& missing)
        {
            missing = false;
            Sha256 sha;
            std::vector<uint8_t> buffer(64 * 1024);
#if defined(_WIN32)
            const DWORD openFlags =
                FILE_FLAG_SEQUENTIAL_SCAN | (containmentRoot.empty() ? 0 : FILE_FLAG_OPEN_REPARSE_POINT);
            const HANDLE handle =
                ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, openFlags, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
            {
                const DWORD openError = ::GetLastError();
                missing = openError == ERROR_FILE_NOT_FOUND || openError == ERROR_PATH_NOT_FOUND;
                error = "failed to open '" + path.string() + "'";
                return false;
            }
            const ScopedHandle input(handle);
            BY_HANDLE_FILE_INFORMATION before{};
            if (::GetFileType(input.Get()) != FILE_TYPE_DISK || !::GetFileInformationByHandle(input.Get(), &before) ||
                (before.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                error = "source is not a regular file: '" + path.string() + "'";
                return false;
            }
            if (!containmentRoot.empty())
            {
                const ScopedHandle root(::CreateFileW(containmentRoot.c_str(), FILE_READ_ATTRIBUTES,
                                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
                if (!root.IsValid() || (before.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
                    !IsDescendantFinalPath(FinalPathOf(input.Get()), FinalPathOf(root.Get())))
                {
                    error = "source asset escapes its containment root: '" + path.string() + "'";
                    return false;
                }
            }
            for (;;)
            {
                DWORD got = 0;
                if (!::ReadFile(input.Get(), buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr))
                {
                    error = "failed while reading '" + path.string() + "'";
                    return false;
                }
                if (got == 0)
                {
                    break;
                }
                sha.Update(buffer.data(), got);
            }
            BY_HANDLE_FILE_INFORMATION after{};
            if (!::GetFileInformationByHandle(input.Get(), &after) ||
                before.dwVolumeSerialNumber != after.dwVolumeSerialNumber ||
                before.nFileIndexHigh != after.nFileIndexHigh || before.nFileIndexLow != after.nFileIndexLow ||
                before.nFileSizeHigh != after.nFileSizeHigh || before.nFileSizeLow != after.nFileSizeLow ||
                before.ftLastWriteTime.dwHighDateTime != after.ftLastWriteTime.dwHighDateTime ||
                before.ftLastWriteTime.dwLowDateTime != after.ftLastWriteTime.dwLowDateTime)
            {
                error = "source asset changed while hashing: '" + path.string() + "'";
                return false;
            }
#else
            const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC |
                                                    (containmentRoot.empty() ? 0 : O_NOFOLLOW));
            if (fd < 0)
            {
                missing = errno == ENOENT || errno == ENOTDIR;
                error = "failed to open '" + path.string() + "'";
                return false;
            }
            const ScopedFd input(fd);
            struct stat before = {};
            if (::fstat(input.Get(), &before) != 0 || !S_ISREG(before.st_mode))
            {
                error = "source is not a regular file: '" + path.string() + "'";
                return false;
            }
            if (!containmentRoot.empty())
            {
                const std::filesystem::path opened = PathOfDescriptor(input.Get());
                if (opened.empty() || !IsContained(opened, containmentRoot))
                {
                    error = "source asset escapes its containment root: '" + path.string() + "'";
                    return false;
                }
            }
            for (;;)
            {
                const ssize_t got = ::read(input.Get(), buffer.data(), buffer.size());
                if (got < 0 && errno == EINTR)
                {
                    continue;
                }
                if (got < 0)
                {
                    error = "failed while reading '" + path.string() + "'";
                    return false;
                }
                if (got == 0)
                {
                    break;
                }
                sha.Update(buffer.data(), static_cast<size_t>(got));
            }
            struct stat after = {};
            if (::fstat(input.Get(), &after) != 0 || after.st_dev != before.st_dev || after.st_ino != before.st_ino ||
                after.st_size != before.st_size)
            {
                error = "source asset changed while hashing: '" + path.string() + "'";
                return false;
            }
#if defined(__APPLE__)
            if (after.st_mtimespec.tv_sec != before.st_mtimespec.tv_sec ||
                after.st_mtimespec.tv_nsec != before.st_mtimespec.tv_nsec ||
                after.st_ctimespec.tv_sec != before.st_ctimespec.tv_sec ||
                after.st_ctimespec.tv_nsec != before.st_ctimespec.tv_nsec)
#else
            if (after.st_mtim.tv_sec != before.st_mtim.tv_sec || after.st_mtim.tv_nsec != before.st_mtim.tv_nsec ||
                after.st_ctim.tv_sec != before.st_ctim.tv_sec || after.st_ctim.tv_nsec != before.st_ctim.tv_nsec)
#endif
            {
                error = "source asset changed while hashing: '" + path.string() + "'";
                return false;
            }
#endif
            digest = sha.Finalize();
            return true;
        }
    } // namespace

    bool ComputeFileSha256(const std::filesystem::path& path, std::string& digest, std::string& error)
    {
        bool missing = false;
        return ComputeFileSha256Opened(path, {}, digest, error, missing);
    }

    bool CookFile(const std::filesystem::path& source, const std::filesystem::path& output,
                  const std::string& expectedSha256, bool dryRun, bool& updated, std::string& error)
    {
        if (IsUnsafeOutputLink(output))
        {
            error = "refusing to replace linked output '" + output.string() + "'";
            return false;
        }
        std::string actualSha256;
        std::uintmax_t actualSize = 0;
        // A single file named by the caller has no source tree to stay inside; it is still read
        // through the handle (regular files only, never blocking on a FIFO).
        return StageAndCookFile(source, {}, output, expectedSha256, dryRun, updated, actualSha256, actualSize, error);
    }

    CookResult CookAssets(const CookRequest& request)
    {
        CookResult result;
        std::error_code ec;
        const auto sourceStatus = std::filesystem::symlink_status(request.sourceRoot, ec);
        if (ec || std::filesystem::is_symlink(sourceStatus) || IsLinkLike(request.sourceRoot))
        {
            result.error = "source root must not be a link or reparse point";
            return result;
        }
        const auto source = std::filesystem::weakly_canonical(request.sourceRoot, ec);
        if (ec || !std::filesystem::is_directory(source))
        {
            result.error = "source root is not a readable directory";
            return result;
        }
        ec.clear();
        if (IsLinkLike(request.outputRoot))
        {
            result.error = "output root must not be a link or reparse point";
            return result;
        }
        ec.clear();
        const auto outputLexical = std::filesystem::absolute(request.outputRoot, ec).lexically_normal();
        if (ec)
        {
            result.error = "failed to resolve output root";
            return result;
        }
        const auto output = std::filesystem::weakly_canonical(outputLexical, ec);
        if (ec)
        {
            result.error = "failed to resolve output root";
            return result;
        }
        if (source == output || IsContained(source, output) || IsContained(output, source))
        {
            result.error = "source and output roots must not overlap";
            return result;
        }
        if (output.filename().empty())
        {
            result.error = "output root must name a directory below its parent";
            return result;
        }

        CookOutputLock outputLock;
        if (!outputLock.Acquire(output, result.error))
            return result;
        if (IsLinkLike(output))
        {
            result.error = "output root must not be a link or reparse point";
            return result;
        }
        const auto outputStatus = std::filesystem::symlink_status(output, ec);
        if (!ec && std::filesystem::exists(outputStatus) && !std::filesystem::is_directory(outputStatus))
        {
            result.error = "output root must be a directory";
            return result;
        }
        ec.clear();

        const auto manifestCandidate =
            request.manifestPath.empty() ? outputLexical / "spark-cook-manifest.json" : request.manifestPath;
        const auto manifestLexical = std::filesystem::absolute(manifestCandidate, ec).lexically_normal();
        if (ec || !IsContained(manifestLexical, outputLexical) ||
            !ValidateOutputTarget(manifestLexical, outputLexical, result.error, "cook manifest"))
        {
            if (result.error.empty())
                result.error = "cook manifest escapes the output root";
            return result;
        }
        const auto manifest = std::filesystem::weakly_canonical(manifestLexical, ec);
        if (ec || !IsContained(manifest, output))
        {
            result.error = "cook manifest escapes the output root";
            return result;
        }
        if (!ValidateOutputTarget(manifest, output, result.error, "cook manifest"))
            return result;

        struct SourceEntry
        {
            std::filesystem::path path;
            std::string portablePath;
        };
        const auto makePortableRelative =
            [&](const std::filesystem::path& path, const std::filesystem::path& root, std::string& portable)
        {
            try
            {
                const std::u8string utf8 = path.lexically_relative(root).generic_u8string();
                portable.assign(reinterpret_cast<const char*>(utf8.data()), utf8.size());
            }
            catch (const std::system_error& exception)
            {
                // std::filesystem::filesystem_error derives from std::system_error, and MSVC's
                // UTF-16 -> UTF-8 conversion of an unpaired surrogate throws the base type.
                result.error = "asset path is not representable as portable UTF-8: " + std::string(exception.what());
                return false;
            }
            // A POSIX name is bytes, and generic_u8string() copies them unchecked: a name that is
            // not UTF-8 was written raw into the JSON manifest, which no strict reader accepts.
            if (!IsValidUtf8(portable))
            {
                result.error = "asset path is not representable as portable UTF-8: " + path.string();
                return false;
            }
            return !portable.empty();
        };

        std::vector<SourceEntry> files;
        std::filesystem::recursive_directory_iterator iterator(source, ec), end;
        while (!ec && iterator != end)
        {
            const auto status = iterator->symlink_status(ec);
            if (ec)
                break;
            if (std::filesystem::is_symlink(status) || IsLinkLike(iterator->path()))
            {
                iterator.disable_recursion_pending();
            }
            else if (std::filesystem::is_regular_file(status))
            {
                const auto canonical = std::filesystem::weakly_canonical(iterator->path(), ec);
                if (ec || !IsContained(canonical, source))
                {
                    result.error = "source entry escapes the source root";
                    return result;
                }
                SourceEntry entry;
                entry.path = canonical;
                if (!makePortableRelative(canonical, source, entry.portablePath))
                    return result;
                files.push_back(std::move(entry));
            }
            iterator.increment(ec);
        }
        if (ec)
        {
            result.error = "failed to enumerate source assets: " + ec.message();
            return result;
        }
        std::sort(files.begin(), files.end(), [](const SourceEntry& left, const SourceEntry& right)
                  { return left.portablePath < right.portablePath; });

        std::filesystem::path generation;
        std::unique_ptr<ScopedDirectoryCleanup> generationCleanup;
        if (!request.dryRun)
        {
            if (!CreateGenerationDirectory(output, generation, result.error))
                return result;
            generationCleanup = std::make_unique<ScopedDirectoryCleanup>(generation);
        }
        for (const auto& file : files)
        {
            CookRecord record;
            record.path = file.portablePath;
            const auto relativePath = std::filesystem::u8path(record.path);
            const auto previousDestination = output / relativePath;
            if (request.dryRun)
            {
                if (!ValidateOutputTarget(previousDestination, output, result.error, "cooked output") ||
                    !StageAndCookFile(file.path, source, previousDestination, {}, true, record.updated, record.sha256,
                                      record.size, result.error))
                    return result;
            }
            else
            {
                const auto generationDestination = generation / relativePath;
                if (!IsContained(generationDestination.lexically_normal(), generation) ||
                    !CopyGenerationFile(file.path, source, generationDestination, previousDestination, output,
                                        record.updated, record.sha256, record.size, result.error))
                {
                    if (result.error.empty())
                        result.error = "cooked output escapes the generation directory";
                    return result;
                }
            }
            record.updated ? ++result.updatedCount : ++result.unchangedCount;
            result.records.push_back(std::move(record));
            if (request.onProgress)
                request.onProgress(result.records.back(), result.records.size(), files.size());
        }
        result.manifestSha256 = HashRecords(result.records);
        if (!request.dryRun)
        {
            const auto manifestRelative = manifest.lexically_relative(output);
            const auto generationManifest = (generation / manifestRelative).lexically_normal();
            std::string manifestRecordPath;
            if (!makePortableRelative(manifest, output, manifestRecordPath))
                return result;
            const bool conflictsWithAsset =
                std::any_of(result.records.begin(), result.records.end(),
                            [&](const CookRecord& record) { return record.path == manifestRecordPath; });
            if (conflictsWithAsset)
            {
                result.error = "cook manifest conflicts with a cooked asset";
                return result;
            }
            if (!IsContained(generationManifest, generation) ||
                !WriteManifest(generationManifest, result.records, result.manifestSha256, result.error))
            {
                if (result.error.empty())
                    result.error = "cook manifest escapes the generation directory";
                return result;
            }
            if (!PublishGeneration(generation, output, result.error))
                return result;
            generationCleanup->Release();
        }
        return result;
    }
    bool IsPathContained(const std::filesystem::path& child, const std::filesystem::path& parent)
    {
        return IsContained(child, parent);
    }
} // namespace Spark::AssetPipeline
