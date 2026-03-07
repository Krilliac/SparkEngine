/**
 * @file HierarchyPanel.cpp
 * @brief Implementation of the Hierarchy panel
 * @author Spark Engine Team
 * @date 2025
 */

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>

#include <imgui.h>

#include "HierarchyPanel.h"
#include "../Core/EditorIcons.h"

namespace SparkEditor {

HierarchyPanel::HierarchyPanel()
    : EditorPanel("Hierarchy", "hierarchy_panel") {
}

HierarchyPanel::~HierarchyPanel() {
}

bool HierarchyPanel::Initialize() {
    std::cout << "Initializing Hierarchy panel\n";
    return true;
}

void HierarchyPanel::Update(float /*deltaTime*/) {
    if (m_needsSelectionUpdate) {
        UpdateSelection();
        m_needsSelectionUpdate = false;
    }
}

void HierarchyPanel::Render() {
    if (!IsVisible()) return;

    if (BeginPanel()) {
        RenderToolbar();
        ImGui::Separator();
        RenderSearchBar();
        ImGui::Separator();
        RenderHierarchyTree();
    }
    EndPanel();
}

void HierarchyPanel::Shutdown() {
    std::cout << "Shutting down Hierarchy panel\n";
}

bool HierarchyPanel::HandleEvent(const std::string& /*eventType*/, void* /*eventData*/) {
    return false;
}

void HierarchyPanel::SetScene(SceneFile* scene) {
    m_scene = scene;
    ClearSelection();
    m_filterCacheDirty = true;
}

void HierarchyPanel::SetSelectedObjects(const std::vector<ObjectID>& objectIDs) {
    m_selectedObjects = objectIDs;
    m_selectedSet.clear();
    for (auto id : objectIDs) {
        m_selectedSet.insert(id);
    }
    NotifySelectionChanged();
}

void HierarchyPanel::SelectObject(ObjectID objectID, bool addToSelection) {
    if (!addToSelection) {
        m_selectedObjects.clear();
        m_selectedSet.clear();
    }
    if (m_selectedSet.find(objectID) == m_selectedSet.end()) {
        m_selectedObjects.push_back(objectID);
        m_selectedSet.insert(objectID);
    }
    m_lastClickedObject = objectID;
    NotifySelectionChanged();
}

void HierarchyPanel::ClearSelection() {
    m_selectedObjects.clear();
    m_selectedSet.clear();
    m_lastClickedObject = INVALID_OBJECT_ID;
    NotifySelectionChanged();
}

bool HierarchyPanel::IsObjectSelected(ObjectID objectID) const {
    return m_selectedSet.count(objectID) > 0;
}

void HierarchyPanel::ExpandObject(ObjectID objectID) {
    m_expandedObjects.insert(objectID);
}

void HierarchyPanel::CollapseObject(ObjectID objectID) {
    m_expandedObjects.erase(objectID);
}

bool HierarchyPanel::IsObjectExpanded(ObjectID objectID) const {
    return m_expandedObjects.count(objectID) > 0;
}

void HierarchyPanel::FocusObject(ObjectID objectID) {
    // Expand all parents up to the root
    if (!m_scene) return;
    SceneObject* obj = m_scene->FindObject(objectID);
    if (!obj) return;

    ObjectID parentID = obj->transform.parentID;
    while (parentID != INVALID_OBJECT_ID) {
        ExpandObject(parentID);
        SceneObject* parent = m_scene->FindObject(parentID);
        if (!parent) break;
        parentID = parent->transform.parentID;
    }

    SelectObject(objectID);
}

void HierarchyPanel::RegisterSelectionCallback(std::function<void(const std::vector<ObjectID>&)> callback) {
    m_selectionCallback = std::move(callback);
}

void HierarchyPanel::RegisterObjectOperationCallback(std::function<void(const std::string&, ObjectID)> callback) {
    m_objectOperationCallback = std::move(callback);
}

void HierarchyPanel::SetSearchFilter(const std::string& searchText) {
    m_searchFilter = searchText;
    m_filterCacheDirty = true;
}

// ============================================================================
// Private Methods
// ============================================================================

void HierarchyPanel::RenderToolbar() {
    if (ImGui::Button(ICON_FA_PLUS " Create")) {
        ImGui::OpenPopup("CreateObject");
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_TRASH " Delete")) {
        for (auto id : m_selectedObjects) {
            DeleteObject(id);
        }
    }

