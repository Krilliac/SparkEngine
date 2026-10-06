/**
 * @file TestPLT210AngelScriptVector3Real.cpp
 * @brief PLT-210: script functions returning Vector3 by value on x86-64 System V (Linux/GCC).
 *
 * The System V ABI returns a 12-byte all-float struct (DirectX::XMFLOAT3) in
 * XMM registers. AngelScript's native calling convention can only marshal that
 * when the value type is registered with asOBJ_APP_CLASS_ALLFLOATS; without it
 * every script build on Linux failed in PrepareEngine with "Don't support
 * returning type 'Vector3' by value from application in native calling
 * convention on this platform", which blocked SparkGameVisualScript from
 * loading. These tests drive the production AngelScriptEngine end to end.
 */

#include "TestFramework.h"

#ifdef SPARK_ANGELSCRIPT_SUPPORT

#include "Engine/Scripting/AngelScriptEngine.h"

#include <cstdint>
#include <format>
#include <string>

TEST(PLT210_AngelScript_Vector3ReturnByValueCompiles)
{
    AngelScriptEngine engine;
    ASSERT_TRUE(engine.Initialize());

    const bool compiled = engine.CompileScriptFromString(
        "class Probe { void Start() { Vector3 p = getPosition(0); Vector3 r = getRotation(0); } }", "PLT210Compile");
    EXPECT_TRUE(compiled);
    EXPECT_EQ(engine.GetLastError(), std::string());
    engine.Shutdown();
}

TEST(PLT210_AngelScript_Vector3ReturnedValuesSurviveNativeCall)
{
    World world;
    const EntityID source = world.CreateEntity("PLT210Source");
    const EntityID target = world.CreateEntity("PLT210Target");
    Transform& sourceTransform = world.AddComponent<Transform>(source);
    sourceTransform.position = DirectX::XMFLOAT3(1.5f, -2.25f, 3.0f);
    sourceTransform.rotation = DirectX::XMFLOAT3(10.0f, 20.0f, 30.0f);
    world.AddComponent<Transform>(target);

    AngelScriptEngine engine;
    ASSERT_TRUE(engine.Initialize());
    AngelScriptEngine::BindWorld(&world);

    // Copy source's position and rotation onto target through by-value
    // returns; a mis-marshalled XMM return would scramble or zero them.
    const std::string script = std::format("class Mover {{ void Start() {{"
                                           "  Vector3 p = getPosition({0}); Vector3 r = getRotation({0});"
                                           "  setPosition({1}, p); setRotation({1}, r);"
                                           "}} }}",
                                           static_cast<uint32_t>(source), static_cast<uint32_t>(target));
    ASSERT_TRUE(engine.CompileScriptFromString(script, "PLT210Mover"));
    ASSERT_TRUE(engine.AttachScript(target, "Mover", "PLT210Mover"));
    engine.CallStart(target);

    const Transform* moved = world.GetComponent<Transform>(target);
    ASSERT_TRUE(moved != nullptr);
    EXPECT_NEAR(moved->position.x, 1.5f, 1e-6f);
    EXPECT_NEAR(moved->position.y, -2.25f, 1e-6f);
    EXPECT_NEAR(moved->position.z, 3.0f, 1e-6f);
    EXPECT_NEAR(moved->rotation.x, 10.0f, 1e-6f);
    EXPECT_NEAR(moved->rotation.y, 20.0f, 1e-6f);
    EXPECT_NEAR(moved->rotation.z, 30.0f, 1e-6f);

    engine.DetachScript(target);
    AngelScriptEngine::BindWorld(nullptr);
    engine.Shutdown();
}

#endif // SPARK_ANGELSCRIPT_SUPPORT
