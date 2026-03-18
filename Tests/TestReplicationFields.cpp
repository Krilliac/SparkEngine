// TestReplicationFields.cpp - Tests for replicated field dirty-tracking and serialization
// Standalone reimplementation to avoid platform-specific header dependencies

#include "TestFramework.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// ============================================================================
// Standalone reimplementation of Spark::Net::ReplicatedField<T> and ReplicatedFieldSet
// ============================================================================

namespace
{

    enum class FieldVisibility : uint8_t
    {
        Public,  // Visible to all clients
        Private, // Visible only to the owning client
        Party    // Visible to party members
    };

    template <typename T> class ReplicatedField
    {
      public:
        ReplicatedField() = default;
        explicit ReplicatedField(T initial, FieldVisibility vis = FieldVisibility::Public)
            : m_value(std::move(initial)), m_visibility(vis)
        {
        }

        const T& Get() const { return m_value; }

        void Set(const T& val)
        {
            if (!(m_value == val))
            {
                m_value = val;
                m_dirty = true;
            }
        }

        bool IsDirty() const { return m_dirty; }

        void ClearDirty() { m_dirty = false; }

        FieldVisibility GetVisibility() const { return m_visibility; }

        void SetVisibility(FieldVisibility vis) { m_visibility = vis; }

      private:
        T m_value{};
        bool m_dirty = false;
        FieldVisibility m_visibility = FieldVisibility::Public;
    };

    // Simple serialization buffer for round-trip tests
    class FieldBuffer
    {
      public:
        void WriteInt(int val) { WriteRaw(&val, sizeof(val)); }
        void WriteFloat(float val) { WriteRaw(&val, sizeof(val)); }
        void WriteString(const std::string& val)
        {
            uint32_t len = static_cast<uint32_t>(val.size());
            WriteRaw(&len, sizeof(len));
            if (len > 0)
                WriteRaw(val.data(), len);
        }

        int ReadInt()
        {
            int val = 0;
            ReadRaw(&val, sizeof(val));
            return val;
        }
        float ReadFloat()
        {
            float val = 0.0f;
            ReadRaw(&val, sizeof(val));
            return val;
        }
        std::string ReadString()
        {
            uint32_t len = 0;
            ReadRaw(&len, sizeof(len));
            std::string val(len, '\0');
            if (len > 0)
                ReadRaw(val.data(), len);
            return val;
        }

      private:
        void WriteRaw(const void* data, size_t size)
        {
            const auto* bytes = static_cast<const uint8_t*>(data);
            m_data.insert(m_data.end(), bytes, bytes + size);
        }
        void ReadRaw(void* data, size_t size)
        {
            if (m_readPos + size <= m_data.size())
            {
                std::memcpy(data, m_data.data() + m_readPos, size);
                m_readPos += size;
            }
        }

        std::vector<uint8_t> m_data;
        size_t m_readPos = 0;
    };

    // Tracks a set of replicated fields with a bitmask
    class ReplicatedFieldSet
    {
      public:
        struct FieldEntry
        {
            std::string name;
            FieldVisibility visibility = FieldVisibility::Public;
            bool dirty = false;
        };

        uint32_t RegisterField(const std::string& name, FieldVisibility vis = FieldVisibility::Public)
        {
            uint32_t index = static_cast<uint32_t>(m_fields.size());
            m_fields.push_back({name, vis, false});
            return index;
        }

        void MarkDirty(uint32_t index)
        {
            if (index < m_fields.size())
                m_fields[index].dirty = true;
        }

        void ClearDirty(uint32_t index)
        {
            if (index < m_fields.size())
                m_fields[index].dirty = false;
        }

        void ClearAllDirty()
        {
            for (auto& f : m_fields)
                f.dirty = false;
        }

        uint32_t GetFieldCount() const { return static_cast<uint32_t>(m_fields.size()); }

        uint64_t GetDirtyMask() const
        {
            uint64_t mask = 0;
            for (size_t i = 0; i < m_fields.size() && i < 64; ++i)
            {
                if (m_fields[i].dirty)
                    mask |= (1ULL << i);
            }
            return mask;
        }

        uint64_t GetDirtyMaskForVisibility(FieldVisibility vis) const
        {
            uint64_t mask = 0;
            for (size_t i = 0; i < m_fields.size() && i < 64; ++i)
            {
                if (m_fields[i].dirty && m_fields[i].visibility == vis)
                    mask |= (1ULL << i);
            }
            return mask;
        }

      private:
        std::vector<FieldEntry> m_fields;
    };

} // namespace

// ============================================================================
// ReplicatedField<T> dirty tracking
// ============================================================================

TEST(ReplicatedField_SetValueMarksDirty)
{
    ReplicatedField<int> field(0);
    EXPECT_FALSE(field.IsDirty());

    field.Set(42);
    EXPECT_TRUE(field.IsDirty());
    EXPECT_EQ(field.Get(), 42);
}

TEST(ReplicatedField_SetSameValueNotDirty)
{
    ReplicatedField<int> field(10);
    field.Set(10);
    EXPECT_FALSE(field.IsDirty());
}

