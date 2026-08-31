/** @file TestGatewaySecurity.cpp @brief HMAC admission and replay tests. */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <aclapi.h>
#endif

#include "TestFramework.h"
#include "GatewaySecurity.h"
#include "Utils/SecureRandom.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace Spark::Gateway;

namespace
{
    int64_t NowMilliseconds()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    AdmissionRequest Request()
    {
        AdmissionRequest request;
        request.clientId = 42;
        request.sessionId = "session-42";
        request.playerName = "Player";
        return request;
    }

#ifdef _WIN32
    enum class AclFixtureResult
    {
        Ready,
        Unsupported,
        Failed
    };

    bool IsUnsupportedAclFixtureError(DWORD status)
    {
        return status == ERROR_ACCESS_DENIED || status == ERROR_PRIVILEGE_NOT_HELD || status == ERROR_NOT_SUPPORTED ||
               status == ERROR_INVALID_OWNER;
    }

    AclFixtureResult AclFixtureError(std::string_view operation, DWORD status, std::string& error)
    {
        error = std::string(operation) + " failed with Windows error " + std::to_string(status);
        return IsUnsupportedAclFixtureError(status) ? AclFixtureResult::Unsupported : AclFixtureResult::Failed;
    }

    bool FinishAclFixtureSetup(AclFixtureResult result, std::string_view testName, const std::string& error)
    {
        if (result == AclFixtureResult::Ready)
            return true;
        if (result == AclFixtureResult::Unsupported)
        {
            SKIP_TEST(std::string(testName) + ": Windows ACL fixture is unsupported (" + error + ")");
        }
        std::cerr << "  FAIL: Windows ACL fixture construction failed: " << error << '\n';
        EXPECT_TRUE(false);
        return false;
    }

    bool IsOwnedByCurrentProcessUser(PSID owner)
    {
        if (!owner || !IsValidSid(owner))
            return false;

        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return false;

        DWORD userSize = 0;
        const BOOL sizeResult = GetTokenInformation(token, TokenUser, nullptr, 0, &userSize);
        if (sizeResult != FALSE || GetLastError() != ERROR_INSUFFICIENT_BUFFER || userSize == 0)
        {
            CloseHandle(token);
            return false;
        }

        std::vector<uint8_t> userBuffer(userSize);
        const BOOL userResult = GetTokenInformation(token, TokenUser, userBuffer.data(), userSize, &userSize);
        CloseHandle(token);
        if (userResult == FALSE)
            return false;

        const auto* tokenUser = reinterpret_cast<const TOKEN_USER*>(userBuffer.data());
        return tokenUser->User.Sid && IsValidSid(tokenUser->User.Sid) && EqualSid(owner, tokenUser->User.Sid);
    }

