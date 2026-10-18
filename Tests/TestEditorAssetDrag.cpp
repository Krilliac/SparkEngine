#include "TestFilesystemLinks.h"
#include "TestFramework.h"
#include "AssetPipeline/EditorAssetDrag.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

using SparkEditor::DecodeAssetDragPayload;
using SparkEditor::EditorAssetKind;
using SparkEditor::MakeAssetDragReference;

namespace
{
    namespace fs = std::filesystem;

    // A scratch project whose Assets/ folder is the drag producer's root.
    struct DragProject
    {
        fs::path root;
        fs::path assets;

        explicit DragProject(const char* tag)
        {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            root = fs::temp_directory_path() / (std::string("spark-asset-drag-") + tag + "-" + std::to_string(stamp));
            assets = root / "Assets";
            fs::create_directories(assets / "Models");
            fs::create_directories(assets / "Materials");
        }

        ~DragProject()
        {
            std::error_code ec;
            fs::remove_all(root, ec);
        }

        DragProject(const DragProject&) = delete;
        DragProject& operator=(const DragProject&) = delete;

        static fs::path Touch(const fs::path& file)
        {
            fs::create_directories(file.parent_path());
            std::ofstream(file, std::ios::binary) << "x";
            return file;
        }
    };
} // namespace

TEST(EditorAssetDrag_ProducerEmitsProjectRelativeForwardSlashReference)
{
    DragProject project("relative");
    const fs::path mesh = DragProject::Touch(project.assets / "Models" / "Crate.GLB");
    const fs::path material = DragProject::Touch(project.assets / "Materials" / "Nested" / "crate.mat");

    EXPECT_EQ(MakeAssetDragReference(mesh, project.assets), std::string("Assets/Models/Crate.GLB"));
    EXPECT_EQ(MakeAssetDragReference(material, project.assets), std::string("Assets/Materials/Nested/crate.mat"));

    // The emitted reference must round-trip through the consumer unchanged.
    const std::string reference = MakeAssetDragReference(material, project.assets);
    std::string decoded;
    EXPECT_TRUE(DecodeAssetDragPayload(reference.c_str(), static_cast<int>(reference.size() + 1),
                                       EditorAssetKind::Material, decoded));
    EXPECT_EQ(decoded, reference);
}

TEST(EditorAssetDrag_ProducerRejectsPathsOutsideAssetsRoot)
{
    DragProject project("outside");
    const fs::path sibling = DragProject::Touch(project.root / "Sibling" / "rogue.obj");
    const fs::path outsideRoot = DragProject::Touch(project.root / "escape.obj");

    EXPECT_TRUE(MakeAssetDragReference(sibling, project.assets).empty());
    EXPECT_TRUE(MakeAssetDragReference(project.assets / "Models" / ".." / ".." / "escape.obj", project.assets).empty());
    EXPECT_TRUE(MakeAssetDragReference(outsideRoot, project.assets).empty());
    // The root itself is not an asset.
    EXPECT_TRUE(MakeAssetDragReference(project.assets, project.assets).empty());

    // A directory link inside Assets/ that points outside (an NTFS junction on
    // Windows, a symlink elsewhere) must not launder an external file into an
    // Assets/... reference.
    ASSERT_TRUE(SparkTestLinks::MakeDirectoryLink(project.root / "Sibling", project.assets / "Linked"));
    EXPECT_TRUE(MakeAssetDragReference(project.assets / "Linked" / "rogue.obj", project.assets).empty());
    SparkTestLinks::RemoveDirectoryLink(project.assets / "Linked");
}

TEST(EditorAssetDrag_ProducerRejectsUnassignableTypes)
{
    DragProject project("types");
    const fs::path texture = DragProject::Touch(project.assets / "Textures" / "grid.png");
    const fs::path audio = DragProject::Touch(project.assets / "Audio" / "hit.wav");
    const fs::path noExtension = DragProject::Touch(project.assets / "Models" / "README");

    EXPECT_TRUE(MakeAssetDragReference(texture, project.assets).empty());
    EXPECT_TRUE(MakeAssetDragReference(audio, project.assets).empty());
    EXPECT_TRUE(MakeAssetDragReference(noExtension, project.assets).empty());
}

TEST(EditorAssetDrag_DecoderRejectsMissingOrEmbeddedNulAndOversize)
{
    std::string decoded = "unchanged";

    const char valid[] = "Assets/Models/a.obj";
    EXPECT_TRUE(DecodeAssetDragPayload(valid, static_cast<int>(sizeof(valid)), EditorAssetKind::Mesh, decoded));
    EXPECT_EQ(decoded, std::string("Assets/Models/a.obj"));

    decoded = "unchanged";
    EXPECT_FALSE(DecodeAssetDragPayload(nullptr, 20, EditorAssetKind::Mesh, decoded));
    // Missing terminator: the declared size stops before the NUL.
    EXPECT_FALSE(DecodeAssetDragPayload(valid, static_cast<int>(sizeof(valid) - 1), EditorAssetKind::Mesh, decoded));
    // Embedded NUL: a second path hidden after the first terminator.
    const char embedded[] = "Assets/Models/a.obj\0Assets/Models/b.obj";
    EXPECT_FALSE(DecodeAssetDragPayload(embedded, static_cast<int>(sizeof(embedded)), EditorAssetKind::Mesh, decoded));
    // Too small to hold any reference, and one byte over the ceiling.
    EXPECT_FALSE(DecodeAssetDragPayload("", 1, EditorAssetKind::Mesh, decoded));
    const std::string prefix = "Assets/Models/";
    const std::string suffix = ".obj";
    // 4096 characters + terminator = 4097 bytes, one over the 4096-byte ceiling.
    const std::string oversize = prefix + std::string(4096 - prefix.size() - suffix.size(), 'a') + suffix;
    EXPECT_EQ(oversize.size(), static_cast<size_t>(4096));
    EXPECT_FALSE(DecodeAssetDragPayload(oversize.c_str(), static_cast<int>(oversize.size() + 1), EditorAssetKind::Mesh,
                                        decoded));
    EXPECT_EQ(decoded, std::string("unchanged"));

    // Exactly at the ceiling is still accepted.
    const std::string atCeiling = prefix + std::string(4095 - prefix.size() - suffix.size(), 'a') + suffix;
    EXPECT_TRUE(DecodeAssetDragPayload(atCeiling.c_str(), static_cast<int>(atCeiling.size() + 1), EditorAssetKind::Mesh,
                                       decoded));
    EXPECT_EQ(decoded, atCeiling);
}

TEST(EditorAssetDrag_DecoderEnforcesMeshVersusMaterialKind)
{
    const char mesh[] = "Assets/Models/crate.fbx";
    const char material[] = "Assets/Materials/crate.material";
    const char traversal[] = "Assets/../crate.obj";
    std::string decoded;

    EXPECT_TRUE(DecodeAssetDragPayload(mesh, static_cast<int>(sizeof(mesh)), EditorAssetKind::Mesh, decoded));
    EXPECT_FALSE(DecodeAssetDragPayload(mesh, static_cast<int>(sizeof(mesh)), EditorAssetKind::Material, decoded));
    EXPECT_TRUE(
        DecodeAssetDragPayload(material, static_cast<int>(sizeof(material)), EditorAssetKind::Material, decoded));
    EXPECT_FALSE(DecodeAssetDragPayload(material, static_cast<int>(sizeof(material)), EditorAssetKind::Mesh, decoded));
    EXPECT_FALSE(
        DecodeAssetDragPayload(traversal, static_cast<int>(sizeof(traversal)), EditorAssetKind::Mesh, decoded));
}
