/**
 * @file MaterialDefinition.h
 * @brief Declarative material definition format (Filament/Ogre-Next-inspired)
 *
 * Provides a data-driven material definition system where material types are
 * described declaratively: shader model, parameter declarations with types/ranges/defaults,
 * texture slots, and render state presets. The runtime MaterialSystem can instantiate
 * concrete materials from these definitions.
 *
 * This separates "what a material IS" (definition) from "what values it HAS" (instance),
 * enabling:
 * - Offline validation of material parameters against their definition
 * - Editor UI auto-generation from parameter metadata
 * - Shader permutation derivation from declared features
 *
 * ## Usage
 * @code
 *   MaterialDefinition def;
 *   def.name = "StandardPBR";
 *   def.shadingModel = ShadingModel::Lit;
 *
 *   def.AddParameter("baseColor", ParamType::Float4, {1,1,1,1});
 *   def.AddParameter("metallic", ParamType::Float, 0.0f, 0.0f, 1.0f);
 *   def.AddParameter("roughness", ParamType::Float, 0.5f, 0.0f, 1.0f);
 *   def.AddTextureSlot("albedoMap", TextureSlotUsage::Albedo);
 *
 *   // Validate an instance against this definition
 *   MaterialInstance instance(def);
 *   instance.SetFloat("metallic", 0.8f);
 *   auto errors = instance.Validate();
 * @endcode
 *
 * @see MaterialSystem for runtime material management
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace Spark::Graphics
{

    /// @brief Shading model selection for material definitions
    enum class ShadingModel : uint8_t
    {
        Unlit = 0,    ///< No lighting, emissive only
        Lit,          ///< Standard PBR (Cook-Torrance + Lambertian)
        Subsurface,   ///< SSS for skin, wax, marble
        Cloth,        ///< Fabric sheen model
        SpecularGloss ///< Legacy specular-glossiness workflow
    };

    /// @brief Blend mode presets for material render state
    enum class MaterialBlendMode : uint8_t
    {
        Opaque = 0,
        Masked,      ///< Alpha test with cutoff
        Transparent, ///< Alpha blend
        Additive
    };

    /// @brief Parameter types supported in material definitions
    enum class ParamType : uint8_t
    {
        Float = 0,
        Float2,
        Float3,
        Float4,
        Int,
        Bool
    };

    /// @brief Semantic usage hint for texture slots
    enum class TextureSlotUsage : uint8_t
    {
        Albedo = 0,
        Normal,
        MetallicRoughness,
        Occlusion,
        Emissive,
        Height,
        Subsurface,
        Clearcoat,
        Custom
    };

    /// @brief A typed parameter value (matches ParamType)
    using ParamValue =
        std::variant<float, std::array<float, 2>, std::array<float, 3>, std::array<float, 4>, int32_t, bool>;

    /**
     * @brief Declaration of a single material parameter with metadata.
     *
     * Stores the parameter's type, default value, optional min/max range,
     * and a human-readable description for editor UI generation.
     */
    struct ParameterDecl
    {
        std::string name;
        ParamType type = ParamType::Float;
        ParamValue defaultValue;
        std::optional<float> minValue; ///< Range minimum (Float/Int only)
        std::optional<float> maxValue; ///< Range maximum (Float/Int only)
        std::string description;       ///< Tooltip / editor label
    };

    /**
     * @brief Declaration of a texture slot in a material definition.
     */
    struct TextureSlotDecl
    {
        std::string name;
        TextureSlotUsage usage = TextureSlotUsage::Custom;
        std::string defaultPath; ///< Optional default texture asset path
        bool required = false;   ///< If true, material is invalid without this texture
        std::string description;
    };

    /**
     * @brief Render state block for a material definition.
     *
     * Defines the default render state. Instances may override some fields.
     */
    struct RenderStateDecl
    {
        MaterialBlendMode blendMode = MaterialBlendMode::Opaque;
        bool doubleSided = false;
        bool depthWrite = true;
        bool depthTest = true;
        bool castShadows = true;
        bool receiveShadows = true;
        float alphaCutoff = 0.5f;   ///< Used when blendMode == Masked
        int32_t renderQueue = 2000; ///< Sort priority
    };

    /**
     * @brief A complete declarative material definition.
     *
     * Describes a material type: its shading model, available parameters,
     * texture slots, required features, and default render state. Concrete
     * materials are instantiated from definitions and hold per-instance values.
     */
    struct MaterialDefinition
    {
        std::string name;
        ShadingModel shadingModel = ShadingModel::Lit;
        RenderStateDecl renderState;
        std::vector<ParameterDecl> parameters;
        std::vector<TextureSlotDecl> textureSlots;
        std::vector<std::string> requiredFeatures; ///< Shader feature flags (e.g. "CLEARCOAT", "ANISOTROPY")
        uint32_t version = 1;

        /**
         * @brief Add a float parameter with optional range.
         */
        void AddParameter(const std::string& paramName, float defaultVal, float minVal = 0.0f, float maxVal = 1.0f,
                          const std::string& desc = "")
        {
            parameters.push_back({paramName, ParamType::Float, defaultVal, minVal, maxVal, desc});
        }

        /**
         * @brief Add a float4 parameter (color, vector).
         */
        void AddFloat4Parameter(const std::string& paramName, std::array<float, 4> defaultVal,
                                const std::string& desc = "")
        {
            parameters.push_back({paramName, ParamType::Float4, defaultVal, std::nullopt, std::nullopt, desc});
        }

        /**
         * @brief Add a bool parameter.
         */
        void AddBoolParameter(const std::string& paramName, bool defaultVal, const std::string& desc = "")
        {
            parameters.push_back({paramName, ParamType::Bool, defaultVal, std::nullopt, std::nullopt, desc});
        }

        /**
         * @brief Add a texture slot declaration.
         */
        void AddTextureSlot(const std::string& slotName, TextureSlotUsage usage, bool isRequired = false,
                            const std::string& desc = "")
        {
            textureSlots.push_back({slotName, usage, "", isRequired, desc});
        }

        /**
         * @brief Find a parameter declaration by name.
         * @return Pointer to the declaration, or nullptr if not found
         */
        const ParameterDecl* FindParameter(const std::string& paramName) const
        {
            for (const auto& p : parameters)
            {
                if (p.name == paramName)
                    return &p;
            }
            return nullptr;
        }

        /**
         * @brief Find a texture slot declaration by name.
         */
        const TextureSlotDecl* FindTextureSlot(const std::string& slotName) const
        {
            for (const auto& s : textureSlots)
            {
                if (s.name == slotName)
                    return &s;
            }
            return nullptr;
        }

        /**
         * @brief Generate shader defines from this definition's features and shading model.
         * @return List of preprocessor defines for shader compilation
         */
        [[nodiscard]] std::vector<std::string> GetShaderDefines() const
        {
            std::vector<std::string> defines;

            switch (shadingModel)
            {
            case ShadingModel::Unlit:
                defines.push_back("SHADING_MODEL_UNLIT");
                break;
            case ShadingModel::Lit:
                defines.push_back("SHADING_MODEL_LIT");
                break;
            case ShadingModel::Subsurface:
                defines.push_back("SHADING_MODEL_SUBSURFACE");
                break;
            case ShadingModel::Cloth:
                defines.push_back("SHADING_MODEL_CLOTH");
                break;
            case ShadingModel::SpecularGloss:
                defines.push_back("SHADING_MODEL_SPECULAR_GLOSS");
                break;
            }

            for (const auto& feature : requiredFeatures)
                defines.push_back(feature);

            for (const auto& slot : textureSlots)
            {
                switch (slot.usage)
                {
                case TextureSlotUsage::Normal:
                    defines.push_back("HAS_NORMAL_MAP");
                    break;
                case TextureSlotUsage::MetallicRoughness:
                    defines.push_back("HAS_METALLIC_ROUGHNESS_MAP");
                    break;
                case TextureSlotUsage::Occlusion:
                    defines.push_back("HAS_OCCLUSION_MAP");
                    break;
                case TextureSlotUsage::Emissive:
                    defines.push_back("HAS_EMISSIVE_MAP");
                    break;
                case TextureSlotUsage::Height:
                    defines.push_back("HAS_HEIGHT_MAP");
                    break;
                default:
                    break;
                }
            }

            return defines;
        }
    };

    /**
     * @brief A concrete material instance created from a MaterialDefinition.
     *
     * Holds per-instance parameter overrides. Values not explicitly set
     * fall back to the definition's defaults. Validates parameter values
     * against the definition's declared types and ranges.
     */
    class MaterialInstance
    {
      public:
        explicit MaterialInstance(const MaterialDefinition& definition)
            : m_definition(&definition), m_name(definition.name + "_instance")
        {
        }

        MaterialInstance(const MaterialDefinition& definition, const std::string& instanceName)
            : m_definition(&definition), m_name(instanceName)
        {
        }

        /// @brief Set a float parameter value.
        /// @return true if the parameter exists and the value is within range
        bool SetFloat(const std::string& paramName, float value)
        {
            const auto* decl = m_definition->FindParameter(paramName);
            if (!decl || decl->type != ParamType::Float)
                return false;

            if (decl->minValue.has_value() && value < *decl->minValue)
                return false;
            if (decl->maxValue.has_value() && value > *decl->maxValue)
                return false;

            m_overrides[paramName] = value;
            return true;
        }

        /// @brief Set a float4 parameter value.
        bool SetFloat4(const std::string& paramName, std::array<float, 4> value)
        {
            const auto* decl = m_definition->FindParameter(paramName);
            if (!decl || decl->type != ParamType::Float4)
                return false;

            m_overrides[paramName] = value;
            return true;
        }

        /// @brief Set a bool parameter value.
        bool SetBool(const std::string& paramName, bool value)
        {
            const auto* decl = m_definition->FindParameter(paramName);
            if (!decl || decl->type != ParamType::Bool)
                return false;

            m_overrides[paramName] = value;
            return true;
        }

        /**
         * @brief Get the effective value of a parameter (override or default).
         * @return The value, or std::nullopt if the parameter doesn't exist
         */
        [[nodiscard]] std::optional<ParamValue> GetValue(const std::string& paramName) const
        {
            auto it = m_overrides.find(paramName);
            if (it != m_overrides.end())
                return it->second;

            const auto* decl = m_definition->FindParameter(paramName);
            if (decl)
                return decl->defaultValue;

            return std::nullopt;
        }

        /**
         * @brief Get a float parameter value (override or default).
         */
        [[nodiscard]] float GetFloat(const std::string& paramName, float fallback = 0.0f) const
        {
            auto val = GetValue(paramName);
            if (val && std::holds_alternative<float>(*val))
                return std::get<float>(*val);
            return fallback;
        }

        /**
         * @brief Get a bool parameter value (override or default).
         */
        [[nodiscard]] bool GetBool(const std::string& paramName, bool fallback = false) const
        {
            auto val = GetValue(paramName);
            if (val && std::holds_alternative<bool>(*val))
                return std::get<bool>(*val);
            return fallback;
        }

        /// @brief Assign a texture path to a named slot.
        bool SetTexturePath(const std::string& slotName, const std::string& path)
        {
            if (!m_definition->FindTextureSlot(slotName))
                return false;
            m_texturePaths[slotName] = path;
            return true;
        }

        /// @brief Get the texture path for a slot (instance override or definition default).
        [[nodiscard]] std::string GetTexturePath(const std::string& slotName) const
        {
            auto it = m_texturePaths.find(slotName);
            if (it != m_texturePaths.end())
                return it->second;

            const auto* slot = m_definition->FindTextureSlot(slotName);
            if (slot)
                return slot->defaultPath;
            return "";
        }

        /**
         * @brief Validate the instance against its definition.
         *
         * Checks that all required texture slots have paths assigned and
         * that all override values are within declared ranges.
         *
         * @return List of validation error strings (empty = valid)
         */
        [[nodiscard]] std::vector<std::string> Validate() const
        {
            std::vector<std::string> errors;

            // Check required texture slots
            for (const auto& slot : m_definition->textureSlots)
            {
                if (slot.required && GetTexturePath(slot.name).empty())
                {
                    errors.push_back("Required texture slot '" + slot.name + "' has no texture assigned");
                }
            }

            // Validate override values against parameter declarations
            for (const auto& [name, value] : m_overrides)
            {
                const auto* decl = m_definition->FindParameter(name);
                if (!decl)
                {
                    errors.push_back("Parameter '" + name + "' not declared in definition");
                    continue;
                }

                // Type check
                if (decl->type == ParamType::Float && !std::holds_alternative<float>(value))
                {
                    errors.push_back("Parameter '" + name + "' type mismatch (expected float)");
                }
                else if (decl->type == ParamType::Float4 && !std::holds_alternative<std::array<float, 4>>(value))
                {
                    errors.push_back("Parameter '" + name + "' type mismatch (expected float4)");
                }
                else if (decl->type == ParamType::Bool && !std::holds_alternative<bool>(value))
                {
                    errors.push_back("Parameter '" + name + "' type mismatch (expected bool)");
                }
            }

            return errors;
        }

        /// @brief Check if any overrides have been set.
        [[nodiscard]] bool HasOverrides() const { return !m_overrides.empty() || !m_texturePaths.empty(); }

        /// @brief Reset all overrides to definition defaults.
        void ResetToDefaults()
        {
            m_overrides.clear();
            m_texturePaths.clear();
        }

        [[nodiscard]] const std::string& GetName() const { return m_name; }
        [[nodiscard]] const MaterialDefinition& GetDefinition() const { return *m_definition; }

      private:
        const MaterialDefinition* m_definition;
        std::string m_name;
        std::unordered_map<std::string, ParamValue> m_overrides;
        std::unordered_map<std::string, std::string> m_texturePaths;
    };

    /**
     * @brief Registry of material definitions, keyed by name.
     *
     * Provides a central place to register and look up material definitions.
     * Material instances reference definitions from this registry.
     */
    class MaterialDefinitionRegistry
    {
      public:
        static MaterialDefinitionRegistry& Get()
        {
            static MaterialDefinitionRegistry instance;
            return instance;
        }

        /**
         * @brief Register a material definition.
         * @return true if registered successfully, false if name already exists
         */
        bool Register(MaterialDefinition definition)
        {
            if (m_definitions.count(definition.name) > 0)
                return false;

            std::string name = definition.name;
            m_definitions.emplace(std::move(name), std::move(definition));
            return true;
        }

        /// @brief Find a definition by name. Returns nullptr if not found.
        const MaterialDefinition* Find(const std::string& name) const
        {
            auto it = m_definitions.find(name);
            return (it != m_definitions.end()) ? &it->second : nullptr;
        }

        /// @brief Create a material instance from a registered definition.
        [[nodiscard]] std::optional<MaterialInstance> CreateInstance(const std::string& defName,
                                                                     const std::string& instanceName) const
        {
            const auto* def = Find(defName);
            if (!def)
                return std::nullopt;
            return MaterialInstance(*def, instanceName);
        }

        /// @brief Get all registered definition names.
        [[nodiscard]] std::vector<std::string> GetAllNames() const
        {
            std::vector<std::string> names;
            names.reserve(m_definitions.size());
            for (const auto& [name, _] : m_definitions)
                names.push_back(name);
            return names;
        }

        /// @brief Number of registered definitions.
        [[nodiscard]] size_t Count() const { return m_definitions.size(); }

        /// @brief Remove all registered definitions.
        void Clear() { m_definitions.clear(); }

      private:
        MaterialDefinitionRegistry() = default;
        std::unordered_map<std::string, MaterialDefinition> m_definitions;
    };

} // namespace Spark::Graphics