    if (ImGui::BeginPopup("CreateObject")) {
        if (ImGui::MenuItem(ICON_FA_CUBE " Empty GameObject")) {
            CreateObject("Empty");
        }
        if (ImGui::BeginMenu(ICON_FA_CUBE " 3D Object")) {
            if (ImGui::MenuItem("Cube")) CreateObject("Cube");
            if (ImGui::MenuItem("Sphere")) CreateObject("Sphere");
            if (ImGui::MenuItem("Plane")) CreateObject("Plane");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(ICON_FA_LIGHTBULB " Light")) {
            if (ImGui::MenuItem("Directional Light")) CreateObject("Directional Light");
            if (ImGui::MenuItem("Point Light")) CreateObject("Point Light");
            if (ImGui::MenuItem("Spot Light")) CreateObject("Spot Light");
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(ICON_FA_CAMERA " Camera")) {
            CreateObject("Camera");
        }
        ImGui::EndPopup();
    }
}

void HierarchyPanel::RenderSearchBar() {
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##HierarchySearch", ICON_FA_SEARCH " Search objects...",
                                  m_searchBuffer, sizeof(m_searchBuffer))) {
        SetSearchFilter(m_searchBuffer);
    }
}

void HierarchyPanel::RenderHierarchyTree() {
    ImGui::BeginChild("##HierarchyTree");

    if (!m_scene) {
        ImGui::Text("No scene loaded");
        ImGui::EndChild();
        return;
    }

    // Render root objects
    auto rootObjects = GetChildObjects(INVALID_OBJECT_ID);
    for (auto* obj : rootObjects) {
        if (!m_searchFilter.empty() && !ObjectOrDescendantPassesFilter(obj)) continue;
        RenderObjectNode(obj, 0);
    }

    // Drop on empty space to unparent (make root)
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_OBJECT")) {
            ObjectID draggedID = *static_cast<const ObjectID*>(payload->Data);
            SceneObject* draggedObj = m_scene->FindObject(draggedID);
            if (draggedObj && draggedObj->transform.parentID != INVALID_OBJECT_ID) {
                // Remove from old parent
                SceneObject* oldParent = m_scene->FindObject(draggedObj->transform.parentID);
                if (oldParent) {
                    auto& children = oldParent->transform.childIDs;
                    children.erase(
                        std::remove(children.begin(), children.end(), draggedID),
                        children.end());
                }
                draggedObj->transform.parentID = INVALID_OBJECT_ID;
            }
        }
        ImGui::EndDragDropTarget();
    }

    // Empty space context menu
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !ImGui::IsAnyItemHovered()) {
        m_showEmptyContextMenu = true;
    }
    if (m_showEmptyContextMenu) {
        ImGui::OpenPopup("EmptyContextMenu");
        m_showEmptyContextMenu = false;
    }
    RenderEmptyContextMenu();

    ImGui::EndChild();
}

