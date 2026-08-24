/** @file TFAssetPaths.h @brief TerraFront content-root-confined render asset resolution. */
#pragma once

#include "Graphics/ProjectAssetPath.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Terrafront
{
    inline std::optional<Spark::ResolvedProjectAssetPath> ResolveContentAssetPath(std::string_view contentRoot,
                                                                                  std::string_view declaredPath)
    {
        if (contentRoot.empty() || declaredPath.empty())
            return std::nullopt;

        std::string confinedPath(declaredPath);
        const std::filesystem::path nativeDeclared = std::filesystem::u8path(confinedPath);
        if (nativeDeclared.is_absolute())
        {
            const std::filesystem::path nativeRoot = std::filesystem::u8path(std::string(contentRoot));
            const std::filesystem::path relative = nativeDeclared.lexically_relative(nativeRoot);
            if (relative.empty())
                return std::nullopt;
            const auto relativeUtf8 = relative.generic_u8string();
            confinedPath.assign(relativeUtf8.begin(), relativeUtf8.end());
        }
        return Spark::ResolveProjectAssetPath(contentRoot, confinedPath);
    }
} // namespace Terrafront
