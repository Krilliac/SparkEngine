/**
 * @file TestSEC2GameModules.cpp
 * @brief SEC2 game-module security fixes: bounded console inputs, fog-of-war work bounds,
 *        owner-scoped network handlers across module hot reload, and server-attributed chat.
 *
 * Each SEC2GM_ test pins one fix from the SEC2-game-modules lane:
 * - ProgressionSystem::AwardXP saturates instead of reaching float-to-int or signed-overflow UB.
 * - WaveComposition bounds every wave to MAX_ENEMIES_PER_WAVE, heavies included.
 * - NetworkManager handler ownership: a hot-reload replacement keeps its handlers when the outgoing
 *   image tears down, and everything an image owns is removed before it is unmapped.
 * - Game modules remove their network handlers with UnregisterHandler instead of installing image-resident
 *   empty lambdas, and every SparkGameMMOFPS registration function has a release that removes each id it
 *   registered (source contract over GameModules/, plus the unowned lazy-handler seam MMOFPS relies on).
 * - MMO chat relays only routable channels, never forwards a client-chosen sender name, and attributes
 *   every relayed line to `<sanitized connection name>#<client id>`, which no connection name can forge.
 * - RTS fog of war clips vision to the grid and restored saves reject absurd vision ranges.
 *
 * The CTest registration SEC2GameModules pins the family size, which depends on the
 * ImGui (module sources) and networking features compiled into SparkTests.
 */

#include "TestFramework.h"

#include "../GameModules/SparkGameFPS/Source/Game/ProgressionSystem.h"
#include "../GameModules/SparkGameFPS/Source/Game/WaveComposition.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// ---------------------------------------------------------------------------
// FPS progression (xp console command)
// ---------------------------------------------------------------------------

TEST(SEC2GM_ProgressionAwardSaturatesInsteadOfOverflowing)
{
    Spark::ProgressionSystem progression;
    progression.Initialize();
    int observedBase = -1;
    int observedModified = -1;
    progression.GetCallbacks().onXPAwarded = [&](int base, const std::string&, int modified)
    {
        observedBase = base;
        observedModified = modified;
    };

    // float(INT_MAX) rounds up to 2^31; the old cast back to int was undefined and produced INT_MIN,
    // leaving a negative XP total that was then persisted.
    progression.AwardXP(INT_MAX, "console");
    EXPECT_EQ(observedBase, Spark::ProgressionSystem::MAX_SINGLE_AWARD);
    EXPECT_EQ(observedModified, Spark::ProgressionSystem::MAX_SINGLE_AWARD);
    EXPECT_EQ(progression.GetCurrentXP(), Spark::ProgressionSystem::MAX_SINGLE_AWARD);
    EXPECT_EQ(progression.GetLevel(), progression.GetMaxLevel());

    // Awards stop at max level, so repetition cannot push the total toward INT_MAX either.
    progression.AwardXP(INT_MAX, "console");
    EXPECT_EQ(progression.GetCurrentXP(), Spark::ProgressionSystem::MAX_SINGLE_AWARD);

    // Ordinary gameplay awards are unchanged.
    Spark::ProgressionSystem ordinary;
    ordinary.Initialize();
    ordinary.AwardXP(Spark::ProgressionSystem::XP_PER_KILL, "kill");
    EXPECT_EQ(ordinary.GetCurrentXP(), Spark::ProgressionSystem::XP_PER_KILL);
}

TEST(SEC2GM_ProgressionIgnoresNonPositiveAwards)
{
    Spark::ProgressionSystem progression;
    progression.Initialize();
    progression.AwardXP(Spark::ProgressionSystem::XP_PER_KILL, "kill");

    progression.AwardXP(0, "console");
    progression.AwardXP(-500, "console");
    progression.AwardXP(INT_MIN, "console");
    EXPECT_EQ(progression.GetCurrentXP(), Spark::ProgressionSystem::XP_PER_KILL);
    EXPECT_EQ(progression.GetLevel(), 1);
}

// ---------------------------------------------------------------------------
// FPS wave composition (wave_skip / wave_difficulty console commands)
// ---------------------------------------------------------------------------

TEST(SEC2GM_WaveCompositionNeverExceedsCap)
{
    using namespace Spark::WaveComposition;
    const float scales[] = {
        1.0f, 3.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -5.0f, 1000.0f};
    std::vector<int> waves = {600000, INT_MAX, INT_MAX - 1, INT_MIN, -1, 0};
    for (int wave = 1; wave <= MAX_WAVE_NUMBER; ++wave)
        waves.push_back(wave);

    for (const int wave : waves)
    {
        for (const float scale : scales)
        {
            const Spark::WaveDefinition definition = Compose(wave, scale);
            const int total = definition.TotalEnemies();
            EXPECT_GT(total, 0);
            EXPECT_LE(total, MAX_ENEMIES_PER_WAVE);
            EXPECT_GE(definition.waveNumber, 1);
            EXPECT_LE(definition.waveNumber, MAX_WAVE_NUMBER);
            if (definition.isBossWave)
                EXPECT_GE(definition.heavyCount, 1);
            EXPECT_TRUE(std::isfinite(definition.healthMultiplier));
            EXPECT_TRUE(std::isfinite(definition.damageMultiplier));
            EXPECT_TRUE(std::isfinite(definition.speedMultiplier));
        }
    }
}

