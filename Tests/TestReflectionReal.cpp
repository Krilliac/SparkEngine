/**
 * @file TestReflectionReal.cpp
 * @brief Real-class tests for Spark::TypeRegistry
 */

#include "TestFramework.h"
#include "Core/Reflection.h"

namespace
{
    struct PhaseMM_TestType
    {
        int x = 0;
        float y = 0.0f;
    };

    struct PhaseMM_BaseType
    {
    };

    struct PhaseMM_DerivedType : public PhaseMM_BaseType
    {
    };
} // namespace

TEST(ReflectionReal_SingletonStable)
{
    auto& a = Spark::TypeRegistry::Get();
    auto& b = Spark::TypeRegistry::Get();
    EXPECT_TRUE(&a == &b);
}

TEST(ReflectionReal_RegisterAndFindType)
{
    auto& reg = Spark::TypeRegistry::Get();
    const auto id = ::GetTypeId<PhaseMM_TestType>();
    reg.RegisterType(id, "PhaseMM_TestType", sizeof(PhaseMM_TestType), alignof(PhaseMM_TestType));

    const auto* info = reg.FindType(id);
    EXPECT_TRUE(info != nullptr);
    if (info)
    {
        EXPECT_EQ(info->name, std::string("PhaseMM_TestType"));
        EXPECT_EQ(info->size, sizeof(PhaseMM_TestType));
        EXPECT_EQ(info->alignment, alignof(PhaseMM_TestType));
    }
}

TEST(ReflectionReal_FindByName)
{
    auto& reg = Spark::TypeRegistry::Get();
    reg.RegisterType(::GetTypeId<PhaseMM_TestType>(), "PhaseMM_TestType", sizeof(PhaseMM_TestType),
                     alignof(PhaseMM_TestType));

    const auto* info = reg.FindTypeByName("PhaseMM_TestType");
    EXPECT_TRUE(info != nullptr);
}

TEST(ReflectionReal_FindUnknownReturnsNull)
{
    auto& reg = Spark::TypeRegistry::Get();
    const auto* info = reg.FindTypeByName("DefinitelyNotRegistered_PhaseMM");
    EXPECT_TRUE(info == nullptr);
}

TEST(ReflectionReal_GetAllTypesNonEmpty)
{
    auto& reg = Spark::TypeRegistry::Get();
    reg.RegisterType(::GetTypeId<PhaseMM_TestType>(), "PhaseMM_TestType", sizeof(PhaseMM_TestType),
                     alignof(PhaseMM_TestType));
    const auto& all = reg.GetAllTypes();
    EXPECT_TRUE(all.size() >= static_cast<size_t>(1));
}

TEST(ReflectionReal_GetTypeCountMatchesAllTypes)
{
    auto& reg = Spark::TypeRegistry::Get();
    EXPECT_EQ(reg.GetTypeCount(), reg.GetAllTypes().size());
}

TEST(ReflectionReal_DerivedType)
{
    auto& reg = Spark::TypeRegistry::Get();
    reg.RegisterType(::GetTypeId<PhaseMM_BaseType>(), "PhaseMM_BaseType", sizeof(PhaseMM_BaseType),
                     alignof(PhaseMM_BaseType));
    reg.RegisterType(::GetTypeId<PhaseMM_DerivedType>(), "PhaseMM_DerivedType", sizeof(PhaseMM_DerivedType),
                     alignof(PhaseMM_DerivedType), ::GetTypeId<PhaseMM_BaseType>());

    auto derived = reg.GetDerivedTypes<PhaseMM_BaseType>();
    EXPECT_TRUE(derived.size() >= static_cast<size_t>(1));
}
