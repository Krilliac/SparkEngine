/**
 * @file HierarchyPanel.cpp
 * @brief Implementation of the Hierarchy panel
 * @author Spark Engine Team
 * @date 2025
 *
 * All scene mutations (create, delete, duplicate, rename, reparent) are
 * routed through CommandHistory so that every operation is undoable.
 */

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>

#include <imgui.h>

#include "HierarchyPanel.h"
#include "../Core/EditorIcons.h"
#include "../CommandHistory.h"
#include "Utils/ContainerUtils.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"

namespace SparkEditor
{

    HierarchyPanel::HierarchyPanel() : EditorPanel("Hierarchy", "hierarchy_panel") {}

    HierarchyPanel::~HierarchyPanel() {}

    bool HierarchyPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        std::cout << "Initializing Hierarchy panel\n";

        // Create a default scene so the panel works immediately
        ResetToDefault();
        return true;
    }

    void HierarchyPanel::Update(float /*deltaTime*/)
    {
        if (m_needsSelectionUpdate)
        {
            UpdateSelection();
            m_needsSelectionUpdate = false;
        }
    }

    void HierarchyPanel::Render()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            RenderToolbar();
            ImGui::Separator();
            RenderSearchBar();
            ImGui::Separator();
            RenderHierarchyTree();
        }
        EndPanel();
    }

    void HierarchyPanel::Shutdown()
    {
        std::cout << "Shutting down Hierarchy panel\n";
    }

    bool HierarchyPanel::HandleEvent(const std::string& /*eventType*/, void* /*eventData*/)
    {
        return false;
    }

    void HierarchyPanel::SetScene(SceneFile* scene)
    {
        m_scene = scene;
        ClearSelection();
        m_filterCacheDirty = true;
    }

    void HierarchyPanel::SetSelectedObjects(const std::vector<ObjectID>& objectIDs)
    {
        m_selectedObjects = objectIDs;
        m_selectedSet.clear();
        for (auto id : objectIDs)
        {
            m_selectedSet.insert(id);
        }
        NotifySelectionChanged();
    }

    void HierarchyPanel::SelectObject(ObjectID objectID, bool addToSelection)
    {
        if (!addToSelection)
        {
            m_selectedObjects.clear();
            m_selectedSet.clear();
        }
        if (!Spark::ContainerUtils::Contains(m_selectedSet, objectID))
        {
            m_selectedObjects.push_back(objectID);
            m_selectedSet.insert(objectID);
        }
        m_lastClickedObject = objectID;
        NotifySelectionChanged();
    }

    void HierarchyPanel::ClearSelection()
    {
        m_selectedObjects.clear();
        m_selectedSet.clear();
        m_lastClickedObject = INVALID_OBJECT_ID;
        NotifySelectionChanged();
    }

    bool HierarchyPanel::IsObjectSelected(ObjectID objectID) const
    {
        return m_selectedSet.count(objectID) > 0;
    }

    void HierarchyPanel::ExpandObject(ObjectID objectID)
    {
        m_expandedObjects.insert(objectID);
    }

    void HierarchyPanel::CollapseObject(ObjectID objectID)
    {
        m_expandedObjects.erase(objectID);
    }

    bool HierarchyPanel::IsObjectExpanded(ObjectID objectID) const
    {
        return m_expandedObjects.count(objectID) > 0;
    }

    void HierarchyPanel::FocusObject(ObjectID objectID)
    {
        // Expand all parents up to the root
        if (!m_scene)
            return;
        SceneObject* obj = m_scene->FindObject(objectID);
        if (!obj)
            return;

        ObjectID parentID = obj->transform.parentID;
        while (parentID != INVALID_OBJECT_ID)
        {
            ExpandObject(parentID);
            SceneObject* parent = m_scene->FindObject(parentID);
            if (!parent)
                break;
            parentID = parent->transform.parentID;
        }

        SelectObject(objectID);
    }

    void HierarchyPanel::RegisterSelectionCallback(std::function<void(const std::vector<ObjectID>&)> callback)
    {
        m_selectionCallback = std::move(callback);
    }

    void HierarchyPanel::RegisterObjectOperationCallback(std::function<void(const std::string&, ObjectID)> callback)
    {
        m_objectOperationCallback = std::move(callback);
    }

    void HierarchyPanel::SetSearchFilter(const std::string& searchText)
    {
        m_searchFilter = searchText;
        m_filterCacheDirty = true;
    }

    // ============================================================================
    // Private Methods
    // ============================================================================

    void HierarchyPanel::RenderToolbar()
    {
        if (ImGui::Button(ICON_FA_PLUS " Create"))
        {
            ImGui::OpenPopup("CreateObject");
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_TRASH " Delete"))
        {
            // Copy selection since deleting will modify it
            auto toDelete = m_selectedObjects;
            for (auto id : toDelete)
            {
                DeleteObject(id);
            }
        }
        ImGui::SameLine();

        // Undo/Redo buttons
        auto& history = Spark::Editor::CommandHistory::GetInstance();
        bool canUndo = history.CanUndo();
        bool canRedo = history.CanRedo();

        if (!canUndo)
            ImGui::BeginDisabled();
        if (ImGui::Button(ICON_FA_UNDO))
        {
            history.Undo();
            m_needsSelectionUpdate = true;
            m_filterCacheDirty = true;
        }
        if (ImGui::IsItemHovered() && canUndo)
        {
            ImGui::SetTooltip("Undo: %s", history.GetUndoDescription().c_str());
        }
        if (!canUndo)
            ImGui::EndDisabled();

        ImGui::SameLine();

        if (!canRedo)
            ImGui::BeginDisabled();
        if (ImGui::Button(ICON_FA_REDO))
        {
            history.Redo();
            m_needsSelectionUpdate = true;
            m_filterCacheDirty = true;
        }
        if (ImGui::IsItemHovered() && canRedo)
        {
            ImGui::SetTooltip("Redo: %s", history.GetRedoDescription().c_str());
        }
        if (!canRedo)
            ImGui::EndDisabled();

        if (ImGui::BeginPopup("CreateObject"))
        {
            if (ImGui::MenuItem(ICON_FA_CUBE " Empty GameObject"))
            {
                CreateObject("Empty");
            }
            if (ImGui::BeginMenu(ICON_FA_CUBE " 3D Object"))
            {
                if (ImGui::MenuItem("Cube"))
                    CreateObject("Cube");
                if (ImGui::MenuItem("Sphere"))
                    CreateObject("Sphere");
                if (ImGui::MenuItem("Plane"))
                    CreateObject("Plane");
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(ICON_FA_LIGHTBULB " Light"))
            {
                if (ImGui::MenuItem("Directional Light"))
                    CreateObject("Directional Light");
                if (ImGui::MenuItem("Point Light"))
                    CreateObject("Point Light");
                if (ImGui::MenuItem("Spot Light"))
                    CreateObject("Spot Light");
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem(ICON_FA_CAMERA " Camera"))
            {
                CreateObject("Camera");
            }
            ImGui::EndPopup();
        }
    }

    void HierarchyPanel::RenderSearchBar()
    {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##HierarchySearch", ICON_FA_SEARCH " Search objects...", m_searchBuffer,
                                     sizeof(m_searchBuffer)))
        {
            SetSearchFilter(m_searchBuffer);
        }
    }

    void HierarchyPanel::RenderHierarchyTree()
    {
        ImGui::BeginChild("##HierarchyTree");

        if (!m_scene)
        {
            ImGui::Text("No scene loaded");
            ImGui::EndChild();
            return;
        }

        // Keyboard shortcuts: Ctrl+Z / Ctrl+Y
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
        {
            auto& history = Spark::Editor::CommandHistory::GetInstance();
            if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z))
            {
                if (ImGui::GetIO().KeyShift)
                    history.Redo();
                else
                    history.Undo();
                m_needsSelectionUpdate = true;
                m_filterCacheDirty = true;
            }
            if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y))
            {
                history.Redo();
                m_needsSelectionUpdate = true;
                m_filterCacheDirty = true;
            }
            // Delete key
            if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !m_selectedObjects.empty())
            {
                auto toDelete = m_selectedObjects;
                for (auto id : toDelete)
                {
                    DeleteObject(id);
                }
            }
            // F2 to rename
            if (ImGui::IsKeyPressed(ImGuiKey_F2) && m_selectedObjects.size() == 1)
            {
                m_renamingObject = m_selectedObjects[0];
                SceneObject* obj = m_scene->FindObject(m_renamingObject);
                if (obj)
                {
                    strncpy(m_renameBuffer, obj->name.c_str(), sizeof(m_renameBuffer) - 1);
                    m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
                }
            }
            // Ctrl+D to duplicate
            if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D))
            {
                auto toDuplicate = m_selectedObjects;
                for (auto id : toDuplicate)
                {
                    DuplicateObject(id);
                }
            }
        }

        // Render root objects
        auto rootObjects = GetChildObjects(INVALID_OBJECT_ID);
        for (auto* obj : rootObjects)
        {
            if (!m_searchFilter.empty() && !ObjectOrDescendantPassesFilter(obj))
                continue;
            RenderObjectNode(obj, 0);
        }

        // Drop on empty space to unparent (make root)
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_OBJECT"))
            {
                ObjectID draggedID = *static_cast<const ObjectID*>(payload->Data);
                SceneObject* draggedObj = m_scene->FindObject(draggedID);
                if (draggedObj && draggedObj->transform.parentID != INVALID_OBJECT_ID)
                {
                    MoveObject(draggedID, INVALID_OBJECT_ID);
                }
            }
            ImGui::EndDragDropTarget();
        }

        // Empty space context menu
        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !ImGui::IsAnyItemHovered())
        {
            m_showEmptyContextMenu = true;
        }
        if (m_showEmptyContextMenu)
        {
            ImGui::OpenPopup("EmptyContextMenu");
            m_showEmptyContextMenu = false;
        }
        RenderEmptyContextMenu();

        ImGui::EndChild();
    }

    void HierarchyPanel::RenderObjectNode(SceneObject* object, int depth)
    {
        if (!object)
            return;
        if (!m_showInactiveObjects && !object->active)
            return;

        auto children = GetChildObjects(object->id);
        bool hasChildren = !children.empty();

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
        if (!hasChildren)
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (IsObjectSelected(object->id))
            flags |= ImGuiTreeNodeFlags_Selected;

        const char* icon = GetObjectIcon(object);

        ImGui::PushID(static_cast<int>(object->id));

        // Inline rename mode
        if (m_renamingObject == object->id)
        {
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            ImGui::SetKeyboardFocusHere();
            if (ImGui::InputText("##Rename", m_renameBuffer, sizeof(m_renameBuffer),
                                 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
            {
                std::string newName = m_renameBuffer;
                if (!newName.empty())
                {
                    RenameObject(m_renamingObject, newName);
                }
                m_renamingObject = INVALID_OBJECT_ID;
            }
            // Cancel on Escape or losing focus
            if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
                m_renamingObject = INVALID_OBJECT_ID;
            }
            if (!ImGui::IsItemActive() && m_renamingObject == object->id)
            {
                // The input just lost focus without Enter -- commit
                std::string newName = m_renameBuffer;
                if (!newName.empty() && newName != object->name)
                {
                    RenameObject(m_renamingObject, newName);
                }
                m_renamingObject = INVALID_OBJECT_ID;
            }
        }
        else
        {
            // Grey out inactive objects
            if (!object->active)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
            }

            bool nodeOpen;
            if (m_showObjectIDs)
            {
                nodeOpen = ImGui::TreeNodeEx("##node", flags, "%s %s [%llu]", icon, object->name.c_str(),
                                             (unsigned long long)object->id);
            }
            else
            {
                nodeOpen = ImGui::TreeNodeEx("##node", flags, "%s %s", icon, object->name.c_str());
            }

            if (!object->active)
            {
                ImGui::PopStyleColor();
            }

            // Selection
            if (ImGui::IsItemClicked())
            {
                bool addToSelection = ImGui::GetIO().KeyCtrl;
                SelectObject(object->id, addToSelection);
            }

            // Double-click to rename
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                m_renamingObject = object->id;
                strncpy(m_renameBuffer, object->name.c_str(), sizeof(m_renameBuffer) - 1);
                m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
            }

            // Drag source
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
            {
                ObjectID dragID = object->id;
                ImGui::SetDragDropPayload("HIERARCHY_OBJECT", &dragID, sizeof(ObjectID));
                ImGui::Text("%s %s", icon, object->name.c_str());
                ImGui::EndDragDropSource();
            }

            // Drop target
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_OBJECT"))
                {
                    ObjectID draggedID = *static_cast<const ObjectID*>(payload->Data);
                    if (draggedID != object->id)
                    {
                        MoveObject(draggedID, object->id);
                    }
                }
                ImGui::EndDragDropTarget();
            }

            // Context menu
            if (ImGui::BeginPopupContextItem("##ObjContext"))
            {
                RenderObjectContextMenu(object);
                ImGui::EndPopup();
            }

            // Render children
            if (nodeOpen && hasChildren)
            {
                for (auto* child : children)
                {
                    RenderObjectNode(child, depth + 1);
                }
                ImGui::TreePop();
            }
        }

        ImGui::PopID();
    }

    void HierarchyPanel::RenderObjectContextMenu(SceneObject* object)
    {
        if (!object)
            return;

        ImGui::Text("%s", object->name.c_str());
        ImGui::Separator();

        if (ImGui::MenuItem(ICON_FA_EDIT " Rename"))
        {
            m_renamingObject = object->id;
            strncpy(m_renameBuffer, object->name.c_str(), sizeof(m_renameBuffer) - 1);
            m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
        }
        if (ImGui::MenuItem(ICON_FA_COPY " Duplicate"))
        {
            DuplicateObject(object->id);
        }
        if (ImGui::MenuItem(ICON_FA_TRASH " Delete"))
        {
            DeleteObject(object->id);
        }

        ImGui::Separator();
        if (ImGui::BeginMenu(ICON_FA_PLUS " Create Child"))
        {
            if (ImGui::MenuItem("Empty"))
                CreateObject("Empty", object->id);
            if (ImGui::MenuItem("Cube"))
                CreateObject("Cube", object->id);
            if (ImGui::MenuItem("Sphere"))
                CreateObject("Sphere", object->id);
            ImGui::EndMenu();
        }

        if (object->transform.parentID != INVALID_OBJECT_ID)
        {
            if (ImGui::MenuItem(ICON_FA_LEVEL_UP_ALT " Unparent"))
            {
                MoveObject(object->id, INVALID_OBJECT_ID);
            }
        }
    }

    void HierarchyPanel::RenderEmptyContextMenu()
    {
        if (ImGui::BeginPopup("EmptyContextMenu"))
        {
            if (ImGui::MenuItem(ICON_FA_CUBE " Create Empty"))
            {
                CreateObject("Empty");
            }
            if (ImGui::BeginMenu(ICON_FA_CUBE " 3D Object"))
            {
                if (ImGui::MenuItem("Cube"))
                    CreateObject("Cube");
                if (ImGui::MenuItem("Sphere"))
                    CreateObject("Sphere");
                if (ImGui::MenuItem("Plane"))
                    CreateObject("Plane");
                ImGui::EndMenu();
            }
            ImGui::EndPopup();
        }
    }

    bool HierarchyPanel::HandleObjectDragDrop(SceneObject* draggedObject, SceneObject* targetObject)
    {
        if (!draggedObject || !targetObject || !m_scene)
            return false;

        // Prevent parenting to self
        if (draggedObject->id == targetObject->id)
            return false;

        // Prevent circular parenting: walk up from target to ensure dragged isn't an ancestor
        ObjectID checkID = targetObject->transform.parentID;
        while (checkID != INVALID_OBJECT_ID)
        {
            if (checkID == draggedObject->id)
                return false; // would create cycle
            SceneObject* parent = m_scene->FindObject(checkID);
            if (!parent)
                break;
            checkID = parent->transform.parentID;
        }

        MoveObject(draggedObject->id, targetObject->id);
        return true;
    }

    const char* HierarchyPanel::GetObjectIcon(const SceneObject* object) const
    {
        if (!object)
            return ICON_FA_CUBE;

        // Check component types first
        for (auto type : object->componentTypes)
        {
            if (type == ComponentType::CAMERA)
                return ICON_FA_CAMERA;
            if (type == ComponentType::LIGHT)
                return ICON_FA_LIGHTBULB;
            if (type == ComponentType::AUDIO_SOURCE)
                return ICON_FA_VOLUME_UP;
            if (type == ComponentType::PARTICLE_SYSTEM)
                return ICON_FA_FIRE;
        }

        // Fallback: check name for hints
        if (object->name.contains("Camera"))
            return ICON_FA_CAMERA;
        if (object->name.contains("Light"))
            return ICON_FA_LIGHTBULB;
        if (object->name.contains("Player"))
            return ICON_FA_CROSSHAIRS;

        return ICON_FA_CUBE;
    }

    std::vector<SceneObject*> HierarchyPanel::GetChildObjects(ObjectID parentID) const
    {
        std::vector<SceneObject*> children;
        if (!m_scene)
            return children;

        for (auto& obj : m_scene->objects)
        {
            if (obj.transform.parentID == parentID)
            {
                children.push_back(&obj);
            }
        }
        return children;
    }

    bool HierarchyPanel::PassesFilter(const SceneObject* object) const
    {
        if (m_searchFilter.empty())
            return true;
        if (!object)
            return false;

        // Case-insensitive name search
        std::string lowerName = object->name;
        std::string lowerFilter = m_searchFilter;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        return lowerName.contains(lowerFilter);
    }

    bool HierarchyPanel::ObjectOrDescendantPassesFilter(const SceneObject* object) const
    {
        if (PassesFilter(object))
            return true;

        auto children = GetChildObjects(object->id);
        for (auto* child : children)
        {
            if (ObjectOrDescendantPassesFilter(child))
                return true;
        }
        return false;
    }

    // ============================================================================
    // Undoable Scene Mutations
    // ============================================================================

    ObjectID HierarchyPanel::CreateObject(const std::string& name, ObjectID parentID)
    {
        if (!m_scene)
            return INVALID_OBJECT_ID;

        ObjectID newID = m_scene->GetNextObjectID();
        SceneFile* capturedScene = m_scene;

        auto& history = Spark::Editor::CommandHistory::GetInstance();
        history.Execute(std::make_unique<Spark::Editor::LambdaCommand>(
            [capturedScene, newID, name, parentID]()
            {
                SceneObject obj;
                obj.id = newID;
                obj.name = name;
                obj.transform.parentID = parentID;
                capturedScene->objects.push_back(obj);

                // Add to parent's child list
                if (parentID != INVALID_OBJECT_ID)
                {
                    SceneObject* parent = capturedScene->FindObject(parentID);
                    if (parent)
                    {
                        parent->transform.childIDs.push_back(newID);
                    }
                }
            },
            [capturedScene, newID, parentID]()
            {
                // Remove from parent's child list
                if (parentID != INVALID_OBJECT_ID)
                {
                    SceneObject* parent = capturedScene->FindObject(parentID);
                    if (parent)
                    {
                        auto& children = parent->transform.childIDs;
                        children.erase(std::remove(children.begin(), children.end(), newID), children.end());
                    }
                }

                // Remove from objects
                auto& objs = capturedScene->objects;
                objs.erase(
                    std::remove_if(objs.begin(), objs.end(), [newID](const SceneObject& o) { return o.id == newID; }),
                    objs.end());
            },
            "Create Object '" + name + "'"));

        SelectObject(newID);
        NotifyObjectOperation("create", newID);
        return newID;
    }

    ObjectID HierarchyPanel::DuplicateObject(ObjectID objectID)
    {
        if (!m_scene)
            return INVALID_OBJECT_ID;

        SceneObject* original = m_scene->FindObject(objectID);
        if (!original)
            return INVALID_OBJECT_ID;

        ObjectID newID = m_scene->GetNextObjectID();
        SceneObject copyData = *original;
        copyData.id = newID;
        copyData.name = original->name + " (Copy)";
        copyData.transform.childIDs.clear(); // Don't copy children references

        SceneFile* capturedScene = m_scene;

        auto& history = Spark::Editor::CommandHistory::GetInstance();
        history.Execute(std::make_unique<Spark::Editor::LambdaCommand>(
            [capturedScene, copyData]()
            {
                capturedScene->objects.push_back(copyData);

                // Add to parent's child list if parented
                if (copyData.transform.parentID != INVALID_OBJECT_ID)
                {
                    SceneObject* parent = capturedScene->FindObject(copyData.transform.parentID);
                    if (parent)
                    {
                        parent->transform.childIDs.push_back(copyData.id);
                    }
                }
            },
            [capturedScene, newID, copyData]()
            {
                if (copyData.transform.parentID != INVALID_OBJECT_ID)
                {
                    SceneObject* parent = capturedScene->FindObject(copyData.transform.parentID);
                    if (parent)
                    {
                        auto& children = parent->transform.childIDs;
                        children.erase(std::remove(children.begin(), children.end(), newID), children.end());
                    }
                }

                auto& objs = capturedScene->objects;
                objs.erase(
                    std::remove_if(objs.begin(), objs.end(), [newID](const SceneObject& o) { return o.id == newID; }),
                    objs.end());
            },
            "Duplicate '" + original->name + "'"));

        SelectObject(newID);
        NotifyObjectOperation("duplicate", newID);
        return newID;
    }

    void HierarchyPanel::DeleteObject(ObjectID objectID)
    {
        if (!m_scene)
            return;

        SceneObject* obj = m_scene->FindObject(objectID);
        if (!obj)
            return;

        // Capture full object state for undo
        SceneObject savedObj = *obj;
        SceneFile* capturedScene = m_scene;

        // Also capture associated components
        std::vector<Component> savedComponents;
        for (auto& c : m_scene->components)
        {
            if (c.objectID == objectID)
            {
                savedComponents.push_back(c);
            }
        }

        // Remove from selection
        m_selectedSet.erase(objectID);
        m_selectedObjects.erase(std::remove(m_selectedObjects.begin(), m_selectedObjects.end(), objectID),
                                m_selectedObjects.end());

        auto& history = Spark::Editor::CommandHistory::GetInstance();
        history.Execute(std::make_unique<Spark::Editor::LambdaCommand>(
            [capturedScene, objectID]()
            {
                // Remove from parent child list
                SceneObject* toDelete = capturedScene->FindObject(objectID);
                if (toDelete && toDelete->transform.parentID != INVALID_OBJECT_ID)
                {
                    SceneObject* parent = capturedScene->FindObject(toDelete->transform.parentID);
                    if (parent)
                    {
                        auto& children = parent->transform.childIDs;
                        children.erase(std::remove(children.begin(), children.end(), objectID), children.end());
                    }
                }

                // Remove components
                auto& comps = capturedScene->components;
                comps.erase(std::remove_if(comps.begin(), comps.end(),
                                           [objectID](const Component& c) { return c.objectID == objectID; }),
                            comps.end());

                // Remove object
                auto& objs = capturedScene->objects;
                objs.erase(std::remove_if(objs.begin(), objs.end(),
                                          [objectID](const SceneObject& o) { return o.id == objectID; }),
                           objs.end());
            },
            [capturedScene, savedObj, savedComponents]()
            {
                // Re-add the object
                capturedScene->objects.push_back(savedObj);

                // Re-add components
                for (const auto& c : savedComponents)
                {
                    capturedScene->components.push_back(c);
                }

                // Re-add to parent child list
                if (savedObj.transform.parentID != INVALID_OBJECT_ID)
                {
                    SceneObject* parent = capturedScene->FindObject(savedObj.transform.parentID);
                    if (parent)
                    {
                        parent->transform.childIDs.push_back(savedObj.id);
                    }
                }
            },
            "Delete '" + savedObj.name + "'"));

        NotifyObjectOperation("delete", objectID);
    }

    void HierarchyPanel::RenameObject(ObjectID objectID, const std::string& newName)
    {
        if (!m_scene)
            return;
        SceneObject* obj = m_scene->FindObject(objectID);
        if (!obj)
            return;

        std::string oldName = obj->name;
        SceneFile* capturedScene = m_scene;

        auto& history = Spark::Editor::CommandHistory::GetInstance();
        history.Execute(std::make_unique<Spark::Editor::LambdaCommand>(
            [capturedScene, objectID, newName]()
            {
                SceneObject* o = capturedScene->FindObject(objectID);
                if (o)
                    o->name = newName;
            },
            [capturedScene, objectID, oldName]()
            {
                SceneObject* o = capturedScene->FindObject(objectID);
                if (o)
                    o->name = oldName;
            },
            "Rename '" + oldName + "' to '" + newName + "'"));

        NotifyObjectOperation("rename", objectID);
    }

    void HierarchyPanel::MoveObject(ObjectID objectID, ObjectID newParentID)
    {
        if (!m_scene)
            return;
        SceneObject* obj = m_scene->FindObject(objectID);
        if (!obj)
            return;

        // Prevent circular parenting
        if (newParentID != INVALID_OBJECT_ID)
        {
            ObjectID checkID = newParentID;
            while (checkID != INVALID_OBJECT_ID)
            {
                if (checkID == objectID)
                    return; // Would create a cycle
                SceneObject* parent = m_scene->FindObject(checkID);
                if (!parent)
                    break;
                checkID = parent->transform.parentID;
            }
        }

        ObjectID oldParentID = obj->transform.parentID;
        SceneFile* capturedScene = m_scene;

        auto& history = Spark::Editor::CommandHistory::GetInstance();
        history.Execute(std::make_unique<Spark::Editor::LambdaCommand>(
            [capturedScene, objectID, newParentID, oldParentID]()
            {
                SceneObject* o = capturedScene->FindObject(objectID);
                if (!o)
                    return;

                // Remove from old parent's child list
                if (oldParentID != INVALID_OBJECT_ID)
                {
                    SceneObject* oldParent = capturedScene->FindObject(oldParentID);
                    if (oldParent)
                    {
                        auto& children = oldParent->transform.childIDs;
                        children.erase(std::remove(children.begin(), children.end(), objectID), children.end());
                    }
                }

                // Set new parent
                o->transform.parentID = newParentID;

                // Add to new parent's child list
                if (newParentID != INVALID_OBJECT_ID)
                {
                    SceneObject* newParent = capturedScene->FindObject(newParentID);
                    if (newParent)
                    {
                        newParent->transform.childIDs.push_back(objectID);
                    }
                }
            },
            [capturedScene, objectID, newParentID, oldParentID]()
            {
                SceneObject* o = capturedScene->FindObject(objectID);
                if (!o)
                    return;

                // Remove from new parent's child list
                if (newParentID != INVALID_OBJECT_ID)
                {
                    SceneObject* newParent = capturedScene->FindObject(newParentID);
                    if (newParent)
                    {
                        auto& children = newParent->transform.childIDs;
                        children.erase(std::remove(children.begin(), children.end(), objectID), children.end());
                    }
                }

                // Restore old parent
                o->transform.parentID = oldParentID;

                if (oldParentID != INVALID_OBJECT_ID)
                {
                    SceneObject* oldParent = capturedScene->FindObject(oldParentID);
                    if (oldParent)
                    {
                        oldParent->transform.childIDs.push_back(objectID);
                    }
                }
            },
            "Reparent Object"));

        // Auto-expand the new parent
        if (newParentID != INVALID_OBJECT_ID)
        {
            ExpandObject(newParentID);
        }

        NotifyObjectOperation("reparent", objectID);
    }

    void HierarchyPanel::UpdateSelection()
    {
        // Verify all selected objects still exist
        if (!m_scene)
        {
            ClearSelection();
            return;
        }

        std::vector<ObjectID> validSelection;
        for (auto id : m_selectedObjects)
        {
            if (m_scene->FindObject(id))
            {
                validSelection.push_back(id);
            }
        }

        if (validSelection.size() != m_selectedObjects.size())
        {
            SetSelectedObjects(validSelection);
        }
    }

    void HierarchyPanel::NotifySelectionChanged()
    {
        if (m_selectionCallback)
        {
            m_selectionCallback(m_selectedObjects);
        }
    }

    void HierarchyPanel::NotifyObjectOperation(const std::string& operation, ObjectID objectID)
    {
        if (m_objectOperationCallback)
        {
            m_objectOperationCallback(operation, objectID);
        }
        m_needsSelectionUpdate = true;
        m_filterCacheDirty = true;
    }

    void HierarchyPanel::ResetToDefault()
    {
        // Create an owned scene if none is set externally
        m_ownedScene = std::make_unique<SceneFile>();
        m_scene = m_ownedScene.get();

        m_scene->objects.clear();
        m_scene->components.clear();
        m_selectedObjects.clear();
        m_selectedSet.clear();
        m_expandedObjects.clear();
        m_filterCacheDirty = true;
        m_searchFilter.clear();
        m_searchBuffer[0] = '\0';

        // Populate with standard default objects
        CreateObject("Main Camera");
        CreateObject("Directional Light");
        CreateObject("Ground Plane");

        std::cout << "Scene hierarchy reset to default\n";
    }

    std::vector<std::string> HierarchyPanel::GetSceneObjects() const
    {
        std::vector<std::string> names;
        if (m_scene)
        {
            names.reserve(m_scene->objects.size());
            for (const auto& obj : m_scene->objects)
            {
                names.push_back(obj.name);
            }
        }
        return names;
    }

} // namespace SparkEditor