TEST(SEC2GM_WaveCompositionClampsInputs)
{
    using namespace Spark::WaveComposition;

    // wave_skip 600000 used to produce a boss wave of 120,001 unscaled heavies.
    const Spark::WaveDefinition huge = Compose(600000, 1.0f);
    EXPECT_EQ(huge.waveNumber, MAX_WAVE_NUMBER);
    EXPECT_LE(huge.heavyCount, MAX_ENEMIES_PER_WAVE);

    // Ordinary early waves are unchanged by the cap.
    const Spark::WaveDefinition first = Compose(1, 1.0f);
    EXPECT_EQ(first.gruntCount, 3);
    EXPECT_EQ(first.TotalEnemies(), 3);
    const Spark::WaveDefinition boss = Compose(5, 1.0f);
    EXPECT_TRUE(boss.isBossWave);
    EXPECT_EQ(boss.heavyCount, 2);

    EXPECT_EQ(ClampWaveNumber(INT_MIN), 1);
    EXPECT_EQ(ClampWaveNumber(INT_MAX), MAX_WAVE_NUMBER);
    EXPECT_TRUE(IsValidDifficultyScale(1.0f));
    EXPECT_TRUE(IsValidDifficultyScale(3.0f));
    EXPECT_FALSE(IsValidDifficultyScale(0.5f));
    EXPECT_FALSE(IsValidDifficultyScale(std::numeric_limits<float>::quiet_NaN()));
    EXPECT_FALSE(IsValidDifficultyScale(std::numeric_limits<float>::infinity()));
    EXPECT_EQ(SanitizeDifficultyScale(std::numeric_limits<float>::quiet_NaN()), 1.0f);
    EXPECT_EQ(SanitizeDifficultyScale(50.0f), MAX_DIFFICULTY_SCALE);
}

// ---------------------------------------------------------------------------
// NetworkManager handler ownership across module hot reload
// ---------------------------------------------------------------------------

#ifdef ENABLE_NETWORKING

#include "Engine/Networking/NetworkManager.h"

namespace
{
    using SEC2NetworkManager = Spark::Net::NetworkManager;

    Spark::Net::MessageType SEC2TestMessageType(uint16_t offset)
    {
        return static_cast<Spark::Net::MessageType>(
            static_cast<uint16_t>(static_cast<uint16_t>(Spark::Net::MessageType::UserDefined) + 200u + offset));
    }

    /// A handler that keeps @p token alive exactly as long as NetworkManager keeps the handler.
    SEC2NetworkManager::MessageHandler SEC2TokenHandler(std::shared_ptr<int> token)
    {
        return [held = std::move(token)](const Spark::Net::NetworkMessage&) { (void)held; };
    }
} // namespace

TEST(SEC2GM_NetworkHotReloadKeepsReplacementHandlers)
{
    auto& network = SEC2NetworkManager::GetInstance();
    const Spark::Net::MessageType type = SEC2TestMessageType(1);
    const std::string outgoingOwner = "SEC2GM_Outgoing#1";
    const std::string replacementOwner = "SEC2GM_Replacement#2";

    auto outgoingToken = std::make_shared<int>(1);
    const std::weak_ptr<int> outgoingAlive = outgoingToken;
    {
        SEC2NetworkManager::ScopedRegistrationOwner scope(network, outgoingOwner);
        network.RegisterHandler(type, SEC2TokenHandler(std::move(outgoingToken)));
    }
    EXPECT_FALSE(outgoingAlive.expired());

    // ModuleManager::ReloadModule initializes the replacement first; its handler takes over the slot,
    // destroying the outgoing callback while the outgoing image is still mapped.
    auto replacementToken = std::make_shared<int>(2);
    const std::weak_ptr<int> replacementAlive = replacementToken;
    {
        SEC2NetworkManager::ScopedRegistrationOwner scope(network, replacementOwner);
        network.RegisterHandler(type, SEC2TokenHandler(std::move(replacementToken)));
    }
    EXPECT_TRUE(outgoingAlive.expired());
    EXPECT_FALSE(replacementAlive.expired());

    // Then the outgoing image tears down. The old MMO code overwrote the slot with an empty lambda compiled
    // into the outgoing image; none of these writes may touch the replacement's handler.
    {
        SEC2NetworkManager::ScopedRegistrationOwner scope(network, outgoingOwner, true);
        network.RegisterHandler(type, [](const Spark::Net::NetworkMessage&) {});
        network.RegisterSensitiveHandler(type, [](const Spark::Net::NetworkMessage&) {});
        network.UnregisterHandler(type);
        network.ClearHandlers();
    }
    EXPECT_FALSE(replacementAlive.expired());
    EXPECT_EQ(network.UnregisterHandlersByOwner(outgoingOwner), static_cast<size_t>(0));
    EXPECT_FALSE(replacementAlive.expired());

    // Unloading the replacement removes what it owns.
    EXPECT_EQ(network.UnregisterHandlersByOwner(replacementOwner), static_cast<size_t>(1));
    EXPECT_TRUE(replacementAlive.expired());
}

TEST(SEC2GM_NetworkUnloadRemovesEveryOwnedCallback)
{
    auto& network = SEC2NetworkManager::GetInstance();
    const Spark::Net::MessageType hostType = SEC2TestMessageType(2);
    const Spark::Net::MessageType moduleType = SEC2TestMessageType(3);
    const Spark::Net::MessageType teardownType = SEC2TestMessageType(4);
    const std::string moduleOwner = "SEC2GM_Module#3";

    auto hostToken = std::make_shared<int>(1);
    const std::weak_ptr<int> hostAlive = hostToken;
    network.RegisterHandler(hostType, SEC2TokenHandler(std::move(hostToken)));

    auto moduleToken = std::make_shared<int>(2);
    const std::weak_ptr<int> moduleAlive = moduleToken;
    auto timeoutToken = std::make_shared<int>(3);
    const std::weak_ptr<int> timeoutAlive = timeoutToken;
    {
        SEC2NetworkManager::ScopedRegistrationOwner scope(network, moduleOwner);
        network.RegisterHandler(moduleType, SEC2TokenHandler(std::move(moduleToken)));
        network.SetTimeoutHandler([held = std::move(timeoutToken)](Spark::Net::ClientID) { (void)held; });
    }

    // A teardown that installs its own placeholder (the old pattern) still produces an owned slot.
    auto teardownToken = std::make_shared<int>(4);
    const std::weak_ptr<int> teardownAlive = teardownToken;
    {
        SEC2NetworkManager::ScopedRegistrationOwner scope(network, moduleOwner, true);
        network.RegisterHandler(teardownType, SEC2TokenHandler(std::move(teardownToken)));
    }

    // Before the image is unmapped every callback it owns is destroyed; host handlers survive.
    EXPECT_EQ(network.UnregisterHandlersByOwner(moduleOwner), static_cast<size_t>(3));
    EXPECT_TRUE(moduleAlive.expired());
    EXPECT_TRUE(timeoutAlive.expired());
    EXPECT_TRUE(teardownAlive.expired());
    EXPECT_FALSE(hostAlive.expired());

    // Host code outside any scope keeps unrestricted removal.
    network.UnregisterHandler(hostType);
    EXPECT_TRUE(hostAlive.expired());
}

