// AngelScript packed-stack regression test.
//
// Build this file together with the pinned sdk/angelscript sources and the
// scriptstdstring add-on.  The include paths are deliberately supplied by the
// caller so this remains useful for both the unpatched and patched trees.

#include <angelscript.h>
#include <scriptstdstring/scriptstdstring.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
int failures = 0;
asIScriptFunction* nestedFunction = nullptr;
bool suspendRequested = false;
unsigned observations = 0;
std::set<std::string> reportedFailures;
std::set<void*> liveObjects;
unsigned constructions = 0, destructions = 0;

void Message(const asSMessageInfo* message, void*)
{
    std::fprintf(stderr, "%s:%d:%d: %s\n", message->section, message->row, message->col, message->message);
}

void Fail(const char* what)
{
    // Keep the negative control readable while counting every bad observation.
    if (reportedFailures.insert(what).second)
        std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

void Check(bool condition, const char* what)
{
    if (!condition)
        Fail(what);
}

bool Require(bool condition, const char* what)
{
    Check(condition, what);
    return condition;
}

void CheckString(const std::string& value, const char* expected)
{
    if ((reinterpret_cast<std::uintptr_t>(&value) & 7u) != 0)
        Fail("std::string observed at an address that is not 8-byte aligned");
    Check(value == expected, "string contents changed");
    ++observations;
}

void TrackedConstruct(asIScriptGeneric* generic)
{
    void* pointer = generic->GetObject();
    Check((reinterpret_cast<std::uintptr_t>(pointer) & 7u) == 0, "Tracked constructor alignment");
    Check(liveObjects.insert(pointer).second, "Tracked constructed over a live object");
    ++constructions;
}

void TrackedDestruct(asIScriptGeneric* generic)
{
    void* pointer = generic->GetObject();
    Check((reinterpret_cast<std::uintptr_t>(pointer) & 7u) == 0, "Tracked destructor alignment");
    Check(liveObjects.erase(pointer) == 1, "Tracked destructor got wrong/already destroyed address");
    ++destructions;
}

void Expect(bool value) { Check(value, "script argument/content assertion failed"); }
void GenericExpect(asIScriptGeneric* generic) { Expect(generic->GetArgByte(0) != 0); }

void Observe(const std::string& value)
{
    if (value == "mixed")
        CheckString(value, "mixed");
    else if (value == "native")
        CheckString(value, "native");
    else if (value == "script")
        CheckString(value, "script");
    else if (value == "suspend")
        CheckString(value, "suspend");
    else if (value == "nested")
        CheckString(value, "nested");
    else
        Fail("unexpected string contents observed");
}

struct Triple
{
    int a;
    int b;
    int c;
};

void ObserveTriple(const Triple& value)
{
    Check(value.a == 11 && value.b == 22 && value.c == 33, "Triple contents changed");
    if ((reinterpret_cast<std::uintptr_t>(&value) & 7u) != 0)
        Fail("12-byte Triple observed at an address that is not 8-byte aligned");
}

std::string NativeString(asDWORD n)
{
    Check(n == 7 || n == 9, "native string argument was corrupted");
    return "native";
}

void SuspendPoint()
{
    asIScriptContext* context = asGetActiveContext();
    Check(context != nullptr, "SuspendPoint has no active context");
    if (!suspendRequested)
    {
        suspendRequested = true;
        context->Suspend();
    }
}

void NestedCall();

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
    Check(generic->GetArgDWord(0) == 7 || generic->GetArgDWord(0) == 9,
          "generic native string argument was corrupted");
    std::string result = "native";
    Check(generic->SetReturnObject(&result) >= 0, "generic string return failed");
}

void GenericSuspendPoint(asIScriptGeneric*) { SuspendPoint(); }
void GenericNestedCall(asIScriptGeneric*) { NestedCall(); }
void GenericThrowNative(asIScriptGeneric*) { ThrowNative(); }
void GenericObserveTriple(asIScriptGeneric* generic)
{
    ObserveTriple(*static_cast<const Triple*>(generic->GetArgObject(0)));
}
#endif

