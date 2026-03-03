// ObjectPool.h
#pragma once

#include "Utils/Assert.h"
#include <vector>
#include <queue>
#include <memory>
#include <functional>

template<typename T>
class ObjectPool
{
private:
    std::vector<std::unique_ptr<T>>        m_objects;
    std::queue<T*>                         m_available;
    std::function<std::unique_ptr<T>()>    m_factory;
    size_t                                 m_maxSize;

public:
    explicit ObjectPool(size_t maxSize = 100)
        : m_maxSize(maxSize)
    {
        ASSERT_MSG(maxSize > 0, "ObjectPool maxSize must be positive");
        m_objects.reserve(maxSize);
    }

    ObjectPool(size_t maxSize, std::function<std::unique_ptr<T>()> factory)
        : m_maxSize(maxSize)
        , m_factory(factory)
    {
        ASSERT_MSG(maxSize > 0, "ObjectPool maxSize must be positive");
        ASSERT_MSG(factory != nullptr, "ObjectPool factory must not be null");
        m_objects.reserve(maxSize);

        // Pre-allocate objects
        for (size_t i = 0; i < maxSize; ++i)
        {
            auto obj = m_factory();
            ASSERT_MSG(obj != nullptr, "Factory returned null object");
            m_available.push(obj.get());
            m_objects.push_back(std::move(obj));
        }
    }

    ~ObjectPool() = default;

    // Acquire an object from the pool
    T* Acquire()
    {
        if (m_available.empty())
        {
            if (m_objects.size() < m_maxSize && m_factory)
            {
                auto obj = m_factory();
                ASSERT_MSG(obj != nullptr, "Factory returned null object");
                T* ptr = obj.get();
                m_objects.push_back(std::move(obj));
                return ptr;
            }
            return nullptr; // Pool exhausted
        }

        T* obj = m_available.front();
        m_available.pop();
        ASSERT_NOT_NULL(obj);
        return obj;
    }

    // Return an object to the pool
    void Release(T* obj)
    {
        if (!obj) return;
        // Reset object if it supports Reset()
        if constexpr (requires { obj->Reset(); })
        {
            obj->Reset();
        }
        m_available.push(obj);
    }

    // Stats
    size_t GetTotalSize()      const { return m_objects.size(); }
    size_t GetAvailableCount() const { return m_available.size(); }
    size_t GetUsedCount()      const { return m_objects.size() - m_available.size(); }
    size_t GetMaxSize()        const { return m_maxSize; }

    // Clear all objects
    void Clear()
    {
        m_objects.clear();
        std::queue<T*> empty;
        std::swap(m_available, empty);
    }

    // Pre-allocate with factory
    void PreAllocate(size_t count, std::function<std::unique_ptr<T>()> factory)
    {
        ASSERT_MSG(factory != nullptr, "PreAllocate factory must not be null");
        m_factory = factory;

        for (size_t i = 0; i < count && m_objects.size() < m_maxSize; ++i)
        {
            auto obj = m_factory();
            ASSERT_MSG(obj != nullptr, "Factory returned null object");
            m_available.push(obj.get());
            m_objects.push_back(std::move(obj));
        }
    }

    // Iterators over all objects
    auto begin() { return m_objects.begin(); }
    auto end() { return m_objects.end(); }
    auto begin() const { return m_objects.begin(); }
    auto end()   const { return m_objects.end(); }
};
