// Regression tests for the portable Git cache used when Git is not on PATH.
//
// Before INST-130 the installer trusted <cache>/mingit/cmd/git.exe purely by
// existence and extracted downloads straight into that live directory, so an
// interrupted or partial extraction became the trusted Git forever. These
// tests drive the production GitBootstrap::EnsurePortableGitCache with an
// injected populator (no network) and prove that only a staged, verified,
// marked tree is ever activated.

#include "GitBootstrap.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace
{
    namespace fs = std::filesystem;

    constexpr const char* kPinnedSha = "7ed2a3ce5bbbf8eea976488de5416894ca3e6a0347cee195a7d768ac146d5290";
    constexpr const char* kOtherSha = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    constexpr const char* kMarkerName = ".spark-portable-git.sha256";

    int Check(bool condition, const std::string& message)
    {
        if (condition)
            return 0;
        std::cerr << "FAIL: " << message << '\n';
        return 1;
    }

    fs::path ExeRelative()
    {
        return fs::path("cmd") / "git.exe";
    }

    bool WriteText(const fs::path& path, const std::string& text)
    {
        std::error_code error;
        fs::create_directories(path.parent_path(), error);
        if (error)
            return false;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
        out.close();
        return static_cast<bool>(out);
    }

    std::string ReadText(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    size_t CountEntriesWithPrefix(const fs::path& directory, const std::string& prefix)
    {
        size_t count = 0;
        std::error_code error;
        for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
        {
            if (it->path().filename().string().compare(0, prefix.size(), prefix) == 0)
                ++count;
        }
        return count;
    }

    fs::path FindEntryWithPrefix(const fs::path& directory, const std::string& prefix)
    {
        std::error_code error;
        for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
        {
            if (it->path().filename().string().compare(0, prefix.size(), prefix) == 0)
                return it->path();
        }
        return {};
    }

    class TestRoot
    {
      public:
        explicit TestRoot(const std::string& name)
        {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            m_root = fs::temp_directory_path() /
                     ("SparkInstallerPortableGitCacheTests_" + name + "_" + std::to_string(stamp));
            std::error_code error;
            fs::create_directories(m_root / "cache", error);
            m_ok = !error;
        }
        ~TestRoot()
        {
            std::error_code ignored;
            fs::remove_all(m_root, ignored);
        }
        TestRoot(const TestRoot&) = delete;
        TestRoot& operator=(const TestRoot&) = delete;

        bool Ok() const { return m_ok; }
        fs::path Root() const { return m_root; }
        fs::path Cache() const { return m_root / "cache"; }
        fs::path Active() const { return Cache() / "mingit"; }

      private:
        fs::path m_root;
        bool m_ok = false;
    };

    SparkInstaller::GitBootstrapResult Ensure(const fs::path& cache, const std::string& sha,
                                              const SparkInstaller::PortableGitPopulator& populate)
    {
        return SparkInstaller::GitBootstrap::EnsurePortableGitCache(cache.string(), sha, ExeRelative().string(),
                                                                    populate, {});
    }

    // Installer_Interrupted: a population that dies after writing git.exe
    // (the exact state a killed extraction leaves) must never activate.
    int RunInterruptedPopulationTest()
    {
        TestRoot root("interrupted");
        int failures = Check(root.Ok(), "could not create interrupted-population test root");
        if (failures != 0)
            return failures;

        int calls = 0;
        const auto result = Ensure(root.Cache(), kPinnedSha,
                                   [&calls](const std::string& staging)
                                   {
                                       ++calls;
                                       (void)WriteText(fs::path(staging) / ExeRelative(), "partial");
                                       return false;
                                   });
        failures += Check(calls == 1, "interrupted population was not attempted exactly once");
        failures += Check(!result.ok, "interrupted population was reported as a usable Git");
        failures += Check(result.gitExe.empty(), "interrupted population returned a Git executable path");
        failures += Check(!result.diagnosticMessage.empty(), "interrupted population produced no diagnostic");
        failures += Check(!fs::exists(root.Active()), "interrupted population created the trusted cache directory");
        failures += Check(CountEntriesWithPrefix(root.Cache(), "mingit.staging-") == 0,
                          "interrupted population left a staging directory behind");
        return failures;
    }

    // Installer_Interrupted / repair: a pre-existing tree that has git.exe but
    // no activation marker (what the pre-fix installer left after a partial
    // extraction, and what it then trusted by existence) must be repaired from
    // a fresh verified population, and the old tree retained, not deleted.
    int RunUnverifiedTreeRepairTest()
    {
        TestRoot root("repair");
        int failures = Check(root.Ok(), "could not create repair test root");
        if (failures != 0)
            return failures;
        failures += Check(WriteText(root.Active() / ExeRelative(), "stale-partial"),
                          "could not create unverified cache fixture");

        int calls = 0;
        const SparkInstaller::PortableGitPopulator good = [&calls](const std::string& staging)
        {
            ++calls;
            return WriteText(fs::path(staging) / ExeRelative(), "fresh");
        };
        const auto result = Ensure(root.Cache(), kPinnedSha, good);
        failures += Check(result.ok, "repair of an unverified cache failed: " + result.diagnosticMessage);
        failures += Check(calls == 1, "unverified cache was trusted by existence instead of being repopulated");
        failures += Check(fs::path(result.gitExe) == root.Active() / ExeRelative(),
                          "repaired cache returned an unexpected Git executable path: " + result.gitExe);
        failures += Check(ReadText(root.Active() / ExeRelative()) == "fresh",
                          "trusted cache does not contain the freshly populated executable");
        failures += Check(ReadText(root.Active() / kMarkerName) == std::string(kPinnedSha) + "\n",
                          "activated cache does not carry the pinned archive hash marker");
        failures += Check(CountEntriesWithPrefix(root.Cache(), "mingit.untrusted-") == 1,
                          "unverified cache was not retired exactly once");
        const fs::path retired = FindEntryWithPrefix(root.Cache(), "mingit.untrusted-");
        failures += Check(!retired.empty() && ReadText(retired / ExeRelative()) == "stale-partial",
                          "unverified cache was deleted or modified instead of being retired intact");
        failures += Check(CountEntriesWithPrefix(root.Cache(), "mingit.staging-") == 0,
                          "successful activation left a staging directory behind");

        // A verified cache is reused without another download.
        int reuseCalls = 0;
        const auto reused = Ensure(root.Cache(), kPinnedSha,
                                   [&reuseCalls](const std::string&)
                                   {
                                       ++reuseCalls;
                                       return false;
                                   });
        failures += Check(reused.ok, "verified cache was not reused: " + reused.diagnosticMessage);
        failures += Check(reuseCalls == 0, "verified cache was needlessly repopulated");
        failures += Check(fs::path(reused.gitExe) == root.Active() / ExeRelative(),
                          "reused cache returned an unexpected Git executable path");

        // Uppercase pins are the same hash, not a different archive.
        int upperCalls = 0;
        std::string upperSha(kPinnedSha);
        for (char& character : upperSha)
        {
            if (character >= 'a' && character <= 'f')
                character = static_cast<char>(character - 'a' + 'A');
        }
        const auto upper = Ensure(root.Cache(), upperSha,
                                  [&upperCalls](const std::string&)
                                  {
                                      ++upperCalls;
                                      return false;
                                  });
        failures += Check(upper.ok && upperCalls == 0, "hash marker comparison was case-sensitive");

        // A marker for a different pin (stale archive or tampering) is not trusted.
        int repinCalls = 0;
        const auto repinned = Ensure(root.Cache(), kOtherSha,
                                     [&repinCalls](const std::string& staging)
                                     {
                                         ++repinCalls;
                                         return WriteText(fs::path(staging) / ExeRelative(), "repinned");
                                     });
        failures += Check(repinned.ok, "re-pinned cache failed to activate: " + repinned.diagnosticMessage);
        failures += Check(repinCalls == 1, "cache verified for another archive hash was trusted");
        failures += Check(ReadText(root.Active() / kMarkerName) == std::string(kOtherSha) + "\n",
                          "re-pinned cache does not carry the new archive hash marker");
        failures += Check(ReadText(root.Active() / ExeRelative()) == "repinned",
                          "re-pinned cache does not contain the new executable");
        return failures;
    }

    // Installer_Tamper / wrong archive: a population that succeeds but lacks the
    // expected executable must not displace the existing tree or be trusted.
    int RunWrongArchiveTest()
    {
        TestRoot root("wrong_archive");
        int failures = Check(root.Ok(), "could not create wrong-archive test root");
        if (failures != 0)
            return failures;
        failures += Check(WriteText(root.Active() / ExeRelative(), "existing"), "could not create existing tree");

        const auto result = Ensure(root.Cache(), kPinnedSha, [](const std::string& staging)
                                   { return WriteText(fs::path(staging) / "unexpected" / "payload.txt", "wrong"); });
        failures += Check(!result.ok, "archive without the Git executable was accepted");
        failures += Check(result.gitExe.empty(), "wrong archive returned a Git executable path");
        failures += Check(ReadText(root.Active() / ExeRelative()) == "existing",
                          "wrong archive replaced or modified the existing tree");
        failures += Check(!fs::exists(root.Active() / kMarkerName), "wrong archive marked the existing tree verified");
        failures += Check(CountEntriesWithPrefix(root.Cache(), "mingit.untrusted-") == 0,
                          "existing tree was retired before a verified replacement existed");
        failures += Check(CountEntriesWithPrefix(root.Cache(), "mingit.staging-") == 0,
                          "wrong archive left a staging directory behind");
        return failures;
    }

    // Invalid requests fail closed before any population or filesystem change.
    int RunInvalidRequestTest()
    {
        TestRoot root("invalid");
        int failures = Check(root.Ok(), "could not create invalid-request test root");
        if (failures != 0)
            return failures;

        int calls = 0;
        const SparkInstaller::PortableGitPopulator counting = [&calls](const std::string&)
        {
            ++calls;
            return true;
        };
        const auto badSha = SparkInstaller::GitBootstrap::EnsurePortableGitCache(root.Cache().string(), "not-a-sha256",
                                                                                 ExeRelative().string(), counting, {});
        failures += Check(!badSha.ok, "malformed archive hash was accepted");
        const auto escaping = SparkInstaller::GitBootstrap::EnsurePortableGitCache(
            root.Cache().string(), kPinnedSha, (fs::path("..") / "git.exe").string(), counting, {});
        failures += Check(!escaping.ok, "executable path escaping the cache was accepted");
        const auto absolute = SparkInstaller::GitBootstrap::EnsurePortableGitCache(
            root.Cache().string(), kPinnedSha, (root.Root() / "git.exe").string(), counting, {});
        failures += Check(!absolute.ok, "absolute executable path was accepted");
        failures += Check(calls == 0, "invalid request reached archive population");
        failures += Check(!fs::exists(root.Active()), "invalid request created the trusted cache directory");
        return failures;
    }

    // Installer_Tamper: a link planted at the trusted path, pointing at a tree
    // that carries a valid-looking marker, must not be trusted or followed.
    int RunPlantedLinkTest()
    {
        TestRoot root("planted_link");
        int failures = Check(root.Ok(), "could not create planted-link test root");
        if (failures != 0)
            return failures;
        const fs::path elsewhere = root.Root() / "elsewhere";
        failures += Check(WriteText(elsewhere / ExeRelative(), "outside"), "could not create link target tree");
        failures += Check(WriteText(elsewhere / kMarkerName, std::string(kPinnedSha) + "\n"),
                          "could not create link target marker");

        std::error_code linkError;
        fs::create_directory_symlink(elsewhere, root.Active(), linkError);
        if (linkError)
        {
            // Unprivileged Windows hosts without Developer Mode cannot create
            // symlinks; the remaining cases still cover the activation contract.
            std::cout << "SKIP: planted-link case (" << linkError.message() << ")\n";
            return failures;
        }

        int calls = 0;
        const auto result = Ensure(root.Cache(), kPinnedSha,
                                   [&calls](const std::string& staging)
                                   {
                                       ++calls;
                                       return WriteText(fs::path(staging) / ExeRelative(), "fresh");
                                   });
        failures += Check(result.ok, "cache behind a planted link could not be repaired: " + result.diagnosticMessage);
        failures += Check(calls == 1, "planted link at the trusted cache path was trusted");
        std::error_code statusError;
        failures += Check(fs::symlink_status(root.Active(), statusError).type() == fs::file_type::directory,
                          "trusted cache path is still a link after repair");
        failures += Check(ReadText(elsewhere / ExeRelative()) == "outside",
                          "repair followed the planted link and modified the target tree");
        failures += Check(fs::exists(elsewhere / kMarkerName), "repair deleted files through the planted link");
        return failures;
    }
} // namespace

// Linked into SparkInstallerGitTests (already built and run by the installer CI
// jobs) and invoked from its main(). Returns the number of failed checks.
int RunPortableGitCacheTests()
{
    int failures = 0;
    failures += RunInterruptedPopulationTest();
    failures += RunUnverifiedTreeRepairTest();
    failures += RunWrongArchiveTest();
    failures += RunInvalidRequestTest();
    failures += RunPlantedLinkTest();
    if (failures == 0)
        std::cout << "SparkInstaller portable Git cache tests passed\n";
    return failures;
}
