#include "SparkAssetPipelineCore/AssetCooker.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#endif

namespace
{
    std::string ReadFile(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    }

#if !defined(_WIN32)
    std::string QuoteShellArgument(const std::filesystem::path& path)
    {
        const std::string input = path.string();
        std::string output = "'";
        for (const char value : input)
        {
            if (value == '\'')
                output += "'\\''";
            else
                output.push_back(value);
        }
        output.push_back('\'');
        return output;
    }
#endif

    int RunCookChild(const std::filesystem::path& executable, const std::filesystem::path& source,
                     const std::filesystem::path& output, const std::filesystem::path& manifest)
    {
#if defined(_WIN32)
        const intptr_t result = ::_wspawnl(_P_WAIT, executable.c_str(), executable.c_str(), L"--cook-child",
                                           source.c_str(), output.c_str(), manifest.c_str(), nullptr);
        return result < 0 ? -1 : static_cast<int>(result);
#else
        const std::string command = QuoteShellArgument(executable) + " --cook-child " + QuoteShellArgument(source) +
                                    " " + QuoteShellArgument(output) + " " + QuoteShellArgument(manifest);
        return std::system(command.c_str());
#endif
    }

    bool HasStagedSibling(const std::filesystem::path& output)
    {
        const std::string prefix = output.filename().string() + ".spark-stage-";
        std::error_code ec;
        for (std::filesystem::directory_iterator iterator(output.parent_path(), ec), end; !ec && iterator != end;
             iterator.increment(ec))
        {
            if (iterator->path().filename().string().starts_with(prefix))
                return true;
        }
        return false;
    }
} // namespace

