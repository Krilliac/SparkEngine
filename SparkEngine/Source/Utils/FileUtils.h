/**
 * @file FileUtils.h
 * @brief File I/O and path manipulation utilities
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides cross-platform file operations using C++17 <filesystem>.
 * All functions are stateless and thread-safe for independent paths.
 *
 * ## Usage
 * @code
 *   using namespace Spark::FileUtils;
 *
 *   auto content = ReadTextFile("config.ini");
 *   WriteTextFile("output.txt", "Hello");
 *
 *   std::string ext = GetExtension("model.fbx");    // ".fbx"
 *   std::string name = GetFilename("path/model.fbx"); // "model.fbx"
 *   std::string dir = GetDirectory("path/model.fbx"); // "path"
 *   std::string path = JoinPath("assets", "model.fbx");
 * @endcode
 */

#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

#if __has_include(<filesystem>)
#include <filesystem>
namespace fs = std::filesystem;
#define SPARK_HAS_FILESYSTEM 1
#else
#define SPARK_HAS_FILESYSTEM 0
#endif

namespace Spark
{
    namespace FileUtils
    {

        // =============================================================================
        // Path encoding
        //
        // Engine path strings are UTF-8. On Windows the narrow std::filesystem::path and
        // std::fstream constructors decode their bytes in the active ANSI code page
        // instead, so a UTF-8 path with any non-ASCII character opens nothing, or writes
        // a mojibake-named sibling. Every function below converts through
        // PathFromUtf8() and returns strings in the encoding it was given.
        // =============================================================================

        /// True when @p text is well-formed UTF-8: no overlong forms, no UTF-16
        /// surrogates, nothing above U+10FFFF, no truncated sequence.
        inline bool IsValidUtf8(std::string_view text) noexcept
        {
            size_t i = 0;
            while (i < text.size())
            {
                const auto lead = static_cast<unsigned char>(text[i]);
                if (lead < 0x80)
                {
                    ++i;
                    continue;
                }

                size_t length = 0;
                uint32_t codePoint = 0;
                if (lead >= 0xC2 && lead <= 0xDF)
                {
                    length = 2;
                    codePoint = lead & 0x1Fu;
                }
                else if (lead >= 0xE0 && lead <= 0xEF)
                {
                    length = 3;
                    codePoint = lead & 0x0Fu;
                }
                else if (lead >= 0xF0 && lead <= 0xF4)
                {
                    length = 4;
                    codePoint = lead & 0x07u;
                }
                else
                {
                    return false; // continuation byte, overlong 2-byte lead, or 0xF5+
                }

                if (text.size() - i < length)
                    return false;
                for (size_t k = 1; k < length; ++k)
                {
                    const auto continuation = static_cast<unsigned char>(text[i + k]);
                    if ((continuation & 0xC0u) != 0x80u)
                        return false;
                    codePoint = (codePoint << 6) | (continuation & 0x3Fu);
                }

                const bool overlong = (length == 3 && codePoint < 0x800u) || (length == 4 && codePoint < 0x10000u);
                const bool surrogate = codePoint >= 0xD800u && codePoint <= 0xDFFFu;
                if (overlong || surrogate || codePoint > 0x10FFFFu)
                    return false;
                i += length;
            }
            return true;
        }

#if SPARK_HAS_FILESYSTEM

        /**
         * @brief Convert an engine path string to a filesystem path.
         *
         * Valid UTF-8 is decoded as UTF-8 on every platform. A string that is not valid
         * UTF-8 can only be a legacy active-code-page string (for example the result of
         * path::string() on Windows), so it keeps the native narrow interpretation. A
         * string the code page cannot decode yields an empty path, which opens nothing.
         */
        inline fs::path PathFromUtf8(const std::string& path)
        {
#ifdef _WIN32
            if (IsValidUtf8(path))
            {
                const auto* first = reinterpret_cast<const char8_t*>(path.data());
                return fs::path(std::u8string(first, first + path.size()));
            }
            try
            {
                return fs::path(path);
            }
            catch (const std::system_error&)
            {
                return {};
            }
#else
            return fs::path(path);
#endif
        }

