/**
 * @file HierarchyPanel.h
 * @brief Scene hierarchy panel for the Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file defines the hierarchy panel that displays the scene graph
 * in a tree structure, allowing for object selection, manipulation,
 * and hierarchical organization.
 */

#pragma once

#include "../Core/EditorPanel.h"
#include "../SceneSystem/SceneFile.h"
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace SparkEditor
{

    /**
 * @brief Scene hierarchy panel
 * 
 * Displays the scene graph as a hierarchical tree, allowing users to:
 * - Browse all objects in the scene
 * - Select single or multiple objects
 * - Drag and drop to reorganize hierarchy
 * - Create, delete, and duplicate objects
 * - Show/hide objects and components
 * - Search and filter objects
 * 
 * This panel is inspired by Unity's Hierarchy window and Unreal's World Outliner.
 */
    class HierarchyPanel : public EditorPanel
    {
      public:
        /**
     * @brief Constructor
     */
        HierarchyPanel();

        /**
     * @brief Destructor
     */
        ~HierarchyPanel() override;

        /**
     * @brief Initialize the hierarchy panel
     * @return true if initialization succeeded
     */
        bool Initialize() override;

        /**
     * @brief Update panel for the current frame
     * @param deltaTime Time elapsed since last frame
     */
        void Update(float deltaTime) override;

        /**
     * @brief Render the hierarchy panel UI
     */
        void Render() override;

        /**
     * @brief Shutdown the panel
     */
        void Shutdown() override;

        /**
     * @brief Handle panel-specific events
     * @param eventType Type of event
     * @param eventData Event data
     * @return true if event was handled
     */
        bool HandleEvent(const std::string& eventType, void* eventData) override;

        /**
     * @brief Set the scene to display
     * @param scene Scene data to display in hierarchy
     */
        void SetScene(SceneFile* scene);

        /** @brief Create a new object in the scene hierarchy. */
        ObjectID CreateObject(const std::string& name, ObjectID parentID = INVALID_OBJECT_ID);

        /** @brief Reset hierarchy to default scene objects (Camera, Light, Ground). */
        void ResetToDefault();

        /** @brief Get all scene object names (for serialization compatibility). */
        std::vector<std::string> GetSceneObjects() const;

        /**
     * @brief Get currently selected objects
     * @return Vector of selected object IDs
     */
        const std::vector<ObjectID>& GetSelectedObjects() const { return m_selectedObjects; }

        /**
     * @brief Set selected objects
     * @param objectIDs Vector of object IDs to select
     */
        void SetSelectedObjects(const std::vector<ObjectID>& objectIDs);

        /**
     * @brief Select a single object
     * @param objectID Object ID to select
     * @param addToSelection Whether to add to current selection or replace it
     */
        void SelectObject(ObjectID objectID, bool addToSelection = false);

        /**
     * @brief Clear selection
     */
        void ClearSelection();

        /**
     * @brief Check if an object is selected
     * @param objectID Object ID to check
     * @return true if object is selected
     */
        bool IsObjectSelected(ObjectID objectID) const;

        /**
     * @brief Expand object to show its children
     * @param objectID Object to expand
     */
        void ExpandObject(ObjectID objectID);

        /**
     * @brief Collapse object to hide its children
     * @param objectID Object to collapse
     */
        void CollapseObject(ObjectID objectID);

        /**
     * @brief Check if object is expanded
     * @param objectID Object to check
     * @return true if object is expanded
     */
        bool IsObjectExpanded(ObjectID objectID) const;

        /**
     * @brief Focus on object (scroll to make it visible)
     * @param objectID Object to focus on
     */
        void FocusObject(ObjectID objectID);

        /**
     * @brief Register callback for selection changes
     * @param callback Function to call when selection changes
     */
        void RegisterSelectionCallback(std::function<void(const std::vector<ObjectID>&)> callback);

        /**
     * @brief Register callback for object operations
     * @param callback Function to call for create/delete/duplicate operations
     */
        void RegisterObjectOperationCallback(std::function<void(const std::string&, ObjectID)> callback);

        /**
     * @brief Set search filter
     * @param searchText Text to filter objects by (name/tag)
     */
        void SetSearchFilter(const std::string& searchText);

        /**
     * @brief Set whether to show inactive objects
     * @param show Whether to show inactive objects
     */
        void SetShowInactiveObjects(bool show) { m_showInactiveObjects = show; }

        /**
     * @brief Set whether to show object IDs
     * @param show Whether to show object IDs in hierarchy
     */
        void SetShowObjectIDs(bool show) { m_showObjectIDs = show; }

      private:
        /**
     * @brief Render the toolbar at the top of the panel
     */
        void RenderToolbar();

        /**
     * @brief Render the search bar
     */
        void RenderSearchBar();

        /**
     * @brief Render the main hierarchy tree
     */
        void RenderHierarchyTree();

        /**
     * @brief Render a single object in the tree
     * @param object Object to render
     * @param depth Current depth in hierarchy (for indentation)
     */
        void RenderObjectNode(SceneObject* object, int depth = 0);

        /**
     * @brief Render object context menu
     * @param object Object for context menu
     */
        void RenderObjectContextMenu(SceneObject* object);

        /**
     * @brief Render empty space context menu
     */
        void RenderEmptyContextMenu();

        /**
     * @brief Handle object drag and drop
     * @param draggedObject Object being dragged
     * @param targetObject Object being dropped onto (nullptr for root)
     * @return true if drop was handled
     */
        bool HandleObjectDragDrop(SceneObject* draggedObject, SceneObject* targetObject);

        /**
     * @brief Get object icon based on components
     * @param object Object to get icon for
     * @return Icon character or string
     */
        const char* GetObjectIcon(const SceneObject* object) const;

        /**
     * @brief Get child objects of a parent
     * @param parentID Parent object ID (INVALID_OBJECT_ID for root objects)
     * @return Vector of child objects
     */
        std::vector<SceneObject*> GetChildObjects(ObjectID parentID) const;

        /**
     * @brief Filter objects based on search criteria
     * @param object Object to check
     * @return true if object matches filter
     */
        bool PassesFilter(const SceneObject* object) const;

        /**
     * @brief Check if object or any descendant matches filter
     * @param object Object to check
     * @return true if object or any descendant matches
     */
        bool ObjectOrDescendantPassesFilter(const SceneObject* object) const;

        /**
     * @brief Duplicate object
     * @param objectID Object to duplicate
     * @return ID of duplicated object
     */
        ObjectID DuplicateObject(ObjectID objectID);

        /**
     * @brief Delete object and all children
     * @param objectID Object to delete
     */
        void DeleteObject(ObjectID objectID);

        /**
     * @brief Rename object
     * @param objectID Object to rename
     * @param newName New object name
     */
        void RenameObject(ObjectID objectID, const std::string& newName);

        /**
     * @brief Move object to new parent
     * @param objectID Object to move
     * @param newParentID New parent ID (INVALID_OBJECT_ID for root)
     */
        void MoveObject(ObjectID objectID, ObjectID newParentID);

        /**
     * @brief Update selection state
     */
        void UpdateSelection();

        /**
     * @brief Notify selection changed
     */
        void NotifySelectionChanged();

        /**
     * @brief Notify object operation occurred
     * @param operation Operation type ("create", "delete", "duplicate", etc.)
     * @param objectID Object involved in operation
     */
        void NotifyObjectOperation(const std::string& operation, ObjectID objectID);

      private:
        // Scene data
        std::unique_ptr<SceneFile> m_ownedScene; ///< Internally owned scene (used if no external scene set)
        SceneFile* m_scene = nullptr;            ///< Current scene being displayed

        // Selection state
        std::vector<ObjectID> m_selectedObjects;          ///< Currently selected objects
        std::unordered_set<ObjectID> m_selectedSet;       ///< For fast lookup
        ObjectID m_lastClickedObject = INVALID_OBJECT_ID; ///< Last clicked object for range selection

        // Expansion state
        std::unordered_set<ObjectID> m_expandedObjects; ///< Expanded objects

        // Search and filtering
        std::string m_searchFilter;        ///< Current search filter
        char m_searchBuffer[256] = {};     ///< Search input buffer
        bool m_showInactiveObjects = true; ///< Show inactive objects
        bool m_showObjectIDs = false;      ///< Show object IDs

        // Drag and drop state
        ObjectID m_draggedObject = INVALID_OBJECT_ID; ///< Object being dragged
        bool m_isDragging = false;                    ///< Whether drag operation is active

        // Context menu state
        ObjectID m_contextMenuObject = INVALID_OBJECT_ID; ///< Object for context menu
        bool m_showObjectContextMenu = false;             ///< Show object context menu
        bool m_showEmptyContextMenu = false;              ///< Show empty space context menu

        // Rename state
        ObjectID m_renamingObject = INVALID_OBJECT_ID; ///< Object being renamed
        char m_renameBuffer[256] = {};                 ///< Rename input buffer

        // Callbacks
        std::function<void(const std::vector<ObjectID>&)> m_selectionCallback;       ///< Selection change callback
        std::function<void(const std::string&, ObjectID)> m_objectOperationCallback; ///< Object operation callback

        // UI state
        bool m_needsSelectionUpdate = false; ///< Whether selection needs updating
        float m_itemHeight = 20.0f;          ///< Height of each item in pixels

        // Performance optimization
        mutable std::vector<SceneObject*> m_filteredObjects; ///< Cached filtered objects
        mutable bool m_filterCacheDirty = true;              ///< Whether filter cache needs updating
    };

} // namespace SparkEditor