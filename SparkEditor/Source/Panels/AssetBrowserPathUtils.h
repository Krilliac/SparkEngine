/** @file AssetBrowserPathUtils.h
 * @brief Internal UTF-8 path conversions shared by the asset browser's file and UI paths.
 */
#pragma once
#include <filesystem>
#include <string>
#include <string_view>
namespace SparkEditor::AssetBrowserDetail
{
    namespace fs = std::filesystem;
    inline std::string PathToUtf8(const fs::path& path) noexcept
    {
        try
        {
            const auto utf8 = path.generic_u8string();
            return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
        }
        catch (...)
        {
            return {};
        }
    }

    inline fs::path PathFromUtf8(std::string_view value) noexcept
    {
        try
        {
            const auto* begin = reinterpret_cast<const char8_t*>(value.data());
            return fs::path(std::u8string(begin, begin + value.size()));
        }
        catch (...)
        {
            return {};
        }
    }

} // namespace SparkEditor::AssetBrowserDetail
