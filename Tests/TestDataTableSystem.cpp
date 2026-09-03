// TestDataTableSystem.cpp - Tests for Spark::Data::DataTableRegistry and DataTable
#include "TestFramework.h"
#include "Engine/DataTable/DataTableSystem.h"

// ============================================================================
// DataTable — basic operations
// ============================================================================

TEST(DataTable_LoadFromCSV_Simple)
{
    Spark::Data::DataTable table;
    bool ok = table.LoadFromCSV("id,name,value\n1,Sword,100\n2,Shield,50\n");
    EXPECT_TRUE(ok);
    EXPECT_EQ(static_cast<size_t>(3), table.GetColumnCount());
    EXPECT_EQ(static_cast<size_t>(2), table.GetRowCount());
}

TEST(DataTable_GetRow_ById)
{
    Spark::Data::DataTable table;
    table.LoadFromCSV("id,name,hp\n1,Goblin,30\n2,Dragon,500\n");
    const auto* row = table.GetRow("2");
    ASSERT_TRUE(row != nullptr);
    EXPECT_EQ(std::string("Dragon"), row->GetString("name"));
    EXPECT_EQ(500, row->GetInt("hp"));
}

TEST(DataTable_FindRows)
{
    Spark::Data::DataTable table;
    table.LoadFromCSV("id,type,name\n1,weapon,Sword\n2,armor,Shield\n3,weapon,Axe\n");
    auto results = table.FindRows("type", "weapon");
    EXPECT_EQ(static_cast<size_t>(2), results.size());
}

TEST(DataTable_QuotedCSVFields)
{
    Spark::Data::DataTable table;
    bool ok = table.LoadFromCSV("id,desc\n1,\"Hello, World\"\n2,\"She said \"\"hi\"\"\"\n");
    EXPECT_TRUE(ok);
    const auto* row1 = table.GetRow("1");
    ASSERT_TRUE(row1 != nullptr);
    EXPECT_EQ(std::string("Hello, World"), row1->GetString("desc"));
    const auto* row2 = table.GetRow("2");
    ASSERT_TRUE(row2 != nullptr);
    EXPECT_EQ(std::string("She said \"hi\""), row2->GetString("desc"));
}

TEST(DataTable_TypeDetection)
{
    Spark::Data::DataTable table;
    table.LoadFromCSV("id,count,price,active,label\n1,10,3.5,true,hello\n2,20,7.2,false,world\n");
    const auto& cols = table.GetColumns();
    EXPECT_EQ(static_cast<size_t>(5), cols.size());
    // id and count are ints, price is float, active is bool, label is string
    EXPECT_TRUE(cols[0].type == Spark::Data::ColumnType::Int);
    EXPECT_TRUE(cols[1].type == Spark::Data::ColumnType::Int);
    EXPECT_TRUE(cols[2].type == Spark::Data::ColumnType::Float);
    EXPECT_TRUE(cols[3].type == Spark::Data::ColumnType::Bool);
    EXPECT_TRUE(cols[4].type == Spark::Data::ColumnType::String);
}

TEST(DataTable_SaveToCSV_Roundtrip)
{
    Spark::Data::DataTable table;
    table.LoadFromCSV("id,name,value\n1,Sword,100\n2,Shield,50\n");
    std::string csv = table.SaveToCSV();

    Spark::Data::DataTable table2;
    bool ok = table2.LoadFromCSV(csv);
    EXPECT_TRUE(ok);
    EXPECT_EQ(table.GetRowCount(), table2.GetRowCount());
    EXPECT_EQ(table.GetColumnCount(), table2.GetColumnCount());
    const auto* row = table2.GetRow("1");
    ASSERT_TRUE(row != nullptr);
    EXPECT_EQ(std::string("Sword"), row->GetString("name"));
}

TEST(DataTable_RowTypedAccessors)
{
    Spark::Data::DataTable table;
    table.LoadFromCSV("id,hp,speed,active\n1,100,3.14,true\n");
    const auto* row = table.GetRow("1");
    ASSERT_TRUE(row != nullptr);
    EXPECT_EQ(100, row->GetInt("hp"));
    EXPECT_NEAR(3.14f, row->GetFloat("speed"), 0.01f);
    EXPECT_TRUE(row->GetBool("active"));
    EXPECT_FALSE(row->GetBool("missing_col"));
}

// ============================================================================
// DataTableRegistry — singleton operations
// ============================================================================

TEST(DataTableRegistry_Initialize)
{
    auto& reg = Spark::Data::DataTableRegistry::GetInstance();
    reg.Initialize();
    EXPECT_EQ(static_cast<size_t>(0), reg.GetTableCount());
    reg.Shutdown();
}

TEST(DataTableRegistry_RegisterAndLookup)
{
    auto& reg = Spark::Data::DataTableRegistry::GetInstance();
    reg.Initialize();

    Spark::Data::DataTable table;
    table.LoadFromCSV("id,name\n1,Test\n");
    reg.RegisterTable("items", std::move(table));

    EXPECT_EQ(static_cast<size_t>(1), reg.GetTableCount());
    auto* found = reg.GetTable("items");
    ASSERT_TRUE(found != nullptr);
    EXPECT_EQ(static_cast<size_t>(1), found->GetRowCount());
    EXPECT_TRUE(reg.GetTable("nonexistent") == nullptr);

    reg.Shutdown();
}

TEST(DataTableRegistry_UnloadTable)
{
    auto& reg = Spark::Data::DataTableRegistry::GetInstance();
    reg.Initialize();

    Spark::Data::DataTable table;
    table.LoadFromCSV("id,name\n1,A\n");
    reg.RegisterTable("t1", std::move(table));
    EXPECT_TRUE(reg.UnloadTable("t1"));
    EXPECT_FALSE(reg.UnloadTable("t1"));
    EXPECT_EQ(static_cast<size_t>(0), reg.GetTableCount());

    reg.Shutdown();
}

TEST(DataTableRegistry_GetTableNames)
{
    auto& reg = Spark::Data::DataTableRegistry::GetInstance();
    reg.Initialize();

    Spark::Data::DataTable t1, t2;
    t1.LoadFromCSV("id\n1\n");
    t2.LoadFromCSV("id\n2\n");
    reg.RegisterTable("alpha", std::move(t1));
    reg.RegisterTable("beta", std::move(t2));

    auto names = reg.GetTableNames();
    EXPECT_EQ(static_cast<size_t>(2), names.size());

    reg.Shutdown();
}