void NestedCall()
{
    asIScriptContext* context = asGetActiveContext();
    Check(context != nullptr, "NestedCall has no active context");
    Check(nestedFunction != nullptr, "nested function was not installed");
    Check(context->PushState() >= 0, "PushState failed");
    Check(context->Prepare(nestedFunction) >= 0, "nested Prepare failed");
    Check(context->SetArgDWord(0, 3) >= 0, "nested SetArgDWord failed");
    Check(context->Execute() == asEXECUTION_FINISHED, "nested Execute failed");
    void* result = context->GetReturnObject();
    if (result != nullptr)
        CheckString(*static_cast<std::string*>(result), "nested");
    else
        Fail("nested string return object was null");
    Check(context->PopState() >= 0, "PopState failed");
}

class MemoryStream final : public asIBinaryStream
{
public:
    int Read(void* destination, asUINT size) override
    {
        if (position + size > bytes.size())
            return -1;
        std::memcpy(destination, bytes.data() + position, size);
        position += size;
        return 0;
    }

    int Write(const void* source, asUINT size) override
    {
        const auto* begin = static_cast<const std::uint8_t*>(source);
        bytes.insert(bytes.end(), begin, begin + size);
        return 0;
    }

    void Rewind() { position = 0; }
    bool Save(const char* path)
    {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return bool(file);
    }
    bool Load(const char* path)
    {
        std::ifstream file(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        position = 0;
        return !bytes.empty() && !file.bad();
    }

private:
    std::vector<std::uint8_t> bytes;
    std::size_t position = 0;
};

asIScriptEngine* MakeEngine()
{
    asIScriptEngine* engine = asCreateScriptEngine();
    Check(engine != nullptr, "asCreateScriptEngine failed");
    if (engine == nullptr)
        return nullptr;
    Check(engine->SetMessageCallback(asFUNCTION(Message), nullptr, asCALL_CDECL) >= 0, "message callback");
    Check(engine->SetEngineProperty(asEP_ALLOW_UNSAFE_REFERENCES, true) >= 0, "reference test setting");
    RegisterStdString(engine);
    Check(engine->RegisterGlobalFunction("void Expect(bool)", asFUNCTION(GenericExpect), asCALL_GENERIC) >= 0,
          "Expect registration");
    Check(engine->RegisterObjectType("Tracked", 8, asOBJ_VALUE | asOBJ_APP_CLASS_CD) >= 0, "Tracked registration");
    Check(engine->RegisterObjectBehaviour("Tracked", asBEHAVE_CONSTRUCT, "void f()",
          asFUNCTION(TrackedConstruct), asCALL_GENERIC) >= 0, "Tracked constructor registration");
    Check(engine->RegisterObjectBehaviour("Tracked", asBEHAVE_DESTRUCT, "void f()",
          asFUNCTION(TrackedDestruct), asCALL_GENERIC) >= 0, "Tracked destructor registration");
#if defined(AS_MAX_PORTABILITY)
    Check(engine->RegisterGlobalFunction("void Observe(const string &in)",
          asFUNCTION(GenericObserve), asCALL_GENERIC) >= 0, "Observe registration failed");
    Check(engine->RegisterGlobalFunction("string NativeString(uint)",
          asFUNCTION(GenericNativeString), asCALL_GENERIC) >= 0, "NativeString registration failed");
    Check(engine->RegisterGlobalFunction("void SuspendPoint()",
          asFUNCTION(GenericSuspendPoint), asCALL_GENERIC) >= 0, "SuspendPoint registration failed");
    Check(engine->RegisterGlobalFunction("void NestedCall()",
          asFUNCTION(GenericNestedCall), asCALL_GENERIC) >= 0, "NestedCall registration failed");
#else
    Check(engine->RegisterGlobalFunction("void Observe(const string &in)",
          asFUNCTION(Observe), asCALL_CDECL) >= 0, "Observe registration failed");
    Check(engine->RegisterGlobalFunction("string NativeString(uint)",
          asFUNCTION(NativeString), asCALL_CDECL) >= 0, "NativeString registration failed");
    Check(engine->RegisterGlobalFunction("void SuspendPoint()",
          asFUNCTION(SuspendPoint), asCALL_CDECL) >= 0, "SuspendPoint registration failed");
    Check(engine->RegisterGlobalFunction("void NestedCall()",
          asFUNCTION(NestedCall), asCALL_CDECL) >= 0, "NestedCall registration failed");
#endif
#if defined(AS_MAX_PORTABILITY)
    Check(engine->RegisterGlobalFunction("void ThrowNative()", asFUNCTION(GenericThrowNative), asCALL_GENERIC) >= 0,
          "ThrowNative registration failed");
#else
    Check(engine->RegisterGlobalFunction("void ThrowNative()", asFUNCTION(ThrowNative), asCALL_CDECL) >= 0,
          "ThrowNative registration failed");
#endif
    Check(engine->RegisterObjectType("Triple", sizeof(Triple), asOBJ_VALUE | asOBJ_POD | asGetTypeTraits<Triple>() | asOBJ_APP_CLASS_ALLINTS) >= 0,
          "Triple registration failed");
    Check(engine->RegisterObjectProperty("Triple", "int a", asOFFSET(Triple, a)) >= 0,
          "Triple.a registration failed");
    Check(engine->RegisterObjectProperty("Triple", "int b", asOFFSET(Triple, b)) >= 0,
          "Triple.b registration failed");
    Check(engine->RegisterObjectProperty("Triple", "int c", asOFFSET(Triple, c)) >= 0,
          "Triple.c registration failed");
#ifdef AS_MAX_PORTABILITY
    Check(engine->RegisterGlobalFunction("void ObserveTriple(const Triple &in)", asFUNCTION(GenericObserveTriple), asCALL_GENERIC) >= 0,
#else
    Check(engine->RegisterGlobalFunction("void ObserveTriple(const Triple &in)", asFUNCTION(ObserveTriple), asCALL_CDECL) >= 0,
#endif
          "ObserveTriple registration failed");
    // Small initial blocks force the argument-copy and stack-block growth path.
    Check(engine->SetEngineProperty(asEP_INIT_STACK_SIZE, 64) >= 0,
          "could not set initial script stack size");
    return engine;
}

asIScriptModule* BuildModule(asIScriptEngine* engine, const char* name)
{
    asIScriptModule* module = engine->GetModule(name, asGM_ALWAYS_CREATE);
    const char* source = R"(
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
    if (!Require(module != nullptr, "GetModule failed") ||
        !Require(module->AddScriptSection("alignment.as", source) >= 0, "AddScriptSection failed") ||
        !Require(module->Build() >= 0, "script module Build failed"))
        return nullptr;
    return module;
}

void Run(asIScriptEngine* engine, asIScriptFunction* function, asDWORD argument = 0)
{
    if (!Require(engine != nullptr && function != nullptr, "Run received a null engine/function"))
        return;
    asIScriptContext* context = engine->CreateContext();
    if (!Require(context != nullptr, "CreateContext failed"))
        return;
    if (!Require(context->Prepare(function) >= 0, "Prepare failed"))
    {
        context->Release();
        return;
    }
    if (function->GetParamCount() != 0)
    {
        if (!Require(context->SetArgDWord(0, argument) >= 0, "SetArgDWord failed"))
        {
            context->Release();
            return;
        }
    }
    Require(context->Execute() == asEXECUTION_FINISHED, "script Execute failed");
    context->Release();
}

void RunReturn(asIScriptEngine* engine, asIScriptFunction* function, asDWORD argument)
{
    if (!Require(engine != nullptr && function != nullptr, "RunReturn received a null engine/function"))
        return;
    asIScriptContext* context = engine->CreateContext();
    if (!Require(context != nullptr, "return CreateContext failed"))
        return;
    if (!Require(context->Prepare(function) >= 0, "return Prepare failed") ||
        !Require(context->SetArgDWord(0, argument) >= 0, "return SetArgDWord failed") ||
        !Require(context->Execute() == asEXECUTION_FINISHED, "return Execute failed"))
    {
        context->Release();
        return;
    }
    void* result = context->GetReturnObject();
    if (result != nullptr)
        CheckString(*static_cast<std::string*>(result), "script");
    else
        Fail("script return object was null");
    context->Release();
}

void RunSaved(bool stripDebug)
{
    asIScriptEngine* sourceEngine = MakeEngine();
    if (!Require(sourceEngine != nullptr, "saved source engine creation failed"))
        return;
    asIScriptModule* sourceModule = BuildModule(sourceEngine, "saved-source");
    if (!Require(sourceModule != nullptr, "saved source module creation failed"))
    {
        sourceEngine->ShutDownAndRelease();
        return;
    }
    MemoryStream stream;
    Check(sourceModule->SaveByteCode(&stream, stripDebug) >= 0, "SaveByteCode failed");
    sourceEngine->ShutDownAndRelease();

    asIScriptEngine* loadedEngine = MakeEngine();
    if (!Require(loadedEngine != nullptr, "saved loaded engine creation failed"))
        return;
    stream.Rewind();
    asIScriptModule* loaded = loadedEngine->GetModule("saved-loaded", asGM_ALWAYS_CREATE);
    if (!Require(loaded != nullptr, "saved loaded module creation failed"))
    {
        loadedEngine->ShutDownAndRelease();
        return;
    }
    bool stripped = false;
    Check(loaded->LoadByteCode(&stream, &stripped) >= 0, "LoadByteCode failed");
    Check(stripped == stripDebug, "LoadByteCode debug-strip flag mismatch");
    Run(loadedEngine, loaded->GetFunctionByDecl("void MixedAndReturns(uint)"), 7);
    Run(loadedEngine, loaded->GetFunctionByDecl("void TripleLocal()"));
    Run(loadedEngine, loaded->GetFunctionByDecl("void Dispatch()"));
    Run(loadedEngine, loaded->GetFunctionByDecl("void Grow(uint)"), 96);
    Run(loadedEngine, loaded->GetFunctionByDecl("void CaughtNative()"));
    // Exercise a second serialization after the reader rebuilt the frame layout.
    MemoryStream again;
    Check(loaded->SaveByteCode(&again, stripDebug) >= 0, "second SaveByteCode failed");
    asIScriptModule* reloaded = loadedEngine->GetModule("saved-twice", asGM_ALWAYS_CREATE);
    Check(reloaded->LoadByteCode(&again) >= 0, "second LoadByteCode failed");
    Run(loadedEngine, reloaded->GetFunctionByDecl("void TripleLocal()"));
    Run(loadedEngine, reloaded->GetFunctionByDecl("void MixedAndReturns(uint)"), 7);
    loadedEngine->ShutDownAndRelease();
}
} // namespace

int main(int argc, char** argv)
{
    asIScriptEngine* engine = MakeEngine();
    if (engine == nullptr)
        return 1;
    asIScriptModule* module = nullptr;
    if (argc == 3 && std::strcmp(argv[1], "--load") == 0)
    {
        MemoryStream stream;
        if (!Require(stream.Load(argv[2]), "bytecode file read failed"))
            return 1;
        module = engine->GetModule("alignment", asGM_ALWAYS_CREATE);
        if (!Require(module->LoadByteCode(&stream) >= 0, "legacy bytecode load failed"))
            return 1;
    }
    else
        module = BuildModule(engine, "alignment");
    if (!Require(module != nullptr, "alignment module creation failed"))
    {
        engine->ShutDownAndRelease();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 3 && std::strcmp(argv[1], "--save") == 0)
    {
        MemoryStream stream;
        Check(module->SaveByteCode(&stream, true) >= 0, "bytecode file serialization failed");
        Check(stream.Save(argv[2]), "bytecode file write failed");
        engine->ShutDownAndRelease();
        return failures == 0 ? 0 : 1;
    }
    nestedFunction = module->GetFunctionByDecl("string NestedString(uint)");

    Run(engine, module->GetFunctionByDecl("void MixedAndReturns(uint)"), 7);
    Run(engine, module->GetFunctionByDecl("void Grow(uint)"), 96);
    RunReturn(engine, module->GetFunctionByDecl("string ScriptString(uint)"), 9);
    Run(engine, module->GetFunctionByDecl("void TripleLocal()"));
    Run(engine, module->GetFunctionByDecl("void Nested()"));
    Run(engine, module->GetFunctionByDecl("void Dispatch()"));

    // Reuse one context across repeated script-to-native return-on-stack calls.
    asIScriptFunction* nativeReturn = module->GetFunctionByDecl("string NativeReturnOnly(uint)");
    asIScriptContext* repeated = engine->CreateContext();
    if (Require(repeated != nullptr, "repeated Prepare CreateContext failed") &&
        Require(nativeReturn != nullptr, "NativeReturnOnly was not found"))
    {
        for (asDWORD i = 0; i != 4; ++i)
        {
            if (!Require(repeated->Prepare(nativeReturn) >= 0, "repeated Prepare failed") ||
                !Require(repeated->SetArgDWord(0, 7) >= 0, "repeated SetArgDWord failed") ||
                !Require(repeated->Execute() == asEXECUTION_FINISHED, "repeated Execute failed"))
                break;
            void* result = repeated->GetReturnObject();
            if (result != nullptr)
                CheckString(*static_cast<std::string*>(result), "native");
            else
                Fail("repeated native return object was null");
        }
        // A directly prepared native function has no script caller return slot.
        asIScriptFunction* directNative = engine->GetGlobalFunctionByDecl("string NativeString(uint)");
        Check(repeated->Prepare(directNative) >= 0, "direct native Prepare failed");
        Check(repeated->SetArgDWord(0, 9) >= 0, "direct native SetArgDWord failed");
        Check(repeated->Execute() == asEXECUTION_FINISHED, "direct native Execute failed");
        void* result = repeated->GetReturnObject();
        if (Require(result != nullptr, "direct native return was null"))
            CheckString(*static_cast<std::string*>(result), "native");
    }
    if (repeated != nullptr)
        repeated->Release();

    Run(engine, module->GetFunctionByDecl("void CaughtNative()"));
    asIScriptContext* uncaught = engine->CreateContext();
    if (Require(uncaught != nullptr, "uncaught exception CreateContext failed") &&
        Require(uncaught->Prepare(module->GetFunctionByDecl("void UncaughtNative()")) >= 0,
                "uncaught exception Prepare failed"))
    {
        Check(uncaught->Execute() == asEXECUTION_EXCEPTION, "uncaught native exception was not reported");
        uncaught->ClearExceptionCallback();
    }
    if (uncaught != nullptr)
        uncaught->Release();

    suspendRequested = false;
    asIScriptContext* suspended = engine->CreateContext();
    if (Require(suspended != nullptr, "suspend CreateContext failed") &&
        Require(suspended->Prepare(module->GetFunctionByDecl("void Suspended()")) >= 0, "suspend Prepare failed"))
    {
        Check(suspended->Execute() == asEXECUTION_SUSPENDED, "first Execute did not suspend");
        suspended->Abort();
        Check(suspended->GetState() == asEXECUTION_ABORTED, "Abort did not unwind suspended frame");
    }
    if (suspended != nullptr)
        suspended->Release();

    suspendRequested = false;
    asIScriptContext* resumed = engine->CreateContext();
    if (Require(resumed != nullptr, "resume CreateContext failed") &&
        Require(resumed->Prepare(module->GetFunctionByDecl("void Suspended()")) >= 0,
                "resume Prepare failed"))
    {
        Check(resumed->Execute() == asEXECUTION_SUSPENDED, "resume first Execute did not suspend");
        Check(resumed->Execute() == asEXECUTION_FINISHED, "resume Execute failed");
    }
    if (resumed != nullptr)
        resumed->Release();

    RunSaved(false);
    RunSaved(true);
    engine->ShutDownAndRelease();
    nestedFunction = nullptr;
    Check(liveObjects.empty(), "value objects survived context cleanup");
    Check(constructions == destructions && constructions > 0, "constructor/destructor balance");

    if (failures != 0)
        std::fprintf(stderr, "%d alignment regression checks failed\n", failures);
    std::printf("%s: %u string observations; %u constructions / %u destructions; %d failures\n",
                failures == 0 ? "PASS" : "FAIL", observations, constructions, destructions, failures);
    return failures == 0 ? 0 : 1;
}
