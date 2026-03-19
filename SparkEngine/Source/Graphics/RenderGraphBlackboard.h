/**
 * @file RenderGraphBlackboard.h
 * @brief Type-erased inter-pass data sharing for the render graph
 * @author Spark Engine Team
 * @date 2026
 *
 * @see RenderGraphTypes.h, RenderGraph.h
 */

#pragma once

#include <any>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>

namespace Spark::Graphics
{

    // ============================================================================
    // RenderGraphBlackboard — type-erased inter-pass data sharing
    // ============================================================================

    /**
     * @brief Type-erased storage for passing structured data between passes.
     *
     * Each data type is stored at most once, keyed by its std::type_index.
     * Passes write data during setup and read it during execution.
     *
     * @code
     *   struct LightingData { RenderGraphResource shadowMap; };
     *   auto& data = blackboard.Add<LightingData>();
     *   data.shadowMap = builder.Create("ShadowMap", desc);
     *   // Later, in another pass:
     *   const auto& data = blackboard.Get<LightingData>();
     * @endcode
     */
    class RenderGraphBlackboard
    {
      public:
        RenderGraphBlackboard() = default;
        ~RenderGraphBlackboard() = default;

        // Non-copyable, movable
        RenderGraphBlackboard(const RenderGraphBlackboard&) = delete;
        RenderGraphBlackboard& operator=(const RenderGraphBlackboard&) = delete;
        RenderGraphBlackboard(RenderGraphBlackboard&&) noexcept = default;
        RenderGraphBlackboard& operator=(RenderGraphBlackboard&&) noexcept = default;

        /**
         * @brief Add or replace data of type T.
         *
         * Constructs a T in-place. If a T already exists, it is replaced.
         *
         * @tparam T   Data type (must be movable).
         * @tparam Args Constructor argument types.
         * @param args  Forwarded to T's constructor.
         * @return Reference to the stored T.
         */
        template <typename T, typename... Args> T& Add(Args&&... args)
        {
            auto key = std::type_index(typeid(T));
            m_storage[key] = std::make_any<T>(std::forward<Args>(args)...);
            return std::any_cast<T&>(m_storage[key]);
        }

        /**
         * @brief Retrieve mutable reference to stored data of type T.
         * @throws std::bad_any_cast if T has not been added.
         */
        template <typename T> T& Get()
        {
            auto key = std::type_index(typeid(T));
            auto it = m_storage.find(key);
            if (it == m_storage.end())
            {
                throw std::bad_any_cast();
            }
            return std::any_cast<T&>(it->second);
        }

        /**
         * @brief Retrieve const reference to stored data of type T.
         * @throws std::bad_any_cast if T has not been added.
         */
        template <typename T> const T& Get() const
        {
            auto key = std::type_index(typeid(T));
            auto it = m_storage.find(key);
            if (it == m_storage.end())
            {
                throw std::bad_any_cast();
            }
            return std::any_cast<const T&>(it->second);
        }

        /**
         * @brief Try to retrieve data of type T without throwing.
         * @return Pointer to the stored T, or nullptr if not present.
         */
        template <typename T> T* TryGet()
        {
            auto key = std::type_index(typeid(T));
            auto it = m_storage.find(key);
            if (it == m_storage.end())
            {
                return nullptr;
            }
            return std::any_cast<T>(&it->second);
        }

        /**
         * @brief Try to retrieve const data of type T without throwing.
         */
        template <typename T> const T* TryGet() const
        {
            auto key = std::type_index(typeid(T));
            auto it = m_storage.find(key);
            if (it == m_storage.end())
            {
                return nullptr;
            }
            return std::any_cast<const T>(&it->second);
        }

        /**
         * @brief Check whether data of type T exists.
         */
        template <typename T> bool Has() const { return m_storage.count(std::type_index(typeid(T))) > 0; }

        /**
         * @brief Remove data of type T.
         */
        template <typename T> void Remove() { m_storage.erase(std::type_index(typeid(T))); }

        /**
         * @brief Remove all stored data.
         */
        void Clear() { m_storage.clear(); }

      private:
        std::unordered_map<std::type_index, std::any> m_storage;
    };

} // namespace Spark::Graphics
