/**
 * @file SceneFile.cpp
 * @brief Implementation of SceneFile data structures and methods
 * @author Spark Engine Team
 * @date 2025
 */

#include "SceneFile.h"
#include "Utils/LogMacros.h"
#include "Utils/Validate.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <unordered_set>

using namespace DirectX;
namespace SparkEditor
{

    // Transform implementation
    XMMATRIX Transform::GetMatrix() const
    {
        XMVECTOR scaleVec = XMLoadFloat3(&scale);
        XMVECTOR rotationQuat = XMLoadFloat4(&rotation);
        XMVECTOR positionVec = XMLoadFloat3(&position);
        return XMMatrixScalingFromVector(scaleVec) * XMMatrixRotationQuaternion(rotationQuat) *
               XMMatrixTranslationFromVector(positionVec);
    }

    void Transform::SetFromMatrix(const XMMATRIX& matrix)
    {
        XMVECTOR scaleVec, rotationQuat, positionVec;
        XMMatrixDecompose(&scaleVec, &rotationQuat, &positionVec, matrix);
        XMStoreFloat3(&scale, scaleVec);
        XMStoreFloat4(&rotation, rotationQuat);
        XMStoreFloat3(&position, positionVec);
    }

    // SceneFile implementation
    ObjectID SceneFile::GetNextObjectID()
    {
        std::unordered_set<ObjectID> usedIDs;
        usedIDs.reserve(objects.size());
        for (const auto& obj : objects)
            if (obj.id != INVALID_OBJECT_ID)
                usedIDs.insert(obj.id);

        ObjectID candidate = 1;
        while (usedIDs.contains(candidate))
        {
            if (candidate == std::numeric_limits<ObjectID>::max())
                return INVALID_OBJECT_ID;
            ++candidate;
        }
        return candidate;
    }

    SceneObject* SceneFile::FindObject(ObjectID id)
    {
        for (auto& obj : objects)
        {
            if (obj.id == id)
            {
                return &obj;
            }
        }
        return nullptr;
    }

    std::vector<SceneObject*> SceneFile::FindObjectsByName(const std::string& name)
    {
        SPARK_WARN_IF(Spark::LogCategory::Editor, name.empty(), "FindObjectsByName called with empty name");
        std::vector<SceneObject*> result;
        for (auto& obj : objects)
        {
            if (obj.name == name)
            {
                result.push_back(&obj);
            }
        }
        return result;
    }

    std::vector<Component*> SceneFile::GetObjectComponents(ObjectID objectID)
    {
        std::vector<Component*> result;
        for (auto& comp : components)
        {
            if (comp.objectID == objectID)
            {
                result.push_back(&comp);
            }
        }
        return result;
    }

