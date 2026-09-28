/**
 * @file TestAngelScriptStackAlignmentReal.cpp
 * @brief AngelScript stack alignment: 64-bit value objects on the script stack reach native code 8-byte aligned.
 *
 * AngelScript's script stack is an array of 4-byte asDWORDs. The stock core
 * placed value-type locals (the script `string`, i.e. std::string) and
 * return-by-value slots at any dword, so native bindings could receive a
 * std::string at an address 4 mod 8 - undefined behaviour that UBSan reports
 * as a misaligned basic_string reference. ThirdParty/Scripting/patches/
 * angelscript-packed-bytecode.patch pads stack-resident value objects and
 * return slots to 8 bytes, aligns prepared frames, and moves arguments when a
 * script call shifts its frame by one dword.
 *
 * These tests drive the raw AngelScript API (the same patched core and
 * scriptstdstring add-on SparkEngine links) through every path the patch
 * touches: string locals behind odd dword offsets, native and script string
 * returns, nested context calls, recursion deep enough to grow the stack block,
 * suspend/abort/resume, native exceptions unwinding value objects, a 12-byte
 * value type, and bytecode save/load (with and without debug info). Every
 * native observation checks the object address explicitly, so the tests fail
 * on a misaligned object even without a sanitizer.
 *
 * Thread affinity: game thread only (one engine per test, no shared state
 * outside the per-test probe).
 */

#include "TestFramework.h"

#ifdef SPARK_ANGELSCRIPT_SUPPORT

#include <angelscript.h>
#include <scriptstdstring/scriptstdstring.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

    /// Everything the native bindings observe during one test.
    struct AlignmentProbe
    {
        int failures = 0;
        unsigned observations = 0;
        unsigned constructions = 0;
        unsigned destructions = 0;
        std::set<std::string> reportedFailures;
        std::set<void*> liveObjects;
        asIScriptFunction* nestedFunction = nullptr;
        bool suspendRequested = false;
    };

    // Native bindings have no user pointer, so they reach the active test's probe here.
    AlignmentProbe* s_probe = nullptr;

    void Fail(const char* what)
    {
        // Print each distinct failure once but count every bad observation.
        if (s_probe->reportedFailures.insert(what).second)
        {
            std::fprintf(stderr, "  ALIGNMENT FAIL: %s\n", what);
        }
        ++s_probe->failures;
    }

    void Check(bool condition, const char* what)
    {
        if (!condition)
        {
            Fail(what);
        }
    }

    bool Require(bool condition, const char* what)
    {
        Check(condition, what);
        return condition;
    }

    bool IsAligned8(const void* pointer)
    {
        return (reinterpret_cast<std::uintptr_t>(pointer) & 7u) == 0;
    }

    void CheckString(const std::string& value, const char* expected)
    {
        // Check the address before touching the object: a misaligned reference is the defect.
        if (!IsAligned8(&value))
        {
            Fail("std::string observed at an address that is not 8-byte aligned");
        }
        Check(value == expected, "string contents changed");
        ++s_probe->observations;
    }

    void MessageCallback(const asSMessageInfo* message, void*)
    {
        std::fprintf(stderr, "  %s:%d:%d: %s\n", message->section, message->row, message->col, message->message);
    }

    void TrackedConstruct(asIScriptGeneric* generic)
    {
        void* pointer = generic->GetObject();
        Check(IsAligned8(pointer), "Tracked constructed at an address that is not 8-byte aligned");
        Check(s_probe->liveObjects.insert(pointer).second, "Tracked constructed over a live object");
        ++s_probe->constructions;
    }

    void TrackedDestruct(asIScriptGeneric* generic)
    {
        void* pointer = generic->GetObject();
        Check(IsAligned8(pointer), "Tracked destroyed at an address that is not 8-byte aligned");
        Check(s_probe->liveObjects.erase(pointer) == 1, "Tracked destructor got a wrong or already destroyed address");
        ++s_probe->destructions;
    }

    void GenericExpect(asIScriptGeneric* generic)
    {
        Check(generic->GetArgByte(0) != 0, "script argument/content assertion failed");
    }

    void Observe(const std::string& value)
    {
        static constexpr const char* kKnownValues[] = {"mixed", "native", "script", "suspend", "nested"};
        for (const char* known : kKnownValues)
        {
            if (IsAligned8(&value) && value == known)
            {
                CheckString(value, known);
                return;
            }
        }
        // Misaligned or unknown: CheckString records the address failure without a matching expectation.
        CheckString(value, "<unexpected>");
    }

    struct Triple
    {
        int a;
        int b;
        int c;
    };

    void ObserveTriple(const Triple& value)
    {
        if (!IsAligned8(&value))
        {
            Fail("12-byte Triple observed at an address that is not 8-byte aligned");
        }
        Check(value.a == 11 && value.b == 22 && value.c == 33, "Triple contents changed");
        ++s_probe->observations;
    }

    std::string NativeString(asDWORD n)
    {
        Check(n == 7 || n == 9, "native string argument was corrupted");
        return "native";
    }

    void SuspendPoint()
    {
        asIScriptContext* context = asGetActiveContext();
        if (Require(context != nullptr, "SuspendPoint has no active context") && !s_probe->suspendRequested)
        {
            s_probe->suspendRequested = true;
            context->Suspend();
        }
    }

    void NestedCall()
    {
        asIScriptContext* context = asGetActiveContext();
        if (!Require(context != nullptr, "NestedCall has no active context") ||
            !Require(s_probe->nestedFunction != nullptr, "nested function was not installed"))
        {
            return;
        }
        Check(context->PushState() >= 0, "PushState failed");
        Check(context->Prepare(s_probe->nestedFunction) >= 0, "nested Prepare failed");
        Check(context->SetArgDWord(0, 3) >= 0, "nested SetArgDWord failed");
        Check(context->Execute() == asEXECUTION_FINISHED, "nested Execute failed");
        void* result = context->GetReturnObject();
        if (Require(result != nullptr, "nested string return object was null"))
        {
            CheckString(*static_cast<std::string*>(result), "nested");
        }
        Check(context->PopState() >= 0, "PopState failed");
    }

    void ThrowNative()
    {
        throw std::runtime_error("intentional native exception");
    }

