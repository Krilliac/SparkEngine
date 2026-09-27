/**
 * @file TestENG200ScriptHotReloadReal.cpp
 * @brief ENG-200: AngelScriptEngine::HotReloadModule() follows the documented state rules.
 *
 * Each case writes a real .as file, compiles and attaches it through the
 * production AngelScriptEngine, mutates the instance by running Update(),
 * rewrites the file and hot-reloads it. The script reports its fields through
 * debugTrace(), so every assertion reads state the reloaded script itself
 * sees. Rules R1-R8 are listed on AngelScriptEngine::HotReloadModule() and in
 * wiki/subsystems/Scripting-with-AngelScript.md ("Hot-reload state rules").
 * Before the rules existed every field was reset by the reload, so the
 * carry-over cases fail against that version.
 */

#include "TestFramework.h"

#ifdef SPARK_ANGELSCRIPT_SUPPORT

#include "Engine/Scripting/AngelScriptEngine.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
    namespace fs = std::filesystem;

    std::string g_lastState; ///< Output of the last debugTrace(1, ...) call
    int g_startCount = 0;    ///< Number of debugTrace(9, ...) calls (made only by Start())

    void CaptureTrace(uint32_t nodeId, const char* /*nodeName*/, const char* output)
    {
        if (nodeId == 9)
            ++g_startCount;
        else if (nodeId == 1)
            g_lastState = output ? output : "";
    }

    /// A real engine plus a private temp directory holding the reloaded script file.
    struct HotReloadFixture
    {
        AngelScriptEngine engine;
        fs::path directory;
        bool ready = false;

        HotReloadFixture()
        {
            static std::atomic<uint32_t> sequence{0};
            directory =
                fs::temp_directory_path() /
                ("spark_eng200_reload_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "_" + std::to_string(sequence++));
            std::error_code error;
            fs::create_directories(directory, error);
            g_lastState.clear();
            g_startCount = 0;
            ASSetDebugTraceCallback(&CaptureTrace);
            ready = !error && engine.Initialize();
        }

        ~HotReloadFixture()
        {
            engine.Shutdown();
            ASSetDebugTraceCallback(nullptr);
            std::error_code error;
            fs::remove_all(directory, error);
        }

        HotReloadFixture(const HotReloadFixture&) = delete;
        HotReloadFixture& operator=(const HotReloadFixture&) = delete;

        /// (Re)write <module>.as; the module name is the file stem.
        std::string Write(const std::string& moduleName, const char* source) const
        {
            const fs::path path = directory / (moduleName + ".as");
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << source;
            return path.string();
        }

        /// Write, compile and attach @p className to @p entity.
        bool Load(const std::string& moduleName, const char* source, EntityID entity, const char* className)
        {
            if (!engine.CompileScriptFile(Write(moduleName, source)) ||
                !engine.AttachScript(entity, className, moduleName))
            {
                std::printf("  script diagnostic: %s\n", engine.GetLastError().c_str());
                return false;
            }
            return true;
        }

        /// Run Update(0): the scripts below only report their state when dt is 0.
        std::string State(EntityID entity)
        {
            g_lastState.clear();
            engine.CallUpdate(entity, 0.0f);
            return g_lastState;
        }
    };

    EntityID MakeEntity(uint32_t id)
    {
        return static_cast<EntityID>(id);
    }

    bool HasNoteContaining(const AngelScriptEngine::HotReloadReport& report, const std::string& text)
    {
        for (const auto& note : report.notes)
        {
            if (note.find(text) != std::string::npos)
                return true;
        }
        return false;
    }

    // v1 of the reloaded class: Update(dt > 0) mutates every carryable field kind.
    const char* const kKeeperV1 =
        "class Keeper\n"
        "{\n"
        "    int counter = 0;\n"
        "    float speed = 1.5f;\n"
        "    bool armed = false;\n"
        "    string label = \"v1\";\n"
        "    Vector3 home;\n"
        "    Keeper() { home = Vector3(0.0f, 0.0f, 0.0f); }\n"
        "    void Update(float dt)\n"
        "    {\n"
        "        if (dt > 0.0f)\n"
        "        {\n"
        "            counter++; speed += dt; armed = true; label += \"!\"; home.x += 1.0f;\n"
        "            return;\n"
        "        }\n"
        "        debugTrace(1, \"state\", \"v1:\" + counter + \"|\" + speed + \"|\" + armed + "
        "\"|\" + label + \"|\" + home.x);\n"
        "    }\n"
        "}\n";
} // namespace

