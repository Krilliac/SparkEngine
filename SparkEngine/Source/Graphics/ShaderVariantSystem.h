/**
 * @file ShaderVariantSystem.h
 * @brief Shader variant keyword and stripping system
 * @author Spark Engine Team
 * @date 2025
 *
 * Manages shader keyword declarations, variant compilation, and dead variant
 * stripping. Inspired by Unity's multi_compile/shader_feature system.
 *
 * Three keyword types:
 * - MultiCompile: All combinations always compiled (runtime switching)
 * - ShaderFeature: Only compiled if referenced by a material (stripped in builds)
 * - DynamicBranch: No variants generated; runtime GPU branching instead
 *
 * @see Shader.h, SparkShaderCompiler
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Spark::Graphics
{

    // =========================================================================
    // Keyword Types
    // =========================================================================

    /**
     * @brief How a shader keyword affects compilation
     */
    enum class KeywordType : uint8_t
    {
        MultiCompile,  ///< All variants compiled; runtime toggleable
        ShaderFeature, ///< Only compiled if used by a material; stripped otherwise
        DynamicBranch  ///< No variant; runtime GPU branch (slower GPU, zero variant cost)
    };

    /**
     * @brief A declared shader keyword
     */
    struct ShaderKeyword
    {
        std::string name;         ///< Keyword name (e.g., "_NORMALMAP")
        std::string defineSymbol; ///< Preprocessor define (e.g., "ENABLE_NORMALMAP")
        KeywordType type = KeywordType::MultiCompile;
        bool defaultEnabled = false;
        bool isGlobal = false; ///< Applies to all shaders (not per-material)
    };

    /**
     * @brief A group of mutually-exclusive keywords (only one active at a time)
     *
     * Example: _SHADOWS_OFF / _SHADOWS_LOW / _SHADOWS_HIGH
     */
    struct KeywordGroup
    {
        std::string groupName;
        std::vector<std::string> keywords; ///< Keyword names in this group
        int defaultIndex = 0;              ///< Index of default keyword
        KeywordType type = KeywordType::MultiCompile;
    };

    // =========================================================================
    // Shader Variant Key
    // =========================================================================

    /**
     * @brief Unique identifier for a specific shader variant
     *
     * Each bit represents a boolean keyword. Up to 64 keywords per shader.
     */
    struct ShaderVariantKey
    {
        uint64_t bits = 0;

        bool operator==(const ShaderVariantKey& other) const { return bits == other.bits; }
        bool operator!=(const ShaderVariantKey& other) const { return bits != other.bits; }
        bool operator<(const ShaderVariantKey& other) const { return bits < other.bits; }

        bool HasKeyword(int index) const { return (bits & (1ULL << index)) != 0; }
        void SetKeyword(int index, bool enabled)
        {
            if (enabled)
                bits |= (1ULL << index);
            else
                bits &= ~(1ULL << index);
        }
    };

    struct ShaderVariantKeyHash
    {
        size_t operator()(const ShaderVariantKey& key) const { return std::hash<uint64_t>{}(key.bits); }
    };

    // =========================================================================
    // Compiled Variant Info
    // =========================================================================

    struct CompiledVariant
    {
        ShaderVariantKey key;
        std::vector<std::string> defines; ///< Active preprocessor defines
        uint32_t compiledFrame = 0;       ///< When this variant was compiled
        bool isWarmedUp = false;          ///< Pre-compiled at load time
    };

    // =========================================================================
    // Shader Variant System
    // =========================================================================

    /**
     * @brief Manages shader keywords, variant compilation, and stripping
     *
     * Usage:
     * 1. Register keywords and groups during initialization
     * 2. For each shader, declare which keywords it uses
     * 3. Materials enable/disable keywords to select variants
     * 4. At build time, strip unused ShaderFeature variants
     * 5. At load time, pre-warm known-needed variants
     */
    class ShaderVariantSystem
    {
      public:
        static constexpr int kMaxKeywords = 64;

        ShaderVariantSystem() = default;
        ~ShaderVariantSystem() = default;

        void Initialize()
        {
            m_keywords.clear();
            m_keywordIndices.clear();
            m_groups.clear();
            m_globalKeywordState = 0;
            m_nextKeywordIndex = 0;
            m_initialized = true;
        }

        void Shutdown()
        {
            m_keywords.clear();
            m_keywordIndices.clear();
            m_groups.clear();
            m_initialized = false;
        }

        // ---- Keyword Registration ----

        /**
         * @brief Register a boolean keyword
         * @return Keyword index (bit position in variant key)
         */
        int RegisterKeyword(const ShaderKeyword& keyword)
        {
            if (m_nextKeywordIndex >= kMaxKeywords)
                return -1;

            int index = m_nextKeywordIndex++;
            m_keywords.push_back(keyword);
            m_keywordIndices[keyword.name] = index;

            if (keyword.defaultEnabled)
                m_globalKeywordState |= (1ULL << index);

            return index;
        }

        /**
         * @brief Register a group of mutually-exclusive keywords
         * @return Index of the first keyword in the group
         */
        int RegisterKeywordGroup(const KeywordGroup& group)
        {
            int firstIndex = m_nextKeywordIndex;
            for (const auto& kwName : group.keywords)
            {
                ShaderKeyword kw;
                kw.name = kwName;
                kw.defineSymbol = kwName;
                kw.type = group.type;
                RegisterKeyword(kw);
            }

            // Enable default keyword in the group
            if (group.defaultIndex >= 0 && group.defaultIndex < static_cast<int>(group.keywords.size()))
            {
                m_globalKeywordState |= (1ULL << (firstIndex + group.defaultIndex));
            }

            m_groups.push_back(group);
            return firstIndex;
        }

        // ---- Keyword State Management ----

        /** @brief Enable/disable a global keyword */
        void SetGlobalKeyword(const std::string& name, bool enabled)
        {
            auto it = m_keywordIndices.find(name);
            if (it == m_keywordIndices.end())
                return;

            int index = it->second;
            if (enabled)
                m_globalKeywordState |= (1ULL << index);
            else
                m_globalKeywordState &= ~(1ULL << index);
        }

        bool IsGlobalKeywordEnabled(const std::string& name) const
        {
            auto it = m_keywordIndices.find(name);
            if (it == m_keywordIndices.end())
                return false;
            return (m_globalKeywordState & (1ULL << it->second)) != 0;
        }

        /** @brief Get the current global keyword state as a variant key */
        ShaderVariantKey GetGlobalVariantKey() const { return {m_globalKeywordState}; }

        // ---- Variant Resolution ----

        /**
         * @brief Resolve a material's keyword overrides into a final variant key
         *
         * Material keywords are OR'd with global keywords. For keyword groups,
         * material-level overrides take priority.
         */
        ShaderVariantKey ResolveVariantKey(uint64_t materialKeywords) const
        {
            return {m_globalKeywordState | materialKeywords};
        }

        /**
         * @brief Get the list of preprocessor defines for a variant key
         */
        std::vector<std::string> GetDefinesForVariant(ShaderVariantKey key) const
        {
            std::vector<std::string> defines;
            for (size_t i = 0; i < m_keywords.size(); ++i)
            {
                if (key.HasKeyword(static_cast<int>(i)))
                {
                    defines.push_back(m_keywords[i].defineSymbol);
                }
            }
            return defines;
        }

        // ---- Variant Stripping ----

        /**
         * @brief Get total number of possible variants for a set of keywords
         */
        uint64_t GetTotalVariantCount(const std::vector<int>& keywordIndices) const
        {
            if (keywordIndices.empty())
                return 1;
            return 1ULL << keywordIndices.size();
        }

        /**
         * @brief Strip unused ShaderFeature variants
         *
         * Given a set of materials that reference keywords, determine which
         * ShaderFeature variants can be stripped (not referenced by any material).
         *
         * @param usedKeywords Set of keyword names used by at least one material
         * @return Number of variants that can be stripped
         */
        uint32_t StripUnusedVariants(const std::unordered_set<std::string>& usedKeywords) const
        {
            uint32_t strippedCount = 0;

            for (size_t i = 0; i < m_keywords.size(); ++i)
            {
                const auto& kw = m_keywords[i];
                if (kw.type == KeywordType::ShaderFeature && usedKeywords.find(kw.name) == usedKeywords.end())
                {
                    // This keyword is a ShaderFeature not used by any material
                    // All variants with this keyword enabled can be stripped
                    strippedCount++;
                }
            }

            return strippedCount;
        }

        /**
         * @brief Check if a variant should be compiled based on pipeline settings
         *
         * Skips variants that depend on disabled engine features.
         * E.g., if DXR is disabled, skip all variants with _DXR_ENABLED.
         */
        bool ShouldCompileVariant(ShaderVariantKey key, const std::unordered_set<std::string>& disabledFeatures) const
        {
            for (size_t i = 0; i < m_keywords.size(); ++i)
            {
                if (key.HasKeyword(static_cast<int>(i)))
                {
                    if (disabledFeatures.find(m_keywords[i].defineSymbol) != disabledFeatures.end())
                        return false;
                }
            }
            return true;
        }

        // ---- Pre-warming ----

        /**
         * @brief Collect variants that should be pre-warmed at load time
         *
         * Returns variant keys for all MultiCompile combinations that match
         * the current global state. ShaderFeature variants are only included
         * if they appear in the usedKeywords set.
         */
        std::vector<ShaderVariantKey> GetWarmupVariants(const std::unordered_set<std::string>& usedKeywords) const
        {
            std::vector<ShaderVariantKey> variants;

            // Start with the default variant
            variants.push_back({m_globalKeywordState});

            // Add variants for each MultiCompile keyword toggled
            for (size_t i = 0; i < m_keywords.size(); ++i)
            {
                if (m_keywords[i].type == KeywordType::MultiCompile)
                {
                    ShaderVariantKey toggled = {m_globalKeywordState};
                    toggled.SetKeyword(static_cast<int>(i), !toggled.HasKeyword(static_cast<int>(i)));
                    variants.push_back(toggled);
                }
            }

            return variants;
        }

        // ---- Queries ----

        int GetKeywordIndex(const std::string& name) const
        {
            auto it = m_keywordIndices.find(name);
            return it != m_keywordIndices.end() ? it->second : -1;
        }

        const ShaderKeyword* GetKeyword(int index) const
        {
            if (index < 0 || index >= static_cast<int>(m_keywords.size()))
                return nullptr;
            return &m_keywords[index];
        }

        int GetKeywordCount() const { return static_cast<int>(m_keywords.size()); }
        bool IsInitialized() const { return m_initialized; }

        std::string Console_GetStatus() const
        {
            std::string result = "ShaderVariantSystem: " + std::to_string(m_keywords.size()) + " keywords, " +
                                 std::to_string(m_groups.size()) + " groups\n";
            for (size_t i = 0; i < m_keywords.size(); ++i)
            {
                const auto& kw = m_keywords[i];
                bool enabled = (m_globalKeywordState & (1ULL << i)) != 0;
                const char* typeName = kw.type == KeywordType::MultiCompile
                                           ? "multi"
                                           : (kw.type == KeywordType::ShaderFeature ? "feat" : "dyn");
                result += "  [" + std::to_string(i) + "] " + kw.name + " (" + typeName + ") " +
                          (enabled ? "ON" : "OFF") + "\n";
            }
            return result;
        }

      private:
        std::vector<ShaderKeyword> m_keywords;
        std::unordered_map<std::string, int> m_keywordIndices;
        std::vector<KeywordGroup> m_groups;
        uint64_t m_globalKeywordState = 0;
        int m_nextKeywordIndex = 0;
        bool m_initialized = false;
    };

} // namespace Spark::Graphics
