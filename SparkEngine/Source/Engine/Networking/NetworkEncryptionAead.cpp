/**
 * @file NetworkEncryptionAead.cpp
 * @brief RFC 8439 ChaCha20-Poly1305 AEAD primitive and constant-time comparison
 *
 * The primitives follow RFC 8439 exactly (ChaCha20 section 2.3-2.4, Poly1305
 * section 2.5 using the 26-bit-limb "donna" reduction, AEAD section 2.8) and are
 * pinned by the RFC's known-answer vectors in Tests/TestNET100TransportReal.cpp.
 * Do not change the arithmetic without re-running those vectors.
 */

#include "NetworkEncryption.h"

#include <cstring>

namespace Spark::Net
{

    namespace
    {
        // ------------------------------------------------------------------------
        // Byte helpers
        // ------------------------------------------------------------------------

        uint32_t LoadLE32(const uint8_t* p)
        {
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
        }

        void StoreLE32(uint8_t* p, uint32_t v)
        {
            p[0] = static_cast<uint8_t>(v);
            p[1] = static_cast<uint8_t>(v >> 8);
            p[2] = static_cast<uint8_t>(v >> 16);
            p[3] = static_cast<uint8_t>(v >> 24);
        }

        void StoreLE64(uint8_t* p, uint64_t v)
        {
            for (size_t i = 0; i < 8; ++i)
                p[i] = static_cast<uint8_t>(v >> (i * 8));
        }

        /// Zero secret material in a way the optimizer may not elide.
        void SecureWipe(void* data, size_t size)
        {
            volatile uint8_t* bytes = static_cast<volatile uint8_t*>(data);
            for (size_t i = 0; i < size; ++i)
                bytes[i] = 0;
        }

        // ------------------------------------------------------------------------
        // ChaCha20 (RFC 8439 section 2.3)
        // ------------------------------------------------------------------------

        uint32_t RotL32(uint32_t v, int bits)
        {
            return (v << bits) | (v >> (32 - bits));
        }

        void QuarterRound(uint32_t* s, int a, int b, int c, int d)
        {
            s[a] += s[b];
            s[d] = RotL32(s[d] ^ s[a], 16);
            s[c] += s[d];
            s[b] = RotL32(s[b] ^ s[c], 12);
            s[a] += s[b];
            s[d] = RotL32(s[d] ^ s[a], 8);
            s[c] += s[d];
            s[b] = RotL32(s[b] ^ s[c], 7);
        }

        void ChaCha20Block(const SessionKey& key, uint32_t counter, const AeadNonce& nonce, uint8_t out[64])
        {
            uint32_t input[16];
            input[0] = 0x61707865u; // "expand 32-byte k"
            input[1] = 0x3320646eu;
            input[2] = 0x79622d32u;
            input[3] = 0x6b206574u;
            for (size_t i = 0; i < 8; ++i)
                input[4 + i] = LoadLE32(key.data() + i * 4);
            input[12] = counter;
            for (size_t i = 0; i < 3; ++i)
                input[13 + i] = LoadLE32(nonce.data() + i * 4);

            uint32_t working[16];
            std::memcpy(working, input, sizeof(input));
            for (int round = 0; round < 10; ++round)
            {
                QuarterRound(working, 0, 4, 8, 12);
                QuarterRound(working, 1, 5, 9, 13);
                QuarterRound(working, 2, 6, 10, 14);
                QuarterRound(working, 3, 7, 11, 15);
                QuarterRound(working, 0, 5, 10, 15);
                QuarterRound(working, 1, 6, 11, 12);
                QuarterRound(working, 2, 7, 8, 13);
                QuarterRound(working, 3, 4, 9, 14);
            }
            for (size_t i = 0; i < 16; ++i)
                StoreLE32(out + i * 4, working[i] + input[i]);

            SecureWipe(input, sizeof(input));
            SecureWipe(working, sizeof(working));
        }

