/**
 * @file MaterialLoader.h
 * @brief Data-driven material loading from .sparkmat text files
 * @author Spark Engine Team
 * @date 2026
 *
 * Loads materials from a simple key=value text format without requiring
 * a JSON library dependency. Each .sparkmat file defines one material
 * with PBR properties and texture paths.
 *
 * ## .sparkmat file format
 * @code
 * // Comment lines start with //
 * name = Brick_01
 * blendMode = Opaque
 * metallic = 0.0
 * roughness = 0.8
 * albedoTexture = textures/brick_albedo.dds
 * normalTexture = textures/brick_normal.dds
 * @endcode
 *
 * @see MaterialSystem.h
 */

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Graphics
{

    /**
     * @brief Parsed material definition from a .sparkmat file.
     *
     * Intermediate representation before creating an actual Material object.
     */
    struct MaterialDefinition
    {
        std::string name;
        std::string blendMode = "Opaque";
        float metallic = 0.0f;
        float roughness = 0.5f;
        float normalScale = 1.0f;
        float occlusionStrength = 1.0f;
        float emissiveFactor = 0.0f;
        float alphaCutoff = 0.5f;
        std::string albedoTexture;
        std::string normalTexture;
        std::string metallicTexture;
        std::string roughnessTexture;
        std::string emissiveTexture;
        std::string occlusionTexture;
    };

    /**
     * @class MaterialLoader
     * @brief Singleton that loads materials from .sparkmat text files.
     *
     * Parses key=value pairs and registers materials with the MaterialSystem.
     * Does not own Material objects — they live in the MaterialSystem.
     */
    class MaterialLoader
    {
      public:
        [[deprecated("Use EngineContext::Get()->GetSystem<MaterialLoader>() instead")]]
        static MaterialLoader& GetInstance();

        /**
         * @brief Load a single .sparkmat file and register the material.
         * @param filePath Path to the .sparkmat file.
         * @return true if parsing and registration succeeded.
         */
        bool LoadMaterial(const std::string& filePath);

        /**
         * @brief Load all .sparkmat files from a directory (non-recursive).
         * @param dirPath Directory containing .sparkmat files.
         * @return true if at least one material was loaded successfully.
         */
        bool LoadMaterialsFromDirectory(const std::string& dirPath);

        /** @brief Get the number of materials loaded by this loader. */
        size_t GetLoadedCount() const { return m_loadedNames.size(); }

        /** @brief Clear tracked material names. */
        void Shutdown();

      private:
        MaterialLoader() = default;

        /**
         * @brief Parse a .sparkmat file into a MaterialDefinition.
         * @param filePath Path to the file.
         * @param outDef   Parsed definition output.
         * @return true if the file was parsed successfully.
         */
        bool ParseFile(const std::string& filePath, MaterialDefinition& outDef) const;

        /**
         * @brief Register a parsed MaterialDefinition with the MaterialSystem.
         * @param def The parsed material definition.
         * @return true if the material was created successfully.
         */
        bool RegisterMaterial(const MaterialDefinition& def);

        std::vector<std::string> m_loadedNames;
    };

} // namespace Spark::Graphics
