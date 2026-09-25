#include "ArchiveExtraction.h"
#include "Downloader.h"
#include "DownloadSecurity.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace
{
    uint64_t CurrentProcessId()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        return static_cast<uint64_t>(GetCurrentProcessId());
#else
        return static_cast<uint64_t>(getpid());
#endif
    }

    std::set<std::filesystem::path> CurrentProcessTempDownloads()
    {
        std::set<std::filesystem::path> paths;
        const std::string prefix = "sparkbuild_download_" + std::to_string(CurrentProcessId()) + "_";
        std::error_code error;
        const std::filesystem::path tempDirectory(SparkBuild::Downloader::GetTempDir());
        for (std::filesystem::directory_iterator it(tempDirectory, error), end; !error && it != end;
             it.increment(error))
        {
            const std::string filename = it->path().filename().string();
            if (filename.compare(0, prefix.size(), prefix) == 0 && it->path().extension() == ".zip")
                paths.insert(it->path());
        }
        return paths;
    }

    class ReservedPathCleanup
    {
      public:
        ~ReservedPathCleanup()
        {
            for (const auto& path : paths)
            {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
        }

        std::vector<std::filesystem::path> paths;
    };

    int RunUniqueReservationTest()
    {
        constexpr size_t reservationCount = 32;
        ReservedPathCleanup cleanup;
        cleanup.paths.resize(reservationCount);
        std::vector<std::thread> workers;
        workers.reserve(reservationCount);

        for (size_t index = 0; index < reservationCount; ++index)
        {
            workers.emplace_back([index, &cleanup]
                                 { cleanup.paths[index] = SparkBuild::Downloader::ReserveTempDownloadPath(); });
        }
        for (auto& worker : workers)
            worker.join();

        int failures = 0;
        auto check = [&failures](bool condition, const std::string& message)
        {
            if (!condition)
            {
                ++failures;
                std::cerr << "FAIL: " << message << '\n';
            }
        };

        std::set<std::filesystem::path> uniquePaths;
        for (size_t index = 0; index < cleanup.paths.size(); ++index)
        {
            const auto& path = cleanup.paths[index];
            check(!path.empty(), "reservation " + std::to_string(index) + " returned an empty path");
            if (path.empty())
                continue;

            uniquePaths.insert(path);
            std::error_code error;
            check(std::filesystem::is_regular_file(path, error) && !error,
                  "reserved path is not an existing regular file: " + path.string());
            check(path.extension() == ".zip", "reserved path does not retain the .zip extension: " + path.string());
        }
        check(uniquePaths.size() == reservationCount, "concurrent reservations returned duplicate paths");

        // Each reservation is a real, independent file rather than a merely
        // predicted name. Writing one must not affect any other reservation.
        for (size_t index = 0; index < cleanup.paths.size(); ++index)
        {
            if (cleanup.paths[index].empty())
                continue;
            std::ofstream file(cleanup.paths[index], std::ios::binary | std::ios::trunc);
            file << index;
            check(file.good(), "could not write reserved path " + cleanup.paths[index].string());
        }
        for (size_t index = 0; index < cleanup.paths.size(); ++index)
        {
            if (cleanup.paths[index].empty())
                continue;
            std::ifstream file(cleanup.paths[index], std::ios::binary);
            size_t storedIndex = reservationCount;
            file >> storedIndex;
            check(storedIndex == index, "reserved files were not independent");
        }

        return failures == 0 ? 0 : 1;
    }

    int RunFailedDownloadCleanupTest()
    {
        const auto before = CurrentProcessTempDownloads();
        const std::filesystem::path unusedDestination =
            std::filesystem::path(SparkBuild::Downloader::GetTempDir()) /
            ("sparkbuild_unused_destination_" + std::to_string(CurrentProcessId()));

        // A file:// URL names a ZIP (so a download path is reserved) but is
        // refused without network I/O: curl only accepts HTTP(S) and WinHTTP
        // cannot open it. Both paths must remove the reserved archive.
        const bool result = SparkBuild::Downloader::DownloadAndExtract(
            "file:///nonexistent/sparkbuild/archive.zip", unusedDestination.string(),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        const auto after = CurrentProcessTempDownloads();

        std::error_code ignored;
        std::filesystem::remove_all(unusedDestination, ignored);
        if (result)
        {
            std::cerr << "FAIL: an empty download URL unexpectedly succeeded\n";
            return 1;
        }
        if (after != before)
        {
            std::cerr << "FAIL: failed download left a temporary archive behind\n";
            return 1;
        }
        return 0;
    }

    int RunSha256VerificationTest()
    {
        const std::filesystem::path archive = std::filesystem::path(SparkBuild::Downloader::GetTempDir()) /
                                              ("sparkbuild_sha256_" + std::to_string(CurrentProcessId()) + ".zip");
        const std::filesystem::path destination =
            std::filesystem::path(SparkBuild::Downloader::GetTempDir()) /
            ("sparkbuild_sha256_destination_" + std::to_string(CurrentProcessId()));
        std::error_code ignored;
        std::filesystem::remove(archive, ignored);
        std::filesystem::remove_all(destination, ignored);
        {
            std::ofstream output(archive, std::ios::binary | std::ios::trunc);
            output << "abc";
        }

        int failures = 0;
        auto check = [&failures](bool condition, const std::string& message)
        {
            if (!condition)
            {
                ++failures;
                std::cerr << "FAIL: " << message << '\n';
            }
        };
        std::string error;
        check(SparkBuild::DownloadSecurity::VerifySha256(
                  archive, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", error),
              "published SHA-256 known-answer vector was rejected: " + error);
        error.clear();
        check(!SparkBuild::DownloadSecurity::VerifySha256(
                  archive, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", error),
              "mismatched SHA-256 unexpectedly passed verification");
        check(error.find("mismatch") != std::string::npos, "checksum mismatch did not produce a useful diagnostic");
        error.clear();
        check(!SparkBuild::DownloadSecurity::VerifySha256(archive, "not-a-sha256", error),
              "malformed expected SHA-256 unexpectedly passed validation");
        check(!SparkBuild::Downloader::ExtractVerifiedArchive(
                  archive.string(), destination.string(),
                  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", SparkBuild::ArchiveFormat::Zip),
              "mismatched archive unexpectedly reached extraction");
        check(!std::filesystem::exists(destination), "checksum mismatch created an extraction destination");
        check(SparkBuild::DownloadSecurity::RedirectProtocolPolicy("https://example.test/archive.zip") == "=https",
              "HTTPS source did not restrict redirect protocols to HTTPS");
        check(SparkBuild::DownloadSecurity::RedirectProtocolPolicy("HTTPS://example.test/archive.zip") == "=https",
              "HTTPS redirect policy was case-sensitive");
        check(SparkBuild::DownloadSecurity::RedirectProtocolPolicy("http://example.test/archive.zip") == "=http,https",
              "HTTP source redirect policy escaped the HTTP(S) protocol family");

        std::filesystem::remove(archive, ignored);
        std::filesystem::remove_all(destination, ignored);
        return failures == 0 ? 0 : 1;
    }

    int RunArchivePolicyTest()
    {
        using SparkBuild::ArchiveFormat;
        using SparkBuild::Downloader;
        using SparkBuild::ArchiveExtraction::IsSafeMemberName;
        using SparkBuild::ArchiveExtraction::MemberSyntax;
        constexpr MemberSyntax zip = MemberSyntax::Zip;
        constexpr MemberSyntax tar = MemberSyntax::Tar;

        int failures = 0;
        auto check = [&failures](bool condition, const std::string& message)
        {
            if (!condition)
            {
                ++failures;
                std::cerr << "FAIL: " << message << '\n';
            }
        };

        check(IsSafeMemberName("tool/bin/cmake", zip), "plain nested member was rejected");
        check(IsSafeMemberName("./tool/", tar), "'./' prefixed tar member was rejected");
        check(!IsSafeMemberName("", tar), "empty member name was accepted");
        check(!IsSafeMemberName("/etc/passwd", tar), "absolute POSIX member was accepted");
        check(!IsSafeMemberName("\\server\\share", tar), "UNC-style member was accepted");
        check(!IsSafeMemberName("C:evil", tar), "drive-relative member was accepted");
        check(!IsSafeMemberName("a/../../escape", tar), "'..' traversal member was accepted");
        check(!IsSafeMemberName("a\\..\\escape", tar), "backslash '..' traversal member was accepted");
        check(!IsSafeMemberName("..", tar), "bare '..' member was accepted");
        check(!IsSafeMemberName("file.txt:stream", zip), "ZIP member with a colon was accepted");
        check(!IsSafeMemberName("dir\\file.txt", zip), "ZIP member with a backslash separator was accepted");
        check(IsSafeMemberName("share/doc:notes", tar), "tar member with an interior colon was rejected");
        check(IsSafeMemberName("odd\\name", tar), "tar member with a literal backslash was rejected");

        check(Downloader::ArchiveFormatFromName("https://x.test/cmake-linux.tar.gz") == ArchiveFormat::GzipTar,
              ".tar.gz URL did not name a gzip tar");
        check(Downloader::ArchiveFormatFromName("https://x.test/a.TGZ?download=1") == ArchiveFormat::GzipTar,
              ".TGZ URL with a query did not name a gzip tar");
        check(Downloader::ArchiveFormatFromName("https://x.test/MinGit.zip") == ArchiveFormat::Zip,
              ".zip URL did not name a ZIP");
        check(Downloader::ArchiveFormatFromName("https://x.test/zip.tar.gz.exe") == ArchiveFormat::Unknown,
              "unrecognised suffix was not Unknown");
        return failures == 0 ? 0 : 1;
    }

#ifndef SPARK_PLATFORM_WINDOWS
    // Fixtures are generated at test time with python3's zipfile/tarfile so the
    // hostile archives never exist in the source tree.
    constexpr const char* kFixtureScript = R"PY(
import io, os, struct, sys, tarfile, zipfile, zlib

root = sys.argv[1]
fixtures = os.path.join(root, "fixtures")
outside = os.path.join(root, "outside")
os.makedirs(fixtures)
os.makedirs(outside)

def path(name):
    return os.path.join(fixtures, name)

def add_file(archive, name, data):
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.mode = 0o644
    archive.addfile(info, io.BytesIO(data))

def add_link(archive, name, target):
    info = tarfile.TarInfo(name)
    info.type = tarfile.SYMTYPE
    info.linkname = target
    archive.addfile(info)

with zipfile.ZipFile(path("good.zip"), "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("tool/bin/run.sh", "echo ok\n")
    z.writestr("tool/README.txt", "hello\n")

with tarfile.open(path("good.tar.gz"), "w:gz") as t:
    add_file(t, "pkg/bin/tool", b"binary")
    add_file(t, "pkg/data.txt", b"data")
    add_link(t, "pkg/lib/tool-link", "../bin/tool")

with tarfile.open(path("targz_named.zip"), "w:gz") as t:
    add_file(t, "renamed/file.txt", b"tar-content")

with tarfile.open(path("symlink_write_through.tar.gz"), "w:gz") as t:
    add_link(t, "evil", outside)
    add_file(t, "evil/pwned", b"pwned")

with tarfile.open(path("symlink_escape.tar.gz"), "w:gz") as t:
    add_file(t, "pkg/ok.txt", b"ok")
    add_link(t, "pkg/escape", outside)

with tarfile.open(path("absolute_member.tar.gz"), "w:gz") as t:
    add_file(t, "/abs/pwned", b"pwned")

with zipfile.ZipFile(path("dotdot.zip"), "w") as z:
    z.writestr("ok.txt", "ok")
    z.writestr("../escaped.txt", "pwned")

with zipfile.ZipFile(path("conflict.zip"), "w") as z:
    z.writestr("keep/file.txt", "replacement")
    z.writestr("fresh/new.txt", "new")

with open(path("garbage.zip"), "wb") as f:
    f.write(b"not an archive at all")

with zipfile.ZipFile(path("backslash.zip"), "w") as z:
    z.writestr("dir\\file.txt", "ambiguous separator")

# Hand-built stored ZIPs for the central-directory parser's own defences.
def raw_zip(name, entries, zip64_offset=False, zip64_extra=True, comment_length=0, central_name_length=None):
    data = b""
    central = b""
    for member, local_member, content in entries:
        offset = len(data)
        crc = zlib.crc32(content)
        local_name = local_member.encode()
        data += struct.pack("<IHHHHHIIIHH", 0x04034b50, 20, 0, 0, 0, 0, crc, len(content), len(content),
                            len(local_name), 0) + local_name + content
        central_name = member.encode()
        extra = b""
        stored_offset = offset
        if zip64_offset:
            stored_offset = 0xFFFFFFFF
            if zip64_extra:
                extra = struct.pack("<HHQ", 1, 8, offset)
        name_length = len(central_name) if central_name_length is None else central_name_length
        central += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014b50, 20, 20, 0, 0, 0, 0, crc, len(content), len(content),
                               name_length, len(extra), 0, 0, 0, 0, stored_offset) + central_name + extra
    eocd = struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, len(entries), len(entries), len(central), len(data),
                       comment_length)
    with open(path(name), "wb") as f:
        f.write(data + central + eocd)

raw_zip("raw_good.zip", [("raw/a.txt", "raw/a.txt", b"alpha"), ("raw/b.txt", "raw/b.txt", b"beta")])
raw_zip("name_mismatch.zip", [("safe/name.txt", "../evil.txt!", b"pwned")])
raw_zip("zip64_offset.zip", [("z64/a.txt", "z64/a.txt", b"alpha"), ("z64/b.txt", "z64/b.txt", b"beta")],
        zip64_offset=True)
raw_zip("zip64_missing_extra.zip", [("z64/a.txt", "z64/a.txt", b"alpha")], zip64_offset=True, zip64_extra=False)
raw_zip("bad_comment_length.zip", [("c/a.txt", "c/a.txt", b"alpha")], comment_length=5)
raw_zip("overrun_entry.zip", [("o/a.txt", "o/a.txt", b"alpha")], central_name_length=200)
)PY";

    std::string ReadWholeFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    std::set<std::string> DirectoryNames(const std::filesystem::path& directory)
    {
        std::set<std::string> names;
        std::error_code error;
        for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
            names.insert(it->path().filename().string());
        return names;
    }

    int RunZipListingTest(const std::filesystem::path& fixtures);

    int RunStagedExtractionTest()
    {
        namespace fs = std::filesystem;
        using SparkBuild::ArchiveFormat;
        using SparkBuild::Downloader;

        const fs::path root =
            fs::path(Downloader::GetTempDir()) / ("sparkbuild_extract_" + std::to_string(CurrentProcessId()));
        std::error_code ignored;
        fs::remove_all(root, ignored);
        fs::create_directories(root);
        const fs::path script = root / "make_fixtures.py";
        {
            std::ofstream output(script, std::ios::binary | std::ios::trunc);
            output << kFixtureScript;
        }
        const std::string command = "python3 \"" + script.string() + "\" \"" + root.string() + "\"";
        if (std::system(command.c_str()) != 0)
        {
            std::cerr << "FAIL: could not generate archive fixtures with python3\n";
            fs::remove_all(root, ignored);
            return 1;
        }

        int failures = 0;
        auto check = [&failures](bool condition, const std::string& message)
        {
            if (!condition)
            {
                ++failures;
                std::cerr << "FAIL: " << message << '\n';
            }
        };
        const fs::path fixtures = root / "fixtures";
        const fs::path outside = root / "outside";
        auto extract = [&fixtures](const std::string& fixture, const fs::path& destination, ArchiveFormat format)
        {
            const fs::path archive = fixtures / fixture;
            std::string digest;
            std::string error;
            if (!SparkBuild::DownloadSecurity::ComputeSha256(archive, digest, error))
                return false;
            return Downloader::ExtractVerifiedArchive(archive.string(), destination.string(), digest, format);
        };
        auto noStagingLeft = [&root]
        {
            std::error_code walkError;
            for (fs::recursive_directory_iterator it(root, walkError), end; !walkError && it != end;
                 it.increment(walkError))
            {
                if (it->path().filename().string().find(".sparkbuild-staging-") != std::string::npos)
                    return false;
            }
            return !walkError;
        };

        // A working tree that every failed extraction below must leave byte-identical.
        const fs::path working = root / "working";
        fs::create_directories(working / "keep");
        {
            std::ofstream output(working / "keep" / "file.txt", std::ios::binary | std::ios::trunc);
            output << "original";
        }
        auto workingIntact = [&working]
        {
            return DirectoryNames(working) == std::set<std::string>{"keep"} &&
                   DirectoryNames(working / "keep") == std::set<std::string>{"file.txt"} &&
                   ReadWholeFile(working / "keep" / "file.txt") == "original";
        };

        // Content, not the name, decides the container type.
        const fs::path renamed = root / "renamed";
        check(Downloader::DetectArchiveFormat((fixtures / "targz_named.zip").string()) == ArchiveFormat::GzipTar,
              "gzip tar bytes under a .zip name were not detected as gzip tar");
        check(Downloader::DetectArchiveFormat((fixtures / "good.zip").string()) == ArchiveFormat::Zip,
              "ZIP bytes were not detected as ZIP");
        check(!extract("targz_named.zip", renamed, ArchiveFormat::Zip),
              "gzip tar content was extracted when a ZIP was expected");
        check(!fs::exists(renamed), "format mismatch created the destination");
        check(extract("targz_named.zip", renamed, ArchiveFormat::GzipTar),
              "gzip tar content under a .zip name was not extracted as gzip tar");
        check(ReadWholeFile(renamed / "renamed" / "file.txt") == "tar-content", "content-detected tar lost data");
        check(!extract("garbage.zip", root / "garbage", ArchiveFormat::Zip), "unrecognised content was extracted");
        check(!extract("good.zip", root / "unknown", ArchiveFormat::Unknown), "an Unknown expected format passed");

        // Symlink attacks: nothing may land outside, and the working tree is untouched.
        check(!extract("symlink_write_through.tar.gz", working, ArchiveFormat::GzipTar),
              "tar writing through an escaping symlink was accepted");
        check(!extract("symlink_escape.tar.gz", working, ArchiveFormat::GzipTar),
              "tar with a symlink resolving outside the root was accepted");
        check(DirectoryNames(outside).empty(), "a symlink archive wrote outside the extraction root");
        check(workingIntact(), "a rejected symlink archive changed the existing destination");

        // Traversal members are refused before any extractor runs.
        check(!extract("dotdot.zip", working, ArchiveFormat::Zip), "ZIP with a '../' member was accepted");
        check(!fs::exists(root / "escaped.txt"), "ZIP '../' member escaped the staging directory");
        check(!extract("absolute_member.tar.gz", working, ArchiveFormat::GzipTar),
              "tar with an absolute member was accepted");
        check(workingIntact(), "a rejected traversal archive changed the existing destination");

        // Structurally inconsistent ZIPs are refused before any extractor runs.
        check(!extract("name_mismatch.zip", working, ArchiveFormat::Zip),
              "ZIP whose local and central names differ was accepted");
        check(!extract("backslash.zip", working, ArchiveFormat::Zip), "ZIP with a backslash member was accepted");
        check(!fs::exists(root / "evil.txt!"), "ZIP local-header name escaped the staging directory");
        check(workingIntact(), "a rejected malformed ZIP changed the existing destination");
        const fs::path unusedDestination = root / "never-created";
        check(!extract("backslash.zip", unusedDestination, ArchiveFormat::Zip) && !fs::exists(unusedDestination),
              "a refused archive left behind a destination it would have created");

        // An entry that already exists is never replaced, and nothing else is committed either.
        check(!extract("conflict.zip", working, ArchiveFormat::Zip), "archive overwrote an existing entry");
        check(workingIntact(), "a conflicting archive changed the existing destination");

        // Good archives land intact beside existing content.
        check(extract("good.zip", working, ArchiveFormat::Zip), "valid ZIP was rejected");
        check(ReadWholeFile(working / "tool" / "bin" / "run.sh") == "echo ok\n", "valid ZIP content was altered");
        check(ReadWholeFile(working / "tool" / "README.txt") == "hello\n", "valid ZIP lost a member");
        check(ReadWholeFile(working / "keep" / "file.txt") == "original", "valid ZIP disturbed existing content");
        const fs::path package = root / "package";
        check(extract("good.tar.gz", package, ArchiveFormat::GzipTar), "valid gzip tar was rejected");
        check(fs::is_symlink(package / "pkg" / "lib" / "tool-link"), "contained tar symlink was not preserved");
        check(ReadWholeFile(package / "pkg" / "lib" / "tool-link") == "binary", "contained tar symlink is broken");
        check(ReadWholeFile(package / "pkg" / "data.txt") == "data", "valid gzip tar content was altered");

        const fs::path raw = root / "raw";
        check(extract("raw_good.zip", raw, ArchiveFormat::Zip), "hand-built stored ZIP was rejected");
        check(ReadWholeFile(raw / "raw" / "b.txt") == "beta", "hand-built stored ZIP content was altered");
        const fs::path zip64 = root / "zip64";
        check(extract("zip64_offset.zip", zip64, ArchiveFormat::Zip), "ZIP with ZIP64 local offsets was rejected");
        check(ReadWholeFile(zip64 / "z64" / "b.txt") == "beta", "ZIP64-offset member content was altered");

        // A destination that is a symlink to another directory (possibly another
        // filesystem) still commits: staging lives inside the resolved target.
        const fs::path sharedMemory = "/dev/shm";
        if (fs::is_directory(sharedMemory))
        {
            const fs::path redirectedTarget =
                sharedMemory / ("sparkbuild_redirect_" + std::to_string(CurrentProcessId()));
            fs::remove_all(redirectedTarget, ignored);
            fs::create_directories(redirectedTarget);
            const fs::path redirected = root / "redirected";
            fs::create_directory_symlink(redirectedTarget, redirected);
            check(extract("good.tar.gz", redirected, ArchiveFormat::GzipTar),
                  "extraction into a symlinked (cross-filesystem) destination failed");
            check(ReadWholeFile(redirectedTarget / "pkg" / "data.txt") == "data",
                  "symlinked destination did not receive the extracted content");
            check(DirectoryNames(redirectedTarget) == std::set<std::string>{"pkg"},
                  "symlinked destination kept a staging directory");
            fs::remove_all(redirectedTarget, ignored);
        }

        failures += RunZipListingTest(fixtures);
        check(noStagingLeft(), "a staging directory was left behind");
        fs::remove_all(root, ignored);
        return failures == 0 ? 0 : 1;
    }

    // Parser defences exercised on the in-process ZIP listing directly.
    int RunZipListingTest(const std::filesystem::path& fixtures)
    {
        using SparkBuild::ArchiveExtraction::ListZipMembers;
        int failures = 0;
        auto check = [&failures](bool condition, const std::string& message)
        {
            if (!condition)
            {
                ++failures;
                std::cerr << "FAIL: " << message << '\n';
            }
        };
        std::vector<std::string> names;
        std::string error;
        check(ListZipMembers(fixtures / "raw_good.zip", names, error) &&
                  names == std::vector<std::string>{"raw/a.txt", "raw/b.txt"},
              "hand-built ZIP listing was wrong: " + error);
        check(ListZipMembers(fixtures / "zip64_offset.zip", names, error) &&
                  names == std::vector<std::string>{"z64/a.txt", "z64/b.txt"},
              "ZIP64 extra-field local offsets were not resolved: " + error);

        const std::vector<std::pair<std::string, std::string>> refused = {
            {"name_mismatch.zip", "does not match the central directory"},
            {"zip64_missing_extra.zip", "ZIP64 local header offset is missing"},
            {"bad_comment_length.zip", "end-of-central-directory record not found"},
            {"overrun_entry.zip", "overruns the directory"},
            {"garbage.zip", "too small to be a ZIP file"},
        };
        for (const auto& [fixture, diagnostic] : refused)
        {
            error.clear();
            check(!ListZipMembers(fixtures / fixture, names, error), fixture + " was listed as a valid ZIP");
            check(error.find(diagnostic) != std::string::npos,
                  fixture + " was refused with an unexpected diagnostic: " + error);
        }
        return failures == 0 ? 0 : 1;
    }

    // The containment and completeness walks tested on hand-built staging trees,
    // independent of how any particular extractor behaves.
    int RunStagedTreeValidationTest()
    {
        namespace fs = std::filesystem;
        using SparkBuild::ArchiveExtraction::ValidateStagedTree;
        using SparkBuild::ArchiveExtraction::VerifyStagedMembers;

        int failures = 0;
        auto check = [&failures](bool condition, const std::string& message)
        {
            if (!condition)
            {
                ++failures;
                std::cerr << "FAIL: " << message << '\n';
            }
        };

        const fs::path root = fs::path(SparkBuild::Downloader::GetTempDir()) /
                              ("sparkbuild_staged_tree_" + std::to_string(CurrentProcessId()));
        std::error_code ignored;
        fs::remove_all(root, ignored);
        const fs::path destination = root / "dest";
        const fs::path staging = destination / ".sparkbuild-staging-test";
        const fs::path outside = root / "outside";
        fs::create_directories(outside);

        // Build a fresh staging tree holding pkg/a plus one extra link.
        auto stage = [&](const fs::path& linkName, const fs::path& linkTarget)
        {
            fs::remove_all(staging, ignored);
            fs::create_directories(staging / "pkg" / "sub");
            std::ofstream(staging / "pkg" / "a", std::ios::binary) << "a";
            if (!linkName.empty())
                fs::create_symlink(linkTarget, staging / linkName);
        };
        std::string error;
        auto accepted = [&]
        {
            error.clear();
            return ValidateStagedTree(staging, error);
        };

        stage({}, {});
        check(accepted(), "plain staged tree was rejected: " + error);
        stage("pkg/sub/up", "../a");
        check(accepted(), "contained relative link was rejected: " + error);
        stage("pkg/sub/dot", "./../../pkg/./a");
        check(accepted(), "contained link with '.' components was rejected: " + error);

        stage("pkg/viaStaging", "../../.sparkbuild-staging-test/pkg/a");
        check(!accepted(), "link re-entering through the staging directory's own name was accepted");
        stage("pkg/absoluteInside", fs::canonical(staging) / "pkg" / "a");
        check(!accepted(), "absolute link into the staging tree was accepted");
        stage("pkg/absoluteOutside", outside);
        check(!accepted(), "absolute link outside the tree was accepted");
        stage("pkg/climb", "../../outside");
        check(!accepted(), "relative link climbing out of the tree was accepted");
        stage("pkg/zigzag", "sub/../../pkg/a");
        check(!accepted(), "link with '..' after a plain component was accepted");
        stage("pkg/dangling", "missing");
        check(!accepted(), "dangling link was accepted");
        stage("pkg/loop", "loop");
        check(!accepted(), "self-referencing link was accepted");
        stage({}, {});
        fs::remove_all(staging / "pkg");
        check(!accepted(), "empty staged tree was accepted");

        // Completeness: the staged set must equal the listed members and their ancestors.
        stage({}, {});
        const std::vector<std::string> exact = {"pkg/", "pkg/a", "pkg/sub/"};
        error.clear();
        check(VerifyStagedMembers(staging, exact, error), "exact staged member set was rejected: " + error);
        error.clear();
        check(VerifyStagedMembers(staging, {"pkg/a", "pkg/sub/"}, error),
              "implied ancestor directory was treated as unlisted: " + error);
        error.clear();
        check(!VerifyStagedMembers(staging, {"pkg/a", "pkg/sub/", "pkg/b"}, error) &&
                  error.find("missing") != std::string::npos,
              "a partial extraction (missing member) was accepted");
        error.clear();
        check(!VerifyStagedMembers(staging, {"pkg/a"}, error) && error.find("does not list") != std::string::npos,
              "an unlisted staged entry was accepted");

        fs::remove_all(root, ignored);
        return failures == 0 ? 0 : 1;
    }
#endif
} // namespace

int main()
{
    const int reservationResult = RunUniqueReservationTest();
    const int cleanupResult = RunFailedDownloadCleanupTest();
    const int sha256Result = RunSha256VerificationTest();
    const int policyResult = RunArchivePolicyTest();
#ifndef SPARK_PLATFORM_WINDOWS
    const int extractionResult = RunStagedExtractionTest() | RunStagedTreeValidationTest();
#else
    const int extractionResult = 0; // tar.exe ZIP extraction needs native Windows evidence.
#endif
    return reservationResult == 0 && cleanupResult == 0 && sha256Result == 0 && policyResult == 0 &&
                   extractionResult == 0
               ? 0
               : 1;
}
