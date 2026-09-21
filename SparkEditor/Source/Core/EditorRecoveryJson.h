/**
 * @file EditorRecoveryJson.h
 * @brief Internal JSON validation and conversion helpers for editor recovery.
 */

#pragma once

#include "EditorRecovery.h"

#include "Engine/ECS/Components.h"
#include "SceneManager/ReflectedSceneSerializer.h"
#include "Utils/JsonUtils.h"

#include <cmath>
#include <exception>
#include <optional>

namespace SparkEditor::RecoveryDetail
{
    constexpr size_t kMaxOperationCount = 50;
    constexpr size_t kMaxOperationBytes = 4096;
    constexpr size_t kMaxProjectIdentityBytes = 4096;
    constexpr size_t kMaxSceneDisplayNameBytes = 1024;
    constexpr size_t kMaxProjectRelativePathBytes = 4096;
    constexpr uint64_t kMaxExactJsonInteger = 9007199254740991ULL;

    inline Spark::Json::JsonLimits RecoveryJsonLimits()
    {
        Spark::Json::JsonLimits limits;
        limits.maxBytes = kEditorRecoveryMaxBytes;
        limits.maxDepth = 64;
        limits.maxNodes = 250000;
        return limits;
    }

    inline bool ParseRecoveryJson(std::string_view text, Spark::Json::Value& value, std::string& error)
    {
        return Spark::Json::ParseBounded(text, RecoveryJsonLimits(), &value, &error);
    }

    inline bool IsSafeRelativePath(const std::string& value)
    {
        if (value.empty())
            return true;
        if (value.find('\0') != std::string::npos)
            return false;

        const std::filesystem::path path(value);
        if (path.is_absolute() || path.has_root_path() || path.has_root_name())
            return false;

        for (const auto& segment : path)
        {
            if (segment == "..")
                return false;
        }
        return true;
    }

    inline bool ValidateSnapshot(const EditorRecoverySnapshot& snapshot, std::string& error)
    {
        if (snapshot.schemaVersion != kEditorRecoverySchemaVersion)
        {
            error = "unsupported recovery schema version";
            return false;
        }
        if (snapshot.projectIdentity.empty())
        {
            error = "recovery project identity is empty";
            return false;
        }
        if (snapshot.projectIdentity.size() > kMaxProjectIdentityBytes)
        {
            error = "recovery project identity exceeds the size limit";
            return false;
        }
        if (snapshot.sceneDisplayName.empty())
        {
            error = "recovery scene display name is empty";
            return false;
        }
        if (snapshot.sceneDisplayName.size() > kMaxSceneDisplayNameBytes)
        {
            error = "recovery scene display name exceeds the size limit";
            return false;
        }
        if (snapshot.projectRelativeScene.size() > kMaxProjectRelativePathBytes ||
            snapshot.layoutIniPath.size() > kMaxProjectRelativePathBytes)
        {
            error = "recovery relative path exceeds the size limit";
            return false;
        }
        if (!IsSafeRelativePath(snapshot.projectRelativeScene))
        {
            error = "recovery scene path is not project-relative";
            return false;
        }
        if (!IsSafeRelativePath(snapshot.layoutIniPath))
        {
            error = "recovery layout path is not project-relative";
            return false;
        }
        if (snapshot.serializedWorld.empty())
        {
            error = "recovery world is empty";
            return false;
        }
        if (snapshot.serializedWorld.size() > kEditorRecoveryMaxBytes)
        {
            error = "recovery world exceeds the size limit";
            return false;
        }
        if (snapshot.dirtySequence > kMaxExactJsonInteger ||
            snapshot.capturedUnixMilliseconds > static_cast<int64_t>(kMaxExactJsonInteger) ||
            snapshot.capturedUnixMilliseconds < -static_cast<int64_t>(kMaxExactJsonInteger))
        {
            error = "recovery sequence or timestamp exceeds JSON integer precision";
            return false;
        }
        if (snapshot.recentOperations.size() > kMaxOperationCount)
        {
            error = "recovery operation count exceeds the limit";
            return false;
        }
        for (const std::string& operation : snapshot.recentOperations)
        {
            if (operation.size() > kMaxOperationBytes)
            {
                error = "recovery operation exceeds the size limit";
                return false;
            }
        }

        Spark::Json::Value world;
        if (!ParseRecoveryJson(snapshot.serializedWorld, world, error))
        {
            error = "recovery world is not valid JSON: " + error;
            return false;
        }
        if (!world.IsObject())
        {
            error = "recovery world root is not an object";
            return false;
        }

        try
        {
            ::World restoredWorld;
            if (!Spark::DeserializeInto(restoredWorld, snapshot.serializedWorld,
                                        Spark::SceneDeserializeMode::StrictRecovery))
            {
                error = "recovery world does not satisfy the strict scene schema";
                return false;
            }
        }
        catch (const std::exception& exception)
        {
            error = "recovery world could not be deserialized: " + std::string(exception.what());
            return false;
        }
        return true;
    }

