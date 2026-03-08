/**
 * @file entt.hpp
 * @brief Minimal EnTT stub for SparkEngine build compatibility.
 *
 * This is a lightweight stub providing the subset of the EnTT ECS library
 * used by SparkEngine. Replace with the full EnTT library for production use.
 * See: https://github.com/skypjack/entt
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace entt
{

    // ---------------------------------------------------------------------------
    // Entity type
    // ---------------------------------------------------------------------------
    enum class entity : std::uint32_t
    {
    };

    /** Null entity sentinel */
    inline constexpr entity null{static_cast<entity>(std::uint32_t(-1))};

    // ---------------------------------------------------------------------------
    // Internal: type-erased component pool
    // ---------------------------------------------------------------------------
    namespace internal
    {

        /** Base for type-erased storage. */
        struct pool_base
        {
            virtual ~pool_base() = default;
            virtual void remove(entity e) = 0;
            virtual bool contains(entity e) const = 0;
        };

        /** Typed component pool. */
        template <typename T> struct pool : pool_base
        {
            std::unordered_map<std::uint32_t, T> data;

            void remove(entity e) override { data.erase(static_cast<std::uint32_t>(e)); }
            bool contains(entity e) const override { return data.count(static_cast<std::uint32_t>(e)) != 0; }

            template <typename... Args> T& emplace(entity e, Args&&... args)
            {
                auto key = static_cast<std::uint32_t>(e);
                auto [it, _] = data.emplace(std::piecewise_construct, std::forward_as_tuple(key),
                                            std::forward_as_tuple(std::forward<Args>(args)...));
                return it->second;
            }

            T* try_get(entity e)
            {
                auto it = data.find(static_cast<std::uint32_t>(e));
                return it != data.end() ? &it->second : nullptr;
            }

            const T* try_get(entity e) const
            {
                auto it = data.find(static_cast<std::uint32_t>(e));
                return it != data.end() ? &it->second : nullptr;
            }
        };

        /** Compile-time type index. */
        inline std::size_t next_type_id()
        {
            static std::size_t id = 0;
            return id++;
        }

        template <typename T> std::size_t type_id()
        {
            static const std::size_t id = next_type_id();
            return id;
        }

    } // namespace internal

    // ---------------------------------------------------------------------------
    // Forward declarations
    // ---------------------------------------------------------------------------
    class registry;

    // ---------------------------------------------------------------------------
    // Entity storage – supports .each() iteration
    // ---------------------------------------------------------------------------
    class entity_storage
    {
      public:
        /** Proxy returned by each() for range-for iteration. */
        struct each_proxy
        {
            const std::vector<entity>& entities;

            struct iterator
            {
                const std::vector<entity>& entities;
                std::size_t idx;

                bool operator!=(const iterator& o) const { return idx != o.idx; }
                iterator& operator++()
                {
                    ++idx;
                    return *this;
                }
                std::tuple<entity> operator*() const { return std::make_tuple(entities[idx]); }
            };

            iterator begin() const { return {entities, 0}; }
            iterator end() const { return {entities, entities.size()}; }
        };

        each_proxy each() const { return {m_alive}; }
        std::size_t size() const { return m_alive.size(); }

      private:
        friend class registry;
        std::vector<entity> m_alive;

        entity create()
        {
            auto e = static_cast<entity>(m_next++);
            m_alive.push_back(e);
            return e;
        }

        void destroy(entity e) { m_alive.erase(std::remove(m_alive.begin(), m_alive.end(), e), m_alive.end()); }

        std::uint32_t m_next = 0;
    };

    // ---------------------------------------------------------------------------
    // View – filtered iteration over entities with specific components
    // ---------------------------------------------------------------------------
    template <typename... Components> class basic_view
    {
      public:
        basic_view(const std::vector<entity>& entities, std::tuple<internal::pool<Components>*...> pools)
            : m_entities(entities), m_pools(pools)
        {
            // Collect matching entities
            for (auto e : m_entities)
            {
                if ((std::get<internal::pool<Components>*>(m_pools)->contains(e) && ...))
                    m_matching.push_back(e);
            }
        }

        /** Range-for iteration yields entity IDs. */
        auto begin() const { return m_matching.begin(); }
        auto end() const { return m_matching.end(); }
        std::size_t size() const { return m_matching.size(); }

        /** Get a specific component for an entity from this view. */
        template <typename T> T& get(entity e)
        {
            auto* p = std::get<internal::pool<T>*>(m_pools)->try_get(e);
            return *p;
        }

        template <typename T> const T& get(entity e) const
        {
            auto* p = std::get<internal::pool<T>*>(m_pools)->try_get(e);
            return *p;
        }

        /** each() with structured bindings: for (auto [e, c1, c2] : view.each()) */
        struct each_proxy
        {
            const basic_view& v;

            struct iterator
            {
                const basic_view& v;
                std::size_t idx;

                bool operator!=(const iterator& o) const { return idx != o.idx; }
                iterator& operator++()
                {
                    ++idx;
                    return *this;
                }
                auto operator*() const
                {
                    entity e = v.m_matching[idx];
                    return std::tuple<entity, Components&...>{
                        e, *std::get<internal::pool<Components>*>(v.m_pools)->try_get(e)...};
                }
            };

            iterator begin() const { return {v, 0}; }
            iterator end() const { return {v, v.m_matching.size()}; }
        };

        each_proxy each() const { return {*this}; }

      private:
        const std::vector<entity>& m_entities;
        std::tuple<internal::pool<Components>*...> m_pools;
        std::vector<entity> m_matching;
    };

    // ---------------------------------------------------------------------------
    // Registry – the central ECS container
    // ---------------------------------------------------------------------------
    class registry
    {
      public:
        /** Create a new entity. */
        entity create() { return m_entities.create(); }

        /** Destroy an entity and remove all its components. */
        void destroy(entity e)
        {
            for (auto& [id, pool] : m_pools)
                pool->remove(e);
            m_entities.destroy(e);
        }

        /** Attach a component to an entity. */
        template <typename T, typename... Args> T& emplace(entity e, Args&&... args)
        {
            return get_or_create_pool<T>().emplace(e, std::forward<Args>(args)...);
        }

        /** Get a component pointer (nullptr if absent). */
        template <typename T> T* try_get(entity e) { return get_or_create_pool<T>().try_get(e); }

        template <typename T> const T* try_get(entity e) const
        {
            auto it = m_pools.find(internal::type_id<T>());
            if (it == m_pools.end())
                return nullptr;
            return static_cast<const internal::pool<T>*>(it->second.get())->try_get(e);
        }

        /** Check if an entity has a component. */
        template <typename T> bool all_of(entity e) const
        {
            auto it = m_pools.find(internal::type_id<T>());
            if (it == m_pools.end())
                return false;
            return it->second->contains(e);
        }

        /** Remove a component from an entity. */
        template <typename T> void remove(entity e)
        {
            auto it = m_pools.find(internal::type_id<T>());
            if (it != m_pools.end())
                it->second->remove(e);
        }

        /** Create a view for entities having all specified components. */
        template <typename... Components> basic_view<Components...> view()
        {
            return basic_view<Components...>(m_entities.m_alive, std::make_tuple(&get_or_create_pool<Components>()...));
        }

        /** Access the entity storage (returns pointer to match real EnTT API). */
        template <typename T> std::enable_if_t<std::is_same_v<T, entity>, entity_storage*> storage()
        {
            return &m_entities;
        }

        template <typename T> std::enable_if_t<std::is_same_v<T, entity>, const entity_storage*> storage() const
        {
            return &m_entities;
        }

      private:
        entity_storage m_entities;
        std::unordered_map<std::size_t, std::unique_ptr<internal::pool_base>> m_pools;

        template <typename T> internal::pool<T>& get_or_create_pool()
        {
            auto id = internal::type_id<T>();
            auto it = m_pools.find(id);
            if (it == m_pools.end())
            {
                auto p = std::make_unique<internal::pool<T>>();
                auto* raw = p.get();
                m_pools.emplace(id, std::move(p));
                return *raw;
            }
            return *static_cast<internal::pool<T>*>(it->second.get());
        }
    };

} // namespace entt