int main(int argc, char** argv)
{
    if (argc == 5 && std::string_view(argv[1]) == "--cook-child")
    {
        bool delayed = false;
        Spark::AssetPipeline::CookRequest request{argv[2], argv[3], argv[4], false};
        request.onProgress = [&](const auto&, std::size_t, std::size_t)
        {
            if (!delayed)
            {
                delayed = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }
        };
        return Spark::AssetPipeline::CookAssets(request).Succeeded() ? 0 : 2;
    }

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() / ("spark-cooker-test-" + std::to_string(nonce));
    const auto source = root / "source";
    const auto output = root / "output";
    std::filesystem::create_directories(source / "nested");
    std::ofstream(source / "nested" / "asset.txt", std::ios::binary) << "deterministic asset";
    std::ofstream(source / "zeta.bin", std::ios::binary) << "z";
    std::ofstream(source / "alpha.bin", std::ios::binary) << "a";

    const auto manifest = output / "metadata" / "manifest.json";
    Spark::AssetPipeline::CookRequest request{source, output, manifest, false};
    const auto first = Spark::AssetPipeline::CookAssets(request);
    const auto second = Spark::AssetPipeline::CookAssets(request);
    std::filesystem::remove(source / "zeta.bin");
    const auto afterStaleRemoval = Spark::AssetPipeline::CookAssets(request);
    const std::string manifestAfterStaleRemoval = ReadFile(manifest);
    const auto overlapping = Spark::AssetPipeline::CookAssets({source, source / "output", {}, true});
    const auto escapedManifest = Spark::AssetPipeline::CookAssets({source, output, root / "outside.json", false});

    bool aliasedOutputPassed = true;
#if !defined(_WIN32)
    const auto aliasedRoot = root / "aliased-root";
    std::error_code aliasError;
    std::filesystem::create_directory_symlink(root, aliasedRoot, aliasError);
    if (!aliasError)
    {
        const auto aliasedOutput = aliasedRoot / "aliased-output";
        const auto aliasedManifest = aliasedOutput / "metadata" / "manifest.json";
        const auto aliasedResult = Spark::AssetPipeline::CookAssets({source, aliasedOutput, aliasedManifest, false});
        const auto aliasedDefaultOutput = aliasedRoot / "aliased-default-output";
        const auto aliasedDefaultResult = Spark::AssetPipeline::CookAssets({source, aliasedDefaultOutput, {}, false});
        aliasedOutputPassed =
            aliasedResult.Succeeded() && aliasedDefaultResult.Succeeded() &&
            std::filesystem::is_regular_file(root / "aliased-output" / "nested" / "asset.txt") &&
            std::filesystem::is_regular_file(root / "aliased-output" / "metadata" / "manifest.json") &&
            std::filesystem::is_regular_file(root / "aliased-default-output" / "spark-cook-manifest.json");
    }
#endif

    std::error_code symlinkError;
    const auto external = root / "external";
    std::filesystem::create_directories(external);
    std::ofstream(external / "escape.bin") << "escape";
    std::filesystem::create_directory_symlink(external, source / "linked", symlinkError);
    const bool symlinkAvailable = !symlinkError;
    const auto afterSymlink = Spark::AssetPipeline::CookAssets(request);

    const auto externalManifest = external / "manifest.json";
    std::ofstream(externalManifest) << "preserve";
    const auto linkedManifest = output / "linked-manifest.json";
    std::error_code manifestLinkError;
    std::filesystem::create_symlink(externalManifest, linkedManifest, manifestLinkError);
    const bool manifestLinkAvailable = !manifestLinkError;
    Spark::AssetPipeline::CookResult throughLinkedManifest;
    if (manifestLinkAvailable)
        throughLinkedManifest = Spark::AssetPipeline::CookAssets({source, output, linkedManifest, false});
    const std::string externalManifestBytes = ReadFile(externalManifest);

    const auto internalManifest = output / "internal-manifest.json";
    std::ofstream(internalManifest) << "preserve-internal";
    const auto internalManifestLink = output / "internal-manifest-link.json";
    std::error_code internalManifestLinkError;
    std::filesystem::create_symlink(internalManifest, internalManifestLink, internalManifestLinkError);
    const bool internalManifestLinkAvailable = !internalManifestLinkError;
    Spark::AssetPipeline::CookResult throughInternalManifestLink;
    if (internalManifestLinkAvailable)
        throughInternalManifestLink = Spark::AssetPipeline::CookAssets({source, output, internalManifestLink, false});

    const auto hardLinkedManifest = output / "hard-linked-manifest.json";
    std::error_code hardLinkError;
    std::filesystem::create_hard_link(internalManifest, hardLinkedManifest, hardLinkError);
    const bool hardLinkAvailable = !hardLinkError;
    Spark::AssetPipeline::CookResult throughHardLinkedManifest;
    if (hardLinkAvailable)
        throughHardLinkedManifest = Spark::AssetPipeline::CookAssets({source, output, hardLinkedManifest, false});
    const std::string internalManifestBytes = ReadFile(internalManifest);

    const auto directSource = root / "direct-source.bin";
    const auto directOutput = root / "direct-output.bin";
    std::ofstream(directSource, std::ios::binary) << "new bytes";
    std::ofstream(directOutput, std::ios::binary) << "old bytes";
    bool directUpdated = false;
    std::string directError;
    const bool wrongDigestAccepted = Spark::AssetPipeline::CookFile(directSource, directOutput, std::string(64, '0'),
                                                                    false, directUpdated, directError);
    const std::string directBytes = ReadFile(directOutput);

    const auto unicodeSource = root / "unicode-source";
    const auto unicodeOutput = root / "unicode-output";
    const auto unicodeNested = std::filesystem::u8path(u8"\u00E9");
    const auto unicodeFile = unicodeNested / std::filesystem::u8path(u8"\u96EA.bin");
    std::filesystem::create_directories(unicodeSource / unicodeNested);
    std::ofstream(unicodeSource / unicodeFile, std::ios::binary) << "portable unicode";
    std::ofstream(unicodeSource / std::filesystem::u8path(u8"\u03A9.txt"), std::ios::binary) << "ordered unicode";
    const auto unicodeFirst = Spark::AssetPipeline::CookAssets({unicodeSource, unicodeOutput, {}, false});
    const auto unicodeSecond = Spark::AssetPipeline::CookAssets({unicodeSource, unicodeOutput, {}, false});
    const std::string expectedUnicodePath = "\xC3\xA9/\xE9\x9B\xAA.bin";
    const bool unicodePassed = unicodeFirst.Succeeded() && unicodeSecond.Succeeded() &&
                               unicodeFirst.manifestSha256 == unicodeSecond.manifestSha256 &&
                               std::any_of(unicodeFirst.records.begin(), unicodeFirst.records.end(),
                                           [&](const auto& record) { return record.path == expectedUnicodePath; });

    bool controlFilenamePassed = true;
#if !defined(_WIN32)
    const auto controlSource = root / "control-source";
    const auto controlOutput = root / "control-output";
    std::filesystem::create_directories(controlSource);
    std::string controlName = "control-";
    for (const int value : {1, 8, 9, 10, 12, 13, 31})
        controlName.push_back(static_cast<char>(value));
    controlName += "-quote\"-slash\\.bin";
    std::ofstream(controlSource / controlName, std::ios::binary) << "control filename";
    const auto controlResult = Spark::AssetPipeline::CookAssets(
        {controlSource, controlOutput, controlOutput / "metadata" / "manifest.json", false});
    const std::string controlManifest = ReadFile(controlOutput / "metadata" / "manifest.json");
    controlFilenamePassed =
        controlResult.Succeeded() &&
        controlManifest.find("control-\\u0001\\b\\t\\n\\f\\r\\u001f-quote\\\"-slash\\\\.bin") != std::string::npos &&
        controlManifest.find(static_cast<char>(1)) == std::string::npos &&
        controlManifest.find(static_cast<char>(8)) == std::string::npos &&
        controlManifest.find(static_cast<char>(12)) == std::string::npos &&
        controlManifest.find(static_cast<char>(13)) == std::string::npos &&
        controlManifest.find(static_cast<char>(31)) == std::string::npos;
#endif

    const auto concurrentSourceA = root / "concurrent-source-a";
    const auto concurrentSourceB = root / "concurrent-source-b";
    const auto concurrentOutput = root / "concurrent-output";
    std::filesystem::create_directories(concurrentSourceA / "pair");
    std::filesystem::create_directories(concurrentSourceB / "pair");
    std::ofstream(concurrentSourceA / "pair" / "first.txt") << "generation-a";
    std::ofstream(concurrentSourceA / "pair" / "second.txt") << "generation-a";
    std::ofstream(concurrentSourceB / "pair" / "first.txt") << "generation-b";
    std::ofstream(concurrentSourceB / "pair" / "second.txt") << "generation-b";
    const auto concurrentManifest = concurrentOutput / "metadata" / "manifest.json";
    const auto executable = std::filesystem::absolute(argv[0]);
    auto childA =
        std::async(std::launch::async,
                   [&] { return RunCookChild(executable, concurrentSourceA, concurrentOutput, concurrentManifest); });
    auto childB =
        std::async(std::launch::async,
                   [&] { return RunCookChild(executable, concurrentSourceB, concurrentOutput, concurrentManifest); });
    const int childAResult = childA.get();
    const int childBResult = childB.get();
    const std::string concurrentFirst = ReadFile(concurrentOutput / "pair" / "first.txt");
    const std::string concurrentSecond = ReadFile(concurrentOutput / "pair" / "second.txt");
    const bool concurrentPassed = childAResult == 0 && childBResult == 0 && !concurrentFirst.empty() &&
                                  concurrentFirst == concurrentSecond && std::filesystem::exists(concurrentManifest);

    // Containment is decided on NORMALIZED paths. lexically_relative does not
    // normalize, so <root>/a/../../escaped.json used to relativize to
    // "a/../../escaped.json", whose FIRST component is "a" - an escape that a
    // check inspecting only the front accepts. IsContained lives in an anonymous
    // namespace, so it is exercised here through the public manifest target.
    // These two cases cook into their OWN output root: a successful cook publishes
    // by renaming a fresh generation over the output root, so sharing `output`
    // here would delete the artifacts the assertions below still describe.
    const auto traversalOutput = root / "traversal-output";
    const auto unnormalizedEscape = traversalOutput / "a" / ".." / ".." / "escaped.json";
    const auto escapeResult = Spark::AssetPipeline::CookAssets({source, traversalOutput, unnormalizedEscape, false});
    const bool escapeRejected = !escapeResult.Succeeded() && !std::filesystem::exists(root / "escaped.json");

    // The mirror case: a path that only *looks* like traversal but normalizes back
    // inside the root must still be accepted, or the fix would be a denial of
    // service dressed up as a security check.
    const auto normalizedInside = traversalOutput / "a" / ".." / "inside-manifest.json";
    const auto insideResult = Spark::AssetPipeline::CookAssets({source, traversalOutput, normalizedInside, false});
    const bool insideAccepted =
        insideResult.Succeeded() && std::filesystem::is_regular_file(traversalOutput / "inside-manifest.json");

    // The two CookAssets cases above cannot distinguish the normalization fix: the
    // cooker runs std::filesystem::absolute(...).lexically_normal() on the manifest
    // candidate BEFORE containment is decided, so the un-normalized form never
    // reaches the predicate and the pre-fix code rejected it just the same. Drive the
    // predicate directly with inputs that are NOT already lexically normal, so these
    // assertions can actually fail if the normalization is removed.
    const std::filesystem::path containmentRoot = std::filesystem::path("C:/Build/Out").make_preferred();
    const bool unnormalizedEscapeCaught =
        !Spark::AssetPipeline::IsPathContained(containmentRoot / "a" / ".." / ".." / "escaped.json", containmentRoot);
    const bool unnormalizedInsideAccepted =
        Spark::AssetPipeline::IsPathContained(containmentRoot / "a" / ".." / "inside.json", containmentRoot);
    const bool plainChildAccepted =
        Spark::AssetPipeline::IsPathContained(containmentRoot / "nested" / "asset.bin", containmentRoot);
    const bool siblingRejected = !Spark::AssetPipeline::IsPathContained(
        std::filesystem::path("C:/Build/Outside/x.bin").make_preferred(), containmentRoot);
#if defined(_WIN32)
    // NTFS is case-insensitive while std::filesystem::path equality is not, so a
    // differently-cased spelling of the same output root (a different CLI flag, a
    // config file, a normalised-drive shell) used to relativize to an escaping
    // "../../build/out/..." form and reject a legitimate target.
    const bool caseInsensitiveAccepted = Spark::AssetPipeline::IsPathContained(
        std::filesystem::path("C:/build/out/manifest.json").make_preferred(), containmentRoot);
#else
    const bool caseInsensitiveAccepted = true;
#endif
    const bool containmentPassed = unnormalizedEscapeCaught && unnormalizedInsideAccepted && plainChildAccepted &&
                                   siblingRejected && caseInsensitiveAccepted;

    const auto asset = std::find_if(first.records.begin(), first.records.end(),
                                    [](const auto& record) { return record.path == "nested/asset.txt"; });
    const bool passed =
        first.Succeeded() && second.Succeeded() && afterStaleRemoval.Succeeded() && afterSymlink.Succeeded() &&
        first.updatedCount == 3 && second.unchangedCount == 3 && afterStaleRemoval.updatedCount == 0 &&
        afterStaleRemoval.unchangedCount == 2 && !std::filesystem::exists(output / "zeta.bin") &&
        manifestAfterStaleRemoval.find("zeta.bin") == std::string::npos && std::filesystem::exists(manifest) &&
        !HasStagedSibling(output) && !HasStagedSibling(concurrentOutput) &&
        first.manifestSha256 == second.manifestSha256 && first.records.size() == 3 &&
        first.records[0].path == "alpha.bin" && first.records[1].path == "nested/asset.txt" &&
        first.records[2].path == "zeta.bin" && asset != first.records.end() &&
        asset->sha256 == "fd6c741e8f5199df6b0f7bff1652d6c8731105d5239533876d929d41b581bc2b" &&
        !overlapping.Succeeded() && !escapedManifest.Succeeded() && !std::filesystem::exists(root / "outside.json") &&
        (!symlinkAvailable || !std::filesystem::exists(output / "linked" / "escape.bin")) &&
        (!manifestLinkAvailable || (!throughLinkedManifest.Succeeded() && externalManifestBytes == "preserve")) &&
        (!internalManifestLinkAvailable ||
         (!throughInternalManifestLink.Succeeded() && internalManifestBytes == "preserve-internal")) &&
        (!hardLinkAvailable ||
         (!throughHardLinkedManifest.Succeeded() && internalManifestBytes == "preserve-internal")) &&
        !wrongDigestAccepted && directBytes == "old bytes" && unicodePassed && controlFilenamePassed &&
        aliasedOutputPassed && concurrentPassed && escapeRejected && insideAccepted && containmentPassed &&
        std::filesystem::is_regular_file(output / "nested" / "asset.txt");
    if (!passed)
    {
        std::cerr << "Asset cooker deterministic/incremental contract failed\n"
                  << "first=" << first.Succeeded() << " error='" << first.error << "' updated=" << first.updatedCount
                  << " unchanged=" << first.unchangedCount << "\n"
                  << "second=" << second.Succeeded() << " error='" << second.error
                  << "' updated=" << second.updatedCount << " unchanged=" << second.unchangedCount << "\n"
                  << "stale=" << afterStaleRemoval.Succeeded() << " error='" << afterStaleRemoval.error
                  << "' updated=" << afterStaleRemoval.updatedCount << " unchanged=" << afterStaleRemoval.unchangedCount
                  << "\n"
                  << "symlink=" << afterSymlink.Succeeded() << " error='" << afterSymlink.error << "'\n"
                  << "overlapRejected=" << !overlapping.Succeeded()
                  << " escapedRejected=" << !escapedManifest.Succeeded()
                  << " linkedRejected=" << (!manifestLinkAvailable || !throughLinkedManifest.Succeeded())
                  << " internalLinkedRejected="
                  << (!internalManifestLinkAvailable || !throughInternalManifestLink.Succeeded())
                  << " hardLinkedRejected=" << (!hardLinkAvailable || !throughHardLinkedManifest.Succeeded()) << "\n"
                  << "unnormalizedEscapeRejected=" << escapeRejected << " normalizedInsideAccepted=" << insideAccepted
                  << " insideError='" << insideResult.error << "'\n"
                  << "containment: unnormalizedEscapeCaught=" << unnormalizedEscapeCaught
                  << " unnormalizedInsideAccepted=" << unnormalizedInsideAccepted
                  << " plainChildAccepted=" << plainChildAccepted << " siblingRejected=" << siblingRejected
                  << " caseInsensitiveAccepted=" << caseInsensitiveAccepted << "\n"
                  << "unicode=" << unicodePassed << " control=" << controlFilenamePassed
                  << " aliasedOutput=" << aliasedOutputPassed << " children=" << childAResult << ',' << childBResult
                  << " concurrentFirst='" << concurrentFirst << "' concurrentSecond='" << concurrentSecond
                  << "' manifest=" << std::filesystem::exists(concurrentManifest) << "\n"
                  << "preserved fixture: " << root << "\n";
        return 1;
    }
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    return 0;
}