TEST(ScriptHotReload_ENG200_CarriesSameNameSameTypeFields)
{
    HotReloadFixture fx;
    ASSERT_TRUE(fx.ready);
    const EntityID entity = MakeEntity(21);
    ASSERT_TRUE(fx.Load("ENG200ReloadCarry", kKeeperV1, entity, "Keeper"));
    fx.engine.CallUpdate(entity, 1.0f);
    fx.engine.CallUpdate(entity, 1.0f);
    EXPECT_EQ(fx.State(entity), std::string("v1:2|3.5|true|v1!!|2"));

    // v2 changes the code and every constructor default; the carried values must win (R2, R3).
    fx.Write("ENG200ReloadCarry",
             "class Keeper\n"
             "{\n"
             "    int counter = 100;\n"
             "    float speed = 9.0f;\n"
             "    bool armed = false;\n"
             "    string label = \"fresh\";\n"
             "    Vector3 home;\n"
             "    Keeper() { home = Vector3(-5.0f, 0.0f, 0.0f); }\n"
             "    void Update(float dt)\n"
             "    {\n"
             "        debugTrace(1, \"state\", \"v2:\" + counter + \"|\" + speed + \"|\" + armed + "
             "\"|\" + label + \"|\" + home.x);\n"
             "    }\n"
             "}\n");
    ASSERT_TRUE(fx.engine.HotReloadModule("ENG200ReloadCarry"));
    EXPECT_EQ(fx.State(entity), std::string("v2:2|3.5|true|v1!!|2"));

    const auto& report = fx.engine.GetLastHotReloadReport();
    EXPECT_EQ(report.instances, static_cast<size_t>(1));
    EXPECT_EQ(report.carried, static_cast<size_t>(5));
    EXPECT_EQ(report.defaulted, static_cast<size_t>(0));
    EXPECT_EQ(report.dropped, static_cast<size_t>(0));
    EXPECT_EQ(report.failedAttaches, static_cast<size_t>(0));
    EXPECT_TRUE(report.notes.empty());
}

TEST(ScriptHotReload_ENG200_NewFieldTakesConstructorDefault)
{
    HotReloadFixture fx;
    ASSERT_TRUE(fx.ready);
    const EntityID entity = MakeEntity(22);
    ASSERT_TRUE(fx.Load("ENG200ReloadNewField", kKeeperV1, entity, "Keeper"));
    fx.engine.CallUpdate(entity, 1.0f);

    fx.Write("ENG200ReloadNewField",
             "class Keeper\n"
             "{\n"
             "    int counter = 0;\n"
             "    int bonus = 7;\n"
             "    string title;\n"
             "    Keeper() { title = \"ctor\"; }\n"
             "    void Update(float dt)\n"
             "    {\n"
             "        debugTrace(1, \"state\", \"\" + counter + \"|\" + bonus + \"|\" + title);\n"
             "    }\n"
             "}\n");
    ASSERT_TRUE(fx.engine.HotReloadModule("ENG200ReloadNewField"));
    EXPECT_EQ(fx.State(entity), std::string("1|7|ctor"));

    // counter carried; bonus and title are new (R5); the four fields v2 no longer declares are removed.
    const auto& report = fx.engine.GetLastHotReloadReport();
    EXPECT_EQ(report.carried, static_cast<size_t>(1));
    EXPECT_EQ(report.defaulted, static_cast<size_t>(2));
    EXPECT_EQ(report.dropped, static_cast<size_t>(4));
}

TEST(ScriptHotReload_ENG200_RetypedAndRemovedFieldsAreDroppedAndReported)
{
    HotReloadFixture fx;
    ASSERT_TRUE(fx.ready);
    const EntityID entity = MakeEntity(23);
    ASSERT_TRUE(fx.Load("ENG200ReloadRetype", kKeeperV1, entity, "Keeper"));
    fx.engine.CallUpdate(entity, 1.0f);

    // speed changes type, armed disappears; the rest is unchanged.
    fx.Write("ENG200ReloadRetype", "class Keeper\n"
                                   "{\n"
                                   "    int counter = 0;\n"
                                   "    int speed = 40;\n"
                                   "    string label = \"v2\";\n"
                                   "    Vector3 home;\n"
                                   "    Keeper() { home = Vector3(0.0f, 0.0f, 0.0f); }\n"
                                   "    void Update(float dt)\n"
                                   "    {\n"
                                   "        debugTrace(1, \"state\", \"\" + counter + \"|\" + speed + \"|\" + label);\n"
                                   "    }\n"
                                   "}\n");
    ASSERT_TRUE(fx.engine.HotReloadModule("ENG200ReloadRetype"));
    EXPECT_EQ(fx.State(entity), std::string("1|40|v1!"));

    const auto& report = fx.engine.GetLastHotReloadReport();
    EXPECT_EQ(report.carried, static_cast<size_t>(3));
    EXPECT_EQ(report.dropped, static_cast<size_t>(2));
    EXPECT_TRUE(HasNoteContaining(report, "ENG200ReloadRetype::Keeper.speed: retyped from float to int"));
    EXPECT_TRUE(HasNoteContaining(report, "ENG200ReloadRetype::Keeper.armed: removed"));
}