#if defined(AS_MAX_PORTABILITY)
    void GenericObserve(asIScriptGeneric* generic)
    {
        Observe(*static_cast<const std::string*>(generic->GetArgObject(0)));
    }

    void GenericNativeString(asIScriptGeneric* generic)
    {
        std::string result = NativeString(generic->GetArgDWord(0));
        Check(generic->SetReturnObject(&result) >= 0, "generic string return failed");
    }

    void GenericSuspendPoint(asIScriptGeneric*)
    {
        SuspendPoint();
    }

    void GenericNestedCall(asIScriptGeneric*)
    {
        NestedCall();
    }

    void GenericThrowNative(asIScriptGeneric*)
    {
        ThrowNative();
    }

    void GenericObserveTriple(asIScriptGeneric* generic)
    {
        ObserveTriple(*static_cast<const Triple*>(generic->GetArgObject(0)));
    }
#endif

    /// In-memory bytecode stream for SaveByteCode/LoadByteCode.
    class MemoryStream final : public asIBinaryStream
    {
      public:
        int Read(void* destination, asUINT size) override
        {
            if (m_position + size > m_bytes.size())
            {
                return -1;
            }
            std::memcpy(destination, m_bytes.data() + m_position, size);
            m_position += size;
            return 0;
        }

        int Write(const void* source, asUINT size) override
        {
            const auto* begin = static_cast<const std::uint8_t*>(source);
            m_bytes.insert(m_bytes.end(), begin, begin + size);
            return 0;
        }

        void Rewind() { m_position = 0; }

      private:
        std::vector<std::uint8_t> m_bytes;
        std::size_t m_position = 0;
    };

    /// Installs a fresh probe for one test and checks it when the test ends.
    class ProbeScope
    {
      public:
        ProbeScope() { s_probe = &m_probe; }
        ~ProbeScope() { s_probe = nullptr; }
        ProbeScope(const ProbeScope&) = delete;
        ProbeScope& operator=(const ProbeScope&) = delete;

        AlignmentProbe& Get() { return m_probe; }

      private:
        AlignmentProbe m_probe;
    };

    /// Owns one script engine; ShutDownAndRelease runs every pending destructor.
    class EngineScope
    {
      public:
        explicit EngineScope(asIScriptEngine* engine) : m_engine(engine) {}
        ~EngineScope() { Release(); }
        EngineScope(const EngineScope&) = delete;
        EngineScope& operator=(const EngineScope&) = delete;

        asIScriptEngine* Get() const { return m_engine; }
        void Release()
        {
            if (m_engine != nullptr)
            {
                m_engine->ShutDownAndRelease();
                m_engine = nullptr;
            }
        }

      private:
        asIScriptEngine* m_engine = nullptr;
    };

    void RegisterNatives(asIScriptEngine* engine)
    {
#if defined(AS_MAX_PORTABILITY)
        constexpr asDWORD kNativeConvention = asCALL_GENERIC;
        const asSFuncPtr observe = asFUNCTION(GenericObserve);
        const asSFuncPtr nativeString = asFUNCTION(GenericNativeString);
        const asSFuncPtr suspendPoint = asFUNCTION(GenericSuspendPoint);
        const asSFuncPtr nestedCall = asFUNCTION(GenericNestedCall);
        const asSFuncPtr throwNative = asFUNCTION(GenericThrowNative);
        const asSFuncPtr observeTriple = asFUNCTION(GenericObserveTriple);
#else
        constexpr asDWORD kNativeConvention = asCALL_CDECL;
        const asSFuncPtr observe = asFUNCTION(Observe);
        const asSFuncPtr nativeString = asFUNCTION(NativeString);
        const asSFuncPtr suspendPoint = asFUNCTION(SuspendPoint);
        const asSFuncPtr nestedCall = asFUNCTION(NestedCall);
        const asSFuncPtr throwNative = asFUNCTION(ThrowNative);
        const asSFuncPtr observeTriple = asFUNCTION(ObserveTriple);
#endif
        Check(engine->RegisterGlobalFunction("void Observe(const string &in)", observe, kNativeConvention) >= 0,
              "Observe registration failed");
        Check(engine->RegisterGlobalFunction("string NativeString(uint)", nativeString, kNativeConvention) >= 0,
              "NativeString registration failed");
        Check(engine->RegisterGlobalFunction("void SuspendPoint()", suspendPoint, kNativeConvention) >= 0,
              "SuspendPoint registration failed");
        Check(engine->RegisterGlobalFunction("void NestedCall()", nestedCall, kNativeConvention) >= 0,
              "NestedCall registration failed");
        Check(engine->RegisterGlobalFunction("void ThrowNative()", throwNative, kNativeConvention) >= 0,
              "ThrowNative registration failed");
        Check(engine->RegisterObjectType("Triple", sizeof(Triple),
                                         asOBJ_VALUE | asOBJ_POD | asGetTypeTraits<Triple>() |
                                             asOBJ_APP_CLASS_ALLINTS) >= 0,
              "Triple registration failed");
        Check(engine->RegisterObjectProperty("Triple", "int a", asOFFSET(Triple, a)) >= 0,
              "Triple.a registration failed");
        Check(engine->RegisterObjectProperty("Triple", "int b", asOFFSET(Triple, b)) >= 0,
              "Triple.b registration failed");
        Check(engine->RegisterObjectProperty("Triple", "int c", asOFFSET(Triple, c)) >= 0,
              "Triple.c registration failed");
        Check(engine->RegisterGlobalFunction("void ObserveTriple(const Triple &in)", observeTriple,
                                             kNativeConvention) >= 0,
              "ObserveTriple registration failed");
    }

    asIScriptEngine* MakeEngine()
    {
        asIScriptEngine* engine = asCreateScriptEngine();
        if (!Require(engine != nullptr, "asCreateScriptEngine failed"))
        {
            return nullptr;
        }
        Check(engine->SetMessageCallback(asFUNCTION(MessageCallback), nullptr, asCALL_CDECL) >= 0, "message callback");
        Check(engine->SetEngineProperty(asEP_ALLOW_UNSAFE_REFERENCES, true) >= 0, "unsafe reference setting");
        RegisterStdString(engine);
        Check(engine->RegisterGlobalFunction("void Expect(bool)", asFUNCTION(GenericExpect), asCALL_GENERIC) >= 0,
              "Expect registration failed");
        Check(engine->RegisterObjectType("Tracked", 8, asOBJ_VALUE | asOBJ_APP_CLASS_CD) >= 0,
              "Tracked registration failed");
        Check(engine->RegisterObjectBehaviour("Tracked", asBEHAVE_CONSTRUCT, "void f()", asFUNCTION(TrackedConstruct),
                                              asCALL_GENERIC) >= 0,
              "Tracked constructor registration failed");
        Check(engine->RegisterObjectBehaviour("Tracked", asBEHAVE_DESTRUCT, "void f()", asFUNCTION(TrackedDestruct),
                                              asCALL_GENERIC) >= 0,
              "Tracked destructor registration failed");
        RegisterNatives(engine);
        // A small initial block forces the argument-copy and stack-block growth paths.
        Check(engine->SetEngineProperty(asEP_INIT_STACK_SIZE, 64) >= 0, "initial script stack size");
        return engine;
    }

    // The odd `int i` / `int odd` locals push each string to an odd dword offset in the frame.
    constexpr const char* kAlignmentScript = R"(