TEST(SEC2GM_NetworkTeardownRemovesUnownedLazyHandler)
{
    auto& network = SEC2NetworkManager::GetInstance();
    const Spark::Net::MessageType type = SEC2TestMessageType(5);
    const std::string moduleOwner = "SEC2GM_LazyModule#6";

    // SparkGameMMOFPS registers its observers lazily from Update once the link is up, outside any
    // ModuleRegistrationScope. They carry no owner, so ModuleManager's UnregisterHandlersByOwner safety net
    // cannot reclaim them before the image is unmapped: the module must remove them itself.
    auto liveToken = std::make_shared<int>(1);
    const std::weak_ptr<int> liveAlive = liveToken;
    network.RegisterHandler(type, SEC2TokenHandler(std::move(liveToken)));
    EXPECT_EQ(network.UnregisterHandlersByOwner(moduleOwner), static_cast<size_t>(0));
    EXPECT_FALSE(liveAlive.expired());

    // The old release on link loss (also outside any scope) swapped in a placeholder compiled into the module
    // image: it destroyed the live callback but left an unowned, image-resident one behind that nothing reclaims
    // at unload, and the engine-shutdown ClearHandlers later destroys it through unmapped code.
    auto placeholderToken = std::make_shared<int>(2);
    const std::weak_ptr<int> placeholderAlive = placeholderToken;
    network.RegisterHandler(type, SEC2TokenHandler(std::move(placeholderToken)));
    EXPECT_TRUE(liveAlive.expired());
    EXPECT_EQ(network.UnregisterHandlersByOwner(moduleOwner), static_cast<size_t>(0));
    EXPECT_FALSE(placeholderAlive.expired());

    // UnregisterHandler removes the slot and destroys its callback at once, outside any scope (link loss)...
    network.UnregisterHandler(type);
    EXPECT_TRUE(placeholderAlive.expired());

    // ...and inside the module's teardown scope (OnUnload -> Shutdown), where an unowned slot may be removed...
    auto lazyToken = std::make_shared<int>(3);
    const std::weak_ptr<int> lazyAlive = lazyToken;
    network.RegisterHandler(type, SEC2TokenHandler(std::move(lazyToken)));
    {
        SEC2NetworkManager::ScopedRegistrationOwner scope(network, moduleOwner, true);
        network.UnregisterHandler(type);
    }
    EXPECT_TRUE(lazyAlive.expired());

    // ...while a slot a hot-reload replacement installed during its OnLoad stays with the replacement.
    const std::string replacementOwner = "SEC2GM_LazyReplacement#7";
    auto replacementToken = std::make_shared<int>(4);
    const std::weak_ptr<int> replacementAlive = replacementToken;
    {
        SEC2NetworkManager::ScopedRegistrationOwner scope(network, replacementOwner);
        network.RegisterHandler(type, SEC2TokenHandler(std::move(replacementToken)));
    }
    {
        SEC2NetworkManager::ScopedRegistrationOwner scope(network, moduleOwner, true);
        network.UnregisterHandler(type);
    }
    EXPECT_FALSE(replacementAlive.expired());
    EXPECT_EQ(network.UnregisterHandlersByOwner(replacementOwner), static_cast<size_t>(1));
    EXPECT_TRUE(replacementAlive.expired());
}

#endif // ENABLE_NETWORKING

// ---------------------------------------------------------------------------
// Game-module network handler teardown (source contract)
// ---------------------------------------------------------------------------
//
// SparkGameMMOFPS registers its NetworkManager observers lazily (outside any module registration scope), so
// ModuleManager cannot reclaim them and every one must be removed by the module itself. These tests read the
// module sources: no module may "clear" a handler by installing an empty lambda compiled into its own image,
// and every MMOFPS *Handlers registration function has a release counterpart that UnregisterHandler()s every
// message id it registered.

namespace
{
    bool SEC2IsIdentChar(char c)
    {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
    }

    /// True when the quote at @p at is a C++14 digit separator (1'000'000) rather than a character literal.
    bool SEC2IsDigitSeparator(const std::string& code, size_t at)
    {
        size_t tokenStart = at;
        while (tokenStart > 0 && (SEC2IsIdentChar(code[tokenStart - 1]) || code[tokenStart - 1] == '\''))
            --tokenStart;
        return tokenStart < at && std::isdigit(static_cast<unsigned char>(code[tokenStart])) != 0 &&
               at + 1 < code.size() && SEC2IsIdentChar(code[at + 1]);
    }

    /// @p text with comments removed and string/character literal contents blanked, offsets preserved, so brace
    /// matching and identifier scans see only code.
    std::string SEC2CodeOnly(std::string code)
    {
        size_t i = 0;
        while (i < code.size())
        {
            const char c = code[i];
            const char next = i + 1 < code.size() ? code[i + 1] : '\0';
            if (c == '/' && next == '/')
            {
                while (i < code.size() && code[i] != '\n')
                    code[i++] = ' ';
            }
            else if (c == '/' && next == '*')
            {
                while (i < code.size() && !(code[i] == '*' && i + 1 < code.size() && code[i + 1] == '/'))
                {
                    if (code[i] != '\n')
                        code[i] = ' ';
                    ++i;
                }
                for (int k = 0; k < 2 && i < code.size(); ++k)
                    code[i++] = ' ';
            }
            else if (c == '"' || (c == '\'' && !SEC2IsDigitSeparator(code, i)))
            {
                ++i;
                while (i < code.size() && code[i] != c && code[i] != '\n')
                {
                    if (code[i] == '\\' && i + 1 < code.size())
                        code[i++] = ' ';
                    code[i++] = ' ';
                }
                ++i;
            }
            else
            {
                ++i;
            }
        }
        return code;
    }

    size_t SEC2SkipSpace(const std::string& code, size_t at)
    {
        while (at < code.size() && std::isspace(static_cast<unsigned char>(code[at])) != 0)
            ++at;
        return at;
    }

