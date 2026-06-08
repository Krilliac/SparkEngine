// TestEngineContext.cpp - Tests for type-safe service locator pattern
// Standalone reimplementation to avoid engine header dependencies

#include "TestFramework.h"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// Standalone reimplementation of the generic system registry
// (mirrors EngineContext using TypeId + void* approach from R1.1/R1.2)
// ============================================================================

namespace
{

    // ========================================================================
    // TypeId — compile-time type identity (works with incomplete types)
    // ========================================================================

    using TypeId = const void*;

    template <typename T> TypeId GetTypeId()
    {
        // Non-const so MSVC /OPT:ICF (Release) cannot fold these per-type markers
        // into one address. See EngineContext::GetTypeId for the full rationale.
        static char id;
        return &id;
    }

    struct TypeIdHash
    {
        size_t operator()(TypeId id) const noexcept { return std::hash<const void*>{}(id); }
    };

    // ========================================================================
    // DependsOn tag and SubsystemEntry (mirrors EngineContext R1.2)
    // ========================================================================

    template <typename... Deps> struct DependsOn
    {
    };

    struct SubsystemEntry
    {
        TypeId type = nullptr;
        std::string name;
        std::vector<TypeId> dependencies;
        std::function<bool()> initFn;
        std::function<void()> shutdownFn;
        bool initialized = false;
    };

    // ========================================================================
    // ServiceLocator — mirrors EngineContext with R1.1 + R1.2
    // ========================================================================

    class ServiceLocator
    {
      public:
        template <typename T> void RegisterSystem(T* system) { m_systems[GetTypeId<T>()] = static_cast<void*>(system); }

        template <typename T> T* GetSystem() const
        {
            auto it = m_systems.find(GetTypeId<T>());
            if (it != m_systems.end())
            {
                return static_cast<T*>(it->second);
            }
            return nullptr;
        }

        // R1.2: dependency-aware registration
        template <typename T, typename... Deps>
        void RegisterSubsystem(T* system, DependsOn<Deps...> /*deps*/, std::function<bool()> initFn = nullptr,
                               std::function<void()> shutdownFn = nullptr)
        {
            RegisterSystem<T>(system);

            std::vector<TypeId> depList;
            (depList.push_back(GetTypeId<Deps>()), ...);

            SubsystemEntry entry{GetTypeId<T>(),    typeid(T).name(),      std::move(depList),
                                 std::move(initFn), std::move(shutdownFn), false};

            auto it = std::find_if(m_entries.begin(), m_entries.end(),
                                   [&](const SubsystemEntry& e) { return e.type == GetTypeId<T>(); });
            if (it != m_entries.end())
            {
                *it = std::move(entry);
            }
            else
            {
                m_entries.push_back(std::move(entry));
            }
        }

        bool InitializeAll()
        {
            std::vector<SubsystemEntry*> sorted;
            if (!TopologicalSort(sorted))
            {
                return false;
            }

            m_initOrder.clear();
            for (auto* entry : sorted)
            {
                if (entry->initFn)
                {
                    if (!entry->initFn())
                    {
                        return false;
                    }
                }
                entry->initialized = true;
                m_initOrder.push_back(entry->type);
            }
            return true;
        }

        void ShutdownAll()
        {
            for (auto it = m_initOrder.rbegin(); it != m_initOrder.rend(); ++it)
            {
                auto entryIt = std::find_if(m_entries.begin(), m_entries.end(),
                                            [&](const SubsystemEntry& e) { return e.type == *it; });
                if (entryIt != m_entries.end() && entryIt->initialized && entryIt->shutdownFn)
                {
                    entryIt->shutdownFn();
                }
                if (entryIt != m_entries.end())
                {
                    entryIt->initialized = false;
                }
            }
            m_initOrder.clear();
        }

        const std::vector<TypeId>& GetInitOrder() const { return m_initOrder; }
        size_t GetSubsystemCount() const { return m_entries.size(); }

