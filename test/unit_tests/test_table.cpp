
#include "calicodb/db.h"
#include "calicodb/table.h"
#include "db_impl.h"
#include "table_impl.h"
#include "unit_tests.h"

namespace calicodb {

static auto print_tables(const TableSet &set)
{
    for (const auto &[table_id, state] : set) {
        std::cerr << "table_id: " << table_id.value << '\n';
        std::cerr << "  iopn: " << state.is_open << '\n';
        std::cerr << "  ttid: " << state.root_id.table_id.value << '\n';
        std::cerr << "  trid: " << state.root_id.page_id.value << '\n';
        std::cerr << "  tree: " << state.tree << "\n\n";
    }
}

static auto db_impl(DB *db) -> DBImpl *
{
    return reinterpret_cast<DBImpl *>(db);
}

static auto table_impl(Table *table) -> TableImpl *
{
    return reinterpret_cast<TableImpl *>(table);
}

class TableTests
    : public InMemoryTest,
      public testing::Test
{
public:
    TableTests()
    {
        options.page_size = kMinPageSize;
        options.cache_size = kMinPageSize * 16;
        options.env = env.get();
    }

    ~TableTests() override = default;

    auto SetUp() -> void override
    {
        ASSERT_OK(DB::open(options, kFilename, &db));
        ASSERT_OK(TableTests::reopen());
    }

    auto TearDown() -> void override
    {
        delete table;
        ASSERT_OK(db->status());
    }

    virtual auto reopen() -> Status
    {
        delete table;
        table = nullptr;

        return db->new_table({}, "table", &table);
    }

    Options options;
    DB *db {};
    Table *table {};
};

TEST_F(TableTests, TablesAreRegistered)
{
    const auto &tables = db_impl(db)->TEST_tables();
    ASSERT_NE(tables.get(Id {1}), nullptr) << "cannot locate root table";
    ASSERT_NE(tables.get(Id {2}), nullptr) << "cannot locate non-root table";
}

TEST_F(TableTests, TablesMustBeUnique)
{
    Table *same;
    ASSERT_TRUE(db->new_table({}, "table", &same).is_invalid_argument());
}

TEST_F(TableTests, EmptyTableGetsRemovedDuringVacuum)
{
    // Root page of "table" and the pointer map page on page 2 should be removed.
    ASSERT_EQ(db_impl(db)->pager->page_count(), 3);
    ASSERT_OK(db->vacuum());
    ASSERT_EQ(db_impl(db)->pager->page_count(), 1);
}

TEST_F(TableTests, TableCreationIsPartOfTransaction)
{
    delete table;
    table = nullptr;

    delete db;
    db = nullptr;

    ASSERT_OK(DB::open(options, kFilename, &db));
    reopen();

    ASSERT_NE(db_impl(db)->TEST_tables().get(Id {1}), nullptr);
    ASSERT_EQ(db_impl(db)->TEST_tables().get(Id {2}), nullptr);
}

class TwoTableTests : public TableTests
{
public:
    ~TwoTableTests() override = default;

    auto SetUp() -> void override
    {
        TableTests::SetUp();
        table_1 = table;
        print_tables(db_impl(db)->TEST_tables());

        ASSERT_OK(db->new_table({}, "table_2", &table_2));
    }

    auto TearDown() -> void override
    {
        TableTests::TearDown();
        delete table_2;
        ASSERT_OK(db->status());
    }

    auto reopen() -> Status override
    {
        if (auto s = TableTests::reopen(); !s.is_ok()) {
            return s;
        }
        delete table_2;
        table_2 = nullptr;

        return db->new_table({}, "table_2", &table);
    }

    Table *table_1 {};
    Table *table_2 {};
};

TEST_F(TwoTableTests, TablesHaveIndependentKeys)
{
    ASSERT_OK(table_1->put("key", "1"));
    ASSERT_OK(table_2->put("key", "2"));

    std::string value;
    ASSERT_OK(table_1->get("key", &value));
    ASSERT_EQ(value, "1");
    ASSERT_OK(table_2->get("key", &value));
    ASSERT_EQ(value, "2");
}

TEST_F(TwoTableTests, EmptyTableGetsRemovedDuringVacuum)
{
    ASSERT_OK(table_2->put("k", "v"));

    // Root page of "table_1" should be removed, leaving the database root page, the
    // pointer map page on page 2, and the root page of "table_2".
    ASSERT_EQ(db_impl(db)->pager->page_count(), 4);
    ASSERT_OK(db->vacuum());
    ASSERT_EQ(db_impl(db)->pager->page_count(), 3);
}

} // namespace calicodb