string ScriptString(uint n) { int i = 1; string s = "script"; Observe(s); return s; }
string NativeReturnOnly(uint n) { int i = 1; float f = 2.0f; string s = NativeString(n); Observe(s); return s; }
string NestedString(uint n) { int i = 1; float f = 2.0f; string s = "nested"; Observe(s); if (n != 0) return NestedString(n - 1); return s; }
void MixedAndReturns(uint n) { int i = 1; float f = 2.0f; string s = "mixed"; Observe(s); string a = NativeString(n); Observe(a); string b = ScriptString(n); Observe(b); }
void Grow(uint n) { int i = 1; Tracked marker; float f = 2.0f; string s = "mixed"; Observe(s); if (n != 0) Grow(n - 1); Observe(s); }
void Suspended() { int i = 1; Tracked marker; float f = 2.0f; string s = "suspend"; Observe(s); SuspendPoint(); Observe(s); }
void Nested() { int i = 1; float f = 2.0f; string s = "mixed"; Observe(s); NestedCall(); }
void TripleLocal() { int odd = 1; Triple t; t.a = 11; t.b = 22; t.c = 33; ObserveTriple(t); }
void ThrowDeep(uint n, string s) { Tracked marker; Observe(s); if (n != 0) ThrowDeep(n - 1, s); else ThrowNative(); }
void CaughtNative() { Tracked outer; try { Tracked inner; string t = "mixed"; ThrowDeep(5, t); } catch { string s = "mixed"; Observe(s); } }
void UncaughtNative() { Tracked marker; string s = "mixed"; ThrowDeep(5, s); }
string Args(uint n, const string &in s, float f, int &inout x) { Expect(n == 7 && f == 2.5f && x == 5); x = 6; Observe(s); string t = s; return t; }
interface IFace { string Value(uint n); }
class Impl : IFace { string Value(uint n) { int odd = 1; string s = "script"; Observe(s); return s; } }
funcdef string Callback(uint);
void Dispatch() { int x = 5; string s = "mixed"; Observe(Args(7, s, 2.5f, x)); Expect(x == 6); IFace@ f = Impl(); Observe(f.Value(7)); Callback@ cb = ScriptString; Observe(cb(7)); @cb = Callback(Impl().Value); Observe(cb(7)); }
)";

    asIScriptModule* BuildModule(asIScriptEngine* engine, const char* name)
    {
        asIScriptModule* module = engine->GetModule(name, asGM_ALWAYS_CREATE);
        if (!Require(module != nullptr, "GetModule failed") ||
            !Require(module->AddScriptSection("alignment.as", kAlignmentScript) >= 0, "AddScriptSection failed") ||
            !Require(module->Build() >= 0, "script module Build failed"))
        {
            return nullptr;
        }
        return module;
    }

    /// Runs a script function to completion; expects asEXECUTION_FINISHED unless told otherwise.
    bool Run(asIScriptEngine* engine, asIScriptFunction* function, asDWORD argument = 0,
             int expectedState = asEXECUTION_FINISHED)
    {
        if (!Require(engine != nullptr && function != nullptr, "Run received a null engine/function"))
        {
            return false;
        }
        asIScriptContext* context = engine->CreateContext();
        if (!Require(context != nullptr, "CreateContext failed"))
        {
            return false;
        }
        bool ok = Require(context->Prepare(function) >= 0, "Prepare failed");
        if (ok && function->GetParamCount() != 0)
        {
            ok = Require(context->SetArgDWord(0, argument) >= 0, "SetArgDWord failed");
        }
        if (ok)
        {
            ok = Require(context->Execute() == expectedState, "script Execute ended in an unexpected state");
        }
        context->Release();
        return ok;
    }

    /// Checks the test-wide invariants: no bad observation, something observed, balanced value objects.
    void ExpectCleanProbe(const AlignmentProbe& probe)
    {
        EXPECT_EQ(probe.failures, 0);
        EXPECT_GT(probe.observations, 0u);
        EXPECT_TRUE(probe.liveObjects.empty());
        EXPECT_EQ(probe.constructions, probe.destructions);
    }

    void RunSavedBytecode(bool stripDebug)
    {
        ProbeScope probe;
        MemoryStream stream;
        {
            EngineScope source(MakeEngine());
            ASSERT_TRUE(source.Get() != nullptr);
            asIScriptModule* module = BuildModule(source.Get(), "saved-source");
            ASSERT_TRUE(module != nullptr);
            EXPECT_TRUE(module->SaveByteCode(&stream, stripDebug) >= 0);
        }

        EngineScope loadedEngine(MakeEngine());
        ASSERT_TRUE(loadedEngine.Get() != nullptr);
        stream.Rewind();
        asIScriptModule* loaded = loadedEngine.Get()->GetModule("saved-loaded", asGM_ALWAYS_CREATE);
        ASSERT_TRUE(loaded != nullptr);
        bool stripped = !stripDebug;
        ASSERT_TRUE(loaded->LoadByteCode(&stream, &stripped) >= 0);
        EXPECT_EQ(stripped, stripDebug);
        EXPECT_TRUE(Run(loadedEngine.Get(), loaded->GetFunctionByDecl("void MixedAndReturns(uint)"), 7));
        EXPECT_TRUE(Run(loadedEngine.Get(), loaded->GetFunctionByDecl("void TripleLocal()")));
        EXPECT_TRUE(Run(loadedEngine.Get(), loaded->GetFunctionByDecl("void Dispatch()")));
        EXPECT_TRUE(Run(loadedEngine.Get(), loaded->GetFunctionByDecl("void Grow(uint)"), 96));
        EXPECT_TRUE(Run(loadedEngine.Get(), loaded->GetFunctionByDecl("void CaughtNative()")));

        // A second serialization after the reader rebuilt the frame layout must round-trip too.
        MemoryStream again;
        EXPECT_TRUE(loaded->SaveByteCode(&again, stripDebug) >= 0);
        asIScriptModule* reloaded = loadedEngine.Get()->GetModule("saved-twice", asGM_ALWAYS_CREATE);
        ASSERT_TRUE(reloaded != nullptr);
        ASSERT_TRUE(reloaded->LoadByteCode(&again) >= 0);
        EXPECT_TRUE(Run(loadedEngine.Get(), reloaded->GetFunctionByDecl("void TripleLocal()")));
        EXPECT_TRUE(Run(loadedEngine.Get(), reloaded->GetFunctionByDecl("void MixedAndReturns(uint)"), 7));
        loadedEngine.Release();

        EXPECT_GT(probe.Get().constructions, 0u);
        ExpectCleanProbe(probe.Get());
    }

} // namespace