    /// Index one past the bracket matching the opener at @p open, or npos when unbalanced.
    size_t SEC2MatchBracket(const std::string& code, size_t open, char opener, char closer)
    {
        int depth = 0;
        for (size_t i = open; i < code.size(); ++i)
        {
            if (code[i] == opener)
                ++depth;
            else if (code[i] == closer && --depth == 0)
                return i + 1;
        }
        return std::string::npos;
    }

    struct SEC2MemberFunction
    {
        std::string owner;
        std::string name;
        size_t bodyBegin = 0;
        size_t bodyEnd = 0;
    };

    /// Every out-of-line `void Owner::Name(...) [const] { ... }` definition in comment-free @p code.
    std::vector<SEC2MemberFunction> SEC2FindMemberFunctions(const std::string& code)
    {
        std::vector<SEC2MemberFunction> functions;
        const auto readIdent = [&code](size_t& at)
        {
            const size_t start = at;
            while (at < code.size() && SEC2IsIdentChar(code[at]))
                ++at;
            return code.substr(start, at - start);
        };
        for (size_t pos = code.find("void"); pos != std::string::npos; pos = code.find("void", pos + 4))
        {
            if ((pos > 0 && SEC2IsIdentChar(code[pos - 1])) || pos + 4 >= code.size() || SEC2IsIdentChar(code[pos + 4]))
                continue;
            size_t at = SEC2SkipSpace(code, pos + 4);
            SEC2MemberFunction function;
            function.owner = readIdent(at);
            if (function.owner.empty() || code.compare(at, 2, "::") != 0)
                continue;
            at += 2;
            function.name = readIdent(at);
            at = SEC2SkipSpace(code, at);
            if (function.name.empty() || at >= code.size() || code[at] != '(')
                continue;
            at = SEC2MatchBracket(code, at, '(', ')');
            if (at == std::string::npos)
                continue;
            at = SEC2SkipSpace(code, at);
            if (code.compare(at, 5, "const") == 0)
                at = SEC2SkipSpace(code, at + 5);
            if (at >= code.size() || code[at] != '{')
                continue;
            function.bodyBegin = at;
            function.bodyEnd = SEC2MatchBracket(code, at, '{', '}');
            if (function.bodyEnd == std::string::npos)
                continue;
            functions.push_back(function);
        }
        return functions;
    }

    /// Offsets of every capture-less, empty-bodied NetworkMessage lambda: `[](const ...NetworkMessage& ...) {}`.
    std::vector<size_t> SEC2FindPlaceholderHandlers(const std::string& code)
    {
        std::vector<size_t> found;
        for (size_t pos = code.find('['); pos != std::string::npos; pos = code.find('[', pos + 1))
        {
            size_t at = SEC2SkipSpace(code, pos + 1);
            if (at >= code.size() || code[at] != ']')
                continue;
            at = SEC2SkipSpace(code, at + 1);
            if (at >= code.size() || code[at] != '(')
                continue;
            const size_t paramsEnd = SEC2MatchBracket(code, at, '(', ')');
            if (paramsEnd == std::string::npos)
                continue;
            const std::string params = code.substr(at, paramsEnd - at);
            if (params.find("NetworkMessage") == std::string::npos || params.find('&') == std::string::npos)
                continue;
            at = SEC2SkipSpace(code, paramsEnd);
            if (at >= code.size() || code[at] != '{')
                continue;
            at = SEC2SkipSpace(code, at + 1);
            if (at < code.size() && code[at] == '}')
                found.push_back(pos);
        }
        return found;
    }

    /// Whether @p code calls @p function (whole identifier: "RegisterHandler" does not match "UnregisterHandler").
    bool SEC2CallsFunction(const std::string& code, const std::string& function)
    {
        for (size_t pos = code.find(function); pos != std::string::npos; pos = code.find(function, pos + 1))
        {
            if (pos > 0 && SEC2IsIdentChar(code[pos - 1]))
                continue;
            const size_t at = SEC2SkipSpace(code, pos + function.size());
            if (at < code.size() && code[at] == '(')
                return true;
        }
        return false;
    }

    bool SEC2RegistersHandler(const std::string& code)
    {
        return SEC2CallsFunction(code, "RegisterHandler") || SEC2CallsFunction(code, "RegisterSensitiveHandler") ||
               SEC2CallsFunction(code, "SetTimeoutHandler");
    }

    /// TERRAFRONT message-id names in @p code: kTF...Msg... constants and lists, and TFMsg::X enumerators.
    std::set<std::string> SEC2MessageIds(const std::string& code)
    {
        std::set<std::string> ids;
        size_t at = 0;
        while (at < code.size())
        {
            if (!SEC2IsIdentChar(code[at]) || (at > 0 && SEC2IsIdentChar(code[at - 1])))
            {
                ++at;
                continue;
            }
            const size_t start = at;
            while (at < code.size() && SEC2IsIdentChar(code[at]))
                ++at;
            std::string ident = code.substr(start, at - start);
            if (ident == "TFMsg" && code.compare(at, 2, "::") == 0)
            {
                size_t member = at + 2;
                while (member < code.size() && SEC2IsIdentChar(code[member]))
                    ++member;
                ids.insert(code.substr(start, member - start));
                at = member;
            }
            else if (ident.rfind("kTF", 0) == 0 && ident.find("Msg") != std::string::npos)
            {
                ids.insert(ident);
            }
        }
        return ids;
    }

    /// Pairs "EnsureClientHandlers" with "ReleaseClientHandlers", "RegisterNetHandlers" with
    /// "UnregisterNetHandlers", "ClientEnsureHandlers" with "ClientReleaseHandlers", and so on.
    std::string SEC2HandlerLifecycleKey(const SEC2MemberFunction& function)
    {
        std::string key = function.name;
        for (const char* verb : {"Unregister", "Register", "Ensure", "Release"})
        {
            const size_t found = key.find(verb);
            if (found != std::string::npos)
                key.erase(found, std::char_traits<char>::length(verb));
        }
        return function.owner + "::" + key;
    }

    bool SEC2IsReleaseFunction(const std::string& name)
    {
        return name.find("Release") != std::string::npos || name.find("Unregister") != std::string::npos;
    }

    std::string SEC2ReadSource(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        std::ostringstream contents;
        contents << file.rdbuf();
        return contents.str();
    }

