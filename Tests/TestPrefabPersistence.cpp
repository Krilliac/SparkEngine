/**
 * @file TestPrefabPersistence.cpp
 * @brief SAVE-230: editor prefab (.sparkprefab) version gate, diagnostics and durable save.
 *
 * PrefabAsset::TryLoad must fail closed on a newer format version, name the component and
 * line of a truncated or malformed file, never hand back a partially read prefab, and recover
 * from the retained `.bak` when the primary is damaged. PrefabAsset::Save must leave the
 * previous prefab byte-identical when the write fails.
 */

#include "TestFramework.h"
#include "Prefabs/PrefabAsset.h"
#include "Prefabs/PrefabManager.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <variant>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace
{
    namespace fs = std::filesystem;

    class PrefabScratch
    {
      public:
        explicit PrefabScratch(const char* tag)
        {
            static std::atomic<unsigned int> sequence{0};
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            m_path = fs::temp_directory_path() / ("spark-prefab-persistence-" + std::string(tag) + "-" +
                                                  std::to_string(stamp) + "-" + std::to_string(sequence++));
            fs::create_directories(m_path);
        }
        ~PrefabScratch()
        {
            std::error_code ec;
            fs::remove_all(m_path, ec);
        }
        PrefabScratch(const PrefabScratch&) = delete;
        PrefabScratch& operator=(const PrefabScratch&) = delete;

        fs::path Native(const char* name) const { return m_path / name; }

        /// PrefabAsset takes UTF-8 paths; path::string() is the ANSI code page on Windows.
        std::string Utf8(const char* name) const
        {
            const std::u8string utf8 = Native(name).u8string();
            return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
        }

      private:
        fs::path m_path;
    };

    std::string ReadBytes(const fs::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    void WriteBytes(const fs::path& path, const std::string& bytes)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << bytes;
    }

    SparkEditor::PrefabAsset MakeCrate(float mass)
    {
        SparkEditor::PrefabAsset crate("Supply Crate");
        SparkEditor::SerializedComponent transform;
        transform.typeName = "Transform";
        transform.properties["position"] = XMFLOAT3{1.0f, 2.0f, 3.0f};
        crate.AddComponent(transform);
        SparkEditor::SerializedComponent body;
        body.typeName = "RigidBody";
        body.properties["mass"] = mass;
        body.properties["label"] = std::string("heavy crate");
        crate.AddComponent(body);
        return crate;
    }

    float MassOf(const SparkEditor::PrefabAsset& prefab)
    {
        const SparkEditor::SerializedComponent* body = prefab.GetComponent("RigidBody");
        if (!body)
            return -1.0f;
        const auto it = body->properties.find("mass");
        if (it == body->properties.end() || !std::holds_alternative<float>(it->second))
            return -1.0f;
        return std::get<float>(it->second);
    }

    /// A prefab the loader must not overwrite on failure.
    SparkEditor::PrefabAsset Sentinel()
    {
        SparkEditor::PrefabAsset sentinel("Sentinel");
        SparkEditor::SerializedComponent marker;
        marker.typeName = "Marker";
        sentinel.AddComponent(marker);
        return sentinel;
    }

    bool IsUntouchedSentinel(const SparkEditor::PrefabAsset& prefab)
    {
        return prefab.GetName() == "Sentinel" && prefab.GetComponents().size() == 1 && prefab.HasComponent("Marker");
    }

#if defined(_WIN32)
    /// Holds @p path open without FILE_SHARE_DELETE, so MoveFileExW cannot replace it while
    /// reads (and the .bak refresh's copy) still succeed.
    class RenameBlocker
    {
      public:
        explicit RenameBlocker(const fs::path& path)
            : m_file(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr))
        {
        }
        ~RenameBlocker()
        {
            if (m_file != INVALID_HANDLE_VALUE)
                ::CloseHandle(m_file);
        }
        RenameBlocker(const RenameBlocker&) = delete;
        RenameBlocker& operator=(const RenameBlocker&) = delete;

        bool Held() const { return m_file != INVALID_HANDLE_VALUE; }

      private:
        HANDLE m_file;
    };
#endif
} // namespace

TEST(PrefabPersistence_FutureVersionFailsClosedNamingVersionAndWindow)
{
    PrefabScratch scratch("future");
    // A loadable previous-good copy is present, and must not be used: loading an older copy
    // and saving over the newer file would discard the newer editor's data.
    SparkEditor::PrefabAsset older = MakeCrate(10.0f);
    ASSERT_TRUE(older.Save(scratch.Utf8("Future.sparkprefab")));
    WriteBytes(scratch.Native("Future.sparkprefab.bak"), ReadBytes(scratch.Native("Future.sparkprefab")));
    WriteBytes(scratch.Native("Future.sparkprefab"),
               "SPARKPREFAB 3\nname Supply Crate\ncomponents 1\ncomponent Transform\nproperties 1\n"
               "  orientation quat 0 0 0 1\n");

    SparkEditor::PrefabAsset out = Sentinel();
    std::string error;
    EXPECT_FALSE(SparkEditor::PrefabAsset::TryLoad(scratch.Utf8("Future.sparkprefab"), out, error));
    EXPECT_TRUE(IsUntouchedSentinel(out));
    EXPECT_STR_CONTAINS(error, "Future.sparkprefab");
    EXPECT_STR_CONTAINS(error, "format version 3");
    EXPECT_STR_CONTAINS(error, "reads versions 1 to 2");
    EXPECT_STR_CONTAINS(error, "newer SparkEditor");
    EXPECT_TRUE(error.find(".bak") == std::string::npos);
}

