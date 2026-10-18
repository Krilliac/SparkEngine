/**
 * @file TFNetProtocolOnboarding.h
 * @brief Packed onboarding wire structs: auth (W5 Task 4), character
 *        list/create/delete, enter-world, unlock-tree purchases (W6), and
 *        server-authoritative continent-hop (W13).
 *
 * Part of the frozen TFNetProtocol.h contract (see DESIGN.md §3), split into a
 * sibling part-header purely for file-size sanity. Do not include directly —
 * include Net/TFNetProtocol.h, the umbrella. Layouts are frozen and
 * static_asserted; all structs are POD, packed, little-endian on the wire.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace Terrafront
{

#pragma pack(push, 1)

    // --- W5 onboarding (Task 4), NET-100 SCRAM-SHA-256 login ----------------------
    //
    // No message carries a password or a password-equivalent. Login is SCRAM
    // (RFC 5802 / 7677; Account/TFAccountSystem.h):
    //   C->S LoginRequest   TF_LoginStart      username
    //   S->C LoginChallenge TF_LoginChallenge  salt, iterations, single-use server nonce
    //   C->S LoginProof     TF_LoginProof      client nonce, echoed server nonce, ClientProof
    //   S->C LoginReply     TF_AuthReply       result + ServerSignature (client verifies it)
    // Registration sends a verifier the client derived locally (TF_RegisterRequest):
    // StoredKey and ServerKey, never the password or SaltedPassword. Every message
    // travels inside the NET-100 sealed channel (Net/TFScramWire.h builds and checks them).

    inline constexpr size_t kTFScramNonceChars = 64; ///< NUL-terminated hex nonce buffer (24-byte nonces use 48)
    inline constexpr size_t kTFScramSaltBytes = 64;  ///< == TFAccountSystem::kMaxScramSaltBytes

    struct TF_LoginStart
    {
        char user[32]; // null-terminated username
    };
    static_assert(sizeof(TF_LoginStart) == 32, "wire layout frozen");

    struct TF_LoginChallenge
    {
        uint8_t saltLen; // valid bytes in salt (16..64)
        uint8_t _pad[3];
        uint32_t iterations;                  // PBKDF2 cost the client must pay
        uint8_t salt[kTFScramSaltBytes];      // first saltLen bytes are the salt
        char serverNonce[kTFScramNonceChars]; // null-terminated hex, single use
    };
    static_assert(sizeof(TF_LoginChallenge) == 136, "wire layout frozen");

    struct TF_LoginProof
    {
        char user[32];                        // null-terminated username (must match the challenge)
        char clientNonce[kTFScramNonceChars]; // null-terminated hex
        char serverNonce[kTFScramNonceChars]; // echoed from the challenge
        uint8_t proof[32];                    // ClientProof = ClientKey XOR HMAC(StoredKey, AuthMessage)
    };
    static_assert(sizeof(TF_LoginProof) == 192, "wire layout frozen");

    struct TF_AuthReply
    {
        uint8_t ok;  // 0/1
        uint8_t err; // TFAuthErr
        uint8_t _pad[2];
        uint64_t accountId;          // 0 if !ok
        uint8_t serverSignature[32]; // LoginReply when ok: HMAC(ServerKey, AuthMessage); zero otherwise
    };
    static_assert(sizeof(TF_AuthReply) == 44, "wire layout frozen");

    struct TF_RegisterRequest
    {
        char user[32]; // null-terminated username
        uint8_t saltLen;
        uint8_t _pad[3];
        uint32_t iterations;
        uint8_t salt[kTFScramSaltBytes];
        uint8_t storedKey[32]; // SHA-256(ClientKey)
        uint8_t serverKey[32]; // HMAC(SaltedPassword, "Server Key")
    };
    static_assert(sizeof(TF_RegisterRequest) == 168, "wire layout frozen");

    struct TF_CharBrief
    {
        uint64_t id;
        char name[24];   // null-terminated, matches TFCharacterSystem's 23-char max name
        uint8_t faction; // FactionId
        uint16_t rank;
        uint8_t _pad;
    };
    static_assert(sizeof(TF_CharBrief) == 36, "wire layout frozen");

    struct TF_CharListReply
    {
        uint8_t count; // number of valid entries in `chars` (0..5)
        uint8_t _pad[3];
        TF_CharBrief chars[5]; // kTFMaxCharSlots (TFCharacterSystem.h)
    };
    static_assert(sizeof(TF_CharListReply) == 4 + 5 * 36, "wire layout frozen");

    struct TF_CharCreateRequest
    {
        char name[24];
        uint8_t faction; // FactionId
        uint8_t _pad[3];
    };
    static_assert(sizeof(TF_CharCreateRequest) == 28, "wire layout frozen");

    struct TF_CharOpReply
    {
        uint8_t ok;  // 0/1
        uint8_t err; // TFCharErr
        uint8_t _pad[2];
        uint64_t charId; // the affected character (0 if !ok and unknown)
    };
    static_assert(sizeof(TF_CharOpReply) == 12, "wire layout frozen");

    struct TF_CharDeleteRequest
    {
        uint64_t charId;
    };
    static_assert(sizeof(TF_CharDeleteRequest) == 8, "wire layout frozen");

    struct TF_EnterWorldRequest
    {
        uint64_t charId;
    };
    static_assert(sizeof(TF_EnterWorldRequest) == 8, "wire layout frozen");

    // --- W6 progression: unlock-tree purchase ------------------------------------

    struct TF_UnlockRequest
    {
        char unlockKey[32]; // NUL-terminated TFUnlockTree key
    };
    static_assert(sizeof(TF_UnlockRequest) == 32, "wire layout frozen");

    struct TF_UnlockReply
    {
        uint8_t result; // Terrafront::TFUnlockResult
        uint8_t _pad[3];
        char unlockKey[32];
    };
    static_assert(sizeof(TF_UnlockReply) == 36, "wire layout frozen");

    // --- W13 multimap server-authoritative continent-hop (docs/TERRAFRONT_MULTIMAP.md §2.2) ---

    struct TF_ContinentHopRequest
    {
        uint8_t mapId; // destination continent (continents.json mapId, NOT the
                       // position-isolation kTFMap* constants — see
                       // World/TFTravelSystem.h::ContinentMeta::mapId)
        uint8_t _pad[3];
    };
    static_assert(sizeof(TF_ContinentHopRequest) == 4, "wire layout frozen");

    struct TF_ContinentHopReply
    {
        uint8_t ok;    // 0/1 — 1 == host/port below are a live registry entry
        uint8_t mapId; // destination echoed back
        uint16_t port; // 0 if !ok
        char host[64]; // null-terminated; empty if !ok
    };
    static_assert(sizeof(TF_ContinentHopReply) == 68, "wire layout frozen");

#pragma pack(pop)

} // namespace Terrafront