void HierarchyPanel::RenderObjectNode(SceneObject* object, int depth) {
    if (!object) return;
    if (!m_showInactiveObjects && !object->active) return;

    auto children = GetChildObjects(object->id);
    bool hasChildren = !children.empty();

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
    if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (IsObjectSelected(object->id)) flags |= ImGuiTreeNodeFlags_Selected;

    const char* icon = GetObjectIcon(object);

    ImGui::PushID(static_cast<int>(object->id));

    bool nodeOpen;
    if (m_showObjectIDs) {
        nodeOpen = ImGui::TreeNodeEx("##node", flags, "%s %s [%llu]", icon, object->name.c_str(), (unsigned long long)object->id);
    } else {
        nodeOpen = ImGui::TreeNodeEx("##node", flags, "%s %s", icon, object->name.c_str());
    }

    // Selection
    if (ImGui::IsItemClicked()) {
        bool addToSelection = ImGui::GetIO().KeyCtrl;
        SelectObject(object->id, addToSelection);
    }

    // Drag source
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        ObjectID dragID = object->id;
        ImGui::SetDragDropPayload("HIERARCHY_OBJECT", &dragID, sizeof(ObjectID));
        ImGui::Text("%s %s", icon, object->name.c_str());
        ImGui::EndDragDropSource();
    }

    // Drop target
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_OBJECT")) {
            ObjectID draggedID = *static_cast<const ObjectID*>(payload->Data);
            if (draggedID != object->id) {
                SceneObject* draggedObj = m_scene->FindObject(draggedID);
                if (draggedObj) {
                    HandleObjectDragDrop(draggedObj, object);
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    // Context menu
    if (ImGui::BeginPopupContextItem("##ObjContext")) {
        RenderObjectContextMenu(object);
        ImGui::EndPopup();
    }

    // Render children
    if (nodeOpen && hasChildren) {
        for (auto* child : children) {
            RenderObjectNode(child, depth + 1);
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
}

void HierarchyPanel::RenderObjectContextMenu(SceneObject* object) {
    if (!object) return;

    ImGui::Text("%s", object->name.c_str());
    ImGui::Separator();

    if (ImGui::MenuItem(ICON_FA_COPY " Duplicate")) {
        DuplicateObject(object->id);
    }
    if (ImGui::MenuItem(ICON_FA_TRASH " Delete")) {
        DeleteObject(object->id);
    }
}

void HierarchyPanel::RenderEmptyContextMenu() {
    if (ImGui::BeginPopup("EmptyContextMenu")) {
        if (ImGui::MenuItem(ICON_FA_CUBE " Create Empty")) {
            CreateObject("Empty");
        }
        ImGui::EndPopup();
    }
}

bool HierarchyPanel::HandleObjectDragDrop(SceneObject* draggedObject, SceneObject* targetObject) {
    if (!draggedObject || !targetObject || !m_scene) return false;

    // Prevent parenting to self
    if (draggedObject->id == targetObject->id) return false;

    // Prevent circular parenting: walk up from target to ensure dragged isn't an ancestor
    ObjectID checkID = targetObject->transform.parentID;
    while (checkID != INVALID_OBJECT_ID) {
        if (checkID == draggedObject->id) return false; // would create cycle
        SceneObject* parent = m_scene->FindObject(checkID);
        if (!parent) break;
        checkID = parent->transform.parentID;
    }

    // Remove from old parent's child list
    ObjectID oldParentID = draggedObject->transform.parentID;
    if (oldParentID != INVALID_OBJECT_ID) {
        SceneObject* oldParent = m_scene->FindObject(oldParentID);
        if (oldParent) {
            auto& children = oldParent->transform.childIDs;
            children.erase(
                std::remove(children.begin(), children.end(), draggedObject->id),
                children.end());
        }
    }

    // Set new parent
    draggedObject->transform.parentID = targetObject->id;
    targetObject->transform.childIDs.push_back(draggedObject->id);

    return true;
}

const char* HierarchyPanel::GetObjectIcon(const SceneObject* object) const {
    if (!object) return ICON_FA_CUBE;

    // Check name for hints
    if (object->name.find("Camera") != std::string::npos) return ICON_FA_CAMERA;
    if (object->name.find("Light") != std::string::npos) return ICON_FA_LIGHTBULB;
    if (object->name.find("Player") != std::string::npos) return ICON_FA_CROSSHAIRS;

    return ICON_FA_CUBE;
}

std::vector<SceneObject*> HierarchyPanel::GetChildObjects(ObjectID parentID) const {
    std::vector<SceneObject*> children;
    if (!m_scene) return children;

    for (auto& obj : m_scene->objects) {
        if (obj.transform.parentID == parentID) {
            children.push_back(&obj);
        }
    }
    return children;
}

bool HierarchyPanel::PassesFilter(const SceneObject* object) const {
    if (m_searchFilter.empty()) return true;
    if (!object) return false;

    // Case-insensitive name search
    std::string lowerName = object->name;
    std::string lowerFilter = m_searchFilter;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return lowerName.find(lowerFilter) != std::string::npos;
}

bool HierarchyPanel::ObjectOrDescendantPassesFilter(const SceneObject* object) const {
    if (PassesFilter(object)) return true;

    auto children = GetChildObjects(object->id);
    for (auto* child : children) {
        if (ObjectOrDescendantPassesFilter(child)) return true;
    }
    return false;
}

ObjectID HierarchyPanel::CreateObject(const std::string& name, ObjectID parentID) {
    if (!m_scene) return INVALID_OBJECT_ID;

    SceneObject obj;
    obj.id = m_scene->GetNextObjectID();
    obj.name = name;
    obj.transform.parentID = parentID;
    m_scene->objects.push_back(obj);

    SelectObject(obj.id);
    NotifyObjectOperation("create", obj.id);
    std::cout << "Created object: " << name << "\n";
    return obj.id;
}

ObjectID HierarchyPanel::DuplicateObject(ObjectID objectID) {
    if (!m_scene) return INVALID_OBJECT_ID;

    SceneObject* original = m_scene->FindObject(objectID);
    if (!original) return INVALID_OBJECT_ID;

    SceneObject copy = *original;
    copy.id = m_scene->GetNextObjectID();
    copy.name = original->name + " (Copy)";
    m_scene->objects.push_back(copy);

    SelectObject(copy.id);
    NotifyObjectOperation("duplicate", copy.id);
    std::cout << "Duplicated object: " << original->name << "\n";
    return copy.id;
}

void HierarchyPanel::DeleteObject(ObjectID objectID) {
    if (!m_scene) return;

    // Remove from selection
    m_selectedSet.erase(objectID);
    m_selectedObjects.erase(
        std::remove(m_selectedObjects.begin(), m_selectedObjects.end(), objectID),
        m_selectedObjects.end());

    // Remove from scene
    auto& objs = m_scene->objects;
    auto it = std::find_if(objs.begin(), objs.end(),
        [objectID](const SceneObject& o) { return o.id == objectID; });
    if (it != objs.end()) {
        std::cout << "Deleted object: " << it->name << "\n";
        objs.erase(it);
    }

    NotifyObjectOperation("delete", objectID);
}

void HierarchyPanel::RenameObject(ObjectID objectID, const std::string& newName) {
    if (!m_scene) return;
    SceneObject* obj = m_scene->FindObject(objectID);
    if (obj) {
        std::cout << "Renamed object: " << obj->name << " -> " << newName << "\n";
        obj->name = newName;
    }
}

void HierarchyPanel::MoveObject(ObjectID objectID, ObjectID newParentID) {
    if (!m_scene) return;
    SceneObject* obj = m_scene->FindObject(objectID);
    if (obj) {
        obj->transform.parentID = newParentID;
    }
}

void HierarchyPanel::UpdateSelection() {
    // Verify all selected objects still exist
    if (!m_scene) {
        ClearSelection();
        return;
    }

    std::vector<ObjectID> validSelection;
    for (auto id : m_selectedObjects) {
        if (m_scene->FindObject(id)) {
            validSelection.push_back(id);
        }
    }

    if (validSelection.size() != m_selectedObjects.size()) {
        SetSelectedObjects(validSelection);
    }
}

void HierarchyPanel::NotifySelectionChanged() {
    if (m_selectionCallback) {
        m_selectionCallback(m_selectedObjects);
    }
}

void HierarchyPanel::NotifyObjectOperation(const std::string& operation, ObjectID objectID) {
    if (m_objectOperationCallback) {
        m_objectOperationCallback(operation, objectID);
    }
    m_needsSelectionUpdate = true;
    m_filterCacheDirty = true;
}

} // namespace SparkEditor