TEST(ReplicatedField_ClearDirty)
{
    ReplicatedField<int> field(0);
    field.Set(5);
    EXPECT_TRUE(field.IsDirty());

    field.ClearDirty();
    EXPECT_FALSE(field.IsDirty());
}

// ============================================================================
// Visibility filtering
// ============================================================================

TEST(ReplicatedField_DefaultVisibilityPublic)
{
    ReplicatedField<float> field;
    EXPECT_TRUE(field.GetVisibility() == FieldVisibility::Public);
}

TEST(ReplicatedField_VisibilityConstructor)
{
    ReplicatedField<int> field(0, FieldVisibility::Private);
    EXPECT_TRUE(field.GetVisibility() == FieldVisibility::Private);
}

TEST(ReplicatedField_ChangeVisibility)
{
    ReplicatedField<int> field(0, FieldVisibility::Public);
    field.SetVisibility(FieldVisibility::Party);
    EXPECT_TRUE(field.GetVisibility() == FieldVisibility::Party);
}

// ============================================================================
// Serialize / Deserialize round-trip
// ============================================================================

TEST(ReplicatedField_RoundTrip_Int)
{
    ReplicatedField<int> original(0);
    original.Set(12345);

    FieldBuffer buf;
    buf.WriteInt(original.Get());

    int deserialized = buf.ReadInt();
    EXPECT_EQ(deserialized, 12345);
}

TEST(ReplicatedField_RoundTrip_Float)
{
    ReplicatedField<float> original(0.0f);
    original.Set(3.14159f);

    FieldBuffer buf;
    buf.WriteFloat(original.Get());

    float deserialized = buf.ReadFloat();
    EXPECT_NEAR(deserialized, 3.14159f, 0.0001f);
}

TEST(ReplicatedField_RoundTrip_String)
{
    ReplicatedField<std::string> original(std::string(""));
    original.Set("Hello, Replication!");

    FieldBuffer buf;
    buf.WriteString(original.Get());

    std::string deserialized = buf.ReadString();
    EXPECT_EQ(deserialized, std::string("Hello, Replication!"));
}

TEST(ReplicatedField_RoundTrip_EmptyString)
{
    FieldBuffer buf;
    buf.WriteString("");

    std::string deserialized = buf.ReadString();
    EXPECT_EQ(deserialized, std::string(""));
}

// ============================================================================
// ReplicatedFieldSet
// ============================================================================

TEST(ReplicatedFieldSet_RegisterAndCount)
{
    ReplicatedFieldSet set;
    EXPECT_EQ(set.GetFieldCount(), 0u);

    set.RegisterField("Health");
    set.RegisterField("Mana");
    set.RegisterField("Position");

    EXPECT_EQ(set.GetFieldCount(), 3u);
}

TEST(ReplicatedFieldSet_DirtyMask)
{
    ReplicatedFieldSet set;
    uint32_t health = set.RegisterField("Health");
    uint32_t mana = set.RegisterField("Mana");
    uint32_t pos = set.RegisterField("Position");

    EXPECT_EQ(set.GetDirtyMask(), 0u);

    set.MarkDirty(health);
    EXPECT_EQ(set.GetDirtyMask(), 1u); // bit 0

    set.MarkDirty(pos);
    EXPECT_EQ(set.GetDirtyMask(), 5u); // bit 0 + bit 2

    set.ClearDirty(health);
    EXPECT_EQ(set.GetDirtyMask(), 4u); // bit 2 only
}

TEST(ReplicatedFieldSet_ClearAllDirty)
{
    ReplicatedFieldSet set;
    uint32_t a = set.RegisterField("A");
    uint32_t b = set.RegisterField("B");

    set.MarkDirty(a);
    set.MarkDirty(b);
    EXPECT_NE(set.GetDirtyMask(), 0u);

    set.ClearAllDirty();
    EXPECT_EQ(set.GetDirtyMask(), 0u);
}

TEST(ReplicatedFieldSet_DirtyMaskForVisibility)
{
    ReplicatedFieldSet set;
    uint32_t health = set.RegisterField("Health", FieldVisibility::Public);
    uint32_t secret = set.RegisterField("Secret", FieldVisibility::Private);
    uint32_t partyBuff = set.RegisterField("PartyBuff", FieldVisibility::Party);

    set.MarkDirty(health);
    set.MarkDirty(secret);
    set.MarkDirty(partyBuff);

    uint64_t publicMask = set.GetDirtyMaskForVisibility(FieldVisibility::Public);
    uint64_t privateMask = set.GetDirtyMaskForVisibility(FieldVisibility::Private);
    uint64_t partyMask = set.GetDirtyMaskForVisibility(FieldVisibility::Party);

    EXPECT_EQ(publicMask, 1u);  // bit 0
    EXPECT_EQ(privateMask, 2u); // bit 1
    EXPECT_EQ(partyMask, 4u);   // bit 2
}

TEST(ReplicatedFieldSet_DirtyMaskForVisibility_NoMatches)
{
    ReplicatedFieldSet set;
    uint32_t a = set.RegisterField("A", FieldVisibility::Public);

    set.MarkDirty(a);

    uint64_t privateMask = set.GetDirtyMaskForVisibility(FieldVisibility::Private);
    EXPECT_EQ(privateMask, 0u);
}