TEST(AngelScriptStackAlignment_StringLocalsAtOddDwordOffsets)
{
    ProbeScope probe;
    {
        EngineScope engine(MakeEngine());
        ASSERT_TRUE(engine.Get() != nullptr);
        asIScriptModule* module = BuildModule(engine.Get(), "alignment");
        ASSERT_TRUE(module != nullptr);
        EXPECT_TRUE(Run(engine.Get(), module->GetFunctionByDecl("void MixedAndReturns(uint)"), 7));
    }
    // Three locals, a native return and a script return, each observed once.
    EXPECT_EQ(probe.Get().observations, 4u);
    ExpectCleanProbe(probe.Get());
}

TEST(AngelScriptStackAlignment_RecursionGrowsStackBlock)
{
    ProbeScope probe;
    {
        EngineScope engine(MakeEngine());
        ASSERT_TRUE(engine.Get() != nullptr);
        asIScriptModule* module = BuildModule(engine.Get(), "alignment");
        ASSERT_TRUE(module != nullptr);
        // 97 frames of locals overflow the 64-dword initial block many times over.
        EXPECT_TRUE(Run(engine.Get(), module->GetFunctionByDecl("void Grow(uint)"), 96));
    }
    EXPECT_EQ(probe.Get().constructions, 97u);
    EXPECT_EQ(probe.Get().observations, 2u * 97u);
    ExpectCleanProbe(probe.Get());
}