TEST(ScriptHotReload_ENG200_HandlesAreNotCarried)
{
    HotReloadFixture fx;
    ASSERT_TRUE(fx.ready);
    const char* const source = "class Box { int value = 1; }\n"
                               "class Holder\n"
                               "{\n"
                               "    Box@ box;\n"
                               "    array<int> items = {1};\n"
                               "    int kept = 0;\n"
                               "    Holder() { @box = Box(); }\n"
                               "    void Update(float dt)\n"
                               "    {\n"
                               "        if (dt > 0.0f) { box.value = 42; items.insertLast(2); kept = 5; return; }\n"
                               "        debugTrace(1, \"state\", \"\" + box.value + \"|\" + items.length() + \"|\" + "
                               "kept);\n"
                               "    }\n"
                               "}\n";
    const EntityID entity = MakeEntity(24);
    ASSERT_TRUE(fx.Load("ENG200ReloadHandles", source, entity, "Holder"));
    fx.engine.CallUpdate(entity, 1.0f);
    EXPECT_EQ(fx.State(entity), std::string("42|2|5"));

    // Same source: the handle and the array keep their constructor values (R4), the int is carried.
    ASSERT_TRUE(fx.engine.HotReloadModule("ENG200ReloadHandles"));
    EXPECT_EQ(fx.State(entity), std::string("1|1|5"));

    const auto& report = fx.engine.GetLastHotReloadReport();
    EXPECT_EQ(report.carried, static_cast<size_t>(1));
    EXPECT_EQ(report.dropped, static_cast<size_t>(2));
    EXPECT_TRUE(HasNoteContaining(report, "Holder.box: Box@ is not carried"));
    // The engine registers the add-on array as the default array type, so AngelScript declares it as "int[]".
    EXPECT_TRUE(HasNoteContaining(report, "Holder.items: int[] is not carried"));
}

TEST(ScriptHotReload_ENG200_StartIsNotRerunAndUpdateContinues)
{
    HotReloadFixture fx;
    ASSERT_TRUE(fx.ready);
    const char* const source = "class Ticker\n"
                               "{\n"
                               "    int starts = 0;\n"
                               "    int ticks = 0;\n"
                               "    void Start() { starts++; debugTrace(9, \"start\", \"\"); }\n"
                               "    void Update(float dt)\n"
                               "    {\n"
                               "        if (dt > 0.0f) { ticks++; return; }\n"
                               "        debugTrace(1, \"state\", \"\" + starts + \"|\" + ticks);\n"
                               "    }\n"
                               "}\n";
    const EntityID entity = MakeEntity(25);
    ASSERT_TRUE(fx.Load("ENG200ReloadStart", source, entity, "Ticker"));
    fx.engine.CallStart(entity);
    fx.engine.CallUpdate(entity, 0.1f);
    fx.engine.CallUpdate(entity, 0.1f);
    EXPECT_EQ(g_startCount, 1);

    ASSERT_TRUE(fx.engine.HotReloadModule("ENG200ReloadStart"));
    EXPECT_EQ(g_startCount, 1); // R6: the reload itself does not call Start()

    // The next Update() continues from the carried state.
    fx.engine.CallUpdate(entity, 0.1f);
    EXPECT_EQ(fx.State(entity), std::string("1|3"));
    EXPECT_EQ(g_startCount, 1);
}

TEST(ScriptHotReload_ENG200_CompileErrorLeavesInstancesAndStateIntact)
{
    HotReloadFixture fx;
    ASSERT_TRUE(fx.ready);
    const EntityID entity = MakeEntity(26);
    ASSERT_TRUE(fx.Load("ENG200ReloadBroken", kKeeperV1, entity, "Keeper"));
    fx.engine.CallUpdate(entity, 1.0f);

    fx.Write("ENG200ReloadBroken", "class Keeper\n"
                                   "{\n"
                                   "    int counter = 0;\n"
                                   "    void Update(float dt) { counter = undefinedReloadSymbol; }\n"
                                   "}\n");
    EXPECT_FALSE(fx.engine.HotReloadModule("ENG200ReloadBroken"));
    EXPECT_STR_CONTAINS(fx.engine.GetLastError(), "Hot-reload aborted");
    EXPECT_STR_CONTAINS(fx.engine.GetLastError(), "undefinedReloadSymbol");

    // R1: the old code and its state are untouched.
    EXPECT_FALSE(fx.engine.IsScriptFaulted(entity));
    EXPECT_EQ(fx.State(entity), std::string("v1:1|2.5|true|v1!|1"));
    EXPECT_EQ(fx.engine.GetLastHotReloadReport().instances, static_cast<size_t>(0));
}