    AclFixtureResult AddInheritableWorldReadAce(const std::filesystem::path& directory, std::string& error)
    {
        PACL existingDacl = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        DWORD status = GetNamedSecurityInfoW(directory.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                             nullptr, &existingDacl, nullptr, &descriptor);
        if (status != ERROR_SUCCESS || !existingDacl)
        {
            if (descriptor)
                LocalFree(descriptor);
            return AclFixtureError("GetNamedSecurityInfoW", status, error);
        }

        std::array<unsigned char, SECURITY_MAX_SID_SIZE> worldSid{};
        DWORD worldSidSize = static_cast<DWORD>(worldSid.size());
        if (!CreateWellKnownSid(WinWorldSid, nullptr, worldSid.data(), &worldSidSize))
        {
            const DWORD code = GetLastError();
            LocalFree(descriptor);
            return AclFixtureError("CreateWellKnownSid", code, error);
        }

        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions = FILE_GENERIC_READ;
        access.grfAccessMode = GRANT_ACCESS;
        access.grfInheritance = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(worldSid.data());

        PACL updatedDacl = nullptr;
        status = SetEntriesInAclW(1, &access, existingDacl, &updatedDacl);
        LocalFree(descriptor);
        if (status != ERROR_SUCCESS)
            return AclFixtureError("SetEntriesInAclW", status, error);

        status = SetNamedSecurityInfoW(const_cast<LPWSTR>(directory.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                       nullptr, nullptr, updatedDacl, nullptr);
        LocalFree(updatedDacl);
        if (status != ERROR_SUCCESS)
            return AclFixtureError("SetNamedSecurityInfoW", status, error);
        return AclFixtureResult::Ready;
    }

    bool HasInheritedWorldReadAce(const std::filesystem::path& file)
    {
        PACL dacl = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        const DWORD status = GetNamedSecurityInfoW(file.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                                   nullptr, &dacl, nullptr, &descriptor);
        if (status != ERROR_SUCCESS || !descriptor || !dacl || !IsValidAcl(dacl))
        {
            if (descriptor)
                LocalFree(descriptor);
            return false;
        }

        std::array<unsigned char, SECURITY_MAX_SID_SIZE> worldSid{};
        DWORD worldSidSize = static_cast<DWORD>(worldSid.size());
        if (!CreateWellKnownSid(WinWorldSid, nullptr, worldSid.data(), &worldSidSize))
        {
            LocalFree(descriptor);
            return false;
        }

        ACL_SIZE_INFORMATION information{};
        bool found = false;
        if (GetAclInformation(dacl, &information, sizeof(information), AclSizeInformation))
        {
            GENERIC_MAPPING fileMapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
            for (DWORD index = 0; index < information.AceCount && !found; ++index)
            {
                void* rawAce = nullptr;
                if (!GetAce(dacl, index, &rawAce))
                    break;
                const auto* header = static_cast<const ACE_HEADER*>(rawAce);
                constexpr size_t sidOffset = offsetof(ACCESS_ALLOWED_ACE, SidStart);
                if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || (header->AceFlags & INHERITED_ACE) == 0 ||
                    header->AceSize < sidOffset + GetSidLengthRequired(0))
                    continue;
                const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(rawAce);
                const auto* trustee = reinterpret_cast<const SID*>(&ace->SidStart);
                const size_t sidBytes = GetSidLengthRequired(trustee->SubAuthorityCount);
                if (sidBytes > header->AceSize - sidOffset || !IsValidSid(const_cast<SID*>(trustee)) ||
                    GetLengthSid(const_cast<SID*>(trustee)) != sidBytes ||
                    !EqualSid(const_cast<SID*>(trustee), worldSid.data()))
                    continue;
                DWORD mappedMask = ace->Mask;
                MapGenericMask(&mappedMask, &fileMapping);
                found = (mappedMask & FILE_GENERIC_READ) == FILE_GENERIC_READ;
            }
        }
        LocalFree(descriptor);
        return found;
    }

    AclFixtureResult AssignAlternateOwnerOnlyAcl(const std::filesystem::path& file, std::string& error)
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return AclFixtureError("OpenProcessToken", GetLastError(), error);

        DWORD userSize = 0;
        (void)GetTokenInformation(token, TokenUser, nullptr, 0, &userSize);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || userSize == 0)
        {
            const DWORD status = GetLastError();
            CloseHandle(token);
            return AclFixtureError("GetTokenInformation(TokenUser size)", status, error);
        }
        std::vector<uint8_t> userBuffer(userSize);
        if (!GetTokenInformation(token, TokenUser, userBuffer.data(), userSize, &userSize))
        {
            const DWORD status = GetLastError();
            CloseHandle(token);
            return AclFixtureError("GetTokenInformation(TokenUser)", status, error);
        }
        const auto* tokenUser = reinterpret_cast<const TOKEN_USER*>(userBuffer.data());

        DWORD groupsSize = 0;
        (void)GetTokenInformation(token, TokenGroups, nullptr, 0, &groupsSize);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || groupsSize == 0)
        {
            const DWORD status = GetLastError();
            CloseHandle(token);
            return AclFixtureError("GetTokenInformation(TokenGroups size)", status, error);
        }
        std::vector<uint8_t> groupsBuffer(groupsSize);
        if (!GetTokenInformation(token, TokenGroups, groupsBuffer.data(), groupsSize, &groupsSize))
        {
            const DWORD status = GetLastError();
            CloseHandle(token);
            return AclFixtureError("GetTokenInformation(TokenGroups)", status, error);
        }
        CloseHandle(token);

        const auto* tokenGroups = reinterpret_cast<const TOKEN_GROUPS*>(groupsBuffer.data());
        PSID alternateOwner = nullptr;
        for (DWORD index = 0; index < tokenGroups->GroupCount; ++index)
        {
            const SID_AND_ATTRIBUTES& group = tokenGroups->Groups[index];
            if ((group.Attributes & (SE_GROUP_OWNER | SE_GROUP_ENABLED)) == (SE_GROUP_OWNER | SE_GROUP_ENABLED) &&
                (group.Attributes & SE_GROUP_USE_FOR_DENY_ONLY) == 0 && IsValidSid(group.Sid) &&
                !EqualSid(group.Sid, tokenUser->User.Sid))
            {
                alternateOwner = group.Sid;
                break;
            }
        }
        if (!alternateOwner)
        {
            error = "the process token has no alternate owner-capable group SID";
            return AclFixtureResult::Unsupported;
        }

        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions = FILE_GENERIC_READ;
        access.grfAccessMode = SET_ACCESS;
        access.grfInheritance = NO_INHERITANCE;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
        access.Trustee.ptstrName = static_cast<LPWSTR>(alternateOwner);

        PACL ownerOnlyDacl = nullptr;
        DWORD status = SetEntriesInAclW(1, &access, nullptr, &ownerOnlyDacl);
        if (status != ERROR_SUCCESS)
            return AclFixtureError("SetEntriesInAclW", status, error);

        status = SetNamedSecurityInfoW(const_cast<LPWSTR>(file.c_str()), SE_FILE_OBJECT,
                                       OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                                           PROTECTED_DACL_SECURITY_INFORMATION,
                                       alternateOwner, nullptr, ownerOnlyDacl, nullptr);
        LocalFree(ownerOnlyDacl);
        if (status != ERROR_SUCCESS)
            return AclFixtureError("SetNamedSecurityInfoW alternate owner", status, error);
        return AclFixtureResult::Ready;
    }