    std::vector<std::filesystem::path> SEC2SourceFiles(const std::filesystem::path& root)
    {
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        {
            const std::filesystem::path& path = it->path();
            const std::string extension = path.extension().string();
            if (it->is_regular_file(ec) && (extension == ".cpp" || extension == ".h"))
                files.push_back(path);
        }
        std::sort(files.begin(), files.end());
        return files;
    }
} // namespace

TEST(SEC2GM_ModuleNetworkTeardownNeverInstallsPlaceholderHandlers)
{
    // The scanner must recognize the pattern it bans, or an empty result would prove nothing.
    const std::string oldTeardown = SEC2CodeOnly("void TFPingSystem::ReleaseClientHandlers()\n{\n"
                                                 "    nm.RegisterHandler(static_cast<MessageType>(kTFMsgPingState),\n"
                                                 "                       [](const Spark::Net::NetworkMessage&) {});\n"
                                                 "    // [](const NetworkMessage&) {} in a comment is not code\n}\n");
    ASSERT_EQ(SEC2FindPlaceholderHandlers(oldTeardown).size(), static_cast<size_t>(1));
    const std::vector<SEC2MemberFunction> oldFunctions = SEC2FindMemberFunctions(oldTeardown);
    ASSERT_EQ(oldFunctions.size(), static_cast<size_t>(1));
    EXPECT_EQ(oldFunctions.front().name, std::string("ReleaseClientHandlers"));

    const std::filesystem::path modules = std::filesystem::path(SPARK_TEST_SOURCE_DIR) / "GameModules";
    const std::vector<std::filesystem::path> files = SEC2SourceFiles(modules);
    ASSERT_TRUE(files.size() > 100u);

    // An empty handler is legitimate only as a live accept-and-ignore route installed by a registration
    // function (and removed again by its release counterpart). Anywhere else it is a teardown placeholder.
    size_t violations = 0;
    for (const std::filesystem::path& path : files)
    {
        const std::string code = SEC2CodeOnly(SEC2ReadSource(path));
        const std::vector<SEC2MemberFunction> functions = SEC2FindMemberFunctions(code);
        for (const size_t placeholder : SEC2FindPlaceholderHandlers(code))
        {
            const SEC2MemberFunction* enclosing = nullptr;
            for (const SEC2MemberFunction& function : functions)
            {
                if (placeholder > function.bodyBegin && placeholder < function.bodyEnd)
                    enclosing = &function;
            }
            const bool registration = enclosing && !SEC2IsReleaseFunction(enclosing->name) &&
                                      (enclosing->name.find("Register") != std::string::npos ||
                                       enclosing->name.find("Ensure") != std::string::npos);
            if (!registration)
            {
                ++violations;
                std::cerr << "  placeholder network handler in " << path.generic_string() << " ("
                          << (enclosing ? enclosing->owner + "::" + enclosing->name : std::string("<unknown>"))
                          << "): remove the handler with NetworkManager::UnregisterHandler instead\n";
            }
        }
    }
    EXPECT_EQ(violations, static_cast<size_t>(0));
}

TEST(SEC2GM_MMOFPSReleasesEveryNetworkHandlerItRegisters)
{
    const std::filesystem::path source =
        std::filesystem::path(SPARK_TEST_SOURCE_DIR) / "GameModules" / "SparkGameMMOFPS" / "Source";
    const std::vector<std::filesystem::path> files = SEC2SourceFiles(source);
    ASSERT_TRUE(files.size() > 100u);

    size_t pairs = 0;
    for (const std::filesystem::path& path : files)
    {
        const std::string code = SEC2CodeOnly(SEC2ReadSource(path));
        std::map<std::string, SEC2MemberFunction> registrations;
        std::map<std::string, SEC2MemberFunction> releases;
        for (const SEC2MemberFunction& function : SEC2FindMemberFunctions(code))
        {
            const std::string_view name = function.name;
            if (!name.ends_with("Handlers"))
                continue;
            const std::string body = code.substr(function.bodyBegin, function.bodyEnd - function.bodyBegin);
            if (SEC2IsReleaseFunction(function.name))
                releases[SEC2HandlerLifecycleKey(function)] = function;
            else if (SEC2RegistersHandler(body))
                registrations[SEC2HandlerLifecycleKey(function)] = function;
        }

        for (const auto& [key, registration] : registrations)
        {
            const auto release = releases.find(key);
            if (release == releases.end())
            {
                std::cerr << "  " << path.generic_string() << ": " << registration.owner << "::" << registration.name
                          << " has no release counterpart in the same file\n";
                EXPECT_TRUE(release != releases.end());
                continue;
            }
            ++pairs;
            const std::string registerBody =
                code.substr(registration.bodyBegin, registration.bodyEnd - registration.bodyBegin);
            const std::string releaseBody =
                code.substr(release->second.bodyBegin, release->second.bodyEnd - release->second.bodyBegin);

            // Release removes; it never installs a replacement callback of its own.
            const bool removes = SEC2CallsFunction(releaseBody, "UnregisterHandler");
            const bool reinstalls = SEC2RegistersHandler(releaseBody);
            if (!removes || reinstalls)
            {
                std::cerr << "  " << path.generic_string() << ": " << release->second.owner
                          << "::" << release->second.name << " must UnregisterHandler, not register a replacement\n";
            }
            EXPECT_TRUE(removes);
            EXPECT_FALSE(reinstalls);

            // Every message id the registration names is released again.
            const std::set<std::string> releasedIds = SEC2MessageIds(releaseBody);
            for (const std::string& id : SEC2MessageIds(registerBody))
            {
                if (!releasedIds.contains(id))
                {
                    std::cerr << "  " << path.generic_string() << ": " << registration.name << " registers " << id
                              << " but " << release->second.name << " never removes it\n";
                    EXPECT_TRUE(releasedIds.contains(id));
                }
            }
        }
    }

    // 24 registration/release pairs exist today; a scanner that stopped finding them must not pass silently.
    EXPECT_GE(pairs, static_cast<size_t>(24));
}

// ---------------------------------------------------------------------------
// Module sources compiled only with ImGui (RTS fog of war, MMO chat)
// ---------------------------------------------------------------------------

#ifdef SPARK_TEST_HAS_IMGUI

