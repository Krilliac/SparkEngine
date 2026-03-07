/**
 * @file ObjectPool.h
 * @brief Generic, cache-friendly object pool for high-frequency allocations.
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * ObjectPool provides a reusable pool of pre-allocated objects, eliminating the
 * per-frame heap allocation overhead that would otherwise be incurred when spawning
 * many short-lived objects such as bullets, particles, audio voices, or UI elements.
 *
 * ## Design
 *
 * The pool owns all object memory via a `std::vector<std::unique_ptr<T>>`. A
 * `std::queue<T*>` tracks which objects are currently available for acquisition.
 * When an object is released back to the pool its raw pointer is pushed onto the
 * queue; when acquired it is popped off.
 *
 * Because all objects reside in heap-allocated nodes owned by unique_ptrs, the pool
 * is **not** contiguous in memory. For strictly cache-optimal layouts consider a
 * custom arena allocator; ObjectPool trades some cache-friendliness for clean C++
 * ownership semantics and convenience.
 *
 * ## Automatic Reset
 *
 * If the pooled type `T` exposes a `Reset()` method (detected at compile-time via a
 * C++20 `requires` constraint), ObjectPool calls it automatically when an object is
 * returned to the pool. This clears any frame-specific state (e.g. velocity, lifetime
 * counter) without the caller needing to remember to do so.
 *
 * ## Thread safety
 *
 * ObjectPool is **not thread-safe**. All Acquire/Release calls must originate from
 * the same thread, or callers must provide external synchronization.
 *
 * ## Usage example
 * @code
 *   // Create a pool of up to 200 Bullet objects with a factory
 *   ObjectPool<Bullet> bulletPool(200, []() {
 *       return std::make_unique<Bullet>();
 *   });
 *
 *   // Acquire a bullet when the player fires
 *   Bullet* b = bulletPool.Acquire();
 *   if (b) {
 *       b->Fire(origin, direction, 800.0f);
 *   }
 *
 *   // Return the bullet when it hits something or expires
 *   bulletPool.Release(b);  // Bullet::Reset() called automatically if present
 * @endcode
 *
 * @tparam T  The pooled object type. Must be default-constructible when using the
 *            `maxSize`-only constructor. Types with a `Reset()` method benefit from
 *            automatic state clearing on Release().
 */
#pragma once

#include "Utils/Assert.h"
#include <vector>
#include <queue>
#include <memory>
#include <functional>

/**
 * @class ObjectPool
 * @brief Generic fixed-capacity pool for reusable objects.
 *
 * Stores up to `maxSize` objects of type `T`, providing O(1) Acquire and Release
 * operations backed by a queue of available pointers. Objects are created either
 * eagerly via a factory (two-argument constructor or PreAllocate()) or lazily on
 * first Acquire() if a factory is registered.
 *
 * ### Ownership model
 * ObjectPool owns all object instances through `std::unique_ptr<T>` stored in
 * an internal vector. Raw `T*` pointers returned by Acquire() are **non-owning**
 * loans. Never delete a pointer returned by Acquire(); always return it via
 * Release() so the pool can reuse it.
 *
 * ### Lazy vs. eager allocation
 * - **Eager (pre-allocated)**: Pass both `maxSize` and a `factory` to the
 *   constructor. All `maxSize` objects are created immediately.
 * - **Lazy (on-demand)**: Pass only `maxSize`; supply a factory later via
 *   PreAllocate(). Objects are created as needed up to `maxSize`.
 *
 * @tparam T  Type of the pooled objects.
 */
template<typename T>
class ObjectPool
{
private:
    /**
     * @brief Storage for all objects owned by this pool.
     *
     * Each element is a `unique_ptr<T>` ensuring proper destruction when the pool
     * itself is destroyed. The vector is reserved to `maxSize` at construction to
     * avoid re-allocations.
     */
    std::vector<std::unique_ptr<T>>        m_objects;

    /**
     * @brief Queue of raw pointers to objects currently available for acquisition.
     *
     * Pointers in this queue point into `m_objects` and are non-owning. The queue
     * provides FIFO ordering so recently released objects are not immediately
     * re-acquired, giving any external state (e.g. particle rendering) time to
     * finish processing before the slot is reused.
     */
    std::queue<T*>                         m_available;

