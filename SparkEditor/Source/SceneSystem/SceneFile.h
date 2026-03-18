/**
 * @file SceneFile.h
 * @brief Scene file format definition and data structures
 * @author Spark Engine Team
 * @date 2025
 *
 * This file defines the scene file format used by the Spark Engine Editor
 * for saving and loading game scenes. The format supports both binary and
 * JSON serialization for different use cases.
 */

#pragma once

#include "SceneFileTypes.h"


namespace SparkEditor
{

    /**
 * @brief Complete scene file data structure
 */
    struct SceneFile
    {
        SceneHeader header;                          ///< Scene file header
        std::vector<SceneObject> objects;            ///< All scene objects
        std::vector<Component> components;           ///< All object components
        std::vector<AssetReference> assetReferences; ///< Referenced assets
        EnvironmentSettings environment;             ///< Environment settings
        Camera defaultCamera;                        ///< Default camera settings

        /**
     * @brief Get next available object ID
     * @return Unique object ID
     */
        ObjectID GetNextObjectID();

        /**
     * @brief Find object by ID
     * @param id Object ID to search for
     * @return Pointer to object, or nullptr if not found
     */
        SceneObject* FindObject(ObjectID id);

        /**
     * @brief Find objects by name
     * @param name Object name to search for
     * @return Vector of pointers to matching objects
     */
        std::vector<SceneObject*> FindObjectsByName(const std::string& name);

        /**
     * @brief Get components for an object
     * @param objectID Object to get components for
     * @return Vector of pointers to object's components
     */
        std::vector<Component*> GetObjectComponents(ObjectID objectID);

        /**
     * @brief Add asset reference if not already present
     * @param assetPath Path to asset
     * @param assetType Type of asset
     */
        void AddAssetReference(const std::string& assetPath, const std::string& assetType);

        /**
     * @brief Validate scene data integrity
     * @param errors Output vector for error messages
     * @return true if scene is valid, false if errors were found
     */
        bool Validate(std::vector<std::string>& errors) const;

        /**
     * @brief Update scene header with current data
     */
        void UpdateHeader();
    };

} // namespace SparkEditor