#endif
} // namespace

TEST(GatewaySecurity_AcceptsOnceAndRejectsReplay)
{
    KeyFileAuthenticator authenticator(std::vector<uint8_t>(32, 0x5a));
    AdmissionRequest request = Request();
    request.credential = authenticator.CreateCredential(request, NowMilliseconds(), 123456);
    EXPECT_TRUE(authenticator.Authenticate(request).accepted);
    EXPECT_FALSE(authenticator.Authenticate(request).accepted);
}

TEST(GatewaySecurity_RejectsTamperingAndExpiredTimestamp)
{
    KeyFileAuthenticator authenticator(std::vector<uint8_t>(32, 0x6b), std::chrono::seconds(2));
    AdmissionRequest request = Request();
    request.credential = authenticator.CreateCredential(request, NowMilliseconds(), 222);
    request.playerName = "Tampered";
    EXPECT_FALSE(authenticator.Authenticate(request).accepted);

    request = Request();
    request.credential = authenticator.CreateCredential(request, NowMilliseconds() - 5000, 333);
    EXPECT_FALSE(authenticator.Authenticate(request).accepted);
}

TEST(GatewaySecurity_BindsAuthoritativeSpawnPosition)
{
    KeyFileAuthenticator authenticator(std::vector<uint8_t>(32, 0x7c));
    AdmissionRequest request = Request();
    request.spawnPosition = {10.0f, 20.0f, -30.0f};
    request.credential = authenticator.CreateCredential(request, NowMilliseconds(), 444);

    request.spawnPosition.x = 11.0f;
    EXPECT_FALSE(authenticator.Authenticate(request).accepted);
}

TEST(GatewaySecurity_RejectsWeakKey)
{
    KeyFileAuthenticator authenticator(std::vector<uint8_t>(31, 0x11));
    EXPECT_FALSE(authenticator.IsReady());
}

TEST(GatewaySecurity_RejectsOversizedCredential)
{
    KeyFileAuthenticator authenticator(std::vector<uint8_t>(32, 0x5a));
    AdmissionRequest request = Request();
    request.credential.assign(GatewayMaximumCredentialSize + 1, 'x');

    const AuthenticationResult result = authenticator.Authenticate(request);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, std::string("Malformed gateway credential"));
}

TEST(GatewaySecurity_RejectsNumericFieldsWithTrailingData)
{
    KeyFileAuthenticator authenticator(std::vector<uint8_t>(32, 0x5a));

    AdmissionRequest request = Request();
    request.credential = authenticator.CreateCredential(request, NowMilliseconds(), 123456);
    const size_t timestampEnd = request.credential.find('.', 3);
    ASSERT_TRUE(timestampEnd != std::string::npos);
    request.credential.insert(timestampEnd, 1, 'x');
    const AuthenticationResult timestampResult = authenticator.Authenticate(request);
    EXPECT_FALSE(timestampResult.accepted);
    EXPECT_EQ(timestampResult.reason, std::string("Malformed gateway credential"));

    request = Request();
    request.credential = authenticator.CreateCredential(request, NowMilliseconds(), 654321);
    const size_t nonceBegin = request.credential.find('.', 3);
    ASSERT_TRUE(nonceBegin != std::string::npos);
    const size_t nonceEnd = request.credential.find('.', nonceBegin + 1);
    ASSERT_TRUE(nonceEnd != std::string::npos);
    request.credential.insert(nonceEnd, 1, 'x');
    const AuthenticationResult nonceResult = authenticator.Authenticate(request);
    EXPECT_FALSE(nonceResult.accepted);
    EXPECT_EQ(nonceResult.reason, std::string("Malformed gateway credential"));
}