        /// XOR `data` with the ChaCha20 keystream starting at block `counter` (RFC 8439 section 2.4).
        void ChaCha20Xor(const SessionKey& key, uint32_t counter, const AeadNonce& nonce, const uint8_t* in,
                         uint8_t* out, size_t length)
        {
            uint8_t block[64];
            for (size_t offset = 0; offset < length; offset += 64)
            {
                ChaCha20Block(key, counter++, nonce, block);
                const size_t chunk = (length - offset < 64) ? (length - offset) : 64;
                for (size_t i = 0; i < chunk; ++i)
                    out[offset + i] = in[offset + i] ^ block[i];
            }
            SecureWipe(block, sizeof(block));
        }

        // ------------------------------------------------------------------------
        // Poly1305 (RFC 8439 section 2.5), 26-bit limbs
        // ------------------------------------------------------------------------

        class Poly1305
        {
          public:
            explicit Poly1305(const uint8_t key[32])
            {
                // r is clamped per RFC 8439 section 2.5.1.
                m_r[0] = LoadLE32(key + 0) & 0x3ffffff;
                m_r[1] = (LoadLE32(key + 3) >> 2) & 0x3ffff03;
                m_r[2] = (LoadLE32(key + 6) >> 4) & 0x3ffc0ff;
                m_r[3] = (LoadLE32(key + 9) >> 6) & 0x3f03fff;
                m_r[4] = (LoadLE32(key + 12) >> 8) & 0x00fffff;
                for (size_t i = 0; i < 4; ++i)
                    m_pad[i] = LoadLE32(key + 16 + i * 4);
            }

            ~Poly1305()
            {
                SecureWipe(m_r, sizeof(m_r));
                SecureWipe(m_h, sizeof(m_h));
                SecureWipe(m_pad, sizeof(m_pad));
                SecureWipe(m_buffer, sizeof(m_buffer));
            }

            void Update(const uint8_t* data, size_t length)
            {
                while (length > 0)
                {
                    const size_t take = (16 - m_bufferLen < length) ? (16 - m_bufferLen) : length;
                    std::memcpy(m_buffer + m_bufferLen, data, take);
                    m_bufferLen += take;
                    data += take;
                    length -= take;
                    if (m_bufferLen == 16)
                    {
                        Block(m_buffer, 1u << 24);
                        m_bufferLen = 0;
                    }
                }
            }

            /// Absorb zero bytes up to the next 16-byte boundary (AEAD pad16).
            void PadTo16()
            {
                if (m_bufferLen == 0)
                    return;
                std::memset(m_buffer + m_bufferLen, 0, 16 - m_bufferLen);
                Block(m_buffer, 1u << 24);
                m_bufferLen = 0;
            }

