/**
 * @file FuzzBinaryReaderProduction.cpp
 * @brief libc++-compiled production adapter for the Spark::BinaryReader libFuzzer harness.
 *
 * Input layout: byte 0 is the op-script length K, the next K bytes are the op
 * script, and every remaining byte is the data buffer handed to the shipped
 * BinaryReader(const uint8_t*, size_t) (copied into an exactly sized heap
 * buffer so ASan sees any read past its end). Each op is one reader call:
 *
 *   op % 10: 0 Read<u8>  1 Read<u16>  2 Read<u32>  3 Read<u64>  4 Read<float>
 *            5 Read<double>  6 ReadString  7 ReadBytes(n)  8 Skip(n)
 *            9 Skip(SIZE_MAX - k)
 *
 * where n is the next two script bytes (little-endian; 0xFFFF means SIZE_MAX)
 * and k the next script byte. An independent shadow model (a cursor and an
 * error flag) predicts every call, and a violated contract aborts so libFuzzer
 * records a crash rather than a silent pass:
 *  - each result and the reader's Tell/Size/Remaining/HasError/IsEOF equal
 *    the model after every op, and Tell() never passes Size(),
 *  - values decode as little-endian data bytes; after an error every Read
 *    returns zero, ReadString returns empty and Remaining() is 0,
 *  - a ReadString result never holds more bytes than were available,
 *  - writing every successfully read value back through BinaryWriter
 *    reproduces exactly the data bytes those reads consumed.
 */

#include "FuzzBinaryReaderProduction.h"

