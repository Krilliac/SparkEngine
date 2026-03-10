// TestEngineContext.cpp - Tests for type-safe service locator pattern
// Standalone reimplementation to avoid engine header dependencies

#include "TestFramework.h"
#include <any>
#include <memory>
#include <string>
#include <typeindex>
#include <unordered_map>

// ============================================================================
// Standalone reimplementation of the generic system registry
// (mirrors EngineContext::RegisterSystem / GetSystem using std::any)
// ============================================================================

namespace
{

    class ServiceLocator
    {
      public:
        template <typename T> void RegisterSystem(T* system) { m_systems[std::type_index(typeid(T))] = system; }

        template <typename T> T* GetSystem() const
        {
            auto it = m_systems.find(std::type_index(typeid(T)));
            if (it != m_systems.end())
            {
                auto* ptr = std::any_cast<T*>(&it->second);
                return ptr ? *ptr : nullptr;
            }
            return nullptr;
        }

      private:
        mutable std::unordered_map<std::type_index, std::any> m_systems;
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
// Registration and Retrieval
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

    // Retrieving with wrong type should return nullptr (type-safe via std::any)
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
