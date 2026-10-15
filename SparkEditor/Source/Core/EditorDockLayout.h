/**
 * @file EditorDockLayout.h
 * @brief Default dock geometry shared by the editor and layout regression tests.
 *
 * The default tree is a fixed toolbar strip on top of a workspace split into Hierarchy (left), Inspector (right),
 * Console/Asset Browser (bottom) and the centre documents. Two rules keep it legible at every UI scale:
 *
 * - **Toolbar strip.** Its height is always the toolbar content height at the current scale and it never shows a tab
 *   bar. Both are re-applied every frame to whichever node hosts the toolbar window, because a tree restored from
 *   imgui.ini or a saved layout was built at another scale, and Dear ImGui does not persist NoResizeY.
 * - **Side and bottom docks.** A fresh tree uses the default ratios (Hierarchy 0.18, Inspector 0.22, bottom 0.25),
 *   raised just enough that each dock gets its scaled minimum extent (220 / 280 / 160 logical px), capped at a share
 *   of the space being split so the centre always keeps room. An existing tree is only ever grown: each frame, a
 *   dock narrower than that scaled minimum is widened to it, and a dock the user made larger is left exactly as is.
 *
 * Thread affinity: main (ImGui) thread only; the builders and fits mutate the current context's dock tree.
 * Ownership: no state; every function works on nodes owned by the ImGui docking context.
 * Allocation: none beyond what Dear ImGui's DockBuilder allocates when a tree is (re)built.
 */
#pragma once

#include <imgui.h>
#include <imgui_internal.h>

namespace SparkEditor
{
    inline constexpr float EditorToolbarButtonSize = 30.0f;
    inline constexpr float EditorToolbarPaddingY = 6.0f;

    /// Window the editor toolbar is submitted as; the per-frame fit finds the toolbar strip through it.
    inline constexpr const char* EditorToolbarWindowName = "##Toolbar";

    /// Dock flags the toolbar strip always carries: an action strip, not a document tab, and not a drop target.
    inline constexpr ImGuiDockNodeFlags EditorToolbarNodeFlags =
        ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoResizeY | ImGuiDockNodeFlags_NoDockingOverMe |
        ImGuiDockNodeFlags_NoDockingSplit;

    /// Sizing rule of one default dock, in logical px at scale 1.
    struct EditorDockExtent
    {
        float defaultRatio; ///< Share of the split space a fresh tree gives the dock at 100 %
        float minExtent;    ///< Logical px the dock needs to stay legible; multiplied by the UI scale
        float maxRatio;     ///< Upper share of the split space, so the centre documents keep room
    };

    /// Hierarchy column: a share of the whole dock width.
    inline constexpr EditorDockExtent EditorLeftDockExtent{0.18f, 220.0f, 0.3f};
    /// Inspector column: a share of the width left beside the Hierarchy column.
    inline constexpr EditorDockExtent EditorRightDockExtent{0.22f, 280.0f, 0.4f};
    /// Console / Asset Browser row: a share of the height under the toolbar.
    inline constexpr EditorDockExtent EditorBottomDockExtent{0.25f, 160.0f, 0.4f};

    /// Non-finite or non-positive scales count as 1.
    inline float SaneEditorDockScale(float scale)
    {
        return (scale > 0.0f && scale < 1.0e6f) ? scale : 1.0f;
    }

    /// Height of the toolbar strip at an effective UI scale: floor((button + 2 x padding) x scale).
    /// Non-finite or non-positive scales count as 1. The builder and the per-frame fit share it.
    inline float EditorToolbarHeight(float scale)
    {
        return ImFloor((EditorToolbarButtonSize + 2.0f * EditorToolbarPaddingY) * SaneEditorDockScale(scale));
    }