TEST(PrefabPersistence_TruncatedFileNamesComponentAndLeavesOutputUntouched)
{
    PrefabScratch scratch("truncated");
    WriteBytes(scratch.Native("Cut.sparkprefab"), "SPARKPREFAB 1\nname Supply Crate\ncomponents 3\n"
                                                  "component Transform\nproperties 1\n"
                                                  "  position float3 1 2 3\n"
                                                  "component RigidBody\nproperties 2\n"
                                                  "  mass float 12.5\n");

    SparkEditor::PrefabAsset out = Sentinel();
    std::string error;
    EXPECT_FALSE(SparkEditor::PrefabAsset::TryLoad(scratch.Utf8("Cut.sparkprefab"), out, error));
    EXPECT_TRUE(IsUntouchedSentinel(out));
    EXPECT_STR_CONTAINS(error, "truncated at property 2 of 2 in component 2 of 3 ('RigidBody')");

    // Cut between components: the next component header is missing.
    WriteBytes(scratch.Native("Cut.sparkprefab"), "SPARKPREFAB 1\nname Supply Crate\ncomponents 2\n"
                                                  "component Transform\nproperties 0\n");
    EXPECT_FALSE(SparkEditor::PrefabAsset::TryLoad(scratch.Utf8("Cut.sparkprefab"), out, error));
    EXPECT_TRUE(IsUntouchedSentinel(out));
    EXPECT_STR_CONTAINS(error, "truncated at component 2 of 2");

    // Trailing content past the declared components is damage too, not ignorable.
    WriteBytes(scratch.Native("Cut.sparkprefab"), "SPARKPREFAB 1\nname Supply Crate\ncomponents 1\n"
                                                  "component Transform\nproperties 0\ncomponent Stray\n");
    EXPECT_FALSE(SparkEditor::PrefabAsset::TryLoad(scratch.Utf8("Cut.sparkprefab"), out, error));
    EXPECT_TRUE(IsUntouchedSentinel(out));
    EXPECT_STR_CONTAINS(error, "line 6 has content after the declared 1 components");
}

TEST(PrefabPersistence_UnknownPropertyTypeIsRejectedWithLocation)
{
    PrefabScratch scratch("unknown");
    WriteBytes(scratch.Native("Quat.sparkprefab"), "SPARKPREFAB 1\nname Turret\ncomponents 1\n"
                                                   "component Transform\nproperties 2\n"
                                                   "  orientation quat 0 0 0 1\n"
                                                   "  position float3 0 1 0\n");

    SparkEditor::PrefabAsset out = Sentinel();
    std::string error;
    EXPECT_FALSE(SparkEditor::PrefabAsset::TryLoad(scratch.Utf8("Quat.sparkprefab"), out, error));
    EXPECT_TRUE(IsUntouchedSentinel(out));
    EXPECT_STR_CONTAINS(error, "line 6: property 'orientation' of component 'Transform' has unknown type 'quat'");

    // A known type with a value that does not parse is located the same way.
    WriteBytes(scratch.Native("Quat.sparkprefab"), "SPARKPREFAB 1\nname Turret\ncomponents 1\n"
                                                   "component Transform\nproperties 1\n"
                                                   "  position float3 0 1\n");
    EXPECT_FALSE(SparkEditor::PrefabAsset::TryLoad(scratch.Utf8("Quat.sparkprefab"), out, error));
    EXPECT_TRUE(IsUntouchedSentinel(out));
    EXPECT_STR_CONTAINS(error, "line 6: property 'position' of component 'Transform' has a malformed float3 value");
}

TEST(PrefabPersistence_FailedSaveKeepsPreviousPrefabByteIdentical)
{
    PrefabScratch scratch("failedsave");
    const std::string path = scratch.Utf8("Crate.sparkprefab");

    SparkEditor::PrefabAsset crate = MakeCrate(10.0f);
    ASSERT_TRUE(crate.Save(path));
    crate.GetComponents()[1].properties["mass"] = 20.0f;
    ASSERT_TRUE(crate.Save(path));
    const std::string primaryBefore = ReadBytes(scratch.Native("Crate.sparkprefab"));
    const std::string backupBefore = ReadBytes(scratch.Native("Crate.sparkprefab.bak"));
    ASSERT_FALSE(backupBefore.empty());

    // Occupy the staging name with a non-empty directory so the staging write fails.
    fs::create_directories(scratch.Native("Crate.sparkprefab.tmp"));
    WriteBytes(scratch.Native("Crate.sparkprefab.tmp") / "occupant", "x");

    crate.GetComponents()[1].properties["mass"] = 30.0f;
    crate.SetModified(true);
    EXPECT_FALSE(crate.Save(path));
    EXPECT_TRUE(crate.IsModified());
    EXPECT_EQ(ReadBytes(scratch.Native("Crate.sparkprefab")), primaryBefore);
    EXPECT_EQ(ReadBytes(scratch.Native("Crate.sparkprefab.bak")), backupBefore);

    SparkEditor::PrefabAsset reloaded;
    std::string error;
    ASSERT_TRUE(SparkEditor::PrefabAsset::TryLoad(path, reloaded, error));
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(MassOf(reloaded), 20.0f);
}