TEST(GatewaySecurity_AcceptsOwnerOnlyGeneratedKeyFile)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / Spark::SecureRandom::HexToken(12);
    const std::filesystem::path keyFile = root / "gateway.key";
    const std::string token = Spark::SecureRandom::HexToken(32);
    std::string error;
    EXPECT_TRUE(Spark::SecureRandom::CreatePrivateFile(keyFile, token + "\n", &error));

    std::vector<uint8_t> key;
    EXPECT_TRUE(LoadPrivateGatewayKey(keyFile, key, error));
    EXPECT_EQ(key.size(), token.size() / 2);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST(GatewaySecurity_FailedReloadRevokesPriorOutputKey)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / Spark::SecureRandom::HexToken(12);
    const std::filesystem::path keyFile = root / "gateway.key";
    std::string error;
    ASSERT_TRUE(Spark::SecureRandom::CreatePrivateFile(keyFile, "too-short\n", &error));

    std::vector<uint8_t> key(64, 0xA5);
    EXPECT_FALSE(LoadPrivateGatewayKey(keyFile, key, error));
    EXPECT_TRUE(key.empty());
    EXPECT_EQ(error, std::string("Gateway key must contain between 32 and 4096 bytes"));

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST(GatewaySecurity_RejectsHardLinkedKeyFile)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / Spark::SecureRandom::HexToken(12);
    const std::filesystem::path keyFile = root / "gateway.key";
    const std::filesystem::path linkedKeyFile = root / "gateway-link.key";
    const std::string token = Spark::SecureRandom::HexToken(32);
    std::string error;
    ASSERT_TRUE(Spark::SecureRandom::CreatePrivateFile(keyFile, token + "\n", &error));

    std::error_code filesystemError;
    std::filesystem::create_hard_link(keyFile, linkedKeyFile, filesystemError);
    ASSERT_FALSE(static_cast<bool>(filesystemError));

    std::vector<uint8_t> key;
    EXPECT_FALSE(LoadPrivateGatewayKey(keyFile, key, error));
    EXPECT_TRUE(key.empty());
    EXPECT_EQ(error, std::string("Gateway key must be a regular, single-link file"));

    std::filesystem::remove_all(root, filesystemError);
}

#ifdef _WIN32
TEST(GatewaySecurity_GeneratedKeyRemainsPrivateUnderInheritableParentAcl)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / Spark::SecureRandom::HexToken(12);
    const std::filesystem::path keyFile = root / "gateway.key";
    std::error_code filesystemError;
    std::filesystem::create_directories(root, filesystemError);
    EXPECT_FALSE(static_cast<bool>(filesystemError));
    if (filesystemError)
        return;

    std::string error;
    const AclFixtureResult parentAclResult = AddInheritableWorldReadAce(root, error);
    if (!FinishAclFixtureSetup(parentAclResult, "GatewaySecurity_GeneratedKeyRemainsPrivateUnderInheritableParentAcl",
                               error))
    {
        std::filesystem::remove_all(root, filesystemError);
        return;
    }

    const std::string token = Spark::SecureRandom::HexToken(32);
    const std::filesystem::path inheritedProbeFile = root / "inherited-probe.key";
    bool inheritedProbeCreated = false;
    {
        std::ofstream inheritedProbe(inheritedProbeFile, std::ios::binary);
        inheritedProbe << token << '\n';
        inheritedProbeCreated = inheritedProbe.good();
    }
    EXPECT_TRUE(inheritedProbeCreated);
    if (!inheritedProbeCreated)
    {
        std::filesystem::remove_all(root, filesystemError);
        return;
    }
    EXPECT_TRUE(HasInheritedWorldReadAce(inheritedProbeFile));

    std::vector<uint8_t> inheritedKey;
    error.clear();
    EXPECT_FALSE(LoadPrivateGatewayKey(inheritedProbeFile, inheritedKey, error));
    EXPECT_EQ(error, std::string("Gateway key ACL must be protected from inheritance"));

    error.clear();
    const bool created = Spark::SecureRandom::CreatePrivateFile(keyFile, token + "\n", &error);
    if (!created)
        std::cerr << "  FAIL: protected key creation failed: " << error << '\n';
    EXPECT_TRUE(created);
    if (created)
    {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        PSID owner = nullptr;
        const DWORD status = GetNamedSecurityInfoW(keyFile.c_str(), SE_FILE_OBJECT,
                                                   OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner,
                                                   nullptr, nullptr, nullptr, &descriptor);
        SECURITY_DESCRIPTOR_CONTROL control{};
        DWORD revision = 0;
        const bool protectedDacl = status == ERROR_SUCCESS && descriptor &&
                                   GetSecurityDescriptorControl(descriptor, &control, &revision) &&
                                   (control & SE_DACL_PROTECTED) != 0;
        EXPECT_TRUE(protectedDacl);
        EXPECT_TRUE(status == ERROR_SUCCESS && IsOwnedByCurrentProcessUser(owner));
        if (descriptor)
            LocalFree(descriptor);

        std::vector<uint8_t> key;
        error.clear();
        EXPECT_TRUE(LoadPrivateGatewayKey(keyFile, key, error));
        EXPECT_EQ(key.size(), token.size() / 2);
    }

    std::filesystem::remove_all(root, filesystemError);
}