        /**
         * @brief Render @p path in the same encoding as @p source.
         *
         * UTF-8 unless @p source was a legacy active-code-page string. Throws
         * std::system_error on Windows when a legacy caller needs a name its code page
         * cannot represent.
         */
        inline std::string PathToString(const fs::path& path, [[maybe_unused]] const std::string& source)
        {
#ifdef _WIN32
            if (IsValidUtf8(source))
            {
                const std::u8string utf8 = path.u8string();
                return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
            }
#endif
            return path.string();
        }

#endif // SPARK_HAS_FILESYSTEM

        namespace detail
        {
#if SPARK_HAS_FILESYSTEM
            inline fs::path StreamPath(const std::string& path)
            {
                return PathFromUtf8(path);
            }
#else
            inline const std::string& StreamPath(const std::string& path) noexcept
            {
                return path;
            }
#endif
        } // namespace detail

        // =============================================================================
        // File I/O
        // =============================================================================

        /// Read entire text file into a string. Returns nullopt on failure.
        inline std::optional<std::string> ReadTextFile(const std::string& path)
        {
            std::ifstream file(detail::StreamPath(path), std::ios::in);
            if (!file.is_open())
                return std::nullopt;
            std::ostringstream ss;
            ss << file.rdbuf();
            return ss.str();
        }

        /// Write string to a text file. Returns true on success.
        inline bool WriteTextFile(const std::string& path, const std::string& content)
        {
            std::ofstream file(detail::StreamPath(path), std::ios::out | std::ios::trunc);
            if (!file.is_open())
                return false;
            file << content;
            return file.good();
        }

        /// Read entire binary file. Returns nullopt on failure.
        inline std::optional<std::vector<uint8_t>> ReadBinaryFile(const std::string& path)
        {
            std::ifstream file(detail::StreamPath(path), std::ios::in | std::ios::binary | std::ios::ate);
            if (!file.is_open())
                return std::nullopt;
            auto size = file.tellg();
            if (size <= 0)
                return std::vector<uint8_t>{};
            file.seekg(0, std::ios::beg);
            std::vector<uint8_t> data(static_cast<size_t>(size));
            file.read(reinterpret_cast<char*>(data.data()), size);
            if (!file.good())
                return std::nullopt;
            return data;
        }

        /// Write binary data to file. Returns true on success.
        inline bool WriteBinaryFile(const std::string& path, const std::vector<uint8_t>& data)
        {
            std::ofstream file(detail::StreamPath(path), std::ios::out | std::ios::binary | std::ios::trunc);
            if (!file.is_open())
                return false;
            file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            return file.good();
        }

        // =============================================================================
        // Path Manipulation
        // =============================================================================

#if SPARK_HAS_FILESYSTEM

        inline std::string GetExtension(const std::string& path)
        {
            return PathToString(PathFromUtf8(path).extension(), path);
        }

        inline std::string GetFilename(const std::string& path)
        {
            return PathToString(PathFromUtf8(path).filename(), path);
        }

        inline std::string GetStem(const std::string& path)
        {
            return PathToString(PathFromUtf8(path).stem(), path);
        }

        inline std::string GetDirectory(const std::string& path)
        {
            return PathToString(PathFromUtf8(path).parent_path(), path);
        }

        inline std::string JoinPath(const std::string& a, const std::string& b)
        {
            // UTF-8 only when both halves are, so a legacy half keeps its code page.
            const std::string& encoding = IsValidUtf8(a) ? b : a;
            return PathToString(PathFromUtf8(a) / PathFromUtf8(b), encoding);
        }

        inline std::string NormalizePath(const std::string& path)
        {
            return PathToString(PathFromUtf8(path).lexically_normal(), path);
        }