TEST(PrefabPersistence_CorruptPrimaryLoadsRetainedBackupAndReportsBothReasons)
{
    PrefabScratch scratch("recover");
    const std::string path = scratch.Utf8("Crate.sparkprefab");

    SparkEditor::PrefabAsset crate = MakeCrate(10.0f);
    ASSERT_TRUE(crate.Save(path));
    crate.GetComponents()[1].properties["mass"] = 20.0f;
    ASSERT_TRUE(crate.Save(path)); // .bak now holds the mass-10 revision

    // Damage the primary the way a torn external copy would: cut before the second component.
    const std::string primary = ReadBytes(scratch.Native("Crate.sparkprefab"));
    const size_t cut = primary.find("component \"RigidBody\"");
    ASSERT_TRUE(cut != std::string::npos);
    WriteBytes(scratch.Native("Crate.sparkprefab"), primary.substr(0, cut));

    SparkEditor::PrefabManager manager;
    std::string error;
    SparkEditor::PrefabAsset* recovered = manager.LoadPrefab(path, &error);
    ASSERT_TRUE(recovered != nullptr);
    EXPECT_EQ(recovered->GetName(), std::string("Supply Crate"));
    EXPECT_EQ(MassOf(*recovered), 10.0f);
    EXPECT_TRUE(recovered->GetFilePath() == path);
    EXPECT_STR_CONTAINS(error, "was rejected: the file is truncated at component 2 of 2");
    EXPECT_STR_CONTAINS(error, "Loaded the previous-good backup");

    // With the backup damaged as well, the load fails and names both reasons.
    WriteBytes(scratch.Native("Crate.sparkprefab.bak"), "SPARKPREFAB one\n");
    SparkEditor::PrefabManager empty;
    EXPECT_TRUE(empty.LoadPrefab(path, &error) == nullptr);
    EXPECT_EQ(empty.GetPrefabCount(), static_cast<size_t>(0));
    EXPECT_STR_CONTAINS(error, "was rejected: the file is truncated at component 2 of 2");
    EXPECT_STR_CONTAINS(error, "Crate.sparkprefab.bak' was not usable: line 1 is not a 'SPARKPREFAB <version>' header");
}

TEST(PrefabPersistence_SaveAfterBackupRecoveryKeepsGoodBackup)
{
    PrefabScratch scratch("recoversave");
    const std::string path = scratch.Utf8("Crate.sparkprefab");
    const fs::path primaryFile = scratch.Native("Crate.sparkprefab");
    const fs::path backupFile = scratch.Native("Crate.sparkprefab.bak");

    SparkEditor::PrefabAsset crate = MakeCrate(10.0f);
    ASSERT_TRUE(crate.Save(path));
    crate.GetComponents()[1].properties["mass"] = 20.0f;
    ASSERT_TRUE(crate.Save(path)); // .bak holds the mass-10 revision
    const std::string goodBackup = ReadBytes(backupFile);

    const std::string primary = ReadBytes(primaryFile);
    const std::string damaged = primary.substr(0, primary.find("component \"RigidBody\""));
    WriteBytes(primaryFile, damaged);

    SparkEditor::PrefabAsset recovered;
    std::string error;
    ASSERT_TRUE(SparkEditor::PrefabAsset::TryLoad(path, recovered, error));
    ASSERT_EQ(MassOf(recovered), 10.0f);
    recovered.GetComponents()[1].properties["mass"] = 30.0f;

#if defined(_WIN32)
    {
        // The final rename fails after the point where the .bak would be refreshed.
        RenameBlocker blocker(primaryFile);
        ASSERT_TRUE(blocker.Held());
        EXPECT_FALSE(recovered.Save(path));
    }
    EXPECT_EQ(ReadBytes(primaryFile), damaged);
    EXPECT_EQ(ReadBytes(backupFile), goodBackup);
#endif

    // The repairing save leaves the .bak on the last good revision, not the damaged primary.
    ASSERT_TRUE(recovered.Save(path));
    EXPECT_EQ(ReadBytes(backupFile), goodBackup);
    SparkEditor::PrefabAsset reloaded;
    ASSERT_TRUE(SparkEditor::PrefabAsset::TryLoad(path, reloaded, error));
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(MassOf(reloaded), 30.0f);

    // Once the primary is good again, saves retain it as usual.
    const std::string repaired = ReadBytes(primaryFile);
    recovered.GetComponents()[1].properties["mass"] = 40.0f;
    ASSERT_TRUE(recovered.Save(path));
    EXPECT_EQ(ReadBytes(backupFile), repaired);
}
