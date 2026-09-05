#include "SparkAssetPipelineCore/AssetCooker.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <memory>
#include <sstream>
#include <string_view>
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
            std::transform(text.begin(), text.end(), text.begin(),
                           [](wchar_t character)
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

        bool CopyGenerationFile(const std::filesystem::path& source, const std::filesystem::path& generationOutput,
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
            if (!std::filesystem::copy_file(source, generationOutput, std::filesystem::copy_options::none, ec))
            {
                error = "failed to copy asset into cook generation: " + ec.message();
                return false;
            }
            actualSize = std::filesystem::file_size(generationOutput, ec);
            if (ec || !ComputeFileSha256(generationOutput, actualSha256, error))
            {
                if (ec)
                    error = "failed to inspect generated asset: " + ec.message();
                return false;
            }

            updated = true;
            std::string validationError;
            const bool safePrevious =
                ValidateOutputTarget(previousOutput, previousOutputRoot, validationError, "previous cooked output");
            if (safePrevious && std::filesystem::is_regular_file(previousOutput, ec) && !ec)
            {
                std::string previousSha256;
                if (!ComputeFileSha256(previousOutput, previousSha256, error))
                    return false;
                updated = previousSha256 != actualSha256;
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

        bool StageAndCookFile(const std::filesystem::path& source, const std::filesystem::path& output,
                              const std::string& expectedSha256, bool dryRun, bool& updated, std::string& actualSha256,
                              std::uintmax_t& actualSize, std::string& error)
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

            if (!std::filesystem::copy_file(source, stage, std::filesystem::copy_options::none, ec))
            {
                error = "failed to stage '" + source.string() + "': " + ec.message();
                return false;
            }
            const auto removeStage = [&]
            {
                std::error_code ignored;
                std::filesystem::remove(stage, ignored);
            };

            actualSize = std::filesystem::file_size(stage, ec);
            if (ec || !ComputeFileSha256(stage, actualSha256, error))
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

            if (std::filesystem::is_regular_file(output, ec) && !ec)
            {
                std::string current;
                if (!ComputeFileSha256(output, current, error))
                {
                    removeStage();
                    return false;
                }
                if (current == actualSha256)
                {
                    updated = false;
                    removeStage();
                    return true;
                }
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
    } // namespace

    bool ComputeFileSha256(const std::filesystem::path& path, std::string& digest, std::string& error)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            error = "failed to open '" + path.string() + "'";
            return false;
        }
        Sha256 sha;
        std::vector<uint8_t> buffer(64 * 1024);
        while (file)
        {
            file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            if (const auto count = file.gcount(); count > 0)
                sha.Update(buffer.data(), static_cast<size_t>(count));
        }
        if (!file.eof())
        {
            error = "failed while reading '" + path.string() + "'";
            return false;
        }
        digest = sha.Finalize();
        return true;
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
        return StageAndCookFile(source, output, expectedSha256, dryRun, updated, actualSha256, actualSize, error);
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
                return !portable.empty();
            }
            catch (const std::filesystem::filesystem_error& exception)
            {
                result.error = "asset path is not representable as portable UTF-8: " + std::string(exception.what());
                return false;
            }
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
                    !StageAndCookFile(file.path, previousDestination, {}, true, record.updated, record.sha256,
                                      record.size, result.error))
                    return result;
            }
            else
            {
                const auto generationDestination = generation / relativePath;
                if (!IsContained(generationDestination.lexically_normal(), generation) ||
                    !CopyGenerationFile(file.path, generationDestination, previousDestination, output, record.updated,
                                        record.sha256, record.size, result.error))
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