            void Finish(uint8_t tag[16])
            {
                if (m_bufferLen > 0)
                {
                    // Final partial block: append the 0x01 byte explicitly, no implicit high bit.
                    m_buffer[m_bufferLen] = 1;
                    std::memset(m_buffer + m_bufferLen + 1, 0, 16 - m_bufferLen - 1);
                    Block(m_buffer, 0);
                    m_bufferLen = 0;
                }

                uint32_t h0 = m_h[0], h1 = m_h[1], h2 = m_h[2], h3 = m_h[3], h4 = m_h[4];

                // Fully carry h.
                uint32_t c = h1 >> 26;
                h1 &= 0x3ffffff;
                h2 += c;
                c = h2 >> 26;
                h2 &= 0x3ffffff;
                h3 += c;
                c = h3 >> 26;
                h3 &= 0x3ffffff;
                h4 += c;
                c = h4 >> 26;
                h4 &= 0x3ffffff;
                h0 += c * 5;
                c = h0 >> 26;
                h0 &= 0x3ffffff;
                h1 += c;

                // Compute h + -p and select it in constant time if h >= p.
                uint32_t g0 = h0 + 5;
                c = g0 >> 26;
                g0 &= 0x3ffffff;
                uint32_t g1 = h1 + c;
                c = g1 >> 26;
                g1 &= 0x3ffffff;
                uint32_t g2 = h2 + c;
                c = g2 >> 26;
                g2 &= 0x3ffffff;
                uint32_t g3 = h3 + c;
                c = g3 >> 26;
                g3 &= 0x3ffffff;
                uint32_t g4 = h4 + c - (1u << 26);

                uint32_t mask = (g4 >> 31) - 1u; // all ones when h >= p
                g0 &= mask;
                g1 &= mask;
                g2 &= mask;
                g3 &= mask;
                g4 &= mask;
                mask = ~mask;
                h0 = (h0 & mask) | g0;
                h1 = (h1 & mask) | g1;
                h2 = (h2 & mask) | g2;
                h3 = (h3 & mask) | g3;
                h4 = (h4 & mask) | g4;

                // h = h % 2^128, then add s.
                h0 = (h0 | (h1 << 26));
                h1 = ((h1 >> 6) | (h2 << 20));
                h2 = ((h2 >> 12) | (h3 << 14));
                h3 = ((h3 >> 18) | (h4 << 8));

                uint64_t f = static_cast<uint64_t>(h0) + m_pad[0];
                StoreLE32(tag + 0, static_cast<uint32_t>(f));
                f = static_cast<uint64_t>(h1) + m_pad[1] + (f >> 32);
                StoreLE32(tag + 4, static_cast<uint32_t>(f));
                f = static_cast<uint64_t>(h2) + m_pad[2] + (f >> 32);
                StoreLE32(tag + 8, static_cast<uint32_t>(f));
                f = static_cast<uint64_t>(h3) + m_pad[3] + (f >> 32);
                StoreLE32(tag + 12, static_cast<uint32_t>(f));
            }

          private:
            void Block(const uint8_t* m, uint32_t hibit)
            {
                const uint32_t r0 = m_r[0], r1 = m_r[1], r2 = m_r[2], r3 = m_r[3], r4 = m_r[4];
                const uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;

                uint32_t h0 = m_h[0] + (LoadLE32(m + 0) & 0x3ffffff);
                uint32_t h1 = m_h[1] + ((LoadLE32(m + 3) >> 2) & 0x3ffffff);
                uint32_t h2 = m_h[2] + ((LoadLE32(m + 6) >> 4) & 0x3ffffff);
                uint32_t h3 = m_h[3] + ((LoadLE32(m + 9) >> 6) & 0x3ffffff);
                uint32_t h4 = m_h[4] + ((LoadLE32(m + 12) >> 8) | hibit);

                using U64 = uint64_t;
                U64 d0 = U64(h0) * r0 + U64(h1) * s4 + U64(h2) * s3 + U64(h3) * s2 + U64(h4) * s1;
                U64 d1 = U64(h0) * r1 + U64(h1) * r0 + U64(h2) * s4 + U64(h3) * s3 + U64(h4) * s2;
                U64 d2 = U64(h0) * r2 + U64(h1) * r1 + U64(h2) * r0 + U64(h3) * s4 + U64(h4) * s3;
                U64 d3 = U64(h0) * r3 + U64(h1) * r2 + U64(h2) * r1 + U64(h3) * r0 + U64(h4) * s4;
                U64 d4 = U64(h0) * r4 + U64(h1) * r3 + U64(h2) * r2 + U64(h3) * r1 + U64(h4) * r0;

                // Partial reduction mod 2^130 - 5.
                uint32_t c = static_cast<uint32_t>(d0 >> 26);
                h0 = static_cast<uint32_t>(d0) & 0x3ffffff;
                d1 += c;
                c = static_cast<uint32_t>(d1 >> 26);
                h1 = static_cast<uint32_t>(d1) & 0x3ffffff;
                d2 += c;
                c = static_cast<uint32_t>(d2 >> 26);
                h2 = static_cast<uint32_t>(d2) & 0x3ffffff;
                d3 += c;
                c = static_cast<uint32_t>(d3 >> 26);
                h3 = static_cast<uint32_t>(d3) & 0x3ffffff;
                d4 += c;
                c = static_cast<uint32_t>(d4 >> 26);
                h4 = static_cast<uint32_t>(d4) & 0x3ffffff;
                h0 += c * 5;
                c = h0 >> 26;
                h0 &= 0x3ffffff;
                h1 += c;

                m_h[0] = h0;
                m_h[1] = h1;
                m_h[2] = h2;
                m_h[3] = h3;
                m_h[4] = h4;
            }

