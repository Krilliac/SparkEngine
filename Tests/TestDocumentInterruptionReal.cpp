/**
 * @file TestDocumentInterruptionReal.cpp
 * @brief SAVE-230: process-interruption rehearsal for the editor document writers.
 *
 * TestSaveInterruptionReal.cpp kills a SaveSystem writer on POSIX only. This file runs
 * the same rehearsal on every platform against the three editor document writers:
 *   - scene:   Spark::SaveWorld / Spark::LoadWorld (ReflectedScenePersistence.cpp);
 *   - prefab:  PrefabAsset::Save / PrefabAsset::TryLoad (SaveFileDurability::WriteFileAtomically);
 *   - project: ProjectManager::RecordOpenedScene -> SaveProjectFile / OpenProject
 *              (SaveFileDurability::WriteFileAtomically).
 *
 * The writer is a freshly launched SparkTests process that runs only the calling test,
 * selected by SPARK_DOCWRITE_KIND, never a fork() of this multi-threaded runner. It saves
 * generation start, start+1, ... of one document, printing DOCWRITE_START=<n> (flushed)
 * before each save and DOCWRITE_DONE=<n> after it returned true. The parent kills it at a
 * seeded random offset through Spark::Process::Kill (SIGKILL on POSIX, TerminateProcess on
 * Windows), so the kill lands before, inside and after every phase of the write. It then
 * requires, with D the last completed generation:
 *   - the primary document on its own (copied away from its .bak) loads through the
 *     production loader as exactly one generation, D or the in-flight D+1, and the
 *     in-place load (which may fall back to .bak) returns the same generation;
 *   - a stray `<document>.tmp` means the rename never ran, so the primary is still D;
 *   - the retained `<document>.bak` loads on its own as the revision the last rename
 *     replaced (D-1, or D once the in-flight save refreshed it);
 *   - a fresh writer saves over whatever the last kill left behind and reads it back.
 * Every generation's payload is tagged with its number, so a torn or mixed document fails
 * the "exactly one generation" check instead of loading as either neighbour.
 *
 * Reproduce a failure with the logged seed: SPARK_DOCWRITE_SEED=<seed>. Change the kill
 * count per writer with SPARK_DOCWRITE_ITERATIONS=<n>.
 */

#include "TestFramework.h"

#include "Core/ProjectManager.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/CoreComponents.h"
#include "Prefabs/PrefabAsset.h"
#include "SceneManager/ReflectedSceneSerializer.h"
#include "Utils/Process.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <stdlib.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace
{
    namespace fs = std::filesystem;

    constexpr const char* kKindEnv = "SPARK_DOCWRITE_KIND";
    constexpr const char* kDirEnv = "SPARK_DOCWRITE_DIR";
    constexpr const char* kStartEnv = "SPARK_DOCWRITE_START";

    constexpr uint32_t kDefaultIterations = 100;
    constexpr uint32_t kDefaultSeed = 0xD0C230u;

    // Payload sizes: large enough that staging, the .bak refresh and the rename each take
    // a measurable share of a save, so randomized offsets land inside every phase.
    constexpr size_t kSceneEntities = 256;
    constexpr size_t kPrefabComponents = 16;
    constexpr size_t kPrefabPayloadBytes = 4096;
    // RecordOpenedScene only accepts scenes that exist, so project generations cycle
    // through this many pre-created scenes; the tag is generation % kProjectScenes.
    constexpr uint32_t kProjectScenes = 64;

    std::string EnvOrEmpty(const char* name)
    {
        const char* value = std::getenv(name);
        return value ? value : "";
    }

    uint32_t EnvOrDefault(const char* name, uint32_t fallback)
    {
        const std::string value = EnvOrEmpty(name);
        return value.empty() ? fallback : static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 0));
    }

    /// The document writers take UTF-8; path::string() is the ANSI code page on Windows.
    std::string Utf8(const fs::path& path)
    {
        const std::u8string utf8 = path.u8string();
        return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    }

    fs::path WithSuffix(fs::path path, const char* suffix)
    {
        path += suffix;
        return path;
    }

    fs::path TestBinaryPath()
    {
#if defined(_WIN32)
        std::wstring buffer(32768, L'\0');
        const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0 || length >= buffer.size())
            return {};
        buffer.resize(length);
        return fs::path(buffer);