      private:
        bool TopologicalSort(std::vector<SubsystemEntry*>& sorted)
        {
            std::unordered_map<TypeId, SubsystemEntry*, TypeIdHash> entryMap;
            std::unordered_map<TypeId, int, TypeIdHash> inDegree;
            std::unordered_map<TypeId, std::vector<TypeId>, TypeIdHash> adjacency;

            for (auto& entry : m_entries)
            {
                entryMap[entry.type] = &entry;
                inDegree[entry.type];
            }

            for (auto& entry : m_entries)
            {
                for (const auto& dep : entry.dependencies)
                {
                    if (entryMap.find(dep) != entryMap.end())
                    {
                        adjacency[dep].push_back(entry.type);
                        inDegree[entry.type]++;
                    }
                }
            }

            std::vector<TypeId> queue;
            for (const auto& [type, degree] : inDegree)
            {
                if (degree == 0)
                {
                    queue.push_back(type);
                }
            }
            std::sort(queue.begin(), queue.end(), [](TypeId a, TypeId b) { return std::less<const void*>{}(a, b); });

            sorted.clear();
            size_t idx = 0;
            while (idx < queue.size())
            {
                auto current = queue[idx++];
                sorted.push_back(entryMap[current]);

                auto adjIt = adjacency.find(current);
                if (adjIt != adjacency.end())
                {
                    auto neighbors = adjIt->second;
                    std::sort(neighbors.begin(), neighbors.end(),
                              [](TypeId a, TypeId b) { return std::less<const void*>{}(a, b); });

                    for (const auto& neighbor : neighbors)
                    {
                        inDegree[neighbor]--;
                        if (inDegree[neighbor] == 0)
                        {
                            queue.push_back(neighbor);
                        }
                    }
                }
            }
            return sorted.size() == m_entries.size();
        }

        mutable std::unordered_map<TypeId, void*, TypeIdHash> m_systems;
        std::vector<SubsystemEntry> m_entries;
        std::vector<TypeId> m_initOrder;
    };

    // Dummy subsystem types for testing
    struct TestSystemA
    {
        int value = 42;
    };

    struct TestSystemB
    {
        float data = 3.14f;
    };

    struct TestSystemC
    {
        std::string name = "test";
    };

} // namespace

// ============================================================================
// R1.1: Registration and Retrieval (generic registry as single source of truth)
// ============================================================================

TEST(ServiceLocator_RegisterAndRetrieve)
{
    ServiceLocator loc;
    TestSystemA sysA;
    loc.RegisterSystem<TestSystemA>(&sysA);

    auto* retrieved = loc.GetSystem<TestSystemA>();
    EXPECT_TRUE(retrieved != nullptr);
    EXPECT_EQ(retrieved->value, 42);
}

TEST(ServiceLocator_RetrieveUnregistered)
{
    ServiceLocator loc;
    auto* result = loc.GetSystem<TestSystemA>();
    EXPECT_TRUE(result == nullptr);
}

TEST(ServiceLocator_MultipleSystemTypes)
{
    ServiceLocator loc;
    TestSystemA sysA;
    TestSystemB sysB;
    TestSystemC sysC;

    loc.RegisterSystem<TestSystemA>(&sysA);
    loc.RegisterSystem<TestSystemB>(&sysB);
    loc.RegisterSystem<TestSystemC>(&sysC);

    EXPECT_TRUE(loc.GetSystem<TestSystemA>() == &sysA);
    EXPECT_TRUE(loc.GetSystem<TestSystemB>() == &sysB);
    EXPECT_TRUE(loc.GetSystem<TestSystemC>() == &sysC);
}

TEST(ServiceLocator_DoubleRegistration)
{
    ServiceLocator loc;
    TestSystemA sysA1;
    sysA1.value = 10;
    TestSystemA sysA2;
    sysA2.value = 20;

    loc.RegisterSystem<TestSystemA>(&sysA1);
    EXPECT_EQ(loc.GetSystem<TestSystemA>()->value, 10);

    // Re-register should overwrite
    loc.RegisterSystem<TestSystemA>(&sysA2);
    EXPECT_EQ(loc.GetSystem<TestSystemA>()->value, 20);
}

TEST(ServiceLocator_TypeSafety)
{
    ServiceLocator loc;
    TestSystemA sysA;
    loc.RegisterSystem<TestSystemA>(&sysA);

    // Retrieving with wrong type should return nullptr (different TypeId)
    auto* wrongType = loc.GetSystem<TestSystemB>();
    EXPECT_TRUE(wrongType == nullptr);
}

TEST(ServiceLocator_RegisterNullptr)
{
    ServiceLocator loc;
    loc.RegisterSystem<TestSystemA>(nullptr);

    auto* result = loc.GetSystem<TestSystemA>();
    EXPECT_TRUE(result == nullptr);
}

TEST(ServiceLocator_ConstAccess)
{
    ServiceLocator loc;
    TestSystemA sysA;
    loc.RegisterSystem<TestSystemA>(&sysA);

    const ServiceLocator& constRef = loc;
    auto* retrieved = constRef.GetSystem<TestSystemA>();
    EXPECT_TRUE(retrieved != nullptr);
    EXPECT_EQ(retrieved->value, 42);
}