    inline Spark::Json::Value SnapshotToJson(const EditorRecoverySnapshot& snapshot)
    {
        Spark::Json::Value root = Spark::Json::Value::MakeObject();
        root["schemaVersion"] = Spark::Json::Value(static_cast<int>(snapshot.schemaVersion));
        root["projectIdentity"] = Spark::Json::Value(snapshot.projectIdentity);
        root["projectRelativeScene"] = Spark::Json::Value(snapshot.projectRelativeScene);
        root["sceneDisplayName"] = Spark::Json::Value(snapshot.sceneDisplayName);
        root["serializedWorld"] = Spark::Json::Value(snapshot.serializedWorld);
        root["layoutIniPath"] = Spark::Json::Value(snapshot.layoutIniPath);
        root["dirtySequence"] = Spark::Json::Value(static_cast<double>(snapshot.dirtySequence));
        root["capturedUnixMilliseconds"] = Spark::Json::Value(static_cast<double>(snapshot.capturedUnixMilliseconds));
        root["recentOperations"] = Spark::Json::Value::MakeArray();
        for (const std::string& operation : snapshot.recentOperations)
            root["recentOperations"].PushBack(Spark::Json::Value(operation));
        return root;
    }

    inline bool ReadStringField(const Spark::Json::Value& root, const char* key, std::string& output,
                                std::string& error)
    {
        const std::string name(key);
        if (!root.HasKey(name) || !root[name].IsString())
        {
            error = "recovery field is missing or not a string: " + name;
            return false;
        }
        output = root[name].AsString();
        return true;
    }

    inline std::optional<EditorRecoverySnapshot> SnapshotFromJson(const Spark::Json::Value& root, std::string& error)
    {
        if (!root.IsObject())
        {
            error = "recovery root is not an object";
            return std::nullopt;
        }
        if (!root.HasKey("schemaVersion") || !root["schemaVersion"].IsNumber())
        {
            error = "recovery schema version is missing";
            return std::nullopt;
        }

        const double schemaNumber = root["schemaVersion"].AsNumber();
        if (!std::isfinite(schemaNumber) || schemaNumber != static_cast<double>(kEditorRecoverySchemaVersion))
        {
            error = "unsupported recovery schema version";
            return std::nullopt;
        }

        EditorRecoverySnapshot snapshot;
        if (!ReadStringField(root, "projectIdentity", snapshot.projectIdentity, error) ||
            !ReadStringField(root, "projectRelativeScene", snapshot.projectRelativeScene, error) ||
            !ReadStringField(root, "sceneDisplayName", snapshot.sceneDisplayName, error) ||
            !ReadStringField(root, "serializedWorld", snapshot.serializedWorld, error) ||
            !ReadStringField(root, "layoutIniPath", snapshot.layoutIniPath, error))
        {
            return std::nullopt;
        }
        if (!root.HasKey("dirtySequence") || !root["dirtySequence"].IsNumber() ||
            !root.HasKey("capturedUnixMilliseconds") || !root["capturedUnixMilliseconds"].IsNumber())
        {
            error = "recovery sequence or timestamp is missing";
            return std::nullopt;
        }

        const double dirtySequence = root["dirtySequence"].AsNumber();
        const double capturedTime = root["capturedUnixMilliseconds"].AsNumber();
        if (!std::isfinite(dirtySequence) || dirtySequence < 0.0 ||
            dirtySequence > static_cast<double>(kMaxExactJsonInteger) || std::trunc(dirtySequence) != dirtySequence ||
            !std::isfinite(capturedTime) || capturedTime < -static_cast<double>(kMaxExactJsonInteger) ||
            capturedTime > static_cast<double>(kMaxExactJsonInteger) || std::trunc(capturedTime) != capturedTime)
        {
            error = "recovery sequence or timestamp is outside its supported range";
            return std::nullopt;
        }
        snapshot.dirtySequence = static_cast<uint64_t>(dirtySequence);
        snapshot.capturedUnixMilliseconds = static_cast<int64_t>(capturedTime);

        if (!root.HasKey("recentOperations") || !root["recentOperations"].IsArray())
        {
            error = "recovery operations are missing";
            return std::nullopt;
        }
        if (root["recentOperations"].Size() > kMaxOperationCount)
        {
            error = "recovery operation count exceeds the limit";
            return std::nullopt;
        }
        for (size_t index = 0; index < root["recentOperations"].Size(); ++index)
        {
            const Spark::Json::Value& value = root["recentOperations"][index];
            if (!value.IsString() || value.AsString().size() > kMaxOperationBytes)
            {
                error = "recovery operation is malformed";
                return std::nullopt;
            }
            snapshot.recentOperations.push_back(value.AsString());
        }

        if (!ValidateSnapshot(snapshot, error))
            return std::nullopt;
        return snapshot;
    }
} // namespace SparkEditor::RecoveryDetail