#include "../GameModules/SparkGameRTS/Source/Building/RTSBuildingSystem.h"
#include "../GameModules/SparkGameRTS/Source/Command/RTSCommandSystem.h"
#include "../GameModules/SparkGameRTS/Source/Core/RTSPersistence.h"
#include "../GameModules/SparkGameRTS/Source/FogOfWar/RTSFogOfWarSystem.h"
#include "../GameModules/SparkGameRTS/Source/Match/RTSMatchSystem.h"
#include "../GameModules/SparkGameRTS/Source/Resource/RTSResourceSystem.h"
#include "../GameModules/SparkGameRTS/Source/Simulation/RTSSkirmishSimulation.h"
#include "../GameModules/SparkGameRTS/Source/Unit/RTSUnitSystem.h"

TEST(SEC2GM_RTSFogVisionIsClippedToTheGrid)
{
    RTS::RTSFogOfWarSystem fog;
    ASSERT_TRUE(fog.Initialize(nullptr, 128, 128));

    // A restored visionRange of 1e9 used to mean ~4e18 loop iterations (and UB in the int arithmetic);
    // clipped to the grid it is at most 128 * 128 cells and reveals the whole map.
    fog.UpdateVision(RTS::RTSFaction::Human, 64.0f, 64.0f, 1.0e9f);
    EXPECT_EQ(fog.GetExploredPercent(RTS::RTSFaction::Human), 100.0f);
    fog.HideArea(RTS::RTSFaction::Human, 64.0f, 64.0f, std::numeric_limits<float>::max());
    EXPECT_FALSE(fog.IsVisible(RTS::RTSFaction::Human, 10.0f, 10.0f));

    // Non-finite ranges and positions far off the grid reveal nothing and are defined behaviour.
    fog.UpdateVision(RTS::RTSFaction::Sentinel, 64.0f, 64.0f, std::numeric_limits<float>::quiet_NaN());
    fog.UpdateVision(RTS::RTSFaction::Sentinel, 64.0f, 64.0f, std::numeric_limits<float>::infinity());
    fog.UpdateVision(RTS::RTSFaction::Sentinel, 64.0f, 64.0f, -3.0f);
    fog.UpdateVision(RTS::RTSFaction::Sentinel, std::numeric_limits<float>::quiet_NaN(), 64.0f, 5.0f);
    fog.UpdateVision(RTS::RTSFaction::Sentinel, 1.0e30f, -1.0e30f, 5.0f);
    EXPECT_EQ(fog.GetExploredPercent(RTS::RTSFaction::Sentinel), 0.0f);

    // An ordinary unit still reveals exactly its disc.
    fog.UpdateVision(RTS::RTSFaction::Swarm, 10.0f, 10.0f, 8.0f);
    EXPECT_TRUE(fog.IsVisible(RTS::RTSFaction::Swarm, 18.0f, 10.0f));
    EXPECT_FALSE(fog.IsVisible(RTS::RTSFaction::Swarm, 19.0f, 10.0f));
    EXPECT_FALSE(fog.IsVisible(RTS::RTSFaction::Swarm, 17.0f, 17.0f));

    // Grids larger than the persisted bound are refused up front.
    RTS::RTSFogOfWarSystem oversized;
    EXPECT_FALSE(oversized.Initialize(nullptr, RTS::RTSFogOfWarSystem::MAX_MAP_DIMENSION + 1, 16));
}

TEST(SEC2GM_RTSRestoreRejectsUnboundedVisionRange)
{
    // The bound is a gameplay range (templates use 7-12), not the map size: a map-sized 1024 is refused.
    EXPECT_TRUE(RTS::RTSUnitSystem::MAX_VISION_RANGE >= 12.0f);
    EXPECT_TRUE(RTS::RTSUnitSystem::MAX_VISION_RANGE <= 64.0f);

    RTS::UnitData unit;
    unit.unitId = 1;
    RTS::RTSUnitSystem units;
    for (const float range : {10000.0f, 1024.0f, RTS::RTSUnitSystem::MAX_VISION_RANGE + 1.0f})
    {
        unit.visionRange = range;
        EXPECT_FALSE(units.RestoreState({unit}, 2));
    }
    unit.visionRange = RTS::RTSUnitSystem::MAX_VISION_RANGE;
    EXPECT_TRUE(units.RestoreState({unit}, 2));

    // The save decoder's validator applies the same bound to a captured snapshot.
    RTS::RTSUnitSystem liveUnits;
    RTS::RTSBuildingSystem buildings;
    RTS::RTSResourceSystem resources;
    RTS::RTSCommandSystem commands;
    RTS::RTSFogOfWarSystem fog;
    RTS::RTSMatchSystem match;
    RTS::RTSSkirmishSimulation simulation;
    const RTS::RTSSkirmishSystems systems{&liveUnits, &buildings, &resources, &commands, &fog, &match};
    simulation.Initialize(nullptr, systems);
    simulation.StartDefaultSkirmish();

    RTS::RTSPersistenceSnapshot snapshot = RTS::RTSPersistence::Capture(systems, simulation);
    ASSERT_FALSE(snapshot.units.empty());
    std::string error;
    EXPECT_TRUE(RTS::RTSPersistence::Validate(snapshot, error));
    snapshot.units.front().visionRange = 1024.0f;
    EXPECT_FALSE(RTS::RTSPersistence::Validate(snapshot, error));
    EXPECT_FALSE(error.empty());
    snapshot.units.front().visionRange = RTS::RTSUnitSystem::MAX_VISION_RANGE;
    EXPECT_TRUE(RTS::RTSPersistence::Validate(snapshot, error));
}