TEST(ServiceLocator_IndependentTypes)
{
    ServiceLocator loc;
    TestSystemA sysA;
    TestSystemB sysB;

    loc.RegisterSystem<TestSystemA>(&sysA);
    loc.RegisterSystem<TestSystemB>(&sysB);

    // Overwriting A should not affect B
    TestSystemA sysA2;
    sysA2.value = 99;
    loc.RegisterSystem<TestSystemA>(&sysA2);

    EXPECT_EQ(loc.GetSystem<TestSystemA>()->value, 99);
    EXPECT_NEAR(loc.GetSystem<TestSystemB>()->data, 3.14f, 0.01f);
}

TEST(ServiceLocator_ManyTypes)
{
    ServiceLocator loc;

    struct S1
    {
        int a = 1;
    };
    struct S2
    {
        int a = 2;
    };
    struct S3
    {
        int a = 3;
    };
    struct S4
    {
        int a = 4;
    };
    struct S5
    {
        int a = 5;
    };

    S1 s1;
    S2 s2;
    S3 s3;
    S4 s4;
    S5 s5;

    loc.RegisterSystem<S1>(&s1);
    loc.RegisterSystem<S2>(&s2);
    loc.RegisterSystem<S3>(&s3);
    loc.RegisterSystem<S4>(&s4);
    loc.RegisterSystem<S5>(&s5);

    EXPECT_EQ(loc.GetSystem<S1>()->a, 1);
    EXPECT_EQ(loc.GetSystem<S2>()->a, 2);
    EXPECT_EQ(loc.GetSystem<S3>()->a, 3);
    EXPECT_EQ(loc.GetSystem<S4>()->a, 4);
    EXPECT_EQ(loc.GetSystem<S5>()->a, 5);
}

// ============================================================================
// R1.2: Dependency-aware subsystem initialization
// ============================================================================

TEST(SubsystemInit_BasicInitializeAll)
{
    ServiceLocator loc;
    TestSystemA sysA;
    TestSystemB sysB;

    std::vector<std::string> initOrder;

    loc.RegisterSubsystem<TestSystemA>(&sysA, DependsOn<>{},
                                       [&]() -> bool
                                       {
                                           initOrder.push_back("A");
                                           return true;
                                       });

    loc.RegisterSubsystem<TestSystemB>(&sysB, DependsOn<>{},
                                       [&]() -> bool
                                       {
                                           initOrder.push_back("B");
                                           return true;
                                       });

    EXPECT_TRUE(loc.InitializeAll());
    EXPECT_EQ(initOrder.size(), static_cast<size_t>(2));
}

TEST(SubsystemInit_DependencyOrder)
{
    // B depends on A, C depends on B => init order must be A, B, C
    ServiceLocator loc;
    TestSystemA sysA;
    TestSystemB sysB;
    TestSystemC sysC;

    std::vector<std::string> initOrder;

    loc.RegisterSubsystem<TestSystemA>(&sysA, DependsOn<>{},
                                       [&]() -> bool
                                       {
                                           initOrder.push_back("A");
                                           return true;
                                       });

    loc.RegisterSubsystem<TestSystemB>(&sysB, DependsOn<TestSystemA>{},
                                       [&]() -> bool
                                       {
                                           initOrder.push_back("B");
                                           return true;
                                       });

    loc.RegisterSubsystem<TestSystemC>(&sysC, DependsOn<TestSystemB>{},
                                       [&]() -> bool
                                       {
                                           initOrder.push_back("C");
                                           return true;
                                       });

    EXPECT_TRUE(loc.InitializeAll());
    EXPECT_EQ(initOrder.size(), static_cast<size_t>(3));

    // A must come before B, and B must come before C
    size_t posA = 0, posB = 0, posC = 0;
    for (size_t i = 0; i < initOrder.size(); ++i)
    {
        if (initOrder[i] == "A")
            posA = i;
        if (initOrder[i] == "B")
            posB = i;
        if (initOrder[i] == "C")
            posC = i;
    }
    EXPECT_TRUE(posA < posB);
    EXPECT_TRUE(posB < posC);
}

TEST(SubsystemInit_MultipleDependencies)
{
    // C depends on both A and B
    ServiceLocator loc;
    TestSystemA sysA;
    TestSystemB sysB;
    TestSystemC sysC;

    std::vector<std::string> initOrder;

    loc.RegisterSubsystem<TestSystemA>(&sysA, DependsOn<>{},
                                       [&]() -> bool
                                       {
                                           initOrder.push_back("A");
                                           return true;
                                       });

    loc.RegisterSubsystem<TestSystemB>(&sysB, DependsOn<>{},
                                       [&]() -> bool
                                       {
                                           initOrder.push_back("B");
                                           return true;
                                       });

    loc.RegisterSubsystem<TestSystemC>(&sysC, DependsOn<TestSystemA, TestSystemB>{},
                                       [&]() -> bool
                                       {
                                           initOrder.push_back("C");
                                           return true;
                                       });

    EXPECT_TRUE(loc.InitializeAll());
    EXPECT_EQ(initOrder.size(), static_cast<size_t>(3));

    // C must come after both A and B
    size_t posA = 0, posB = 0, posC = 0;
    for (size_t i = 0; i < initOrder.size(); ++i)
    {
        if (initOrder[i] == "A")
            posA = i;
        if (initOrder[i] == "B")
            posB = i;
        if (initOrder[i] == "C")
            posC = i;
    }
    EXPECT_TRUE(posA < posC);
    EXPECT_TRUE(posB < posC);
}