    /**
     * @brief Optional factory function used to create new objects on demand.
     *
     * If set, new objects are created via this factory when the pool has not yet
     * reached `maxSize` and Acquire() is called on an empty available queue.
     * If null, Acquire() returns nullptr when the pool is exhausted.
     */
    std::function<std::unique_ptr<T>()>    m_factory;

    /**
     * @brief Maximum number of objects this pool will ever hold.
     *
     * Once `m_objects.size() == m_maxSize` no new objects are created; subsequent
     * Acquire() calls on an empty queue return nullptr.
     */
    size_t                                 m_maxSize;

public:
    /**
     * @brief Construct an empty pool with a given capacity.
     *
     * No objects are created immediately. Objects can be added later via
     * PreAllocate(). Acquire() will return nullptr until objects have been
     * added or a factory is supplied.
     *
     * @param maxSize  Maximum number of objects the pool will hold. Must be > 0.
     *
     * @code
     *   ObjectPool<Particle> pool(500);   // capacity 500, no objects yet
     *   pool.PreAllocate(500, []() { return std::make_unique<Particle>(); });
     * @endcode
     */
    explicit ObjectPool(size_t maxSize = 100)
        : m_maxSize(maxSize)
    {
        ASSERT_MSG(maxSize > 0, "ObjectPool maxSize must be positive");
        m_objects.reserve(maxSize);
    }

    /**
     * @brief Construct a fully pre-allocated pool using the given factory.
     *
     * Creates all `maxSize` objects immediately by invoking `factory` once per
     * slot and pushing each object into the available queue. This is the preferred
     * constructor when you want deterministic startup-time allocation and zero
     * per-frame heap activity.
     *
     * @param maxSize  Maximum number of objects; also the initial population count.
     *                 Must be > 0.
     * @param factory  Callable that returns `std::unique_ptr<T>`. Called exactly
     *                 `maxSize` times. Must not return nullptr.
     *
     * @code
     *   ObjectPool<Bullet> pool(200, []() {
     *       return std::make_unique<Bullet>();
     *   });
     *   // All 200 Bullet instances now exist and are available for Acquire().
     * @endcode
     */
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

    /**
     * @brief Destroy the pool and all objects it owns.
     *
     * All `unique_ptr<T>` elements in `m_objects` are destroyed, which in turn
     * calls `~T()` for each pooled object. Any raw pointers previously returned
     * by Acquire() become dangling after this point — do not use them.
     */
    ~ObjectPool() = default;

    /**
     * @brief Acquire a pooled object for use.
     *
     * Returns the next available object from the pool. If the available queue is
     * empty and the pool has not reached `maxSize` and a factory is registered, a
     * new object is created and returned immediately (without being queued first,
     * so it is not returned to the available queue on the same call).
     *
     * Returns `nullptr` in two cases:
     *  1. The available queue is empty and no factory is registered.
     *  2. The available queue is empty and the pool has already reached `maxSize`.
     *
     * @return  Non-owning pointer to an available object, or `nullptr` if the pool
     *          is exhausted.
     *
     * @warning Do **not** delete the returned pointer. Always return it via Release()
     *          to avoid a memory leak or double-free.
     *
     * @code
     *   Bullet* b = bulletPool.Acquire();
     *   if (!b) {
     *       // Pool exhausted; skip spawning or expand pool capacity.
     *       return;
     *   }
     *   b->Fire(origin, dir, speed);
     * @endcode
     */
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

    /**
     * @brief Return a previously acquired object back to the pool.
     *
     * Marks the object as available for future Acquire() calls. If `T` has a
     * `Reset()` method it is invoked automatically (via C++20 `requires`) before
     * the pointer is re-queued, clearing any per-use state.
     *
     * Passing `nullptr` is a safe no-op; no assertion is raised.
     *
     * @param obj  Pointer previously obtained from Acquire(). May be `nullptr`.
     *
     * @note After calling Release() the caller should treat their copy of `obj` as
     *       invalid; the pool may hand the same pointer to the next Acquire() call.
     *
     * @code
     *   // When the bullet expires or hits something:
     *   bulletPool.Release(b);
     *   b = nullptr;  // Recommended: prevent accidental use-after-release
     * @endcode
     */
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

