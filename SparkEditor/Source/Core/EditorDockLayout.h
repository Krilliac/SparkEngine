/**
 * @file EditorDockLayout.h
 * @brief Default dock geometry shared by the editor and layout regression tests.
 */
#pragma once

#include <imgui.h>
#include <imgui_internal.h>

namespace SparkEditor
{
    inline constexpr float EditorToolbarButtonSize = 30.0f;
    inline constexpr float EditorToolbarPaddingY = 6.0f;

    /// Default workspace nodes; window bindings remain owned by EditorUI.
    struct EditorDockNodes
    {
        ImGuiID toolbar, left, right, bottom, center;
    };

    /// Preserve an existing saved dock tree unless the user explicitly requests a reset.
    inline bool NeedsDefaultEditorDockLayout(ImGuiID root, bool forceReset)
    {
        const ImGuiDockNode* existing = ImGui::DockBuilderGetNode(root);
        return forceReset || existing == nullptr || existing->IsEmpty();
    }

    /// Build geometry for first launch or explicit Reset Layout, never each frame.
    inline EditorDockNodes BuildEditorDockLayout(ImGuiID root, ImVec2 size)
    {
        ImGui::DockBuilderRemoveNode(root);
        ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(root, size);
        ImGuiID main = root;
        EditorDockNodes nodes{};
        const float toolbarHeight = EditorToolbarButtonSize + 2.0f * EditorToolbarPaddingY;
        const float splitHeight = ImMax(1.0f, size.y - ImGui::GetStyle().DockingSeparatorSize);
        ImGui::DockBuilderSplitNode(main, ImGuiDir_Up, ImClamp(toolbarHeight / splitHeight, 0.0f, 0.95f),
                                    &nodes.toolbar, &main);
        if (auto* toolbar = ImGui::DockBuilderGetNode(nodes.toolbar))
        {
            // This is an action strip, not a document tab. Keep the workspace
            // below it dockable without reserving an empty title/tab header.
            toolbar->LocalFlags |= ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoResizeY;
            toolbar->UpdateMergedFlags();
        }
        ImGui::DockBuilderSplitNode(main, ImGuiDir_Left, 0.18f, &nodes.left, &main);
        ImGui::DockBuilderSplitNode(main, ImGuiDir_Right, 0.22f, &nodes.right, &main);
        ImGui::DockBuilderSplitNode(main, ImGuiDir_Down, 0.25f, &nodes.bottom, &nodes.center);
        return nodes;
    }
} // namespace SparkEditor
