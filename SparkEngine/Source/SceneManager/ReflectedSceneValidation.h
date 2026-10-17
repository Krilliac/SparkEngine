/**
 * @file ReflectedSceneValidation.h
 * @brief Internal version and schema checks for the reflected-World scene dialect.
 *
 * Shared by ReflectedSceneSerializer.cpp only. Every check that rejects a document
 * writes an actionable reason (file version plus supported window, or the offending
 * entity, id, parent, component or field) to the optional @c error output.
 */

#pragma once

#include "Core/Reflection.h"

#include <nlohmann_json.h>

#include <cstddef>
#include <string>

namespace Spark::ReflectedSceneDetail
{
    /// The reflected dialect has shipped exactly one `version` value, so its read
    /// window is that version plus the pre-reflection editor dialect
    /// (`sceneVersion: 1`), which migrates in memory on load.
    inline constexpr int kCurrentSceneVersion = 1;
    inline constexpr int kLegacyEditorSceneVersion = 1;

    /// @brief Store @p message in @p error (when non-null) and return false.
    bool Reject(std::string* error, std::string message);

    /// @brief Human-readable JSON type name for diagnostics.
    const char* JsonTypeName(const nlohmann::json& value);

    /// @brief "entity #N ('name')" label for diagnostics.
    std::string DescribeEntity(size_t index, const nlohmann::json& entity);

    /**
     * @brief Validate the version declaration of a scene root object.
     * @param root Parsed scene root (must be an object).
     * @param legacyScene Set to true when the document uses the legacy `sceneVersion` dialect.
     * @param error Receives the file's version field and value plus the supported window on rejection.
     * @return true when the version is inside the supported window.
     */
    bool CheckSceneVersion(const nlohmann::json& root, bool& legacyScene, std::string* error);

    /// @brief Components serialized at entity level rather than in "components".
    bool IsEntityLevel(const std::string& type);

    /// @brief True for serialized fields whose type the reflected dialect round-trips.
    bool IsRoundTrippableField(const FieldInfo& field);

    /**
     * @brief Validate a crash-recovery record against this build's reflected schema.
     * @param root Parsed document whose version was already accepted by CheckSceneVersion.
     * @param factory Component registry the record's types must be registered in.
     * @param error Receives the offending entity, id, parent, component or field on rejection.
     * @return true when every entity, component and reflected field is understood.
     */
    bool ValidateStrictRecoveryDocument(const nlohmann::json& root, ComponentFactory& factory, std::string* error);
} // namespace Spark::ReflectedSceneDetail