            uint32_t m_r[5]{};
            uint32_t m_h[5]{};
            uint32_t m_pad[4]{};
            uint8_t m_buffer[16]{};
            size_t m_bufferLen = 0;
        };

        /// AEAD tag over aad || pad16 || ciphertext || pad16 || le64(len aad) || le64(len ct) (RFC 8439 2.8).
        void ComputeAeadTag(const SessionKey& key, const AeadNonce& nonce, std::span<const uint8_t> aad,
                            const uint8_t* ciphertext, size_t ciphertextLen, uint8_t tag[16])
        {
            uint8_t block0[64];
            ChaCha20Block(key, 0, nonce, block0);
            Poly1305 mac(block0); // one-time key = first 32 bytes of block 0
            SecureWipe(block0, sizeof(block0));

            mac.Update(aad.data(), aad.size());
            mac.PadTo16();
            mac.Update(ciphertext, ciphertextLen);
            mac.PadTo16();
            uint8_t lengths[16];
            StoreLE64(lengths, aad.size());
            StoreLE64(lengths + 8, ciphertextLen);
            mac.Update(lengths, sizeof(lengths));
            mac.Finish(tag);
        }

    } // namespace

    bool ConstantTimeEqual(std::span<const uint8_t> a, std::span<const uint8_t> b)
    {
        if (a.size() != b.size())
            return false;
        uint8_t diff = 0;
        for (size_t i = 0; i < a.size(); ++i)
            diff |= static_cast<uint8_t>(a[i] ^ b[i]);
        return diff == 0;
    }

    // ============================================================================
    // RFC 8439 AEAD
    // ============================================================================

    std::vector<uint8_t> ChaCha20Poly1305Seal(const SessionKey& key, const AeadNonce& nonce,
                                              std::span<const uint8_t> aad, std::span<const uint8_t> plaintext)
    {
        std::vector<uint8_t> out(plaintext.size() + AEAD_TAG_SIZE);
        ChaCha20Xor(key, 1, nonce, plaintext.data(), out.data(), plaintext.size());
        ComputeAeadTag(key, nonce, aad, out.data(), plaintext.size(), out.data() + plaintext.size());
        return out;
    }

    bool ChaCha20Poly1305Open(const SessionKey& key, const AeadNonce& nonce, std::span<const uint8_t> aad,
                              std::span<const uint8_t> ciphertextAndTag, std::vector<uint8_t>& outPlaintext)
    {
        outPlaintext.clear();
        if (ciphertextAndTag.size() < AEAD_TAG_SIZE)
            return false;

        const size_t ciphertextLen = ciphertextAndTag.size() - AEAD_TAG_SIZE;
        uint8_t expectedTag[AEAD_TAG_SIZE];
        ComputeAeadTag(key, nonce, aad, ciphertextAndTag.data(), ciphertextLen, expectedTag);
        if (!ConstantTimeEqual(expectedTag, ciphertextAndTag.subspan(ciphertextLen)))
            return false;

        // Decrypt only after the tag verified, so unauthenticated plaintext is never released.
        outPlaintext.resize(ciphertextLen);
        ChaCha20Xor(key, 1, nonce, ciphertextAndTag.data(), outPlaintext.data(), ciphertextLen);
        return true;
    }

} // namespace Spark::Net
