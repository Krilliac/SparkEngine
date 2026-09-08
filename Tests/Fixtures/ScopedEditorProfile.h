/**
 * @file ScopedEditorProfile.h
 * @brief Test-owned project history, isolated from the user's editor profile.
 */
#pragma once

#include "Core/ProjectManager.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace SparkEditor::Testing
{
    class IsolatedProjectManager final : public ProjectManager
    {
      public:
        IsolatedProjectManager() : IsolatedProjectManager(CreateDirectory()) {}
        const std::filesystem::path& ProfileDirectory() const { return m_directory; }
        ~IsolatedProjectManager()
        {
            std::error_code ec;
            std::filesystem::remove_all(m_directory, ec);
        }

      private:
        explicit IsolatedProjectManager(const std::filesystem::path& directory)
            : ProjectManager(Utf8(directory)), m_directory(directory)
        {
        }

        static std::string Utf8(const std::filesystem::path& path)
        {
            const auto value = path.generic_u8string();
            return {reinterpret_cast<const char*>(value.data()), value.size()};
        }

        static std::filesystem::path CreateDirectory()
        {
            static std::atomic<unsigned int> sequence{0};
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            for (int attempt = 0; attempt < 16; ++attempt)
            {
                const auto path = std::filesystem::temp_directory_path() /
                                  ("spark-test-editor-profile-" + std::to_string(stamp) + "-" +
                                   std::to_string(sequence.fetch_add(1)));
                if (std::filesystem::create_directory(path))
                    return path;
            }
            throw std::runtime_error("Cannot allocate an isolated editor test profile");
        }

        std::filesystem::path m_directory;
    };
}
