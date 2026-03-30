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
        // File I/O
        // =============================================================================

        /// Read entire text file into a string. Returns nullopt on failure.
        inline std::optional<std::string> ReadTextFile(const std::string& path)
        {
            std::ifstream file(path, std::ios::in);
            if (!file.is_open())
                return std::nullopt;
            std::ostringstream ss;
            ss << file.rdbuf();
            return ss.str();
        }

        /// Write string to a text file. Returns true on success.
        inline bool WriteTextFile(const std::string& path, const std::string& content)
        {
            std::ofstream file(path, std::ios::out | std::ios::trunc);
            if (!file.is_open())
                return false;
            file << content;
            return file.good();
        }

        /// Read entire binary file. Returns nullopt on failure.
        inline std::optional<std::vector<uint8_t>> ReadBinaryFile(const std::string& path)
        {
            std::ifstream file(path, std::ios::in | std::ios::binary | std::ios::ate);
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
            std::ofstream file(path, std::ios::out | std::ios::binary | std::ios::trunc);
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
            return fs::path(path).extension().string();
        }

        inline std::string GetFilename(const std::string& path)
        {
            return fs::path(path).filename().string();
        }

        inline std::string GetStem(const std::string& path)
        {
            return fs::path(path).stem().string();
        }

        inline std::string GetDirectory(const std::string& path)
        {
            return fs::path(path).parent_path().string();
        }

        inline std::string JoinPath(const std::string& a, const std::string& b)
        {
            return (fs::path(a) / fs::path(b)).string();
        }

        inline std::string NormalizePath(const std::string& path)
        {
            return fs::path(path).lexically_normal().string();
        }

        inline std::string ChangeExtension(const std::string& path, const std::string& newExt)
        {
            fs::path p(path);
            p.replace_extension(newExt);
            return p.string();
        }

        // =============================================================================
        // File Queries
        // =============================================================================

        inline bool FileExists(const std::string& path)
        {
            std::error_code ec;
            return fs::exists(path, ec);
        }

        inline bool IsDirectory(const std::string& path)
        {
            std::error_code ec;
            return fs::is_directory(path, ec);
        }

        inline std::optional<uintmax_t> GetFileSize(const std::string& path)
        {
            std::error_code ec;
            auto size = fs::file_size(path, ec);
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
            fs::create_directories(path, ec);
            return !ec;
        }

        /// List files in a directory, optionally filtering by extension (e.g. ".png")
        inline std::vector<std::string> ListFiles(const std::string& dir, const std::string& extensionFilter = "")
        {
            std::vector<std::string> files;
            std::error_code ec;
            if (!fs::is_directory(dir, ec))
                return files;

            for (const auto& entry : fs::directory_iterator(dir, ec))
            {
                if (!entry.is_regular_file())
                    continue;
                if (!extensionFilter.empty())
                {
                    if (entry.path().extension().string() != extensionFilter)
                        continue;
                }
                files.push_back(entry.path().string());
            }
            return files;
        }

        /// Recursively list all files in a directory tree
        inline std::vector<std::string> ListFilesRecursive(const std::string& dir,
                                                           const std::string& extensionFilter = "")
        {
            std::vector<std::string> files;
            std::error_code ec;
            if (!fs::is_directory(dir, ec))
                return files;

            for (const auto& entry : fs::recursive_directory_iterator(dir, ec))
            {
                if (!entry.is_regular_file())
                    continue;
                if (!extensionFilter.empty())
                {
                    if (entry.path().extension().string() != extensionFilter)
                        continue;
                }
                files.push_back(entry.path().string());
            }
            return files;
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