TEST(AngelScriptStackAlignment_ScriptStringReturnSlot)
{
    ProbeScope probe;
    {
        EngineScope engine(MakeEngine());
        ASSERT_TRUE(engine.Get() != nullptr);
        asIScriptModule* module = BuildModule(engine.Get(), "alignment");
        ASSERT_TRUE(module != nullptr);
        asIScriptContext* context = engine.Get()->CreateContext();
        ASSERT_TRUE(context != nullptr);
        const bool finished = context->Prepare(module->GetFunctionByDecl("string ScriptString(uint)")) >= 0 &&
                              context->SetArgDWord(0, 9) >= 0 && context->Execute() == asEXECUTION_FINISHED;
        EXPECT_TRUE(finished);
        void* result = finished ? context->GetReturnObject() : nullptr;
        EXPECT_TRUE(result != nullptr);
        if (result != nullptr)
        {
            CheckString(*static_cast<std::string*>(result), "script");
        }
        context->Release();
    }
    EXPECT_EQ(probe.Get().observations, 2u);
    ExpectCleanProbe(probe.Get());
}

TEST(AngelScriptStackAlignment_TwelveByteValueType)
{
    ProbeScope probe;
    {
        EngineScope engine(MakeEngine());
        ASSERT_TRUE(engine.Get() != nullptr);
        asIScriptModule* module = BuildModule(engine.Get(), "alignment");
        ASSERT_TRUE(module != nullptr);
        EXPECT_TRUE(Run(engine.Get(), module->GetFunctionByDecl("void TripleLocal()")));
    }
    EXPECT_EQ(probe.Get().observations, 1u);
    ExpectCleanProbe(probe.Get());
}