    void SceneFile::AddAssetReference(const std::string& assetPath, const std::string& assetType)
    {
        SPARK_VALIDATE_NOT_EMPTY(Spark::LogCategory::Editor, assetPath);
        SPARK_VALIDATE_NOT_EMPTY(Spark::LogCategory::Editor, assetType);
        SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Adding asset reference: %s (type=%s)", assetPath.c_str(),
                        assetType.c_str());
        // Check if already referenced
        for (const auto& ref : assetReferences)
        {
            if (ref.assetPath == assetPath)
            {
                return;
            }
        }
        AssetReference ref;
        ref.assetPath = assetPath;
        ref.assetType = assetType;
        assetReferences.push_back(ref);
    }

    bool SceneFile::Validate(std::vector<std::string>& errors) const
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        bool valid = true;

        if (header.magic != SCENE_FILE_MAGIC)
        {
            errors.push_back("Invalid scene file magic number");
            valid = false;
        }
        if (header.version != SCENE_FILE_VERSION)
        {
            errors.push_back("Unsupported scene file version: " + std::to_string(header.version));
            valid = false;
        }

        // Check for duplicate IDs
        std::unordered_map<ObjectID, int> idCounts;
        std::unordered_map<ObjectID, const SceneObject*> objectsByID;
        idCounts.reserve(objects.size());
        objectsByID.reserve(objects.size());
        for (const auto& obj : objects)
        {
            if (obj.id == INVALID_OBJECT_ID)
            {
                errors.push_back("Scene object uses the reserved invalid ID");
                valid = false;
            }
            if (obj.id == std::numeric_limits<ObjectID>::max())
            {
                errors.push_back("Scene object uses the maximum ID, leaving no allocatable successor");
                valid = false;
            }
            idCounts[obj.id]++;
            if (idCounts[obj.id] > 1)
            {
                errors.push_back("Duplicate object ID: " + std::to_string(obj.id));
                valid = false;
            }
            else
            {
                objectsByID.emplace(obj.id, &obj);
            }
        }

        std::unordered_map<ObjectID, std::unordered_set<uint32_t>> declaredComponentTypes;
        declaredComponentTypes.reserve(objectsByID.size());
        for (const auto& obj : objects)
        {
            if (obj.transform.parentID != INVALID_OBJECT_ID && !idCounts.contains(obj.transform.parentID))
            {
                errors.push_back("Object " + std::to_string(obj.id) + " references a missing parent");
                valid = false;
            }
            else if (obj.transform.parentID == obj.id)
            {
                errors.push_back("Object " + std::to_string(obj.id) + " cannot be its own parent");
                valid = false;
            }

            std::unordered_set<ObjectID> childIDs;
            childIDs.reserve(obj.transform.childIDs.size());
            for (ObjectID childID : obj.transform.childIDs)
            {
                if (childID == INVALID_OBJECT_ID || !idCounts.contains(childID))
                {
                    errors.push_back("Object " + std::to_string(obj.id) + " references a missing child");
                    valid = false;
                }
                else if (childID == obj.id)
                {
                    errors.push_back("Object " + std::to_string(obj.id) + " cannot be its own child");
                    valid = false;
                }
                if (!childIDs.insert(childID).second)
                {
                    errors.push_back("Object " + std::to_string(obj.id) + " contains a duplicate child ID");
                    valid = false;
                }

                const auto child = objectsByID.find(childID);
                if (child != objectsByID.end() && child->second->transform.parentID != obj.id)
                {
                    errors.push_back("Object " + std::to_string(obj.id) +
                                     " has a child whose parent link points elsewhere");
                    valid = false;
                }
            }
            if (obj.transform.parentID != INVALID_OBJECT_ID)
            {
                const auto parent = objectsByID.find(obj.transform.parentID);
                if (parent != objectsByID.end() && std::count(parent->second->transform.childIDs.begin(),
                                                              parent->second->transform.childIDs.end(), obj.id) != 1)
                {
                    errors.push_back("Object " + std::to_string(obj.id) +
                                     " is not referenced exactly once by its parent");
                    valid = false;
                }
            }
            for (ComponentType componentType : obj.componentTypes)
            {
                const uint32_t value = static_cast<uint32_t>(componentType);
                if (value > 64 && value < static_cast<uint32_t>(ComponentType::CUSTOM))
                {
                    errors.push_back("Object contains a reserved component type value");
                    valid = false;
                }
                if (!declaredComponentTypes[obj.id].insert(value).second)
                {
                    errors.push_back("Object " + std::to_string(obj.id) + " declares a duplicate component type");
                    valid = false;
                }
            }
        }

        // A parent graph is a forest. Traverse it iteratively with three-state
        // coloring so a malicious 250k-object chain cannot overflow the stack
        // or turn validation into quadratic work.
        std::unordered_map<ObjectID, uint8_t> parentState;
        parentState.reserve(objectsByID.size());
        for (const auto& [objectID, object] : objectsByID)
        {
            if (parentState[objectID] == 2)
                continue;

            std::vector<ObjectID> path;
            ObjectID current = objectID;
            while (current != INVALID_OBJECT_ID)
            {
                const auto currentObject = objectsByID.find(current);
                if (currentObject == objectsByID.end() || parentState[current] == 2)
                    break;
                if (parentState[current] == 1)
                {
                    errors.push_back("Scene hierarchy contains a parent cycle involving object " +
                                     std::to_string(current));
                    valid = false;
                    break;
                }
                parentState[current] = 1;
                path.push_back(current);
                current = currentObject->second->transform.parentID;
            }
            for (ObjectID visited : path)
                parentState[visited] = 2;
        }

        std::unordered_map<ObjectID, std::unordered_set<uint32_t>> storedComponentTypes;
        storedComponentTypes.reserve(objectsByID.size());
        for (const auto& component : components)
        {
            if (component.objectID == INVALID_OBJECT_ID || !idCounts.contains(component.objectID))
            {
                errors.push_back("Component references a missing scene object");
                valid = false;
            }
            const uint32_t typeValue = static_cast<uint32_t>(component.type);
            if (typeValue > 64 && typeValue < static_cast<uint32_t>(ComponentType::CUSTOM))
            {
                errors.push_back("Component uses a reserved type value");
                valid = false;
            }
            if (!storedComponentTypes[component.objectID].insert(typeValue).second)
            {
                errors.push_back("Scene contains duplicate component records for object " +
                                 std::to_string(component.objectID));
                valid = false;
            }
            const auto declared = declaredComponentTypes.find(component.objectID);
            if (declared == declaredComponentTypes.end() || !declared->second.contains(typeValue))
            {
                errors.push_back("Component record is absent from its object's declared component types");
                valid = false;
            }
        }

        for (const auto& [objectID, declaredTypes] : declaredComponentTypes)
        {
            const auto stored = storedComponentTypes.find(objectID);
            for (uint32_t type : declaredTypes)
            {
                if (stored == storedComponentTypes.end() || !stored->second.contains(type))
                {
                    errors.push_back("Object " + std::to_string(objectID) +
                                     " declares a component with no matching record");
                    valid = false;
                }
            }
        }

        return valid;
    }

    void SceneFile::UpdateHeader()
    {
        header.objectCount = static_cast<uint32_t>(objects.size());
        header.componentCount = static_cast<uint32_t>(components.size());
        header.assetReferenceCount = static_cast<uint32_t>(assetReferences.size());
        header.timestamp =
            static_cast<uint64_t>(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    }

} // namespace SparkEditor