TEST(SEC2GM_RTSVisionRefreshWorkIsBoundedAtRecordCap)
{
    RTS::RTSUnitSystem units;
    RTS::RTSBuildingSystem buildings;
    RTS::RTSResourceSystem resources;
    RTS::RTSCommandSystem commands;
    RTS::RTSFogOfWarSystem fog;
    RTS::RTSMatchSystem match;
    RTS::RTSSkirmishSimulation simulation;
    const RTS::RTSSkirmishSystems systems{&units, &buildings, &resources, &commands, &fog, &match};
    ASSERT_TRUE(simulation.Initialize(nullptr, systems));
    ASSERT_TRUE(simulation.StartDefaultSkirmish());

    constexpr size_t factionCount = static_cast<size_t>(RTS::RTSFaction::Count);
    constexpr size_t refreshCap = factionCount * RTS::RTSFogOfWarSystem::MAX_VISION_CELLS_PER_REFRESH;

    // A legitimate skirmish is far below the cap, so every unit still reveals its own cell.
    simulation.Step();
    EXPECT_TRUE(simulation.GetLastVisionCellWork() > 0);
    EXPECT_TRUE(simulation.GetLastVisionCellWork() < RTS::RTSFogOfWarSystem::MAX_VISION_CELLS_PER_REFRESH);
    for (const RTS::RTSFaction faction : {RTS::RTSFaction::Human, RTS::RTSFaction::Swarm})
    {
        for (const uint32_t unitId : units.GetUnitsByFaction(faction))
        {
            const RTS::UnitData* unit = units.GetUnit(unitId);
            ASSERT_TRUE(unit != nullptr);
            EXPECT_TRUE(fog.IsVisible(faction, unit->posX, unit->posY));
        }
    }

    // A crafted save that passes validation: the widest fog grid, MAX_RECORDS units, every one at the maximum
    // vision range and spread so their discs are not clipped by the grid edge.
    RTS::RTSPersistenceSnapshot snapshot = RTS::RTSPersistence::Capture(systems, simulation);
    ASSERT_FALSE(snapshot.units.empty());
    constexpr int dimension = RTS::RTSFogOfWarSystem::MAX_MAP_DIMENSION;
    for (RTS::FogGrid& grid : snapshot.fog)
    {
        grid.width = dimension;
        grid.height = dimension;
        grid.cells.assign(static_cast<size_t>(dimension) * static_cast<size_t>(dimension),
                          RTS::RTSVisibility::Unexplored);
    }
    for (RTS::UnitData& existing : snapshot.units)
    {
        existing.visionRange = RTS::RTSUnitSystem::MAX_VISION_RANGE;
    }
    uint32_t nextId = snapshot.nextUnitId;
    for (size_t index = 0; snapshot.units.size() < RTS::RTSPersistence::MAX_RECORDS; ++index)
    {
        RTS::UnitData unit;
        unit.unitId = nextId++;
        unit.type = RTS::RTSUnitType::Scout;
        unit.faction = (index % 2 == 0) ? RTS::RTSFaction::Human : RTS::RTSFaction::Swarm;
        unit.damage = 0.0f;
        unit.attackSpeed = 0.0f;
        unit.visionRange = RTS::RTSUnitSystem::MAX_VISION_RANGE;
        unit.posX = 70.0f + static_cast<float>(index % 96) * 9.0f;
        unit.posY = 70.0f + static_cast<float>(index / 96) * 8.0f;
        snapshot.units.push_back(unit);
    }
    snapshot.nextUnitId = nextId;
    snapshot.tick = 1; // keep the AI's once-a-second decision pass out of the measured tick
    std::string error;
    ASSERT_TRUE(RTS::RTSPersistence::Apply(snapshot, systems, simulation, error));
    ASSERT_EQ(units.GetUnitCount(), RTS::RTSPersistence::MAX_RECORDS);

    // Without the per-refresh cap this one tick would visit every unit's full disc rectangle.
    size_t uncappedWork = 0;
    for (const RTS::UnitData& unit : snapshot.units)
    {
        uncappedWork += fog.VisionCellCost(unit.faction, unit.posX, unit.posY, unit.visionRange);
    }
    EXPECT_TRUE(uncappedWork > 20 * refreshCap);

    simulation.Step();
    EXPECT_TRUE(simulation.GetLastVisionCellWork() <= refreshCap);
    EXPECT_TRUE(simulation.GetLastVisionCellWork() > 0);

    // Units are revealed in ascending id order, so the lowest-id unit of each faction still sees.
    for (const RTS::RTSFaction faction : {RTS::RTSFaction::Human, RTS::RTSFaction::Swarm})
    {
        const std::vector<uint32_t> ids = units.GetUnitsByFaction(faction);
        ASSERT_FALSE(ids.empty());
        const RTS::UnitData* first = units.GetUnit(ids.front());
        ASSERT_TRUE(first != nullptr);
        EXPECT_TRUE(fog.IsVisible(faction, first->posX, first->posY));
    }
}

#ifdef ENABLE_NETWORKING

#include "../GameModules/SparkGameMMO/Source/Chat/MMOChatSystem.h"
#include "Spark/IEngineContext.h"

namespace
{
    /// Context exposing only the engine NetworkManager, as the MMO module sees it.
    class SEC2NetworkContext final : public Spark::IEngineContext
    {
      public:
        GraphicsEngine* GetGraphics() override { return nullptr; }
        const GraphicsEngine* GetGraphics() const override { return nullptr; }
        InputManager* GetInput() override { return nullptr; }
        const InputManager* GetInput() const override { return nullptr; }
        Timer* GetTimer() override { return nullptr; }
        const Timer* GetTimer() const override { return nullptr; }
        Spark::EventBus* GetEventBus() override { return nullptr; }
        const Spark::EventBus* GetEventBus() const override { return nullptr; }
        ::AudioEngine* GetAudio() override { return nullptr; }
        const ::AudioEngine* GetAudio() const override { return nullptr; }
        PhysicsSystem* GetPhysics() override { return nullptr; }
        const PhysicsSystem* GetPhysics() const override { return nullptr; }
        Spark::Net::NetworkManager* GetNetwork() override { return &Spark::Net::NetworkManager::GetInstance(); }
        uint32_t GetEngineVersion() const override { return 0; }
        uint32_t GetSDKVersion() const override { return 0; }
    };
} // namespace

