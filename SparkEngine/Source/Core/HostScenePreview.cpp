/**
 * @file HostScenePreview.cpp
 * @brief Platform-neutral `-scene <path>` load and machine-readable launch records.
 */

#include "HostScenePreview.h"

#include "Engine/ECS/Components.h"
#include "Graphics/ProjectAssetPath.h"
#include "SceneManager/ReflectedSceneSerializer.h"

#include <cstdio>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace Spark
{
    namespace
    {
        constexpr std::string_view kProjectAssetPrefix = "Assets/";

        /// Count one reference when it is project-relative; report whether it resolves to a file.
        void AuditAssetReference(const std::string& reference, HostSceneLoadReport& report)
        {
            if (reference.empty() || !std::string_view(reference).starts_with(kProjectAssetPrefix))
                return;

            ++report.assetRefs;
            if (report.projectRoot.empty())
            {
                ++report.assetsMissing;
                return;
            }

            const auto resolved = ResolveProjectAssetPath(report.projectRoot, reference);
            std::error_code ec;
            if (!resolved || !std::filesystem::is_regular_file(resolved->nativePath, ec) || ec)
                ++report.assetsMissing;
        }
    } // namespace

    bool LoadHostScene(::World& world, const std::string& scenePath, HostSceneLoadReport& report)
    {
        report = HostSceneLoadReport{};
        if (scenePath.empty())
        {
            report.error = "-scene requires a non-empty scene path";
            return false;
        }

        std::string loadError;
        if (!LoadWorld(world, scenePath, &loadError))
        {
            report.error = loadError.empty() ? "scene could not be loaded" : loadError;
            return false;
        }

        if (const auto root = DeriveProjectRootFromScenePath(scenePath))
            report.projectRoot = *root;

        report.entities = world.GetEntityCount();
        for (auto entity : world.GetEntitiesWith<::Transform, ::MeshRenderer>())
        {
            ++report.renderables;
            if (const ::MeshRenderer* renderer = world.GetComponent<::MeshRenderer>(entity))
            {
                AuditAssetReference(renderer->meshPath, report);
                AuditAssetReference(renderer->materialPath, report);
            }
        }
        return true;
    }

    void PrintHostSceneRecords(const HostSceneLoadReport& report)
    {
        // Two separate lines: launch checks anchor a regex on the exact
        // SPARK_SCENE_LOADED form, so the asset audit must not extend it.
        std::fprintf(stdout, "SPARK_SCENE_LOADED entities=%zu renderables=%zu\n", report.entities, report.renderables);
        std::fprintf(stdout, "SPARK_SCENE_ASSETS refs=%zu missing=%zu\n", report.assetRefs, report.assetsMissing);
        std::fflush(stdout);
    }
} // namespace Spark
