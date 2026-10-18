/**
 * @file CrashSymbolication.h
 * @brief Signal-safe module identity and module-relative frame records for build-id symbolication.
 *
 * Module identity (name, GNU build-id, load bias, PT_LOAD range) is captured
 * outside the signal handler into fixed-capacity storage. The signal handler
 * only reads that storage and formats it into a caller-supplied buffer, so the
 * symbolication section adds no allocation, no ELF parsing and no loader calls
 * to the crash path. tools/ops/symbolicate_crash.py consumes the section and
 * resolves frames against a .build-id/xx/yyyy.debug symbol store.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Spark::CrashHandlerDetail
{
    inline constexpr size_t kMaxCrashModules = 512;
    inline constexpr size_t kMinCrashBuildIdBytes = 2; ///< Matches MIN_BUILD_ID_BYTES in tools/ops/symbolicate_crash.py
    inline constexpr size_t kMaxCrashBuildIdBytes = 64;
    inline constexpr size_t kMaxCrashModuleNameBytes = 128;
    inline constexpr size_t kMaxSymbolicCrashFrames = 64;

    /** @brief Identity of one loaded ELF module, precomputed before any crash. */
    struct CrashModuleIdentity
    {
        std::uintptr_t loadBias = 0;     ///< dl_iterate_phdr dlpi_addr: runtime address minus ELF vaddr
        std::uintptr_t beginAddress = 0; ///< First runtime address covered by a PT_LOAD segment
        std::uintptr_t endAddress = 0;   ///< One past the last runtime address covered by a PT_LOAD segment
        size_t buildIdSize = 0;          ///< 0 when the module carries no NT_GNU_BUILD_ID note
        std::array<std::uint8_t, kMaxCrashBuildIdBytes> buildId{};
        std::array<char, kMaxCrashModuleNameBytes> name{}; ///< NUL-terminated basename only (no user paths)
    };

    /**
     * @brief Find the NT_GNU_BUILD_ID descriptor in one ELF note region.
     *
     * Walks 4-byte-aligned ELF notes with every length checked against the
     * region, so a malformed note ends the walk instead of reading past it.
     *
     * @param notes Start of a PT_NOTE / SHT_NOTE region
     * @param size Size of the region in bytes
     * @param out Receives the build-id bytes
     * @return Number of build-id bytes copied, or 0 when none is present, the
     *         descriptor is shorter than kMinCrashBuildIdBytes (the store reader
     *         refuses such ids) or it does not fit in @p out
     */
    inline size_t FindGnuBuildIdNote(const std::uint8_t* notes, size_t size,
                                     std::array<std::uint8_t, kMaxCrashBuildIdBytes>& out)
    {
        constexpr std::uint32_t kNtGnuBuildId = 3;
        constexpr size_t kHeaderBytes = 12;
        const auto alignUp = [](size_t value) { return (value + 3u) & ~static_cast<size_t>(3u); };
        const auto readWord = [](const std::uint8_t* at)
        {
            std::uint32_t word = 0;
            std::memcpy(&word, at, sizeof(word)); // Notes use the host byte order of the loaded module
            return word;
        };

        size_t offset = 0;
        while (notes && size >= kHeaderBytes && offset <= size - kHeaderBytes)
        {
            const size_t nameSize = readWord(notes + offset);
            const size_t descSize = readWord(notes + offset + 4);
            const std::uint32_t type = readWord(notes + offset + 8);
            const size_t nameOffset = offset + kHeaderBytes;
            if (nameSize > size - nameOffset)
                return 0;
            const size_t descOffset = nameOffset + alignUp(nameSize);
            if (descOffset > size || descSize > size - descOffset)
                return 0;

            if (type == kNtGnuBuildId && nameSize == 4 && std::memcmp(notes + nameOffset, "GNU", 4) == 0)
            {
                if (descSize < kMinCrashBuildIdBytes || descSize > out.size())
                    return 0;
                std::memcpy(out.data(), notes + descOffset, descSize);
                return descSize;
            }

            const size_t next = descOffset + alignUp(descSize);
            if (next <= offset || next > size)
                return 0;
            offset = next;
        }
        return 0;
    }

    /** @brief Return the module whose PT_LOAD range contains @p address, or nullptr. */
    inline const CrashModuleIdentity* FindCrashModuleForAddress(const CrashModuleIdentity* modules, size_t moduleCount,
                                                                std::uintptr_t address)
    {
        for (size_t index = 0; index < moduleCount; ++index)
        {
            if (address >= modules[index].beginAddress && address < modules[index].endAddress)
                return &modules[index];
        }
        return nullptr;
    }

    /**
     * @brief Fixed-buffer text writer for the signal path.
     *
     * Appends whole lines only: a line that does not fit is dropped and the
     * writer stops, so a truncated section is still well-formed.
     */
    class CrashSectionWriter
    {
      public:
        CrashSectionWriter(char* buffer, size_t capacity) : m_buffer(buffer), m_capacity(capacity) {}

        void BeginLine() { m_lineStart = m_length; }

        void Text(const char* text)
        {
            while (text && *text)
                Put(*text++);
        }

        void Hex(std::uintptr_t value)
        {
            char digits[2 * sizeof(std::uintptr_t)];
            size_t count = 0;
            do
            {
                digits[count++] = "0123456789abcdef"[value & 0xfu];
                value >>= 4;
            } while (value != 0);
            Text("0x");
            while (count > 0)
                Put(digits[--count]);
        }

        void Decimal(size_t value)
        {
            char digits[3 * sizeof(size_t)];
            size_t count = 0;
            do
            {
                digits[count++] = static_cast<char>('0' + value % 10);
                value /= 10;
            } while (value != 0);
            while (count > 0)
                Put(digits[--count]);
        }

        void BuildId(const CrashModuleIdentity& module)
        {
            for (size_t index = 0; index < module.buildIdSize; ++index)
            {
                Put("0123456789abcdef"[module.buildId[index] >> 4]);
                Put("0123456789abcdef"[module.buildId[index] & 0xfu]);
            }
        }

        /** @brief Terminate the current line; returns false once the buffer is full. */
        bool EndLine()
        {
            Put('\n');
            if (m_overflow)
            {
                m_length = m_lineStart;
                m_full = true;
            }
            return !m_full;
        }

        [[nodiscard]] size_t Length() const { return m_length; }

      private:
        void Put(char character)
        {
            if (m_full || m_length >= m_capacity)
            {
                m_overflow = true;
                return;
            }
            m_buffer[m_length++] = character;
        }

        char* m_buffer;
        size_t m_capacity;
        size_t m_length = 0;
        size_t m_lineStart = 0;
        bool m_overflow = false;
        bool m_full = false;
    };

    /**
     * @brief Format the module-identity and module-relative frame sections.
     *
     * Output (one record per line, consumed by tools/ops/symbolicate_crash.py):
     * @code
     * *** SYMBOLIC FRAMES ***
     * MODULE <index> build_id=<hex> bias=0x<hex> name=<basename>
     * SYMFRAME <n> kind=<pc|ra> module=<index> offset=0x<hex>
     * SYMFRAME <n> kind=<pc|ra> module=- address=0x<hex>
     * *** END SYMBOLIC FRAMES ***
     * @endcode
     * `kind=pc` is an exact faulting instruction address; `kind=ra` is a return
     * address the symbolizer must step back into the call instruction. Only
     * modules that carry a build-id and contain at least one frame are listed.
     * Performs no allocation; safe to call from a signal handler.
     *
     * @return Bytes written to @p out (never more than @p capacity)
     */
    inline size_t FormatSymbolicCrashFrames(const CrashModuleIdentity* modules, size_t moduleCount,
                                            const std::uintptr_t* frames, size_t frameCount, bool firstFrameIsExactPc,
                                            char* out, size_t capacity)
    {
        CrashSectionWriter writer(out, capacity);
        writer.BeginLine();
        writer.Text("*** SYMBOLIC FRAMES ***");
        if (!writer.EndLine())
            return 0;

        frameCount = std::min(frameCount, kMaxSymbolicCrashFrames);
        std::array<const CrashModuleIdentity*, kMaxSymbolicCrashFrames> frameModules{};
        for (size_t frame = 0; frame < frameCount; ++frame)
        {
            const CrashModuleIdentity* module = FindCrashModuleForAddress(modules, moduleCount, frames[frame]);
            frameModules[frame] = (module && module->buildIdSize > 0) ? module : nullptr;
        }

        // Module records first, each listed once, so the reader can validate
        // every SYMFRAME module index against an already-declared record.
        for (size_t moduleIndex = 0; moduleIndex < moduleCount; ++moduleIndex)
        {
            const CrashModuleIdentity* module = &modules[moduleIndex];
            if (std::find(frameModules.begin(), frameModules.begin() + frameCount, module) ==
                frameModules.begin() + frameCount)
                continue;
            writer.BeginLine();
            writer.Text("MODULE ");
            writer.Decimal(moduleIndex);
            writer.Text(" build_id=");
            writer.BuildId(*module);
            writer.Text(" bias=");
            writer.Hex(module->loadBias);
            writer.Text(" name=");
            writer.Text(module->name.data());
            if (!writer.EndLine())
                return writer.Length();
        }

        for (size_t frame = 0; frame < frameCount; ++frame)
        {
            writer.BeginLine();
            writer.Text("SYMFRAME ");
            writer.Decimal(frame);
            writer.Text((frame == 0 && firstFrameIsExactPc) ? " kind=pc" : " kind=ra");
            if (const CrashModuleIdentity* module = frameModules[frame])
            {
                writer.Text(" module=");
                writer.Decimal(static_cast<size_t>(module - modules));
                writer.Text(" offset=");
                writer.Hex(frames[frame] - module->loadBias);
            }
            else
            {
                writer.Text(" module=- address=");
                writer.Hex(frames[frame]);
            }
            if (!writer.EndLine())
                return writer.Length();
        }

        writer.BeginLine();
        writer.Text("*** END SYMBOLIC FRAMES ***");
        writer.EndLine();
        return writer.Length();
    }
} // namespace Spark::CrashHandlerDetail