TEST(ScriptHotReload_ENG200_FaultedInstanceIsClearedButKeepsState)
{
    HotReloadFixture fx;
    ASSERT_TRUE(fx.ready);
    const char* const faulty = "class Fragile\n"
                               "{\n"
                               "    int counter = 0;\n"
                               "    int zero = 0;\n"
                               "    void Update(float dt)\n"
                               "    {\n"
                               "        counter++;\n"
                               "        if (counter == 2) { int boom = 1 / zero; }\n"
                               "    }\n"
                               "}\n";
    const EntityID entity = MakeEntity(27);
    ASSERT_TRUE(fx.Load("ENG200ReloadFaulted", faulty, entity, "Fragile"));
    fx.engine.CallUpdate(entity, 0.1f);
    fx.engine.CallUpdate(entity, 0.1f);
    ASSERT_TRUE(fx.engine.IsScriptFaulted(entity));

    fx.Write("ENG200ReloadFaulted", "class Fragile\n"
                                    "{\n"
                                    "    int counter = 0;\n"
                                    "    int zero = 0;\n"
                                    "    void Update(float dt) { debugTrace(1, \"state\", \"\" + counter); }\n"
                                    "}\n");
    ASSERT_TRUE(fx.engine.HotReloadModule("ENG200ReloadFaulted"));

    // R7: dispatch resumes, and the counter the faulting call had reached is carried.
    EXPECT_FALSE(fx.engine.IsScriptFaulted(entity));
    EXPECT_EQ(fx.State(entity), std::string("2"));
}

TEST(ScriptHotReload_ENG200_MissingClassFailsReattachAndReports)
{
    HotReloadFixture fx;
    ASSERT_TRUE(fx.ready);
    const EntityID entity = MakeEntity(28);
    ASSERT_TRUE(fx.Load("ENG200ReloadMissing", kKeeperV1, entity, "Keeper"));

    fx.Write("ENG200ReloadMissing", "class Renamed\n"
                                    "{\n"
                                    "    void Update(float dt) { debugTrace(1, \"state\", \"renamed\"); }\n"
                                    "}\n");
    EXPECT_FALSE(fx.engine.HotReloadModule("ENG200ReloadMissing"));

    // R8: the entity is left without a script and the failure is reported.
    const auto& report = fx.engine.GetLastHotReloadReport();
    EXPECT_EQ(report.failedAttaches, static_cast<size_t>(1));
    EXPECT_EQ(report.instances, static_cast<size_t>(0));
    EXPECT_TRUE(HasNoteContaining(report, "ENG200ReloadMissing::Keeper: re-attach to entity 28 failed"));
    EXPECT_TRUE(fx.engine.GetEntitiesForModule("ENG200ReloadMissing").empty());
    EXPECT_EQ(fx.State(entity), std::string(""));
    EXPECT_FALSE(fx.engine.IsScriptFaulted(entity));
}

TEST(ScriptHotReload_ENG200_RecordsAbsolutePathAcrossWorkingDirectoryChange)
{
    HotReloadFixture fx;
    ASSERT_TRUE(fx.ready);
    fx.Write("ENG200ReloadRelative", kKeeperV1);

    // Compile through a path relative to the working directory, then leave that directory.
    std::error_code error;
    const fs::path original = fs::current_path(error);
    ASSERT_TRUE(!error);
    struct RestoreWorkingDirectory
    {
        fs::path directory;
        ~RestoreWorkingDirectory()
        {
            std::error_code ignored;
            fs::current_path(directory, ignored);
        }
    } restore{original};

    fs::current_path(fx.directory, error);
    ASSERT_TRUE(!error);
    const EntityID entity = MakeEntity(29);
    ASSERT_TRUE(fx.engine.CompileScriptFile("ENG200ReloadRelative.as"));
    ASSERT_TRUE(fx.engine.AttachScript(entity, "Keeper", "ENG200ReloadRelative"));
    fx.engine.CallUpdate(entity, 1.0f);

    const fs::path recorded = fx.engine.GetModuleFilePath("ENG200ReloadRelative");
    EXPECT_TRUE(recorded.is_absolute());
    EXPECT_TRUE(fs::equivalent(recorded, fx.directory / "ENG200ReloadRelative.as", error));

    fs::current_path(original, error);
    ASSERT_TRUE(!error);
    ASSERT_TRUE(fx.engine.HotReloadModule("ENG200ReloadRelative"));
    EXPECT_EQ(fx.State(entity), std::string("v1:1|2.5|true|v1!|1"));
}

#endif // SPARK_ANGELSCRIPT_SUPPORT