    /// Split ratio a fresh tree gives @p extent across @p available px at @p scale: the default ratio, raised until
    /// the dock is minExtent x scale px, never above maxRatio. Non-positive @p available keeps the default ratio.
    inline float EditorDockSplitRatio(const EditorDockExtent& extent, float available, float scale)
    {
        if (!(available > 0.0f))
            return extent.defaultRatio;
        const float needed = extent.minExtent * SaneEditorDockScale(scale) / available;
        return ImClamp(needed, extent.defaultRatio, extent.maxRatio);
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
    /// @param scale Effective UI scale (Spark::Ui::Scale()); the toolbar strip and the scaled minimum extents of the
    ///              side and bottom docks grow with it. Non-finite or non-positive means 1.
    inline EditorDockNodes BuildEditorDockLayout(ImGuiID root, ImVec2 size, float scale)
    {
        ImGui::DockBuilderRemoveNode(root);
        ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(root, size);
        ImGuiID main = root;
        EditorDockNodes nodes{};
        const float separator = ImGui::GetStyle().DockingSeparatorSize;
        const float toolbarHeight = EditorToolbarHeight(scale);
        const float splitHeight = ImMax(1.0f, size.y - separator);
        ImGui::DockBuilderSplitNode(main, ImGuiDir_Up, ImClamp(toolbarHeight / splitHeight, 0.0f, 0.95f),
                                    &nodes.toolbar, &main);
        if (auto* toolbar = ImGui::DockBuilderGetNode(nodes.toolbar))
        {
            // This is an action strip, not a document tab. Keep the workspace
            // below it dockable without reserving an empty title/tab header.
            toolbar->LocalFlags |= EditorToolbarNodeFlags;
            toolbar->UpdateMergedFlags();
        }

        // Each split measures the node it divides, so the Inspector's share is of the width beside the Hierarchy.
        const auto splitSpace = [separator](ImGuiID id, ImGuiAxis axis)
        {
            const ImGuiDockNode* node = ImGui::DockBuilderGetNode(id);
            return node != nullptr ? node->Size[axis] - separator : 0.0f;
        };
        ImGui::DockBuilderSplitNode(main, ImGuiDir_Left,
                                    EditorDockSplitRatio(EditorLeftDockExtent, splitSpace(main, ImGuiAxis_X), scale),
                                    &nodes.left, &main);
        ImGui::DockBuilderSplitNode(main, ImGuiDir_Right,
                                    EditorDockSplitRatio(EditorRightDockExtent, splitSpace(main, ImGuiAxis_X), scale),
                                    &nodes.right, &main);
        ImGui::DockBuilderSplitNode(main, ImGuiDir_Down,
                                    EditorDockSplitRatio(EditorBottomDockExtent, splitSpace(main, ImGuiAxis_Y), scale),
                                    &nodes.bottom, &nodes.center);
        return nodes;
    }

    /// Unscaled form (100 %) kept for existing callers and layout regression tests.
    inline EditorDockNodes BuildEditorDockLayout(ImGuiID root, ImVec2 size)
    {
        return BuildEditorDockLayout(root, size, 1.0f);
    }

    /// Request @p extent px for @p node along its parent's split axis, the way ImGui resizes a node from its splitter
    /// (size locked once): the sibling takes the remainder at the next dock tree update.
    inline void LockEditorDockNodeExtent(ImGuiDockNode* node, ImGuiAxis axis, float extent)
    {
        node->Size[axis] = extent;
        node->SizeRef[axis] = extent;
        node->WantLockSizeOnce = true;
    }

    /// Keep the node hosting the toolbar window a fixed strip as tall as the toolbar at @p scale, whether the tree
    /// was built at another scale or restored from imgui.ini / a saved layout (where NoResizeY is not persisted and
    /// older files lack NoTabBar). The node is recognised by position: the top child of a vertical split. Its strip
    /// flags (EditorToolbarNodeFlags) are re-applied every call. Cheap enough to call every frame, right after
    /// ImGui::DockSpace() has sized the tree.
    /// @param toolbar The toolbar window's DockNode (null when the toolbar is not docked).
    /// @return True when the node height changed.
    inline bool FitEditorToolbarNode(ImGuiDockNode* toolbar, float scale)
    {
        if (toolbar == nullptr || toolbar->ParentNode == nullptr || toolbar->ParentNode->SplitAxis != ImGuiAxis_Y ||
            toolbar->ParentNode->ChildNodes[0] != toolbar || !toolbar->IsLeafNode())
        {
            return false;
        }
        if ((toolbar->LocalFlags & EditorToolbarNodeFlags) != EditorToolbarNodeFlags)
        {
            toolbar->LocalFlags |= EditorToolbarNodeFlags;
            toolbar->UpdateMergedFlags();
        }

        const float height = EditorToolbarHeight(scale);
        // A collapsed or tiny workspace has no room to honour the height; ImGui's locked-size split
        // requires both children to keep a positive size.
        const float room = toolbar->ParentNode->Size.y - ImGui::GetStyle().DockingSeparatorSize;
        if (ImFabs(toolbar->Size.y - height) < 1.0f || room <= height + 1.0f)
        {
            return false;
        }
        LockEditorDockNodeExtent(toolbar, ImGuiAxis_Y, height);
        return true;
    }

    /// Grow-only scaled minimum for the dock that holds @p leaf: walks up to the nearest split along @p axis and, when
    /// the child on that path is smaller than min(extent.minExtent x scale, extent.maxRatio x split space), widens it
    /// to that size. A dock at or above the minimum (for example one the user dragged wider) is never touched, so a
    /// saved layout keeps its proportions apart from docks too narrow for the current scale. Call every frame right
    /// after ImGui::DockSpace(); a resize lands at the next dock tree update.
    /// @param leaf The DockNode of a panel window (null when the panel is floating or closed).
    /// @return True when a node was resized.
    inline bool FitEditorDockExtent(ImGuiDockNode* leaf, ImGuiAxis axis, const EditorDockExtent& extent, float scale)
    {
        ImGuiDockNode* cell = leaf;
        while (cell != nullptr && cell->ParentNode != nullptr && cell->ParentNode->SplitAxis != axis)
            cell = cell->ParentNode;
        if (cell == nullptr || cell->ParentNode == nullptr)
            return false;

        ImGuiDockNode* parent = cell->ParentNode;
        ImGuiDockNode* sibling = parent->ChildNodes[parent->ChildNodes[0] == cell ? 1 : 0];
        if (sibling == nullptr || !cell->IsVisible || !sibling->IsVisible)
            return false;

        const float available = parent->Size[axis] - ImGui::GetStyle().DockingSeparatorSize;
        if (available <= 2.0f)
            return false;
        const float target = ImFloor(ImMin(extent.minExtent * SaneEditorDockScale(scale), available * extent.maxRatio));
        if (cell->Size[axis] + 0.5f >= target)
            return false;
        LockEditorDockNodeExtent(cell, axis, target);
        return true;
    }

    /// DockNode of the window named @p windowName when it is docked and was submitted last frame, else null.
    /// Window::DockIsActive is deliberately not consulted: DockSpace() clears it for every window that is alone in its
    /// node until that window's next Begin(), and the fit runs right after DockSpace().
    inline ImGuiDockNode* FindEditorDockedWindowNode(const char* windowName)
    {
        ImGuiWindow* window = ImGui::FindWindowByName(windowName);
        return (window != nullptr && window->WasActive && window->DockNode != nullptr) ? window->DockNode : nullptr;
    }

    /// Window names (the "###id" part is enough) of the panels the default side and bottom docks hold.
    struct EditorDockPanelWindows
    {
        const char* hierarchy;
        const char* inspector;
        const char* console;
    };

    /// Per-frame upkeep of the dock tree at @p scale: the toolbar strip height and flags (FitEditorToolbarNode) and
    /// the grow-only minimum extents of the Hierarchy, Inspector and Console docks (FitEditorDockExtent). Call right
    /// after ImGui::DockSpace(). Null window names are skipped.
    inline void FitEditorDockTree(const EditorDockPanelWindows& panels, float scale)
    {
        if (ImGuiWindow* toolbar = ImGui::FindWindowByName(EditorToolbarWindowName))
            FitEditorToolbarNode(toolbar->DockNode, scale);
        if (panels.hierarchy != nullptr)
            FitEditorDockExtent(FindEditorDockedWindowNode(panels.hierarchy), ImGuiAxis_X, EditorLeftDockExtent, scale);
        if (panels.inspector != nullptr)
            FitEditorDockExtent(FindEditorDockedWindowNode(panels.inspector), ImGuiAxis_X, EditorRightDockExtent,
                                scale);
        if (panels.console != nullptr)
            FitEditorDockExtent(FindEditorDockedWindowNode(panels.console), ImGuiAxis_Y, EditorBottomDockExtent, scale);
    }
} // namespace SparkEditor