TEST(AngelScriptStackAlignment_NestedContextCall)
{
    ProbeScope probe;
    {
        EngineScope engine(MakeEngine());
        ASSERT_TRUE(engine.Get() != nullptr);
        asIScriptModule* module = BuildModule(engine.Get(), "alignment");
        ASSERT_TRUE(module != nullptr);
        probe.Get().nestedFunction = module->GetFunctionByDecl("string NestedString(uint)");
        // Nested() pushes the context state from native code and runs NestedString(3) recursively.
        EXPECT_TRUE(Run(engine.Get(), module->GetFunctionByDecl("void Nested()")));
    }
    EXPECT_EQ(probe.Get().observations, 1u + 4u + 1u);
    ExpectCleanProbe(probe.Get());
}

TEST(AngelScriptStackAlignment_InterfaceAndCallbackDispatch)
{
    ProbeScope probe;
    {
        EngineScope engine(MakeEngine());
        ASSERT_TRUE(engine.Get() != nullptr);
        asIScriptModule* module = BuildModule(engine.Get(), "alignment");
        ASSERT_TRUE(module != nullptr);
        EXPECT_TRUE(Run(engine.Get(), module->GetFunctionByDecl("void Dispatch()")));
    }
    // Args, an interface method, a function handle and a delegate, each observed inside and outside.
    EXPECT_EQ(probe.Get().observations, 2u + 2u + 2u + 2u);
    ExpectCleanProbe(probe.Get());
}