#include "Utils/Serializer.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzBinaryReader: reader violated: %s\n", what);
        std::abort();
    }

    /// The shadow reader: what BinaryReader's documented contract says must happen.
    struct Model
    {
        const std::vector<std::uint8_t>& data;
        std::size_t cursor = 0;
        bool error = false;

        bool Consume(std::size_t count)
        {
            if (error || count > data.size() - cursor)
            {
                error = true;
                return false;
            }
            cursor += count;
            return true;
        }

        /// Little-endian value of the @p width bytes ending at the cursor.
        std::uint64_t Value(std::size_t width) const
        {
            std::uint64_t value = 0;
            for (std::size_t index = 0; index < width; ++index)
                value |= static_cast<std::uint64_t>(data[cursor - width + index]) << (8 * index);
            return value;
        }
    };

    /// Reads the script's operand bytes; missing bytes read as zero.
    struct Script
    {
        const std::uint8_t* bytes;
        std::size_t size;
        std::size_t position = 0;

        bool Done() const { return position >= size; }
        std::uint8_t Next() { return position < size ? bytes[position++] : 0; }
        std::size_t Length()
        {
            const std::size_t low = Next();
            const std::size_t value = low | (static_cast<std::size_t>(Next()) << 8);
            return value == 0xFFFF ? std::numeric_limits<std::size_t>::max() : value;
        }
    };

    template <typename T>
    void CheckRead(Spark::BinaryReader& reader, Model& model, Spark::BinaryWriter& writer,
                   std::vector<std::uint8_t>& consumed)
    {
        const std::size_t before = model.cursor;
        const T actual = reader.Read<T>();
        std::uint64_t actualBits = 0;
        std::memcpy(&actualBits, &actual, sizeof(T));
        if (!model.Consume(sizeof(T)))
        {
            if (actualBits != 0)
                InvariantFailure("a failed Read returned a non-zero value");
            return;
        }
        if (actualBits != model.Value(sizeof(T)))
            InvariantFailure("Read is not the little-endian value of the data bytes");
        writer.Write<T>(actual);
        consumed.insert(consumed.end(), model.data.begin() + static_cast<std::ptrdiff_t>(before),
                        model.data.begin() + static_cast<std::ptrdiff_t>(model.cursor));
    }

    void CheckState(const Spark::BinaryReader& reader, const Model& model)
    {
        if (reader.Tell() != model.cursor || reader.Size() != model.data.size() || reader.Tell() > reader.Size())
            InvariantFailure("Tell/Size disagree with the model");
        if (reader.HasError() != model.error)
            InvariantFailure("HasError disagrees with the model");
        if (reader.Remaining() != (model.error ? 0 : model.data.size() - model.cursor))
            InvariantFailure("Remaining disagrees with the model");
        if (reader.IsEOF() != (!model.error && model.cursor >= model.data.size()))
            InvariantFailure("IsEOF disagrees with the model");
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled production reader.
extern "C" int SparkFuzzRunBinaryReaderScript(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    if (data == nullptr || size == 0)
        return 0;

    const std::size_t scriptSize = data[0] < size - 1 ? data[0] : size - 1;
    Script script{data + 1, scriptSize};
    const std::vector<std::uint8_t> buffer(data + 1 + scriptSize, data + size);

    Spark::BinaryReader reader(buffer.data(), buffer.size());
    Model model{buffer};
    Spark::BinaryWriter writer;
    std::vector<std::uint8_t> consumed;
    std::vector<std::uint8_t> scratch;

    while (!script.Done())
    {
        switch (script.Next() % 10u)
        {
        case 0:
            CheckRead<std::uint8_t>(reader, model, writer, consumed);
            break;
        case 1:
            CheckRead<std::uint16_t>(reader, model, writer, consumed);
            break;
        case 2:
            CheckRead<std::uint32_t>(reader, model, writer, consumed);
            break;
        case 3:
            CheckRead<std::uint64_t>(reader, model, writer, consumed);
            break;
        case 4:
            CheckRead<float>(reader, model, writer, consumed);
            break;
        case 5:
            CheckRead<double>(reader, model, writer, consumed);
            break;
        case 6:
        {
            const std::size_t before = model.cursor;
            const std::size_t available = model.error ? 0 : buffer.size() - model.cursor;
            const std::string actual = reader.ReadString();
            if (actual.size() > available)
                InvariantFailure("ReadString returned more bytes than were available");
            std::size_t length = 0;
            if (model.Consume(sizeof(std::uint32_t)))
                length = static_cast<std::size_t>(model.Value(sizeof(std::uint32_t)));
            if (!model.Consume(length))
            {
                if (!actual.empty())
                    InvariantFailure("a failed ReadString returned data");
                break;
            }
            if (actual.size() != length ||
                std::memcmp(actual.data(), buffer.data() + model.cursor - length, length) != 0)
                InvariantFailure("ReadString differs from its data bytes");
            writer.WriteString(actual);
            consumed.insert(consumed.end(), buffer.begin() + static_cast<std::ptrdiff_t>(before),
                            buffer.begin() + static_cast<std::ptrdiff_t>(model.cursor));
            break;
        }
        case 7:
        {
            const std::size_t count = script.Length();
            if (count > buffer.size())
            {
                // Never allocate the claimed size; a request past the buffer must fail.
                std::uint8_t probe = 0;
                if (reader.ReadBytes(&probe, count) || model.Consume(count))
                    InvariantFailure("ReadBytes accepted a length past the buffer");
                break;
            }
            scratch.assign(count, 0);
            const std::size_t before = model.cursor;
            const bool actual = reader.ReadBytes(scratch.data(), count);
            // A zero-byte read succeeds without touching the reader, even after an error.
            const bool expected = count == 0 || model.Consume(count);
            if (actual != expected)
                InvariantFailure("ReadBytes result disagrees with the model");
            if (actual && count != 0)
            {
                if (std::memcmp(scratch.data(), buffer.data() + before, count) != 0)
                    InvariantFailure("ReadBytes copied bytes other than the data bytes");
                writer.WriteBytes(scratch.data(), count);
                consumed.insert(consumed.end(), scratch.begin(), scratch.end());
            }
            break;
        }
        case 8:
        {
            const std::size_t count = script.Length();
            if (reader.Skip(count) != model.Consume(count))
                InvariantFailure("Skip result disagrees with the model");
            break;
        }
        default:
        {
            const std::size_t count = std::numeric_limits<std::size_t>::max() - script.Next();
            if (reader.Skip(count) != model.Consume(count))
                InvariantFailure("Skip near SIZE_MAX disagrees with the model");
            break;
        }
        }
        CheckState(reader, model);
    }

    if (writer.GetBuffer() != consumed)
        InvariantFailure("BinaryWriter does not reproduce the bytes the reads consumed");
    return 0;
}