#elif defined(__APPLE__)
        uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::string buffer(size, '\0');
        if (_NSGetExecutablePath(buffer.data(), &size) != 0)
            return {};
        return fs::path(buffer.c_str());
#else
        std::error_code error;
        const fs::path self = fs::read_symlink("/proc/self/exe", error);
        return error ? fs::path{} : self;
#endif
    }

    unsigned long CurrentProcessId()
    {
#if defined(_WIN32)
        return static_cast<unsigned long>(::GetCurrentProcessId());
#else
        return static_cast<unsigned long>(::getpid());
#endif
    }

    /// Sets or clears environment variables for the child launched while it is alive and
    /// restores the previous values on destruction. The child inherits this process's
    /// environment block on both platforms.
    class ScopedEnvironment
    {
      public:
        ScopedEnvironment() = default;
        ScopedEnvironment(const ScopedEnvironment&) = delete;
        ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

        ~ScopedEnvironment()
        {
            for (auto it = m_saved.rbegin(); it != m_saved.rend(); ++it)
                Apply(it->first.c_str(), it->second);
        }

        void Set(const char* name, const std::string& value)
        {
            Remember(name);
            Apply(name, value);
        }

        void Unset(const char* name)
        {
            Remember(name);
            Apply(name, std::nullopt);
        }

      private:
        void Remember(const char* name)
        {
            const char* value = std::getenv(name);
            m_saved.emplace_back(name, value ? std::optional<std::string>(value) : std::nullopt);
        }

        static void Apply(const char* name, const std::optional<std::string>& value)
        {
#if defined(_WIN32)
            // An empty value removes the variable from the CRT and the process environment.
            _putenv_s(name, value ? value->c_str() : "");
#else
            if (value)
                ::setenv(name, value->c_str(), 1);
            else
                ::unsetenv(name);
#endif
        }

        std::vector<std::pair<std::string, std::optional<std::string>>> m_saved;
    };

    /// Saves one generation of the document; false when the production writer refused.
    using GenerationWriter = std::function<bool(uint32_t)>;

    /// One production document writer under rehearsal.
    struct DocumentKind
    {
        const char* id;       ///< SPARK_DOCWRITE_KIND value that selects the writer role.
        const char* testName; ///< The TEST that hosts both roles.
        uint32_t tagModulus;  ///< The loader reports generation % tagModulus.
        fs::path (*documentPath)(const fs::path& directory);
        /// Prepares the document directory on first use; empty when the writer cannot start.
        GenerationWriter (*openWriter)(const fs::path& directory);
        /// Production load of @p document; the generation tag, or nullopt when the loader
        /// rejects it or the loaded content mixes generations.
        std::optional<uint32_t> (*loadTag)(const fs::path& document);
    };

    // ---------------------------------------------------------------- scene writer

    fs::path SceneDocument(const fs::path& directory)
    {
        return directory / "Scenes" / "Rehearsal.sparkscene";
    }

    std::string SceneEntityName(uint32_t generation, size_t index)
    {
        return "g" + std::to_string(generation) + "_" + std::to_string(index);
    }

    GenerationWriter OpenSceneWriter(const fs::path& directory)
    {
        fs::create_directories(directory / "Scenes");
        struct SceneState
        {
            ::World world;
            std::vector<::EntityID> entities;
        };
        auto state = std::make_shared<SceneState>();
        for (size_t index = 0; index < kSceneEntities; ++index)
        {
            const ::EntityID entity = state->world.CreateEntity(SceneEntityName(0, index));
            state->world.AddComponent<::Transform>(entity);
            state->entities.push_back(entity);
        }
        const std::string document = Utf8(SceneDocument(directory));
        return [state, document](uint32_t generation)
        {
            entt::registry& registry = state->world.GetRegistry();
            for (size_t index = 0; index < state->entities.size(); ++index)
                registry.get<::NameComponent>(state->entities[index]).name = SceneEntityName(generation, index);
            return Spark::SaveWorld(state->world, document);
        };
    }

    std::optional<uint32_t> LoadSceneTag(const fs::path& document)
    {
        ::World world;
        if (!Spark::LoadWorld(world, Utf8(document)))
            return std::nullopt;
        std::optional<uint32_t> generation;
        size_t named = 0;
        for (const auto [entity, name] : world.GetRegistry().view<::NameComponent>().each())
        {
            (void)entity;
            const size_t separator = name.name.find('_');
            if (name.name.empty() || name.name[0] != 'g' || separator == std::string::npos)
                return std::nullopt;
            const uint32_t tag = static_cast<uint32_t>(std::strtoul(name.name.c_str() + 1, nullptr, 10));
            if (generation && *generation != tag)
                return std::nullopt;
            generation = tag;
            ++named;
        }
        if (named != kSceneEntities)
            return std::nullopt;
        return generation;
    }

    // ---------------------------------------------------------------- prefab writer

    fs::path PrefabDocument(const fs::path& directory)
    {
        return directory / "Prefabs" / "Rehearsal.sparkprefab";
    }

    std::string PrefabPayload(uint32_t generation)
    {
        return std::string(kPrefabPayloadBytes, static_cast<char>('a' + generation % 26u));
    }

    GenerationWriter OpenPrefabWriter(const fs::path& directory)
    {
        fs::create_directories(directory / "Prefabs");
        auto prefab = std::make_shared<SparkEditor::PrefabAsset>("generation 0");
        const std::string document = Utf8(PrefabDocument(directory));
        return [prefab, document](uint32_t generation)
        {
            prefab->SetName("generation " + std::to_string(generation));
            std::vector<SparkEditor::SerializedComponent>& components = prefab->GetComponents();
            components.clear();
            for (size_t index = 0; index < kPrefabComponents; ++index)
            {
                SparkEditor::SerializedComponent component;
                component.typeName = "Filler" + std::to_string(index);
                component.properties["generation"] = static_cast<int>(generation);
                component.properties["payload"] = PrefabPayload(generation);
                components.push_back(std::move(component));
            }
            return prefab->Save(document);
        };
    }

    std::optional<uint32_t> LoadPrefabTag(const fs::path& document)
    {
        SparkEditor::PrefabAsset prefab;
        std::string error;
        if (!SparkEditor::PrefabAsset::TryLoad(Utf8(document), prefab, error))
            return std::nullopt;
        const std::string_view prefix = "generation ";
        if (!prefab.GetName().starts_with(prefix) || prefab.GetComponents().size() != kPrefabComponents)
            return std::nullopt;
        const uint32_t generation =
            static_cast<uint32_t>(std::strtoul(prefab.GetName().c_str() + prefix.size(), nullptr, 10));
        for (const SparkEditor::SerializedComponent& component : prefab.GetComponents())
        {
            const auto tag = component.properties.find("generation");
            const auto payload = component.properties.find("payload");
            if (tag == component.properties.end() || payload == component.properties.end() ||
                !std::holds_alternative<int>(tag->second) || !std::holds_alternative<std::string>(payload->second) ||
                std::get<int>(tag->second) != static_cast<int>(generation) ||
                std::get<std::string>(payload->second) != PrefabPayload(generation))
                return std::nullopt;
        }
        return generation;
    }

    // ---------------------------------------------------------------- project writer

    fs::path ProjectDocument(const fs::path& directory)
    {
        return directory / "Rehearsal" / "Rehearsal.sparkproject";
    }

    std::string ProjectScene(uint32_t generation)
    {
        return "Scenes/g" + std::to_string(generation % kProjectScenes) + ".sparkscene";
    }

    /// Recent-project history stays next to the document, never in the user's profile.
    fs::path ProjectProfile(const fs::path& document)
    {
        return document.parent_path().parent_path() / "profile";
    }

    /// Clears the process-wide active project path on every exit, so later tests never
    /// see this rehearsal's scratch project as the open one.
    struct OpenedProject
    {
        explicit OpenedProject(const fs::path& document) : manager(Utf8(ProjectProfile(document))) {}
        ~OpenedProject() { manager.CloseProject(); }
        OpenedProject(const OpenedProject&) = delete;
        OpenedProject& operator=(const OpenedProject&) = delete;

        SparkEditor::ProjectManager manager;
    };

    /// Generation 0: the seed document lists every rotation scene and opens g0.
    void WriteProjectSeed(const fs::path& document)
    {
        const fs::path root = document.parent_path();
        fs::create_directories(root / "Scenes");
        std::ostringstream seed;
        seed << "{\n  \"projectFileVersion\": 1,\n  \"name\": \"Rehearsal\",\n  \"version\": \"1.0.0\",\n"
             << "  \"description\": \"SAVE-230 interruption rehearsal\",\n  \"engineVersion\": \"1.0.0\",\n"
             << "  \"defaultScene\": \"" << ProjectScene(0) << "\",\n  \"lastOpenedScene\": \"" << ProjectScene(0)
             << "\",\n  \"createdTime\": 0,\n  \"lastModified\": 0,\n  \"modules\": [],\n  \"scenes\": [\n";
        for (uint32_t scene = 0; scene < kProjectScenes; ++scene)
        {
            std::ofstream(root / ProjectScene(scene), std::ios::binary) << "{\"version\": 1, \"entities\": []}\n";
            seed << "    \"" << ProjectScene(scene) << "\"" << (scene + 1 < kProjectScenes ? "," : "") << "\n";
        }
        seed << "  ]\n}\n";
        std::ofstream(document, std::ios::binary | std::ios::trunc) << seed.str();
    }

    GenerationWriter OpenProjectWriter(const fs::path& directory)
    {
        const fs::path document = ProjectDocument(directory);
        if (!fs::exists(document))
            WriteProjectSeed(document);
        auto project = std::make_shared<OpenedProject>(document);
        project->manager.Initialize();
        if (!project->manager.OpenProject(Utf8(document)))
            return {};
        return [project](uint32_t generation) { return project->manager.RecordOpenedScene(ProjectScene(generation)); };
    }

    std::optional<uint32_t> LoadProjectTag(const fs::path& document)
    {
        OpenedProject project(document);
        project.manager.Initialize();
        if (!project.manager.OpenProject(Utf8(document)))
            return std::nullopt;
        const SparkEditor::ProjectInfo& info = project.manager.GetCurrentProject();
        const std::string_view prefix = "Scenes/g";
        if (info.scenes.size() != kProjectScenes || !info.lastOpenedScene.starts_with(prefix))
            return std::nullopt;
        const uint32_t tag =
            static_cast<uint32_t>(std::strtoul(info.lastOpenedScene.c_str() + prefix.size(), nullptr, 10));
        if (info.lastOpenedScene != ProjectScene(tag))
            return std::nullopt;
        return tag;
    }

    constexpr uint32_t kUntagged = std::numeric_limits<uint32_t>::max();

    const DocumentKind kSceneKind{"scene",          "DocumentInterruption_KilledSceneWriterKeepsLastGeneration",
                                  kUntagged,        &SceneDocument,
                                  &OpenSceneWriter, &LoadSceneTag};
    const DocumentKind kPrefabKind{"prefab",          "DocumentInterruption_KilledPrefabWriterKeepsLastGeneration",
                                   kUntagged,         &PrefabDocument,
                                   &OpenPrefabWriter, &LoadPrefabTag};
    const DocumentKind kProjectKind{"project",          "DocumentInterruption_KilledProjectWriterKeepsLastGeneration",
                                    kProjectScenes,     &ProjectDocument,
                                    &OpenProjectWriter, &LoadProjectTag};

    // ---------------------------------------------------------------- rehearsal harness

    /// Writer role: save generation start, start+1, ... until killed.
    int RunWriterRole(const DocumentKind& kind)
    {
        const std::string directory = EnvOrEmpty(kDirEnv);
        const uint32_t start = EnvOrDefault(kStartEnv, 0);
        GenerationWriter writer =
            directory.empty() || start == 0
                ? GenerationWriter{}
                : kind.openWriter(
                      fs::path(std::u8string(reinterpret_cast<const char8_t*>(directory.data()), directory.size())));
        if (!writer)
        {
            std::printf("DOCWRITE_SETUP_FAILED\n");
            std::fflush(stdout);
            return 2;
        }
        for (uint32_t generation = start; generation < start + 100000u; ++generation)
        {
            std::printf("DOCWRITE_START=%u\n", generation);
            std::fflush(stdout);
            if (!writer(generation))
            {
                std::printf("DOCWRITE_FAIL=%u\n", generation);
                std::fflush(stdout);
                return 3;
            }
            std::printf("DOCWRITE_DONE=%u\n", generation);
            std::fflush(stdout);
        }
        return 0;
    }

    std::expected<Spark::Process, std::string> SpawnWriter(const DocumentKind& kind, const fs::path& directory,
                                                           uint32_t startGeneration)
    {
        const fs::path self = TestBinaryPath();
        if (self.empty())
            return std::unexpected(std::string("cannot resolve the test binary path"));

        // The child inherits this environment: select exactly the calling test in its
        // writer role and drop the parent's family selection.
        ScopedEnvironment environment;
        for (const char* selection : {"SPARK_TEST_FILE", "SPARK_TEST_NAME_PREFIX", "SPARK_TEST_EXPECT_COUNT",
                                      "SPARK_TEST_EXCLUDE", "SPARK_TEST_LIMIT"})
        {
            environment.Unset(selection);
        }
        environment.Set("SPARK_TEST_NAME", kind.testName);
        environment.Set(kKindEnv, kind.id);
        environment.Set(kDirEnv, Utf8(fs::absolute(directory)));
        environment.Set(kStartEnv, std::to_string(startGeneration));
        return Spark::Process::Builder(Utf8(self))
            .WorkingDirectory(Utf8(fs::current_path()))
            .CaptureStdout()
            .MergeStderrIntoStdout()
            .NoWindow()
            .Launch();
    }

    struct WriterProgress
    {
        uint32_t lastStarted = 0;
        uint32_t lastDone = 0;
        bool failed = false;
        std::string log;
    };

    void FoldLine(const std::string& line, WriterProgress& progress)
    {
        auto valueOf = [&](std::string_view key) -> std::optional<uint32_t>
        {
            const size_t at = line.find(key);
            if (at == std::string::npos)
                return std::nullopt;
            return static_cast<uint32_t>(std::strtoul(line.c_str() + at + key.size(), nullptr, 10));
        };
        if (const auto started = valueOf("DOCWRITE_START="))
            progress.lastStarted = std::max(progress.lastStarted, *started);
        else if (const auto done = valueOf("DOCWRITE_DONE="))
            progress.lastDone = std::max(progress.lastDone, *done);
        else if (line.find("DOCWRITE_FAIL") != std::string::npos ||
                 line.find("DOCWRITE_SETUP_FAILED") != std::string::npos)
            progress.failed = true;
        progress.log += line + '\n';
    }

    /// Load @p file on its own: copied as the document's name into an empty directory, so
    /// the loader cannot fall back to a sibling .bak.
    std::optional<uint32_t> ProbeTag(const DocumentKind& kind, const fs::path& directory, const fs::path& file)
    {
        const fs::path document = kind.documentPath(directory);
        const fs::path probeRoot = directory / "probe";
        const fs::path probe = probeRoot / document.parent_path().filename() / document.filename();
        fs::remove_all(probeRoot);
        fs::create_directories(probe.parent_path());
        fs::copy_file(file, probe);
        const std::optional<uint32_t> tag = kind.loadTag(probe);
        fs::remove_all(probeRoot);
        return tag;
    }

    /// The candidate generation @p tag names, or nullopt when it names none of them.
    std::optional<uint32_t> Resolve(const DocumentKind& kind, const std::optional<uint32_t>& tag,
                                    std::initializer_list<uint32_t> candidates)
    {
        if (!tag)
            return std::nullopt;
        for (const uint32_t generation : candidates)
        {
            if (generation % kind.tagModulus == *tag)
                return generation;
        }
        return std::nullopt;
    }

    void RunInterruptionRehearsal(const DocumentKind& kind)
    {
        const fs::path directory = fs::temp_directory_path() / ("spark_docwrite_" + std::string(kind.id) + "_" +
                                                                std::to_string(CurrentProcessId()));
        fs::remove_all(directory);
        fs::create_directories(directory);
        const fs::path document = kind.documentPath(directory);
        const fs::path temporary = WithSuffix(document, ".tmp");
        const fs::path retained = WithSuffix(document, ".bak");

        // Seed generation 1 and time one full save, which scales the kill offsets below.
        int64_t saveMicros = 0;
        {
            GenerationWriter seed = kind.openWriter(directory);
            ASSERT_TRUE(static_cast<bool>(seed));
            const auto seedStart = std::chrono::steady_clock::now();
            ASSERT_TRUE(seed(1));
            saveMicros = std::max<int64_t>(
                200, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - seedStart)
                         .count());
        }

        const uint32_t iterations =
            std::max<uint32_t>(1, EnvOrDefault("SPARK_DOCWRITE_ITERATIONS", kDefaultIterations));
        const uint32_t seed = EnvOrDefault("SPARK_DOCWRITE_SEED", kDefaultSeed);
        std::mt19937 random(seed);
        // Offsets span three saves, so kills land before, inside and after every write phase.
        std::uniform_int_distribution<int64_t> killOffset(0, 3 * saveMicros);
        std::printf("[DocumentInterruption] kind=%s seed=0x%X iterations=%u saveMicros=%lld\n", kind.id, seed,
                    iterations, static_cast<long long>(saveMicros));

        uint32_t onDisk = 1;
        uint32_t killsWithStrayTemp = 0;
        uint32_t killsAfterInFlightRename = 0;
        for (uint32_t iteration = 0; iteration < iterations; ++iteration)
        {
            const int64_t offsetMicros = killOffset(random);
            auto launched = SpawnWriter(kind, directory, onDisk + 1);
            ASSERT_TRUE(launched.has_value());
            Spark::Process& writer = *launched;

            // Wait for the writer's first completed save, so the kill always lands inside its
            // save loop. Bounded: a wedged child fails the test instead of hanging the run.
            WriterProgress progress;
            std::string line;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            while (progress.lastDone == 0 && !progress.failed && writer.IsRunning() &&
                   std::chrono::steady_clock::now() < deadline)
            {
                while (writer.TryReadLine(line))
                    FoldLine(line, progress);
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            const bool reachedLoop = progress.lastDone != 0;
            if (reachedLoop)
                std::this_thread::sleep_for(std::chrono::microseconds(offsetMicros));
            const bool stillSaving = writer.IsRunning();
            writer.Kill();
            // Markers written before the kill still count.
            std::istringstream rest(writer.ReadAllStdout());
            while (std::getline(rest, line))
                FoldLine(line, progress);

            auto dump = [&](const char* reason)
            {
                std::fprintf(stderr,
                             "[DocumentInterruption] kind=%s iteration %u (seed=0x%X offset=%lldus): %s; "
                             "started=%u done=%u\n---- writer output ----\n%s----\n",
                             kind.id, iteration, seed, static_cast<long long>(offsetMicros), reason,
                             progress.lastStarted, progress.lastDone, progress.log.c_str());
            };
            if (!reachedLoop || !stillSaving || progress.failed)
            {
                dump("writer did not reach or stay in its save loop");
                ASSERT_TRUE(reachedLoop && stillSaving && !progress.failed);
            }

            const uint32_t done = progress.lastDone;
            const bool inFlight = progress.lastStarted > done;
            const bool strayTemp = fs::exists(temporary);

            const std::optional<uint32_t> primaryTag = ProbeTag(kind, directory, document);
            const std::optional<uint32_t> inPlaceTag = kind.loadTag(document);
            const std::optional<uint32_t> loaded =
                inFlight ? Resolve(kind, primaryTag, {done, done + 1}) : Resolve(kind, primaryTag, {done});
            const bool loadedExpected = loaded.has_value() && inPlaceTag == primaryTag;
            // A leftover temp file means its rename never ran, so the document still holds `done`.
            const bool tempNotPromoted = !strayTemp || (inFlight && loaded && *loaded == done);

            // The retained copy is a complete save of the revision the last rename replaced:
            // killed after the in-flight rename it holds `done`; before that rename, `done - 1`,
            // or `done` once the in-flight save already refreshed it.
            const bool retainedExists = fs::exists(retained);
            const std::optional<uint32_t> retainedTag =
                retainedExists ? ProbeTag(kind, directory, retained) : std::nullopt;
            const std::optional<uint32_t> retainedGeneration = Resolve(kind, retainedTag, {done - 1, done});
            const bool retainedExpected =
                retainedGeneration && loaded &&
                (*loaded == done + 1 ? *retainedGeneration == done
                                     : (*retainedGeneration + 1 == done || (inFlight && *retainedGeneration == done)));

            if (!loadedExpected || !tempNotPromoted || !retainedExpected)
            {
                dump("post-kill invariant violated");
                std::fprintf(stderr,
                             "[DocumentInterruption] primary=%lld inPlace=%lld strayTemp=%d retainedExists=%d "
                             "retained=%lld\n",
                             primaryTag ? static_cast<long long>(*primaryTag) : -1LL,
                             inPlaceTag ? static_cast<long long>(*inPlaceTag) : -1LL, strayTemp ? 1 : 0,
                             retainedExists ? 1 : 0, retainedTag ? static_cast<long long>(*retainedTag) : -1LL);
            }
            ASSERT_TRUE(loadedExpected);
            EXPECT_TRUE(tempNotPromoted);
            EXPECT_TRUE(retainedExpected);

            killsWithStrayTemp += strayTemp ? 1u : 0u;
            killsAfterInFlightRename += *loaded == done + 1 ? 1u : 0u;
            onDisk = *loaded;
        }
        std::printf("[DocumentInterruption] kind=%s %u kills: %u left a stray temp file, %u landed after the in-flight "
                    "rename\n",
                    kind.id, iterations, killsWithStrayTemp, killsAfterInFlightRename);

        // A fresh writer saves over whatever the last kill left behind and reads it back.
        {
            GenerationWriter writer = kind.openWriter(directory);
            ASSERT_TRUE(static_cast<bool>(writer));
            ASSERT_TRUE(writer(onDisk + 1));
        }
        EXPECT_FALSE(fs::exists(temporary));
        EXPECT_TRUE(Resolve(kind, ProbeTag(kind, directory, document), {onDisk + 1}).has_value());
        EXPECT_TRUE(Resolve(kind, ProbeTag(kind, directory, retained), {onDisk}).has_value());

        std::error_code cleanup;
        fs::remove_all(directory, cleanup);
    }

    /// Each TEST hosts both roles: the exec'd child runs the writer and never returns.
    void RunRole(const DocumentKind& kind)
    {
        if (EnvOrEmpty(kKindEnv) == kind.id)
        {
            std::fflush(stdout);
            std::_Exit(RunWriterRole(kind));
        }
        RunInterruptionRehearsal(kind);
    }
} // namespace

TEST(DocumentInterruption_KilledSceneWriterKeepsLastGeneration)
{
    RunRole(kSceneKind);
}

TEST(DocumentInterruption_KilledPrefabWriterKeepsLastGeneration)
{
    RunRole(kPrefabKind);
}

TEST(DocumentInterruption_KilledProjectWriterKeepsLastGeneration)
{
    RunRole(kProjectKind);
}
