/**
 * @file InspectorWorldAssetDrop.cpp
 * @brief Asset drag/drop onto World-backed Inspector fields (EDT-210).
 */

#include "InspectorWorldAssetDrop.h"

#include "Engine/ECS/Components/CoreComponents.h"

#include <array>

namespace SparkEditor
{
    namespace
    {
        // Alias the global ECS type: `std::string ::MeshRenderer::*` would be
        // parsed as the nested-name `std::string::MeshRenderer`.
        using EcsMeshRenderer = ::MeshRenderer;

        /// One reflected field that accepts an asset drop.
        struct WorldAssetField
        {
            std::string_view component;
            std::string_view field;
            EditorAssetKind kind;
            std::string EcsMeshRenderer::*member;
            const char* undoLabel;
        };

        constexpr std::array<WorldAssetField, 2> kWorldAssetFields = {{
            {"MeshRenderer", "meshPath", EditorAssetKind::Mesh, &::MeshRenderer::meshPath, "Assign Mesh Asset"},
            {"MeshRenderer", "materialPath", EditorAssetKind::Material, &::MeshRenderer::materialPath,
             "Assign Material Asset"},
        }};

        const WorldAssetField* FindWorldAssetField(std::string_view componentType, std::string_view fieldName)
        {
            for (const WorldAssetField& entry : kWorldAssetFields)
            {
                if (entry.component == componentType && entry.field == fieldName)
                {
                    return &entry;
                }
            }
            return nullptr;
        }
    } // namespace

    std::optional<EditorAssetKind> AssetKindForField(std::string_view componentType, std::string_view fieldName)
    {
        if (const WorldAssetField* entry = FindWorldAssetField(componentType, fieldName))
        {
            return entry->kind;
        }
        return std::nullopt;
    }

    AssetDropResult ApplyWorldAssetDrop(::World& world, ::EntityID entity, std::string_view componentType,
                                        std::string_view fieldName, const std::string& reference,
                                        const std::function<std::string()>& snapshot,
                                        const InspectorPendingWorldEdit::CommitFn& commit)
    {
        const WorldAssetField* entry = FindWorldAssetField(componentType, fieldName);
        if (entry == nullptr || !IsValidEditorAssetReference(reference, entry->kind))
        {
            return AssetDropResult::Rejected;
        }

        entt::registry& registry = world.GetRegistry();
        if (entity == entt::null || !registry.valid(entity))
        {
            return AssetDropResult::NoComponent;
        }
        auto* renderer = registry.try_get<::MeshRenderer>(entity);
        if (renderer == nullptr)
        {
            return AssetDropResult::NoComponent;
        }

        std::string& slot = renderer->*(entry->member);
        if (slot == reference)
        {
            return AssetDropResult::Unchanged;
        }

        const std::string before = snapshot ? snapshot() : std::string{};
        if (before.empty() || !commit)
        {
            return AssetDropResult::Rejected;
        }

        std::string previous = slot;
        const bool previousDirty = renderer->worldMatrixDirty;
        slot = reference;
        renderer->worldMatrixDirty = true;
        if (commit(before, entry->undoLabel))
        {
            return AssetDropResult::Applied;
        }

        // Nothing was recorded, so the registry was not replaced and the
        // component pointer is still live: undo the write in place.
        slot = std::move(previous);
        renderer->worldMatrixDirty = previousDirty;
        return AssetDropResult::Rejected;
    }
} // namespace SparkEditor
