/**
 * @file SceneFile.cpp
 * @brief Implementation of SceneFile data structures and methods
 * @author Spark Engine Team
 * @date 2025
 */

#include "SceneFile.h"
#include <algorithm>
#include <chrono>
#include <cstring>

using namespace DirectX;
namespace SparkEditor {

// Transform implementation
XMMATRIX Transform::GetMatrix() const {
    XMVECTOR scaleVec = XMLoadFloat3(&scale);
    XMVECTOR rotationQuat = XMLoadFloat4(&rotation);
    XMVECTOR positionVec = XMLoadFloat3(&position);
    return XMMatrixScalingFromVector(scaleVec) *
           XMMatrixRotationQuaternion(rotationQuat) *
           XMMatrixTranslationFromVector(positionVec);
}

void Transform::SetFromMatrix(const XMMATRIX& matrix) {
    XMVECTOR scaleVec, rotationQuat, positionVec;
    XMMatrixDecompose(&scaleVec, &rotationQuat, &positionVec, matrix);
    XMStoreFloat3(&scale, scaleVec);
    XMStoreFloat4(&rotation, rotationQuat);
    XMStoreFloat3(&position, positionVec);
}

// SceneFile implementation
ObjectID SceneFile::GetNextObjectID() {
    ObjectID maxID = 0;
    for (const auto& obj : objects) {
        if (obj.id > maxID) {
            maxID = obj.id;
        }
    }
    return maxID + 1;
}

SceneObject* SceneFile::FindObject(ObjectID id) {
    for (auto& obj : objects) {
        if (obj.id == id) {
            return &obj;
        }
    }
    return nullptr;
}

std::vector<SceneObject*> SceneFile::FindObjectsByName(const std::string& name) {
    std::vector<SceneObject*> result;
    for (auto& obj : objects) {
        if (obj.name == name) {
            result.push_back(&obj);
        }
    }
    return result;
}

std::vector<Component*> SceneFile::GetObjectComponents(ObjectID objectID) {
    std::vector<Component*> result;
    for (auto& comp : components) {
        if (comp.objectID == objectID) {
            result.push_back(&comp);
        }
    }
    return result;
}

void SceneFile::AddAssetReference(const std::string& assetPath, const std::string& assetType) {
    // Check if already referenced
    for (const auto& ref : assetReferences) {
        if (ref.assetPath == assetPath) {
            return;
        }
    }
    AssetReference ref;
    ref.assetPath = assetPath;
    ref.assetType = assetType;
    assetReferences.push_back(ref);
}

bool SceneFile::Validate(std::vector<std::string>& errors) const {
    bool valid = true;

    if (header.magic != SCENE_FILE_MAGIC) {
        errors.push_back("Invalid scene file magic number");
        valid = false;
    }

    // Check for duplicate IDs
    std::unordered_map<ObjectID, int> idCounts;
    for (const auto& obj : objects) {
        idCounts[obj.id]++;
        if (idCounts[obj.id] > 1) {
            errors.push_back("Duplicate object ID: " + std::to_string(obj.id));
            valid = false;
        }
    }

    return valid;
}

void SceneFile::UpdateHeader() {
    header.objectCount = static_cast<uint32_t>(objects.size());
    header.componentCount = static_cast<uint32_t>(components.size());
    header.assetReferenceCount = static_cast<uint32_t>(assetReferences.size());
    header.timestamp = static_cast<uint64_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
}

} // namespace SparkEditor