TEST(GatewaySecurity_RejectsUnprotectedAcl)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / Spark::SecureRandom::HexToken(12);
    const std::filesystem::path keyFile = root / "gateway.key";
    const std::string token = Spark::SecureRandom::HexToken(32);
    std::string error;
    ASSERT_TRUE(Spark::SecureRandom::CreatePrivateFile(keyFile, token + "\n", &error));

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    DWORD status = GetNamedSecurityInfoW(keyFile.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                         &dacl, nullptr, &descriptor);
    ASSERT_EQ(status, DWORD{ERROR_SUCCESS});
    ASSERT_TRUE(descriptor != nullptr);
    ASSERT_TRUE(dacl != nullptr);

    status = SetNamedSecurityInfoW(const_cast<LPWSTR>(keyFile.c_str()), SE_FILE_OBJECT,
                                   DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                   dacl, nullptr);
    LocalFree(descriptor);
    ASSERT_EQ(status, DWORD{ERROR_SUCCESS});

    descriptor = nullptr;
    status = GetNamedSecurityInfoW(keyFile.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                   nullptr, nullptr, &descriptor);
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    const bool unprotected = status == ERROR_SUCCESS && descriptor &&
                             GetSecurityDescriptorControl(descriptor, &control, &revision) &&
                             (control & SE_DACL_PROTECTED) == 0;
    EXPECT_TRUE(unprotected);
    if (descriptor)
        LocalFree(descriptor);

    std::vector<uint8_t> key(64, 0xa5);
    error.clear();
    EXPECT_FALSE(LoadPrivateGatewayKey(keyFile, key, error));
    EXPECT_TRUE(key.empty());
    EXPECT_EQ(error, std::string("Gateway key ACL must be protected from inheritance"));

    std::error_code filesystemError;
    std::filesystem::remove_all(root, filesystemError);
}

TEST(GatewaySecurity_AllowsConcurrentOwnerReadHandle)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / Spark::SecureRandom::HexToken(12);
    const std::filesystem::path keyFile = root / "gateway.key";
    const std::string token = Spark::SecureRandom::HexToken(32);
    std::string error;
    ASSERT_TRUE(Spark::SecureRandom::CreatePrivateFile(keyFile, token + "\n", &error));

    const HANDLE existingReader =
        CreateFileW(keyFile.c_str(), GENERIC_READ | READ_CONTROL, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    EXPECT_TRUE(existingReader != INVALID_HANDLE_VALUE);
    if (existingReader != INVALID_HANDLE_VALUE)
    {
        std::vector<uint8_t> key;
        error.clear();
        EXPECT_TRUE(LoadPrivateGatewayKey(keyFile, key, error));
        EXPECT_EQ(key.size(), token.size() / 2);
        CloseHandle(existingReader);
    }

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST(GatewaySecurity_RejectsOwnerOnlyAclWhenOwnerIsNotCurrentProcessUser)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / Spark::SecureRandom::HexToken(12);
    const std::filesystem::path keyFile = root / "gateway.key";
    const std::string token = Spark::SecureRandom::HexToken(32);
    std::string error;
    ASSERT_TRUE(Spark::SecureRandom::CreatePrivateFile(keyFile, token + "\n", &error));

    const AclFixtureResult ownerAclResult = AssignAlternateOwnerOnlyAcl(keyFile, error);
    if (!FinishAclFixtureSetup(ownerAclResult, "GatewaySecurity_RejectsOwnerOnlyAclWhenOwnerIsNotCurrentProcessUser",
                               error))
    {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        return;
    }

    std::vector<uint8_t> key(32, 0xA5);
    error.clear();
    EXPECT_FALSE(LoadPrivateGatewayKey(keyFile, key, error));
    EXPECT_TRUE(key.empty());
    EXPECT_EQ(error, std::string("Gateway key file must be owned by the current process user"));

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}
#endif