        inline std::string ChangeExtension(const std::string& path, const std::string& newExt)
        {
            fs::path p = PathFromUtf8(path);
            p.replace_extension(PathFromUtf8(newExt));
            return PathToString(p, IsValidUtf8(path) ? newExt : path);
        }

        // =============================================================================
        // File Queries
        // =============================================================================

        inline bool FileExists(const std::string& path)
        {
            std::error_code ec;
            return fs::exists(PathFromUtf8(path), ec);
        }

        inline bool IsDirectory(const std::string& path)
        {
            std::error_code ec;
            return fs::is_directory(PathFromUtf8(path), ec);
        }

        inline std::optional<uintmax_t> GetFileSize(const std::string& path)
        {
            std::error_code ec;
            auto size = fs::file_size(PathFromUtf8(path), ec);
            if (ec)
                return std::nullopt;
            return size;
        }

        // =============================================================================
        // Directory Operations
        // =============================================================================

        inline bool CreateDirectories(const std::string& path)
        {
            std::error_code ec;
            fs::create_directories(PathFromUtf8(path), ec);
            return !ec;
        }

        namespace detail
        {
            /// Shared body of ListFiles / ListFilesRecursive. Entries come back in the
            /// encoding of @p dir; a name a legacy code-page caller cannot represent is skipped.
            template <typename Iterator>
            std::vector<std::string> ListFilesWith(const std::string& dir, const std::string& extensionFilter)
            {
                std::vector<std::string> files;
                std::error_code ec;
                const fs::path root = PathFromUtf8(dir);
                if (!fs::is_directory(root, ec))
                    return files;

                const fs::path extension = PathFromUtf8(extensionFilter);
                for (const auto& entry : Iterator(root, ec))
                {
                    if (!entry.is_regular_file())
                        continue;
                    if (!extensionFilter.empty() && entry.path().extension() != extension)
                        continue;
                    try
                    {
                        files.push_back(PathToString(entry.path(), dir));
                    }
                    catch (const std::system_error&)
                    {
                    }
                }
                return files;
            }
        } // namespace detail

        /// List files in a directory, optionally filtering by extension (e.g. ".png")
        inline std::vector<std::string> ListFiles(const std::string& dir, const std::string& extensionFilter = "")
        {
            return detail::ListFilesWith<fs::directory_iterator>(dir, extensionFilter);
        }

        /// Recursively list all files in a directory tree
        inline std::vector<std::string> ListFilesRecursive(const std::string& dir,
                                                           const std::string& extensionFilter = "")
        {
            return detail::ListFilesWith<fs::recursive_directory_iterator>(dir, extensionFilter);
        }

#else
        // Fallback implementations without <filesystem>

        inline std::string GetExtension(const std::string& path)
        {
            auto dot = path.rfind('.');
            if (dot == std::string::npos)
                return "";
            return path.substr(dot);
        }

        inline std::string GetFilename(const std::string& path)
        {
            auto sep = path.find_last_of("/\\");
            if (sep == std::string::npos)
                return path;
            return path.substr(sep + 1);
        }

        inline std::string GetStem(const std::string& path)
        {
            std::string name = GetFilename(path);
            auto dot = name.rfind('.');
            if (dot == std::string::npos)
                return name;
            return name.substr(0, dot);
        }

        inline std::string GetDirectory(const std::string& path)
        {
            auto sep = path.find_last_of("/\\");
            if (sep == std::string::npos)
                return "";
            return path.substr(0, sep);
        }

        inline std::string JoinPath(const std::string& a, const std::string& b)
        {
            if (a.empty())
                return b;
            char last = a.back();
            if (last == '/' || last == '\\')
                return a + b;
            return a + "/" + b;
        }

        inline bool FileExists(const std::string& path)
        {
            std::ifstream f(path);
            return f.good();
        }

#endif // SPARK_HAS_FILESYSTEM

    } // namespace FileUtils
} // namespace Spark