TEST(AngelScriptStackAlignment_RepeatedNativeReturns)
{
    ProbeScope probe;
    {
        EngineScope engine(MakeEngine());
        ASSERT_TRUE(engine.Get() != nullptr);
        asIScriptModule* module = BuildModule(engine.Get(), "alignment");
        ASSERT_TRUE(module != nullptr);
        asIScriptFunction* nativeReturn = module->GetFunctionByDecl("string NativeReturnOnly(uint)");
        ASSERT_TRUE(nativeReturn != nullptr);

        // Reuse one context across repeated script-to-native return-on-stack calls.
        asIScriptContext* context = engine.Get()->CreateContext();
        ASSERT_TRUE(context != nullptr);
        for (asDWORD i = 0; i != 4; ++i)
        {
            const bool finished = context->Prepare(nativeReturn) >= 0 && context->SetArgDWord(0, 7) >= 0 &&
                                  context->Execute() == asEXECUTION_FINISHED;
            EXPECT_TRUE(finished);
            void* result = finished ? context->GetReturnObject() : nullptr;
            EXPECT_TRUE(result != nullptr);
            if (result != nullptr)
            {
                CheckString(*static_cast<std::string*>(result), "native");
            }
        }

        // A directly prepared native function has no script caller's return slot.
        asIScriptFunction* directNative = engine.Get()->GetGlobalFunctionByDecl("string NativeString(uint)");
        const bool finished = context->Prepare(directNative) >= 0 && context->SetArgDWord(0, 9) >= 0 &&
                              context->Execute() == asEXECUTION_FINISHED;
        EXPECT_TRUE(finished);
        void* result = finished ? context->GetReturnObject() : nullptr;
        EXPECT_TRUE(result != nullptr);
        if (result != nullptr)
        {
            CheckString(*static_cast<std::string*>(result), "native");
        }
        context->Release();
    }
    EXPECT_EQ(probe.Get().observations, 4u * 2u + 1u);
    ExpectCleanProbe(probe.Get());
}

TEST(AngelScriptStackAlignment_NativeExceptionsUnwindValueObjects)
{
    ProbeScope probe;
    {
        EngineScope engine(MakeEngine());
        ASSERT_TRUE(engine.Get() != nullptr);
        asIScriptModule* module = BuildModule(engine.Get(), "alignment");
        ASSERT_TRUE(module != nullptr);
        EXPECT_TRUE(Run(engine.Get(), module->GetFunctionByDecl("void CaughtNative()")));
        EXPECT_TRUE(Run(engine.Get(), module->GetFunctionByDecl("void UncaughtNative()"), 0, asEXECUTION_EXCEPTION));
    }
    // outer + inner + 6 ThrowDeep markers, then marker + 6 ThrowDeep markers.
    EXPECT_EQ(probe.Get().constructions, 8u + 7u);
    ExpectCleanProbe(probe.Get());
}

TEST(AngelScriptStackAlignment_SuspendAbortAndResume)
{
    ProbeScope probe;
    {
        EngineScope engine(MakeEngine());
        ASSERT_TRUE(engine.Get() != nullptr);
        asIScriptModule* module = BuildModule(engine.Get(), "alignment");
        ASSERT_TRUE(module != nullptr);
        asIScriptFunction* suspended = module->GetFunctionByDecl("void Suspended()");
        ASSERT_TRUE(suspended != nullptr);

        // Suspend, then abort: the suspended frame's string and Tracked must unwind.
        asIScriptContext* aborted = engine.Get()->CreateContext();
        ASSERT_TRUE(aborted != nullptr);
        EXPECT_TRUE(aborted->Prepare(suspended) >= 0);
        EXPECT_EQ(static_cast<int>(aborted->Execute()), static_cast<int>(asEXECUTION_SUSPENDED));
        aborted->Abort();
        EXPECT_EQ(static_cast<int>(aborted->GetState()), static_cast<int>(asEXECUTION_ABORTED));
        aborted->Release();

        // Suspend, then resume to completion.
        probe.Get().suspendRequested = false;
        asIScriptContext* resumed = engine.Get()->CreateContext();
        ASSERT_TRUE(resumed != nullptr);
        EXPECT_TRUE(resumed->Prepare(suspended) >= 0);
        EXPECT_EQ(static_cast<int>(resumed->Execute()), static_cast<int>(asEXECUTION_SUSPENDED));
        EXPECT_EQ(static_cast<int>(resumed->Execute()), static_cast<int>(asEXECUTION_FINISHED));
        resumed->Release();
    }
    EXPECT_EQ(probe.Get().observations, 1u + 2u);
    EXPECT_EQ(probe.Get().constructions, 2u);
    ExpectCleanProbe(probe.Get());
}

TEST(AngelScriptStackAlignment_SavedBytecode)
{
    RunSavedBytecode(false);
}

TEST(AngelScriptStackAlignment_SavedBytecodeStripped)
{
    RunSavedBytecode(true);
}

#endif // SPARK_ANGELSCRIPT_SUPPORT