TEST(SEC2GM_MMOChatRelayUsesServerIdentityAndDropsPrivateChannels)
{
    using MMO::ChatChannel;
    using MMO::MMOChatSystem;

    // The payload's own sender field is ignored: the relay carries the connection's attributed name.
    const std::vector<uint8_t> spoofed = MMOChatSystem::EncodeWirePayload(ChatChannel::Global, "System", "hi all");
    const auto relayed = MMOChatSystem::BuildServerRelayPayload(spoofed, "alice", 3);
    ASSERT_TRUE(relayed.has_value());
    const auto decoded = MMOChatSystem::DecodeWirePayload(*relayed);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_TRUE(decoded->channel == ChatChannel::Global);
    EXPECT_EQ(decoded->senderName, std::string("alice#3"));
    EXPECT_EQ(decoded->text, std::string("hi all"));

    const auto area = MMOChatSystem::EncodeWirePayload(ChatChannel::Area, "alice", "nearby");
    EXPECT_TRUE(MMOChatSystem::BuildServerRelayPayload(area, "alice", 3).has_value());

    // Party and Whisper have no recipient on the wire; relaying them meant broadcasting them to everyone.
    const auto party = MMOChatSystem::EncodeWirePayload(ChatChannel::Party, "alice", "party secret");
    const auto whisper = MMOChatSystem::EncodeWirePayload(ChatChannel::Whisper, "alice", "for bob only");
    EXPECT_FALSE(MMOChatSystem::BuildServerRelayPayload(party, "alice", 3).has_value());
    EXPECT_FALSE(MMOChatSystem::BuildServerRelayPayload(whisper, "alice", 3).has_value());

    // Malformed packets and the invalid client id are dropped.
    std::vector<uint8_t> badChannel = spoofed;
    badChannel[0] = 9;
    EXPECT_FALSE(MMOChatSystem::BuildServerRelayPayload(badChannel, "alice", 3).has_value());
    EXPECT_FALSE(MMOChatSystem::BuildServerRelayPayload({1}, "alice", 3).has_value());
    EXPECT_FALSE(MMOChatSystem::BuildServerRelayPayload(spoofed, "alice", Spark::Net::INVALID_CLIENT).has_value());
}

TEST(SEC2GM_MMOChatConnectionNameCannotForgeIdentity)
{
    using MMO::ChatChannel;
    using MMO::MMOChatSystem;

    // The connect-request playerName is client-chosen and unauthenticated (NetworkConnection keeps it as
    // ClientInfo::name). A connection that names itself "System" still cannot produce a "System" sender.
    const std::vector<uint8_t> line = MMOChatSystem::EncodeWirePayload(ChatChannel::Global, "x", "server restart");
    const auto relayed = MMOChatSystem::BuildServerRelayPayload(line, "System", 7);
    ASSERT_TRUE(relayed.has_value());
    const auto decoded = MMOChatSystem::DecodeWirePayload(*relayed);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_TRUE(decoded->senderName != std::string("System"));
    EXPECT_EQ(decoded->senderName, std::string("System#7"));

    // Two connections claiming the same name stay distinguishable, and a name that embeds another
    // player's suffix cannot reproduce that player's attributed name.
    EXPECT_EQ(MMOChatSystem::ServerAttributedSenderName("alice", 3), std::string("alice#3"));
    EXPECT_EQ(MMOChatSystem::ServerAttributedSenderName("alice", 9), std::string("alice#9"));
    EXPECT_EQ(MMOChatSystem::ServerAttributedSenderName("alice#3", 9), std::string("alice3#9"));

    // Control characters (log/UI line injection), non-ASCII bytes (bidi overrides) and oversized names are
    // stripped or truncated; a name with nothing printable left falls back to "Player".
    EXPECT_EQ(MMOChatSystem::ServerAttributedSenderName("Sys\r\ntem\x1b", 4), std::string("System#4"));
    EXPECT_EQ(MMOChatSystem::ServerAttributedSenderName("\xE2\x80\xAE"
                                                        "metsyS",
                                                        5),
              std::string("metsyS#5"));
    EXPECT_EQ(MMOChatSystem::ServerAttributedSenderName("", 6), std::string("Player#6"));
    EXPECT_EQ(MMOChatSystem::ServerAttributedSenderName("   ", 6), std::string("Player#6"));
    EXPECT_EQ(MMOChatSystem::ServerAttributedSenderName("\x01\x02", 6), std::string("Player#6"));
    const std::string attributedLong = MMOChatSystem::ServerAttributedSenderName(std::string(500, 'a'), 8);
    EXPECT_EQ(attributedLong, std::string(MMOChatSystem::MAX_SENDER_DISPLAY_NAME, 'a') + "#8");

    // Whatever the connection name, the attributed name ends in exactly one "#<id>" of that connection.
    for (const char* name : {"System", "System#1", "#", "##7", "Player#2", "\x7f"})
    {
        const std::string attributed = MMOChatSystem::ServerAttributedSenderName(name, 42);
        EXPECT_EQ(attributed.find('#'), attributed.rfind('#'));
        EXPECT_EQ(attributed.substr(attributed.find('#')), std::string("#42"));
    }
}

TEST(SEC2GM_MMOChatHotReloadTeardownKeepsReplacementHandler)
{
    auto& network = Spark::Net::NetworkManager::GetInstance();
    SEC2NetworkContext context;
    const std::string outgoingOwner = "SEC2GM_MMOOutgoing#4";
    const std::string replacementOwner = "SEC2GM_MMOReplacement#5";

    MMO::MMOChatSystem outgoing;
    {
        Spark::Net::NetworkManager::ScopedRegistrationOwner scope(network, outgoingOwner);
        ASSERT_TRUE(outgoing.Initialize(&context));
    }
    MMO::MMOChatSystem replacement;
    {
        Spark::Net::NetworkManager::ScopedRegistrationOwner scope(network, replacementOwner);
        ASSERT_TRUE(replacement.Initialize(&context));
    }

    // Reload order: the replacement is live before the outgoing module's OnUnload runs.
    {
        Spark::Net::NetworkManager::ScopedRegistrationOwner scope(network, outgoingOwner, true);
        outgoing.Shutdown();
    }

    // The outgoing image owns nothing any more, and the chat slot still belongs to the replacement.
    EXPECT_EQ(network.UnregisterHandlersByOwner(outgoingOwner), static_cast<size_t>(0));
    EXPECT_EQ(network.UnregisterHandlersByOwner(replacementOwner), static_cast<size_t>(1));
    replacement.Shutdown();
}

#endif // ENABLE_NETWORKING
#endif // SPARK_TEST_HAS_IMGUI
