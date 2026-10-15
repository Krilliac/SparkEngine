/**
 * @file EditorDockLayout.h
 * @brief Default dock geometry shared by the editor and layout regression tests.
 *
 * Thread affinity: main (ImGui) thread only; the builders mutate the current context's dock tree.
 */
#pragma once

#include <imgui.h>
#include <imgui_internal.h>

namespace SparkEditor
{
    inline constexpr float EditorToolbarButtonSize = 30.0f;
    inline constexpr float EditorToolbarPaddingY = 6.0f;

    /// Height of the toolbar strip at an effective UI scale: floor((button + 2 x padding) x scale).
    /// Non-finite or non-positive scales count as 1. The builder and the per-frame fit share it.
    inline float EditorToolbarHeight(float scale)
    {
        const float saneScale = (scale > 0.0f && scale < 1.0e6f) ? scale : 1.0f;
        return ImFloor((EditorToolbarButtonSize + 2.0f * EditorToolbarPaddingY) * saneScale);
    }

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
    /// @param scale Effective UI scale (Spark::Ui::Scale()); the toolbar strip grows with it so
    ///              scaled toolbar buttons are not clipped. Non-finite or non-positive means 1.
    inline EditorDockNodes BuildEditorDockLayout(ImGuiID root, ImVec2 size, float scale)
    {
        ImGui::DockBuilderRemoveNode(root);
        ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(root, size);
        ImGuiID main = root;
        EditorDockNodes nodes{};
        const float toolbarHeight = EditorToolbarHeight(scale);
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

    /// Unscaled form (100 %) kept for existing callers and layout regression tests.
    inline EditorDockNodes BuildEditorDockLayout(ImGuiID root, ImVec2 size)
    {
        return BuildEditorDockLayout(root, size, 1.0f);
    }

    /// Keep the toolbar strip as tall as the toolbar at @p scale after the dock tree was built (or
    /// restored from imgui.ini or a saved layout) at another scale. Only the fixed strip the default
    /// layout creates is touched: a node with NoResizeY at the top of a vertical split. The height is
    /// set the way ImGui resizes a node from its splitter (size locked once), so the sibling below takes
    /// the remainder at the next dock tree update. Cheap enough to call every frame.
    /// @return True when the node height changed.
    inline bool FitEditorToolbarNode(ImGuiDockNode* toolbar, float scale)
    {
        if (toolbar == nullptr || toolbar->ParentNode == nullptr ||
            (toolbar->LocalFlags & ImGuiDockNodeFlags_NoResizeY) == 0 ||
            toolbar->ParentNode->SplitAxis != ImGuiAxis_Y || toolbar->ParentNode->ChildNodes[0] != toolbar)
        {
            return false;
        }
        const float height = EditorToolbarHeight(scale);
        // A collapsed or tiny workspace has no room to honour the height; ImGui's locked-size split
        // requires both children to keep a positive size.
        const float room = toolbar->ParentNode->Size.y - ImGui::GetStyle().DockingSeparatorSize;
        if (ImFabs(toolbar->Size.y - height) < 1.0f || room <= height + 1.0f)
        {
            return false;
        }
        toolbar->Size.y = height;
        toolbar->SizeRef.y = height;
        toolbar->WantLockSizeOnce = true;
        return true;
    }
} // namespace SparkEditor