TEST(SubsystemInit_ShutdownReverseOrder)
{
    ServiceLocator loc;
    TestSystemA sysA;
    TestSystemB sysB;
    TestSystemC sysC;

    std::vector<std::string> shutdownOrder;

    loc.RegisterSubsystem<TestSystemA>(
        &sysA, DependsOn<>{}, [&]() -> bool { return true; }, [&]() { shutdownOrder.push_back("A"); });

    loc.RegisterSubsystem<TestSystemB>(
        &sysB, DependsOn<TestSystemA>{}, [&]() -> bool { return true; }, [&]() { shutdownOrder.push_back("B"); });

    loc.RegisterSubsystem<TestSystemC>(
        &sysC, DependsOn<TestSystemB>{}, [&]() -> bool { return true; }, [&]() { shutdownOrder.push_back("C"); });

    EXPECT_TRUE(loc.InitializeAll());
    loc.ShutdownAll();

    EXPECT_EQ(shutdownOrder.size(), static_cast<size_t>(3));

    // Shutdown must be reverse of init: C before B, B before A
    size_t posA = 0, posB = 0, posC = 0;
    for (size_t i = 0; i < shutdownOrder.size(); ++i)
    {
        if (shutdownOrder[i] == "A")
            posA = i;
        if (shutdownOrder[i] == "B")
            posB = i;
        if (shutdownOrder[i] == "C")
            posC = i;
    }
    EXPECT_TRUE(posC < posB);
    EXPECT_TRUE(posB < posA);
}

TEST(SubsystemInit_InitFailurePropagates)
{
    ServiceLocator loc;
    TestSystemA sysA;
    TestSystemB sysB;

    loc.RegisterSubsystem<TestSystemA>(&sysA, DependsOn<>{}, [&]() -> bool { return false; }); // Fails

    loc.RegisterSubsystem<TestSystemB>(&sysB, DependsOn<TestSystemA>{}, [&]() -> bool { return true; });

    // InitializeAll should return false because A's init fails
    EXPECT_TRUE(!loc.InitializeAll());
}

TEST(SubsystemInit_NoInitCallback)
{
    // Subsystems with no init callback should still be marked initialized
    ServiceLocator loc;
    TestSystemA sysA;

    loc.RegisterSubsystem<TestSystemA>(&sysA, DependsOn<>{});

    EXPECT_TRUE(loc.InitializeAll());
    EXPECT_EQ(loc.GetInitOrder().size(), static_cast<size_t>(1));
}

TEST(SubsystemInit_SubsystemCount)
{
    ServiceLocator loc;
    TestSystemA sysA;
    TestSystemB sysB;

    loc.RegisterSubsystem<TestSystemA>(&sysA, DependsOn<>{});
    loc.RegisterSubsystem<TestSystemB>(&sysB, DependsOn<TestSystemA>{});

    EXPECT_EQ(loc.GetSubsystemCount(), static_cast<size_t>(2));
}

TEST(SubsystemInit_ExternalDependencyIgnored)
{
    // B declares dependency on a type that was never registered as a subsystem.
    // The dependency should be silently ignored.
    ServiceLocator loc;
    TestSystemB sysB;

    loc.RegisterSubsystem<TestSystemB>(&sysB, DependsOn<TestSystemA>{}, [&]() -> bool { return true; });

    EXPECT_TRUE(loc.InitializeAll());
    EXPECT_EQ(loc.GetInitOrder().size(), static_cast<size_t>(1));
}

TEST(SubsystemInit_RegisterSubsystemAlsoRegistersInGenericRegistry)
{
    // RegisterSubsystem should make the system available via GetSystem<T>
    ServiceLocator loc;
    TestSystemA sysA;
    sysA.value = 99;

    loc.RegisterSubsystem<TestSystemA>(&sysA, DependsOn<>{});

    auto* retrieved = loc.GetSystem<TestSystemA>();
    EXPECT_TRUE(retrieved != nullptr);
    EXPECT_EQ(retrieved->value, 99);
}