    // =========================================================================
    // Statistics / capacity queries
    // =========================================================================

    /**
     * @brief Return the total number of objects owned by the pool (active + available).
     *
     * This count grows from 0 up to `GetMaxSize()` as objects are lazily created.
     * In an eagerly pre-allocated pool this always equals `GetMaxSize()`.
     *
     * @return  Total object count (created so far), including those currently in use.
     */
    size_t GetTotalSize()      const { return m_objects.size(); }

    /**
     * @brief Return the number of objects currently available for Acquire().
     *
     * A return value of 0 means the next Acquire() will either create a new object
     * (if the pool is not full and a factory is available) or return nullptr.
     *
     * @return  Number of objects waiting in the available queue.
     */
    size_t GetAvailableCount() const { return m_available.size(); }

    /**
     * @brief Return the number of objects currently on loan (i.e. acquired but not released).
     *
     * Computed as `GetTotalSize() - GetAvailableCount()`. Useful for detecting
     * pool exhaustion trends and tuning pool capacity.
     *
     * @return  Number of objects currently held by callers.
     */
    size_t GetUsedCount()      const { return m_objects.size() - m_available.size(); }

    /**
     * @brief Return the maximum capacity of the pool.
     *
     * Set at construction time and never changes. Acquire() will never create more
     * than this many objects.
     *
     * @return  Maximum number of objects the pool can hold.
     */
    size_t GetMaxSize()        const { return m_maxSize; }

    // =========================================================================
    // Lifetime management
    // =========================================================================

    /**
     * @brief Destroy all pooled objects and reset the pool to an empty state.
     *
     * All `unique_ptr<T>` entries are destroyed (calling `~T()`), and the available
     * queue is cleared. The pool's `maxSize` limit is preserved; objects can be
     * re-populated via PreAllocate().
     *
     * @warning Any raw pointers previously returned by Acquire() become dangling
     *          after this call. Ensure all borrowed objects have been released or
     *          are no longer referenced before calling Clear().
     */
    void Clear()
    {
        m_objects.clear();
        std::queue<T*> empty;
        std::swap(m_available, empty);
    }

    /**
     * @brief Eagerly populate the pool using the provided factory.
     *
     * Creates up to `count` new objects (respecting `maxSize`) and pushes them
     * into the available queue. Also registers `factory` as the pool's internal
     * factory for any future lazy-creation via Acquire().
     *
     * This method is useful when the pool was constructed without a factory and
     * you want to defer the allocation cost to a later, more controlled point in
     * the frame or loading sequence.
     *
     * @param count    Number of objects to create. Clamped to `maxSize - currentSize`.
     * @param factory  Callable returning `std::unique_ptr<T>`. Must not be null
     *                 and must not return nullptr.
     *
     * @code
     *   // Allocate during level load to avoid hitches during gameplay
     *   explosionPool.PreAllocate(50, []() {
     *       return std::make_unique<ExplosionEffect>();
     *   });
     * @endcode
     */
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

    // =========================================================================
    // Range iteration
    // =========================================================================

    /**
     * @brief Iterator to the first owned object (including currently acquired ones).
     *
     * Iterates over **all** objects in the pool regardless of whether they are
     * currently available or in use. Useful for systems that need to update all
     * objects unconditionally (e.g. a renderer that draws all active particles).
     *
     * @return  Begin iterator into the internal `unique_ptr` vector.
     */
    auto begin() { return m_objects.begin(); }

    /**
     * @brief Past-the-end iterator for owned objects.
     * @return  End iterator into the internal `unique_ptr` vector.
     */
    auto end() { return m_objects.end(); }

    /**
     * @brief Const begin iterator for read-only traversal of all owned objects.
     * @return  Const begin iterator into the internal `unique_ptr` vector.
     */
    auto begin() const { return m_objects.begin(); }

    /**
     * @brief Const end iterator for read-only traversal of all owned objects.
     * @return  Const end iterator into the internal `unique_ptr` vector.
     */
    auto end()   const { return m_objects.end(); }
};
