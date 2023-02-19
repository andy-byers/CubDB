#include "pager/pager.h"
#include "tree/cursor_internal.h"
#include "tree/node.h"
#include "tree/memory.h"
#include "tree/tree.h"
#include "unit_tests.h"
#include "wal/helpers.h"
#include <gtest/gtest.h>

namespace Calico {

class HeaderTests: public testing::Test {
protected:
    HeaderTests()
        : backing(0x200, '\x00'),
          page {Page {Id::root(), backing, true}}
    {}

    std::string backing;
    Page page;
};

TEST_F(HeaderTests, FileHeader)
{
    FileHeader source;
    source.magic_code = 1;
    source.page_count = 3;
    source.record_count = 4;
    source.freelist_head.value = 5;
    source.recovery_lsn.value = 6;
    source.page_size = backing.size();
    source.header_crc = source.compute_crc();

    source.write(page);
    // Write a node header to make sure it doesn't overwrite the file header memory.
    NodeHeader unused;
    unused.write(page);
    FileHeader target {page};

    ASSERT_EQ(source.magic_code, target.magic_code);
    ASSERT_EQ(source.header_crc, target.header_crc);
    ASSERT_EQ(source.page_count, target.page_count);
    ASSERT_EQ(source.record_count, target.record_count);
    ASSERT_EQ(source.freelist_head, target.freelist_head);
    ASSERT_EQ(source.recovery_lsn, target.recovery_lsn);
    ASSERT_EQ(source.page_size, target.page_size);
    ASSERT_EQ(source.compute_crc(), target.header_crc);
}

TEST_F(HeaderTests, NodeHeader)
{
    NodeHeader source;
    source.page_lsn.value = 1;
    source.next_id.value = 3;
    source.prev_id.value = 4;
    source.cell_count = 5;
    source.cell_start = 6;
    source.frag_count = 7;
    source.free_start = 8;
    source.free_total = 9;
    source.is_external = false;

    source.write(page);
    FileHeader unused;
    unused.write(page);
    NodeHeader target {page};

    ASSERT_EQ(source.page_lsn, target.page_lsn);
    ASSERT_EQ(source.next_id, target.next_id);
    ASSERT_EQ(source.prev_id, target.prev_id);
    ASSERT_EQ(source.cell_count, target.cell_count);
    ASSERT_EQ(source.cell_start, target.cell_start);
    ASSERT_EQ(source.frag_count, target.frag_count);
    ASSERT_EQ(source.free_start, target.free_start);
    ASSERT_EQ(source.free_total, target.free_total);
    ASSERT_EQ(source.is_external, target.is_external);
}

class NodeMetaManager {
    NodeMeta m_external_meta;
    NodeMeta m_internal_meta;

public:
    explicit NodeMetaManager(Size page_size)
    {
        // min_local and max_local fields are only needed in external nodes.
        m_external_meta.min_local = compute_min_local(page_size);
        m_external_meta.max_local = compute_max_local(page_size);

        m_external_meta.cell_size = external_cell_size;
        m_external_meta.parse_cell = parse_external_cell;

        m_internal_meta.cell_size = internal_cell_size;
        m_internal_meta.parse_cell = parse_internal_cell;
    }

    [[nodiscard]]
    auto operator()(bool is_external) const -> const NodeMeta &
    {
        return is_external ? m_external_meta : m_internal_meta;
    }
};

TEST(NodeSlotTests, SlotsAreConsistent)
{
    std::string backing(0x200, '\x00');
    std::string scratch(0x200, '\x00');
    Page page {Id::root(), backing, true};
    Node node {std::move(page), scratch.data()};
    const auto space = usable_space(node);

    node.insert_slot(0, 2);
    node.insert_slot(1, 4);
    node.insert_slot(1, 3);
    node.insert_slot(0, 1);
    ASSERT_EQ(usable_space(node), space - 8);

    node.set_slot(0, node.get_slot(0) + 1);
    node.set_slot(1, node.get_slot(1) + 1);
    node.set_slot(2, node.get_slot(2) + 1);
    node.set_slot(3, node.get_slot(3) + 1);

    ASSERT_EQ(node.get_slot(0), 2);
    ASSERT_EQ(node.get_slot(1), 3);
    ASSERT_EQ(node.get_slot(2), 4);
    ASSERT_EQ(node.get_slot(3), 5);

    node.remove_slot(0);
    ASSERT_EQ(node.get_slot(0), 3);
    node.remove_slot(0);
    ASSERT_EQ(node.get_slot(0), 4);
    node.remove_slot(0);
    ASSERT_EQ(node.get_slot(0), 5);
    node.remove_slot(0);
    ASSERT_EQ(usable_space(node), space);
}

class TestWithPager: public InMemoryTest {
public:
    const Size PAGE_SIZE {0x200};

    TestWithPager()
        : scratch(PAGE_SIZE, '\x00'),
          log_scratch(wal_scratch_size(PAGE_SIZE), '\x00')
    {}

    auto SetUp() -> void override
    {
        auto r = Pager::open({
            PREFIX,
            storage.get(),
            &log_scratch,
            &wal,
            nullptr,
            &status,
            &commit_lsn,
            &in_xact,
            8,
            PAGE_SIZE,
        });
        ASSERT_TRUE(r.has_value()) << r.error().what().data();
        pager = std::move(*r);
    }

    std::string log_scratch;
    Status status;
    bool in_xact {true};
    Lsn commit_lsn;
    DisabledWriteAheadLog wal;
    std::string scratch;
    std::string collect_scratch;
    std::unique_ptr<Pager> pager;
    Tools::RandomGenerator random {1'024 * 1'024 * 8};
};

class ComponentTests: public TestWithPager {
public:
    ~ComponentTests() override = default;

    auto SetUp() -> void override
    {
        collect_scratch.resize(PAGE_SIZE);

        TestWithPager::SetUp();
        pointers = std::make_unique<PointerMap>(*pager);
        freelist = std::make_unique<FreeList>(*pager, *pointers);
        overflow = std::make_unique<OverflowList>(*pager, *freelist, *pointers);
        payloads = std::make_unique<PayloadManager>(meta(true), *overflow);

        auto root = pager->allocate().value();
        Node node {std::move(root), scratch.data()};
        node.header.is_external = true;
        node.header.write(node.page);
        pager->release(std::move(node.page));
    }

    auto acquire_node(Id pid, bool writable = false)
    {
        Node node {pager->acquire(pid).value(), scratch.data()};
        node.meta = &meta(node.header.is_external);
        if (writable) {
            pager->upgrade(node.page);
        }
        return node;
    }

    auto release_node(Node node) const
    {
        if (node.page.is_writable()) {
            node.header.write(node.page);
        }
        pager->release(std::move(node).take());
    }

    NodeMetaManager meta {PAGE_SIZE};
    std::unique_ptr<PointerMap> pointers;
    std::unique_ptr<FreeList> freelist;
    std::unique_ptr<OverflowList> overflow;
    std::unique_ptr<PayloadManager> payloads;
};

TEST_F(ComponentTests, EmplacesCell)
{
    auto root = acquire_node(Id::root(), true);
    auto *start = root.page.data() + 0x100;
    const auto *end = emplace_cell(start, 1, 1, "a", "1");
    const auto cell_size = static_cast<Size>(end - start);
    const auto cell = read_cell_at(root, 0x100);
    ASSERT_EQ(cell.size, cell_size);
    ASSERT_EQ(cell.key_size, 1);
    ASSERT_EQ(cell.local_size, 2);
    ASSERT_EQ(cell.has_remote, false);
    const Slice key {cell.key, 1};
    const Slice val {cell.key + 1, 1};
    ASSERT_EQ(key, "a");
    ASSERT_EQ(val, "1");
    release_node(std::move(root));
}

TEST_F(ComponentTests, EmplacesCellWithOverflowValue)
{
    auto root = acquire_node(Id::root(), true);
    const auto value = random.Generate(PAGE_SIZE).to_string();
    const auto local = Slice {value}.truncate(root.meta->min_local - 1);
    auto *start = root.page.data() + 0x100;
    const auto *end = emplace_cell(start, 1, value.size(), "a", local, Id {123});
    const auto cell_size = static_cast<Size>(end - start);
    const auto cell = read_cell_at(root, 0x100);
    ASSERT_EQ(cell.size, cell_size);
    ASSERT_EQ(cell.key_size, 1);
    ASSERT_EQ(cell.local_size, root.meta->min_local);
    ASSERT_EQ(cell.has_remote, true);
    const Slice key {cell.key, 1};
    const Slice val {cell.key + 1, cell.local_size - cell.key_size};
    ASSERT_EQ(key, "a");
    ASSERT_EQ(val, local);
    ASSERT_EQ(read_overflow_id(cell), Id {123});
    release_node(std::move(root));
}

TEST_F(ComponentTests, EmplacesCellWithOverflowKey)
{
    auto root = acquire_node(Id::root(), true);
    const auto long_key = random.Generate(PAGE_SIZE).to_string();
    const auto local = Slice {long_key}.truncate(root.meta->min_local);
    auto *start = root.page.data() + 0x100;
    auto *end = emplace_cell(start, long_key.size(), 1, local, "", Id {123});
    const auto cell_size = static_cast<Size>(end - start);
    const auto cell = read_cell_at(root, 0x100);
    ASSERT_EQ(cell.size, cell_size);
    ASSERT_EQ(cell.key_size, long_key.size());
    ASSERT_EQ(cell.local_size, root.meta->min_local);
    ASSERT_EQ(cell.has_remote, true);
    const Slice key {cell.key, cell.local_size};
    ASSERT_EQ(key, local);
    ASSERT_EQ(read_overflow_id(cell), Id {123});
    release_node(std::move(root));
}

TEST_F(ComponentTests, CollectsPayload)
{
    auto root = acquire_node(Id::root(), true);
    ASSERT_HAS_VALUE(payloads->emplace(collect_scratch.data(), root, "hello", "world", 0));
    const auto cell = read_cell(root, 0);
    ASSERT_EQ(payloads->collect_key(scratch, cell).value(), "hello");
    ASSERT_EQ(payloads->collect_value(scratch, cell).value(), "world");
    root.TEST_validate();
    release_node(std::move(root));
}

TEST_F(ComponentTests, CollectsPayloadWithOverflowValue)
{
    auto root = acquire_node(Id::root(), true);
    const auto value = random.Generate(PAGE_SIZE * 100).to_string();
    ASSERT_HAS_VALUE(payloads->emplace(collect_scratch.data(), root, "hello", value, 0));
    const auto cell = read_cell(root, 0);
    ASSERT_EQ(payloads->collect_key(collect_scratch, cell).value(), "hello");
    ASSERT_EQ(payloads->collect_value(collect_scratch, cell).value(), value);
    root.TEST_validate();
    release_node(std::move(root));
}

TEST_F(ComponentTests, CollectsPayloadWithOverflowKey)
{
    auto root = acquire_node(Id::root(), true);
    const auto key = random.Generate(PAGE_SIZE * 100).to_string();
    ASSERT_HAS_VALUE(payloads->emplace(collect_scratch.data(), root, key, "world", 0));
    const auto cell = read_cell(root, 0);
    ASSERT_EQ(payloads->collect_key(collect_scratch, cell).value(), key);
    ASSERT_EQ(payloads->collect_value(collect_scratch, cell).value(), "world");
    root.TEST_validate();
    release_node(std::move(root));
}

TEST_F(ComponentTests, CollectsPayloadWithOverflowKeyValue)
{
    auto root = acquire_node(Id::root(), true);
    const auto key = random.Generate(PAGE_SIZE * 100).to_string();
    const auto value = random.Generate(PAGE_SIZE * 100).to_string();
    ASSERT_HAS_VALUE(payloads->emplace(collect_scratch.data(), root, key, value, 0));
    const auto cell = read_cell(root, 0);
    ASSERT_EQ(payloads->collect_key(collect_scratch, cell).value(), key);
    ASSERT_EQ(payloads->collect_value(collect_scratch, cell).value(), value);
    root.TEST_validate();
    release_node(std::move(root));
}

TEST_F(ComponentTests, CollectsMultiple)
{
    std::vector<std::string> data;
    auto root = acquire_node(Id::root(), true);
    for (Size i {}; i < 3; ++i) {
        const auto key = random.Generate(PAGE_SIZE * 10);
        const auto value = random.Generate(PAGE_SIZE * 10);
        ASSERT_HAS_VALUE(payloads->emplace(collect_scratch.data(), root, key, value, i));
        data.emplace_back(key.to_string());
        data.emplace_back(value.to_string());
    }
    for (Size i {}; i < 3; ++i) {
        const auto &key = data[2 * i];
        const auto &value = data[2*i + 1];
        const auto cell = read_cell(root, i);
        ASSERT_EQ(payloads->collect_key(collect_scratch, cell).value(), key);
        ASSERT_EQ(payloads->collect_value(collect_scratch, cell).value(), value);
    }
    root.TEST_validate();
    release_node(std::move(root));
}

TEST_F(ComponentTests, PromotesCell)
{
    auto root = acquire_node(Id::root(), true);
    const auto key = random.Generate(PAGE_SIZE * 100).to_string();
    const auto value = random.Generate(PAGE_SIZE * 100).to_string();
    auto *scratch = collect_scratch.data() + 10;
    ASSERT_HAS_VALUE(payloads->emplace(scratch, root, key, value, 0));
    auto cell = read_cell(root, 0);
    ASSERT_HAS_VALUE(payloads->promote(scratch, cell, Id::root()));
    // Needs to consult overflow pages for the key.
    ASSERT_EQ(payloads->collect_key(collect_scratch, cell).value(), key);
    root.TEST_validate();
    release_node(std::move(root));
}

static auto run_promotion_test(ComponentTests &test, Size key_size, Size value_size)
{
    auto root = test.acquire_node(Id::root(), true);
    const auto key = test.random.Generate(key_size).to_string();
    const auto value = test.random.Generate(value_size).to_string();
    auto *scratch = test.collect_scratch.data() + 10;
    ASSERT_HAS_VALUE(test.payloads->emplace(scratch, root, key, value, 0));
    auto external_cell = read_cell(root, 0);
    ASSERT_EQ(external_cell.size, varint_length(key.size()) + varint_length(value.size()) + external_cell.local_size + external_cell.has_remote*8);
    auto internal_cell = external_cell;
    ASSERT_HAS_VALUE(test.payloads->promote(scratch, internal_cell, Id::root()));
    ASSERT_EQ(internal_cell.size, sizeof(Id) + varint_length(key.size()) + internal_cell.local_size + internal_cell.has_remote*8);
    test.release_node(std::move(root));
}

TEST_F(ComponentTests, CellIsPromoted)
{
    run_promotion_test(*this, 10, 10);
}

TEST_F(ComponentTests, PromotionCopiesOverflowKey)
{
    run_promotion_test(*this, PAGE_SIZE, 10);
    const auto old_head = pointers->read_entry(Id {3}).value();
    ASSERT_EQ(old_head.type, PointerMap::OVERFLOW_HEAD);
    ASSERT_EQ(old_head.back_ptr, Id::root());

    // Copy of the overflow key.
    const auto new_head = pointers->read_entry(Id {4}).value();
    ASSERT_EQ(new_head.type, PointerMap::OVERFLOW_HEAD);
    ASSERT_EQ(new_head.back_ptr, Id::root());
}

TEST_F(ComponentTests, PromotionIgnoresOverflowValue)
{
    run_promotion_test(*this, 10, PAGE_SIZE);
    const auto old_head = pointers->read_entry(Id {3}).value();
    ASSERT_EQ(old_head.type, PointerMap::OVERFLOW_HEAD);
    ASSERT_EQ(old_head.back_ptr, Id::root());

    // No overflow key, so the chain didn't need to be copied.
    const auto nothing = pointers->read_entry(Id {4}).value();
    ASSERT_EQ(nothing.back_ptr, Id::null());
}

TEST_F(ComponentTests, PromotionCopiesOverflowKeyButIgnoresOverflowValue)
{
    run_promotion_test(*this, PAGE_SIZE, PAGE_SIZE);
    const auto old_head = pointers->read_entry(Id {3}).value();
    ASSERT_EQ(old_head.type, PointerMap::OVERFLOW_HEAD);
    ASSERT_EQ(old_head.back_ptr, Id::root());

    // 1 overflow page needed for the key, and 1 for the value.
    const auto new_head = pointers->read_entry(Id {5}).value();
    ASSERT_EQ(new_head.type, PointerMap::OVERFLOW_HEAD);
    ASSERT_EQ(new_head.back_ptr, Id::root());
}

struct BPlusTreeTestParameters {
    Size page_size {};
    Size extra {};
};

class BPlusTreeTests : public ParameterizedInMemoryTest<BPlusTreeTestParameters> {
public:
    BPlusTreeTests()
        : param {GetParam()},
          scratch(param.page_size, '\x00'),
          collect_scratch(param.page_size, '\x00'),
          log_scratch(wal_scratch_size(param.page_size), '\x00')
    {}

    auto SetUp() -> void override
    {
        auto r = Pager::open({
            PREFIX,
            storage.get(),
            &log_scratch,
            &wal,
            nullptr,
            &status,
            &commit_lsn,
            &in_xact,
            8,
            param.page_size,
        });
        ASSERT_TRUE(r.has_value()) << r.error().what().data();
        pager = std::move(*r);
        tree = std::make_unique<BPlusTree>(*pager);

        // Root page setup.
        auto root = tree->setup();
        pager->release(std::move(*root).take());
        ASSERT_TRUE(pager->flush({}).is_ok());
    }

    auto TearDown() -> void override
    {
        validate();
    }

    [[nodiscard]]
    auto make_value(char c, bool overflow = false) const
    {
        Size size {param.page_size};
        if (overflow) {
            size /= 3;
        } else {
            size /= 20;
        }
        return std::string(size, c);
    }

    auto acquire_node(Id pid)
    {
        // Warning: Using one of these nodes will likely cause a crash, as the meta stuff doesn't get set up.
        return Node {*pager->acquire(pid), scratch.data()};
    }

    auto release_node(Node node) const
    {
        pager->release(std::move(node).take());
    }

    auto is_root_external()
    {
        auto root = acquire_node(Id::root());
        const auto answer = root.header.is_external;
        release_node(std::move(root));
        return answer;
    }

    auto validate() const -> void
    {
        tree->TEST_check_nodes();
        tree->TEST_check_links();
        tree->TEST_check_order();
    }

    BPlusTreeTestParameters param;
    std::string log_scratch;
    std::string collect_scratch;
    Status status;
    bool in_xact {true};
    Lsn commit_lsn;
    DisabledWriteAheadLog wal;
    std::string scratch;
    std::unique_ptr<Pager> pager;
    std::unique_ptr<BPlusTree> tree;
    Tools::RandomGenerator random {1'024 * 1'024 * 8};
};

TEST_P(BPlusTreeTests, ConstructsAndDestructs)
{
    validate();
}

TEST_P(BPlusTreeTests, InsertsRecords)
{
    ASSERT_TRUE(tree->insert("a", make_value('1')).value());
    ASSERT_TRUE(tree->insert("b", make_value('2')).value());
    ASSERT_TRUE(tree->insert("c", make_value('3')).value());
    validate();
}

TEST_P(BPlusTreeTests, ErasesRecords)
{
    (void)tree->insert("a", make_value('1'));
    (void)tree->insert("b", make_value('2'));
    (void)tree->insert("c", make_value('3'));
    ASSERT_HAS_VALUE(tree->erase("a"));
    ASSERT_HAS_VALUE(tree->erase("b"));
    ASSERT_HAS_VALUE(tree->erase("c"));
    validate();
}

TEST_P(BPlusTreeTests, FindsRecords)
{
    const auto keys = "abc";
    const auto vals = "123";
    (void)tree->insert(std::string(1, keys[0]), make_value(vals[0]));
    (void)tree->insert(std::string(1, keys[1]), make_value(vals[1]));
    (void)tree->insert(std::string(1, keys[2]), make_value(vals[2]));

    for (Size i {}; i < 3; ++i) {
        auto r = tree->search(std::string(1, keys[i])).value();
        ASSERT_EQ(r.index, i);
        const auto cell = read_cell(r.node, r.index);
        ASSERT_EQ(cell.key[0], keys[i]);
        ASSERT_EQ(cell.key[cell.key_size], vals[i]);
    }
}

TEST_P(BPlusTreeTests, CannotFindNonexistentRecords)
{
    auto slot = tree->search("a");
    ASSERT_HAS_VALUE(slot);
    ASSERT_EQ(slot->node.header.cell_count, 0);
    ASSERT_FALSE(slot->exact);
}

TEST_P(BPlusTreeTests, CannotEraseNonexistentRecords)
{
    ASSERT_TRUE(tree->erase("a").error().is_not_found());
}

TEST_P(BPlusTreeTests, WritesOverflowChains)
{
    ASSERT_TRUE(tree->insert("a", make_value('1', true)).value());
    ASSERT_TRUE(tree->insert("b", make_value('2', true)).value());
    ASSERT_TRUE(tree->insert("c", make_value('3', true)).value());
    validate();
}

TEST_P(BPlusTreeTests, ErasesOverflowChains)
{
    (void)tree->insert("a", make_value('1', true)).value();
    (void)tree->insert("b", make_value('2', true)).value();
    (void)tree->insert("c", make_value('3', true)).value();
    ASSERT_HAS_VALUE(tree->erase("a"));
    ASSERT_HAS_VALUE(tree->erase("b"));
    ASSERT_HAS_VALUE(tree->erase("c"));
}

TEST_P(BPlusTreeTests, ReadsValuesFromOverflowChains)
{
    const auto keys = "abc";
    const auto vals = "123";
    std::string values[3];
    values[0] = random.Generate(param.page_size).to_string();
    values[1] = random.Generate(param.page_size).to_string();
    values[2] = random.Generate(param.page_size).to_string();
    (void)tree->insert(std::string(1, keys[0]), values[0]);
    (void)tree->insert(std::string(1, keys[1]), values[1]);
    (void)tree->insert(std::string(1, keys[2]), values[2]);

    for (Size i {}; i < 3; ++i) {
        auto r = tree->search(std::string(1, keys[i])).value();
        const auto cell = read_cell(r.node, r.index);
        const auto pid = read_overflow_id(cell);
        const auto value = tree->collect_value(collect_scratch, cell).value();
        pager->release(std::move(r.node.page));
        ASSERT_EQ(value, values[i]);
    }
}

TEST_P(BPlusTreeTests, ResolvesFirstOverflowOnRightmostPosition)
{
    for (Size i {}; is_root_external(); ++i) {
        ASSERT_TRUE(*tree->insert(Tools::integral_key(i), make_value('v')));
    }
    validate();
}

TEST_P(BPlusTreeTests, ResolvesFirstOverflowOnLeftmostPosition)
{
    for (Size i {}; is_root_external(); ++i) {
        ASSERT_LE(i, 100);
        ASSERT_TRUE(*tree->insert(Tools::integral_key(100 - i), make_value('v')));
    }
    validate();
}

TEST_P(BPlusTreeTests, ResolvesFirstOverflowOnMiddlePosition)
{
    for (Size i {}; is_root_external(); ++i) {
        ASSERT_LE(i, 100);
        ASSERT_TRUE(*tree->insert(Tools::integral_key(i & 1 ? 100 - i : i), make_value('v')));
    }
    validate();
}

TEST_P(BPlusTreeTests, ResolvesMultipleOverflowsOnLeftmostPosition)
{
    for (Size i {}; i < 1'000; ++i) {
        ASSERT_TRUE(*tree->insert(Tools::integral_key(999 - i), make_value('v')));
        if (i % 100 == 99) {
            validate();
        }
    }
}

TEST_P(BPlusTreeTests, ResolvesMultipleOverflowsOnRightmostPosition)
{
    for (Size i {}; i < 1'000; ++i) {
        ASSERT_TRUE(*tree->insert(Tools::integral_key(i), make_value('v')));
        if (i % 100 == 99) {
            validate();
        }
    }
}

TEST_P(BPlusTreeTests, ResolvesMultipleOverflowsOnMiddlePosition)
{
    for (Size i {}, j {999}; i < j; ++i, --j) {
        ASSERT_TRUE(*tree->insert(Tools::integral_key(i), make_value('v')));
        ASSERT_TRUE(*tree->insert(Tools::integral_key(j), make_value('v')));

        if (i % 100 == 99) {
            validate();
        }
    }
}

TEST_P(BPlusTreeTests, ResolvesFirstUnderflowOnRightmostPosition)
{
    long i {};
    for (; is_root_external(); ++i) {
        (void)tree->insert(Tools::integral_key(i), make_value('v'));
    }

    while (i--) {
        ASSERT_HAS_VALUE(tree->erase(Tools::integral_key(i)));
        validate();
    }
}

TEST_P(BPlusTreeTests, ResolvesFirstUnderflowOnLeftmostPosition)
{
    Size i {};
    for (; is_root_external(); ++i) {
        (void)tree->insert(Tools::integral_key(i), make_value('v'));
    }
    for (Size j {}; j < i; ++j) {
        ASSERT_HAS_VALUE(tree->erase(Tools::integral_key(j)));
        validate();
    }
}

TEST_P(BPlusTreeTests, ResolvesFirstUnderflowOnMiddlePosition)
{
    Size i {};
    for (; is_root_external(); ++i) {
        (void)tree->insert(Tools::integral_key(i), make_value('v'));
    }
    for (Size j {1}; j < i/2 - 1; ++j) {
        ASSERT_HAS_VALUE(tree->erase(Tools::integral_key(i/2 - j + 1)));
        ASSERT_HAS_VALUE(tree->erase(Tools::integral_key(i/2 + j)));
        validate();
    }
}

static auto insert_1000(BPlusTreeTests &test, bool has_overflow = false)
{
    for (Size i {}; i < 1'000; ++i) {
        (void)*test.tree->insert(Tools::integral_key(i), test.make_value('v', has_overflow));
    }
}

TEST_P(BPlusTreeTests, ResolvesMultipleUnderflowsOnRightmostPosition)
{
    insert_1000(*this);
    for (Size i {}; i < 1'000; ++i) {
        ASSERT_HAS_VALUE(tree->erase(Tools::integral_key(999 - i)));
        if (i % 100 == 99) {
            validate();
        }
    }
}

TEST_P(BPlusTreeTests, ResolvesMultipleUnderflowsOnLeftmostPosition)
{
    insert_1000(*this);
    for (Size i {}; i < 1'000; ++i) {
        ASSERT_HAS_VALUE(tree->erase(Tools::integral_key(i)));
        if (i % 100 == 99) {
            validate();
        }
    }
}

TEST_P(BPlusTreeTests, ResolvesMultipleUnderflowsOnMiddlePosition)
{
    insert_1000(*this);
    for (Size i {}, j {999}; i < j; ++i, --j) {
        ASSERT_HAS_VALUE(tree->erase(Tools::integral_key(i)));
        ASSERT_HAS_VALUE(tree->erase(Tools::integral_key(j)));
        if (i % 100 == 99) {
            validate();
        }
    }
}

TEST_P(BPlusTreeTests, ResolvesOverflowsFromOverwrite)
{
    for (Size i {}; i < 1'000; ++i) {
        ASSERT_HAS_VALUE(tree->insert(Tools::integral_key(i), "v"));
    }
    // Replace the small values with very large ones.
    for (Size i {}; i < 1'000; ++i) {
        ASSERT_HAS_VALUE(tree->insert(Tools::integral_key(i), make_value('v', true)));
    }
    validate();
}

TEST_P(BPlusTreeTests, SplitWithLongKeys)
{
    for (unsigned i {}; i < 1'000; ++i) {
        const auto key = random.Generate(GetParam().page_size * 2);
        ASSERT_HAS_VALUE(tree->insert(key, "v"));
        if (i % 100 == 99) {
            validate();
        }
    }
}

TEST_P(BPlusTreeTests, SplitWithShortAndLongKeys)
{
    for (unsigned i {}; i < 1'000; ++i) {
        char key[3] {};
        put_u16(key, 999 - i);
        ASSERT_HAS_VALUE(tree->insert({key, 2}, "v"));
    }
    for (unsigned i {}; i < 1'000; ++i) {
        const auto key = random.Generate(GetParam().page_size);
        ASSERT_HAS_VALUE(tree->insert(key, "v"));
        if (i % 100 == 99) {
            validate();
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    BPlusTreeTests,
    BPlusTreeTests,
    ::testing::Values(
        BPlusTreeTestParameters {MINIMUM_PAGE_SIZE},
        BPlusTreeTestParameters {MINIMUM_PAGE_SIZE * 2},
        BPlusTreeTestParameters {MAXIMUM_PAGE_SIZE / 2},
        BPlusTreeTestParameters {MAXIMUM_PAGE_SIZE}));

class BPlusTreeSanityChecks: public BPlusTreeTests {
public:
    auto random_chunk(bool overflow, bool nonzero = true)
    {
        const auto val_size = random.Next<Size>(nonzero, param.page_size*overflow + 12);
        return random.Generate(val_size);
    }

    auto random_write() -> Record
    {
        const auto key = random_chunk(overflow_keys);
        const auto val = random_chunk(overflow_values, false);
        EXPECT_HAS_VALUE(tree->insert(key, val));
        return {key.to_string(), val.to_string()};
    }

    bool overflow_keys = GetParam().extra & 0b10;
    bool overflow_values = GetParam().extra & 0b01;
};

TEST_P(BPlusTreeSanityChecks, Insert)
{
    for (Size i {}; i < 1'000; ++i) {
        random_write();
        if (i % 100 == 99) {
            validate();
        }
    }
}

TEST_P(BPlusTreeSanityChecks, Search)
{
    std::unordered_map<std::string, std::string> records;
    for (Size i {}; i < 1'000; ++i) {
        const auto [k, v] = random_write();
        records[k] = v;
    }
    validate();

    for (const auto &[key, value]: records) {
        auto slot = tree->search(key);
        ASSERT_HAS_VALUE(slot);
        ASSERT_TRUE(slot->exact);
        const auto cell = read_cell(slot->node, slot->index);
        ASSERT_EQ(key, tree->collect_key(collect_scratch, cell).value());
        ASSERT_EQ(value, tree->collect_value(collect_scratch, cell).value());
        release_node(std::move(slot->node));
    }
}

TEST_P(BPlusTreeSanityChecks, Erase)
{
    std::unordered_map<std::string, std::string> records;
    for (Size i {}; i < 1'000; ++i) {
        const auto [k, v] = random_write();
        records[k] = v;
    }

    Size i {};
    for (const auto &[key, value]: records) {
        ASSERT_HAS_VALUE(tree->erase(key));
        if (i % 100 == 99) {
            validate();
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    BPlusTreeSanityChecks,
    BPlusTreeSanityChecks,
    ::testing::Values(
        BPlusTreeTestParameters {MINIMUM_PAGE_SIZE, 0b00},
        BPlusTreeTestParameters {MAXIMUM_PAGE_SIZE, 0b01},
        BPlusTreeTestParameters {MINIMUM_PAGE_SIZE, 0b10},
        BPlusTreeTestParameters {MAXIMUM_PAGE_SIZE, 0b11}));

class CursorTests: public BPlusTreeTests {
protected:
    static constexpr Size RECORD_COUNT {1'000};

    auto SetUp() -> void override
    {
        BPlusTreeTests::SetUp();
        insert_1000(*this);
    }
};

TEST_P(CursorTests, KeyAndValueUseSeparateStorage)
{
    std::unique_ptr<Cursor> cursor {CursorInternal::make_cursor(*tree)};
    cursor->seek_first();
    ASSERT_TRUE(cursor->is_valid());
    const auto k = cursor->key();
    const auto v = cursor->value();
    ASSERT_NE(k, v);
}

TEST_P(CursorTests, SeeksForward)
{
    std::unique_ptr<Cursor> cursor {CursorInternal::make_cursor(*tree)};
    cursor->seek_first();
    for (Size i {}; i < RECORD_COUNT; ++i) {
        ASSERT_TRUE(cursor->is_valid());
        ASSERT_EQ(cursor->key(), Tools::integral_key(i));
        ASSERT_EQ(cursor->value(), make_value('v'));
        cursor->next();
    }
    ASSERT_FALSE(cursor->is_valid());
}

TEST_P(CursorTests, SeeksForwardFromBoundary)
{
    std::unique_ptr<Cursor> cursor {CursorInternal::make_cursor(*tree)};
    cursor->seek(Tools::integral_key(RECORD_COUNT / 4));
    for (Size i {}; i < RECORD_COUNT * 3 / 4; ++i) {
        ASSERT_TRUE(cursor->is_valid());
        cursor->next();
    }
    ASSERT_FALSE(cursor->is_valid());
    }

TEST_P(CursorTests, SeeksForwardToBoundary)
{
    std::unique_ptr<Cursor> cursor {CursorInternal::make_cursor(*tree)};
    std::unique_ptr<Cursor> bounds {CursorInternal::make_cursor(*tree)};
    cursor->seek_first();
    bounds->seek(Tools::integral_key(RECORD_COUNT * 3 / 4));
    for (Size i {}; i < RECORD_COUNT * 3 / 4; ++i) {
        ASSERT_TRUE(cursor->is_valid());
        ASSERT_NE(cursor->key(), bounds->key());
        cursor->next();
    }
    ASSERT_EQ(cursor->key(), bounds->key());
}

TEST_P(CursorTests, SeeksForwardBetweenBoundaries)
{
    std::unique_ptr<Cursor> cursor {CursorInternal::make_cursor(*tree)};
    cursor->seek(Tools::integral_key(250));
    std::unique_ptr<Cursor> bounds {CursorInternal::make_cursor(*tree)};
    bounds->seek(Tools::integral_key(750));
    for (Size i {}; i < 500; ++i) {
        ASSERT_TRUE(cursor->is_valid());
        ASSERT_NE(cursor->key(), bounds->key());
        cursor->next();
    }
    ASSERT_EQ(cursor->key(), bounds->key());
}

TEST_P(CursorTests, SeeksBackward)
{
    std::unique_ptr<Cursor> cursor {CursorInternal::make_cursor(*tree)};
    cursor->seek_last();
    for (Size i {}; i < RECORD_COUNT; ++i) {
        ASSERT_TRUE(cursor->is_valid());
        ASSERT_EQ(cursor->key().to_string(), Tools::integral_key(RECORD_COUNT - i - 1));
        ASSERT_EQ(cursor->value().to_string(), make_value('v'));
        cursor->previous();
    }
    ASSERT_FALSE(cursor->is_valid());
}

TEST_P(CursorTests, SeeksBackwardFromBoundary)
{
    std::unique_ptr<Cursor> cursor {CursorInternal::make_cursor(*tree)};
    const auto bounds = RECORD_COUNT * 3 / 4;
    cursor->seek(Tools::integral_key(bounds));
    for (Size i {}; i <= bounds; ++i) {
        ASSERT_TRUE(cursor->is_valid());
        cursor->previous();
    }
    ASSERT_FALSE(cursor->is_valid());
}

TEST_P(CursorTests, SeeksBackwardToBoundary)
{
    std::unique_ptr<Cursor> cursor {CursorInternal::make_cursor(*tree)};
    cursor->seek_last();
    std::unique_ptr<Cursor> bounds {CursorInternal::make_cursor(*tree)};
    bounds->seek(Tools::integral_key(RECORD_COUNT / 4));
    for (Size i {}; i < RECORD_COUNT*3/4 - 1; ++i) {
        ASSERT_TRUE(cursor->is_valid());
        ASSERT_NE(cursor->key(), bounds->key());
        cursor->previous();
    }
    ASSERT_EQ(cursor->key(), bounds->key());
}

TEST_P(CursorTests, SeeksBackwardBetweenBoundaries)
{
    std::unique_ptr<Cursor> cursor {CursorInternal::make_cursor(*tree)};
    std::unique_ptr<Cursor> bounds {CursorInternal::make_cursor(*tree)};
    cursor->seek(Tools::integral_key(RECORD_COUNT * 3 / 4));
    bounds->seek(Tools::integral_key(RECORD_COUNT / 4));
    for (Size i {}; i < RECORD_COUNT / 2; ++i) {
        ASSERT_TRUE(cursor->is_valid());
        ASSERT_NE(cursor->key(), bounds->key());
        cursor->previous();
    }
    ASSERT_EQ(cursor->key(), bounds->key());
}

TEST_P(CursorTests, SanityCheck_Forward)
{
    std::unique_ptr<Cursor> cursor {CursorInternal::make_cursor(*tree)};
    for (Size iteration {}; iteration < 100; ++iteration) {
        const auto i = random.Next<Size>(RECORD_COUNT);
        const auto key = Tools::integral_key(i);
        cursor->seek(key);

        ASSERT_TRUE(cursor->is_valid());
        ASSERT_EQ(cursor->key(), key);

        for (Size n {}; n < random.Next<Size>(10); ++n) {
            cursor->next();

            if (const auto j = i + n + 1; j < RECORD_COUNT) {
                ASSERT_TRUE(cursor->is_valid());
                ASSERT_EQ(cursor->key(), Tools::integral_key(j));
            } else {
                ASSERT_FALSE(cursor->is_valid());
            }
        }
    }
}

TEST_P(CursorTests, SanityCheck_Backward)
{
    std::unique_ptr<Cursor> cursor {CursorInternal::make_cursor(*tree)};
    for (Size iteration {}; iteration < 100; ++iteration) {
        const auto i = random.Next<Size>(RECORD_COUNT);
        const auto key = Tools::integral_key(i);
        cursor->seek(key);

        ASSERT_TRUE(cursor->is_valid());
        ASSERT_EQ(cursor->key(), key);

        for (Size n {}; n < random.Next<Size>(10); ++n) {
            cursor->previous();

            if (i > n) {
                ASSERT_TRUE(cursor->is_valid());
                ASSERT_EQ(cursor->key(), Tools::integral_key(i - n - 1));
            } else {
                ASSERT_FALSE(cursor->is_valid());
                break;
            }
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    CursorTests,
    CursorTests,
    ::testing::Values(
        BPlusTreeTestParameters {MINIMUM_PAGE_SIZE},
        BPlusTreeTestParameters {MINIMUM_PAGE_SIZE * 2},
        BPlusTreeTestParameters {MAXIMUM_PAGE_SIZE / 2},
        BPlusTreeTestParameters {MAXIMUM_PAGE_SIZE}));

class PointerMapTests: public BPlusTreeTests {
public:
    [[nodiscard]]
    auto map_size() -> Size
    {
        return (pager->page_size()-sizeof(Lsn)) / (sizeof(Byte)+sizeof(Id));
    }
};

TEST_P(PointerMapTests, FirstPointerMapIsPage2)
{
    PointerMap pointers {*pager};
    ASSERT_EQ(pointers.lookup(Id {0}), Id {0});
    ASSERT_EQ(pointers.lookup(Id {1}), Id {0});
    ASSERT_EQ(pointers.lookup(Id {2}), Id {2});
    ASSERT_EQ(pointers.lookup(Id {3}), Id {2});
    ASSERT_EQ(pointers.lookup(Id {4}), Id {2});
    ASSERT_EQ(pointers.lookup(Id {5}), Id {2});
}

TEST_P(PointerMapTests, ReadsAndWritesEntries)
{
    std::string buffer(pager->page_size(), '\0');
    Page map_page {Id {2}, buffer, true};
    PointerMap map {*pager};

    ASSERT_HAS_VALUE(map.write_entry(Id {3}, PointerMap::Entry {Id {33}, PointerMap::NODE}));
    ASSERT_HAS_VALUE(map.write_entry(Id {4}, PointerMap::Entry {Id {44}, PointerMap::FREELIST_LINK}));
    ASSERT_HAS_VALUE(map.write_entry(Id {5}, PointerMap::Entry {Id {55}, PointerMap::OVERFLOW_LINK}));

    const auto entry_1 = map.read_entry(Id {3}).value();
    const auto entry_2 = map.read_entry(Id {4}).value();
    const auto entry_3 = map.read_entry(Id {5}).value();

    ASSERT_EQ(entry_1.back_ptr.value, 33);
    ASSERT_EQ(entry_2.back_ptr.value, 44);
    ASSERT_EQ(entry_3.back_ptr.value, 55);
    ASSERT_EQ(entry_1.type, PointerMap::NODE);
    ASSERT_EQ(entry_2.type, PointerMap::FREELIST_LINK);
    ASSERT_EQ(entry_3.type, PointerMap::OVERFLOW_LINK);
}

TEST_P(PointerMapTests, PointerMapCanFitAllPointers)
{
    PointerMap pointers {*pager};

    // PointerMap::find_map() expects the given pointer map page to be allocated already.
    for (Size i {}; i < map_size() * 2; ++i) {
        auto page = pager->allocate().value();
        pager->release(std::move(page));
    }

    for (Size i {}; i < map_size() + 10; ++i) {
        if (i != map_size()) {
            const Id id {i + 3};
            const PointerMap::Entry entry {id, PointerMap::NODE};
            ASSERT_HAS_VALUE(pointers.write_entry(id, entry));
        }
    }
    for (Size i {}; i < map_size() + 10; ++i) {
        if (i != map_size()) {
            const Id id {i + 3};
            const auto entry = pointers.read_entry(id).value();
            ASSERT_EQ(entry.back_ptr.value, id.value);
            ASSERT_EQ(entry.type, PointerMap::NODE);
        }
    }
}

TEST_P(PointerMapTests, MapPagesAreRecognized)
{
    Id id {2};
    PointerMap pointers {*pager};
    ASSERT_EQ(pointers.lookup(id), id);

    // Back pointers for the next "map.map_size()" pages are stored on page 2. The next pointermap page is
    // the page following the last page whose back pointer is on page 2. This pattern continues forever.
    for (Size i {}; i < 1'000'000; ++i) {
        id.value += map_size() + 1;
        ASSERT_EQ(pointers.lookup(id), id);
    }
}

TEST_P(PointerMapTests, FindsCorrectMapPages)
{
    Size counter {};
    Id map_id {2};
    PointerMap pointers {*pager};

    for (Id pid {3}; pid.value <= 100 * map_size(); ++pid.value) {
        if (counter++ == map_size()) {
            // Found a map page. Calls to find() with a page ID between this page and the next map page
            // should map to this page ID.
            map_id.value += map_size() + 1;
            counter = 0;
        } else {
            ASSERT_EQ(pointers.lookup(pid), map_id);
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    PointerMapTests,
    PointerMapTests,
    ::testing::Values(
        BPlusTreeTestParameters {MINIMUM_PAGE_SIZE},
        BPlusTreeTestParameters {MINIMUM_PAGE_SIZE * 2},
        BPlusTreeTestParameters {MAXIMUM_PAGE_SIZE / 2},
        BPlusTreeTestParameters {MAXIMUM_PAGE_SIZE}));

class VacuumTests: public TestWithPager {
public:
    static constexpr Size FRAME_COUNT {8};

    VacuumTests()
        : meta {PAGE_SIZE}
    {}

    auto SetUp() -> void override
    {
        TestWithPager::SetUp();
        tree = std::make_unique<BPlusTree>(*pager);
        auto root = tree->setup();
        pager->release(std::move(*root).take());
        ASSERT_TRUE(pager->flush({}).is_ok());
        c = tree->TEST_components();
    }

    auto acquire_node(Id pid, bool is_writable)
    {
        Node node {*pager->acquire(pid), scratch.data()};
        node.meta = &meta(node.header.is_external);
        if (is_writable) {
            pager->upgrade(node.page);
        }
        return node;
    }

    auto allocate_node(bool is_external)
    {
        auto page = pager->allocate().value();
        if (c.pointers->lookup(page.id()) == page.id()) {
            pager->release(std::move(page));
            page = pager->allocate().value();
        }
        NodeHeader header;
        header.is_external = is_external;
        header.cell_start = static_cast<PageSize>(page.size());
        header.write(page);
        const auto pid = page.id();
        pager->release(std::move(page));
        return acquire_node(pid, true);
    }

    auto release_node(Node node) const
    {
        pager->release(std::move(node).take());
    }

    auto sanity_check(Size lower_bounds, Size record_count, Size max_value_size) const
    {
        std::unordered_map<std::string, std::string> map;

        for (Size iteration {}; iteration < 5; ++iteration) {
            while (map.size() < lower_bounds + record_count) {
                const auto key = random.Generate(16);
                const auto value_size = random.Next<Size>(max_value_size);
                const auto value = random.Generate(value_size);
                ASSERT_HAS_VALUE(tree->insert(key, value));
                map[key.to_string()] = value.to_string();
            }

            auto itr = begin(map);
            while (map.size() > lower_bounds) {
                ASSERT_HAS_VALUE(tree->erase(itr->first));
                itr = map.erase(itr);
            }

            Id target {pager->page_count()};
            while (tree->vacuum_one(target).value()) {
                tree->TEST_check_links();
                tree->TEST_check_nodes();
                tree->TEST_check_order();
                target.value--;
            }
            ASSERT_HAS_VALUE(pager->truncate(target.value));

            auto *cursor = CursorInternal::make_cursor(*tree);
            for (const auto &[key, value]: map) {
                cursor->seek(key);
                ASSERT_TRUE(cursor->is_valid());
                ASSERT_EQ(cursor->key(), key);
                ASSERT_EQ(cursor->value(), value);
            }
            delete cursor;
        }
    }

    NodeMetaManager meta;
    std::unique_ptr<BPlusTree> tree;
    BPlusTree::Components c;
};

//      P   1   2   3
// [1] [2] [3] [4] [5]
//
TEST_F(VacuumTests, FreelistRegistersBackPointers)
{
    // Should skip page 2, leaving it available for use as a pointer map.
    auto node_3 = allocate_node(true);
    auto node_4 = allocate_node(true);
    auto node_5 = allocate_node(true);
    ASSERT_EQ(node_5.page.id().value, 5);

    ASSERT_HAS_VALUE(c.freelist->push(std::move(node_5.page)));
    ASSERT_HAS_VALUE(c.freelist->push(std::move(node_4.page)));
    ASSERT_HAS_VALUE(c.freelist->push(std::move(node_3.page)));

    auto entry = c.pointers->read_entry(Id {5}).value();
    ASSERT_EQ(entry.type, PointerMap::FREELIST_LINK);
    ASSERT_EQ(entry.back_ptr, Id {4});

    entry = c.pointers->read_entry(Id {4}).value();
    ASSERT_EQ(entry.type, PointerMap::FREELIST_LINK);
    ASSERT_EQ(entry.back_ptr, Id {3});

    entry = c.pointers->read_entry(Id {3}).value();
    ASSERT_EQ(entry.type, PointerMap::FREELIST_LINK);
    ASSERT_EQ(entry.back_ptr, Id::null());
}

TEST_F(VacuumTests, OverflowChainRegistersBackPointers)
{
    // Creates an overflow chain of length 2, rooted at the second cell on the root page.
    std::string overflow_data(PAGE_SIZE * 2, 'x');
    ASSERT_HAS_VALUE(tree->insert("a", "value"));
    ASSERT_HAS_VALUE(tree->insert("b", overflow_data));

    const auto head_entry = c.pointers->read_entry(Id {3});
    const auto tail_entry = c.pointers->read_entry(Id {4});

    ASSERT_TRUE(head_entry->back_ptr.is_root());
    ASSERT_EQ(tail_entry->back_ptr, Id {3});
    ASSERT_EQ(head_entry->type, PointerMap::OVERFLOW_HEAD);
    ASSERT_EQ(tail_entry->type, PointerMap::OVERFLOW_LINK);
}

TEST_F(VacuumTests, OverflowChainIsNullTerminated)
{
    {
        // allocate_node() accounts for the first pointer map page.
        auto node_3 = allocate_node(true);
        auto page_4 = pager->allocate().value();
        ASSERT_EQ(page_4.id().value, 4);
        write_next_id(node_3.page, Id {123});
        write_next_id(page_4, Id {123});
        ASSERT_HAS_VALUE(c.freelist->push(std::move(page_4)));
        ASSERT_HAS_VALUE(c.freelist->push(std::move(node_3.page)));
    }

    ASSERT_HAS_VALUE(tree->insert("a", "value"));
    ASSERT_HAS_VALUE(tree->insert("b", std::string(PAGE_SIZE * 2, 'x')));

    auto page_3 = pager->acquire(Id {3}).value();
    auto page_4 = pager->acquire(Id {4}).value();
    ASSERT_EQ(read_next_id(page_3), Id {4});
    ASSERT_EQ(read_next_id(page_4), Id::null());
    pager->release(std::move(page_3));
    pager->release(std::move(page_4));
}

TEST_F(VacuumTests, VacuumsFreelistInOrder)
{
    auto node_3 = allocate_node(true);
    auto node_4 = allocate_node(true);
    auto node_5 = allocate_node(true);
    ASSERT_EQ(node_5.page.id().value, 5);

    // Page Types:     N   P   3   2   1
    // Page Contents: [1] [2] [3] [4] [5]
    // Page IDs:       1   2   3   4   5
    ASSERT_HAS_VALUE(c.freelist->push(std::move(node_3.page)));
    ASSERT_HAS_VALUE(c.freelist->push(std::move(node_4.page)));
    ASSERT_HAS_VALUE(c.freelist->push(std::move(node_5.page)));

    // Page Types:     N   P   2   1
    // Page Contents: [1] [2] [3] [4] [X]
    // Page IDs:       1   2   3   4   5
    ASSERT_HAS_VALUE(tree->vacuum_one(Id {5}));
    auto entry = c.pointers->read_entry(Id {4}).value();
    ASSERT_EQ(entry.type, PointerMap::FREELIST_LINK);
    ASSERT_EQ(entry.back_ptr, Id::null());

    // Page Types:     N   P   1
    // Page Contents: [1] [2] [3] [X] [X]
    // Page IDs:       1   2   3   4   5
    ASSERT_HAS_VALUE(tree->vacuum_one(Id {4}));
    entry = c.pointers->read_entry(Id {3}).value();
    ASSERT_EQ(entry.type, PointerMap::FREELIST_LINK);
    ASSERT_EQ(entry.back_ptr, Id::null());

    // Page Types:     N   P
    // Page Contents: [1] [2] [X] [X] [X]
    // Page IDs:       1   2   3   4   5
    ASSERT_HAS_VALUE(tree->vacuum_one(Id {3}));
    ASSERT_TRUE(c.freelist->is_empty());

    // Page Types:     N
    // Page Contents: [1] [X] [X] [X] [X]
    // Page IDs:       1   2   3   4   5
    ASSERT_HAS_VALUE(tree->vacuum_one(Id {2}));

    // Page Types:     N
    // Page Contents: [1]
    // Page IDs:       1
    ASSERT_HAS_VALUE(pager->truncate(1));
    ASSERT_EQ(pager->page_count(), 1);
}

TEST_F(VacuumTests, VacuumsFreelistInReverseOrder)
{
    auto node_3 = allocate_node(true);
    auto node_4 = allocate_node(true);
    auto node_5 = allocate_node(true);

    // Page Types:     N   P   1   2   3
    // Page Contents: [a] [b] [c] [d] [e]
    // Page IDs:       1   2   3   4   5
    ASSERT_HAS_VALUE(c.freelist->push(std::move(node_5.page)));
    ASSERT_HAS_VALUE(c.freelist->push(std::move(node_4.page)));
    ASSERT_HAS_VALUE(c.freelist->push(std::move(node_3.page)));

    // Step 1:
    //     Page Types:     N   P       1   2
    //     Page Contents: [a] [b] [e] [d] [e]
    //     Page IDs:       1   2   3   4   5

    // Step 2:
    //     Page Types:     N   P   2   1
    //     Page Contents: [a] [b] [e] [d] [ ]
    //     Page IDs:       1   2   3   4   5
    ASSERT_HAS_VALUE(tree->vacuum_one(Id {5}));
    auto entry = c.pointers->read_entry(Id {4}).value();
    ASSERT_EQ(entry.back_ptr, Id::null());
    ASSERT_EQ(entry.type, PointerMap::FREELIST_LINK);
    {
        auto page = pager->acquire(Id {4}).value();
        ASSERT_EQ(read_next_id(page), Id {3});
        pager->release(std::move(page));
    }

    // Page Types:     N   P   1
    // Page Contents: [a] [b] [e] [ ] [ ]
    // Page IDs:       1   2   3   4   5
    ASSERT_HAS_VALUE(tree->vacuum_one(Id {4}));
    entry = c.pointers->read_entry(Id {3}).value();
    ASSERT_EQ(entry.type, PointerMap::FREELIST_LINK);
    ASSERT_EQ(entry.back_ptr, Id::null());

    // Page Types:     N   P
    // Page Contents: [a] [b] [ ] [ ] [ ]
    // Page IDs:       1   2   3   4   5
    ASSERT_HAS_VALUE(tree->vacuum_one(Id {3}));
    ASSERT_TRUE(c.freelist->is_empty());

    // Page Types:     N
    // Page Contents: [a] [ ] [ ] [ ] [ ]
    // Page IDs:       1   2   3   4   5
    ASSERT_HAS_VALUE(tree->vacuum_one(Id {2}));

    // Page Types:     N
    // Page Contents: [a]
    // Page IDs:       1
    ASSERT_HAS_VALUE(pager->truncate(1));
    ASSERT_EQ(pager->page_count(), 1);
}

TEST_F(VacuumTests, VacuumFreelistSanityCheck)
{
    std::default_random_engine rng {42};

    for (Size iteration {}; iteration < 1'000; ++iteration) {
        std::vector<Node> nodes;
        for (Size i {}; i < FRAME_COUNT - 1; ++i) {
            nodes.emplace_back(allocate_node(true));
        }

        std::shuffle(begin(nodes), end(nodes), rng);

        for (auto &node: nodes) {
            ASSERT_HAS_VALUE(c.freelist->push(std::move(node.page)));
        }

        // This will vacuum the whole freelist, as well as the pointer map page on page 2.
        Id target {pager->page_count()};
        for (Size i {}; i < FRAME_COUNT; ++i) {
            ASSERT_TRUE(tree->vacuum_one(target).value());
            target.value--;
        }
        ASSERT_FALSE(tree->vacuum_one(target).value());
        ASSERT_HAS_VALUE(pager->truncate(1));
        ASSERT_EQ(pager->page_count(), 1);
    }
}

static auto vacuum_and_validate(VacuumTests &test, const std::string &value)
{
    ASSERT_EQ(test.pager->page_count(), 6);
    ASSERT_HAS_VALUE(test.tree->vacuum_one(Id {6}));
    ASSERT_HAS_VALUE(test.tree->vacuum_one(Id {5}));
    ASSERT_HAS_VALUE(test.pager->truncate(4));
    ASSERT_EQ(test.pager->page_count(), 4);

    auto *cursor = CursorInternal::make_cursor(*test.tree);
    cursor->seek_first();
    ASSERT_TRUE(cursor->is_valid());
    ASSERT_EQ(cursor->key(), "a");
    ASSERT_EQ(cursor->value(), "value");
    cursor->next();

    ASSERT_TRUE(cursor->is_valid());
    ASSERT_EQ(cursor->key(), "b");
    ASSERT_EQ(cursor->value(), value);
    cursor->next();

    ASSERT_FALSE(cursor->is_valid());
    delete cursor;
}

TEST_F(VacuumTests, VacuumsOverflowChain_A)
{
    // Save these pages until the overflow chain is created, otherwise they will be used for it.
    auto node_3 = allocate_node(true);
    auto node_4 = allocate_node(true);
    ASSERT_EQ(node_4.page.id().value, 4);

    // Creates an overflow chain of length 2, rooted at the second cell on the root page.
    std::string overflow_data(PAGE_SIZE * 2, 'x');
    ASSERT_HAS_VALUE(tree->insert("a", "value"));
    ASSERT_HAS_VALUE(tree->insert("b", overflow_data));

    ASSERT_HAS_VALUE(c.freelist->push(std::move(node_3.page)));
    ASSERT_HAS_VALUE(c.freelist->push(std::move(node_4.page)));

    // Page Types:     n   p   2   1   A   B
    // Page Contents: [a] [b] [c] [d] [e] [f]
    // Page IDs:       1   2   3   4   5   6

    // Page Types:     n   p   1   B   A
    // Page Contents: [a] [b] [c] [f] [e] [ ]
    // Page IDs:       1   2   3   4   5   6

    // Page Types:     n   p   A   B
    // Page Contents: [a] [b] [e] [f] [ ] [ ]
    // Page IDs:       1   2   3   4   5   6

    // Page Types:     n   p   A   B
    // Page Contents: [a] [b] [e] [f]
    // Page IDs:       1   2   3   4
    vacuum_and_validate(*this, overflow_data);

    const auto head_entry = c.pointers->read_entry(Id {3});
    const auto tail_entry = c.pointers->read_entry(Id {4});

    ASSERT_TRUE(head_entry->back_ptr.is_root());
    ASSERT_EQ(tail_entry->back_ptr, Id {3});
    ASSERT_EQ(head_entry->type, PointerMap::OVERFLOW_HEAD);
    ASSERT_EQ(tail_entry->type, PointerMap::OVERFLOW_LINK);
}

TEST_F(VacuumTests, VacuumsOverflowChain_B)
{
    // This time, we'll force the head of the overflow chain to be the last page in the file.
    auto node_3 = allocate_node(true);
    auto node_4 = allocate_node(true);
    auto node_5 = allocate_node(true);
    auto node_6 = allocate_node(true);
    ASSERT_EQ(node_6.page.id().value, 6);
    ASSERT_HAS_VALUE(c.freelist->push(std::move(node_5.page)));
    ASSERT_HAS_VALUE(c.freelist->push(std::move(node_6.page)));

    std::string overflow_data(PAGE_SIZE * 2, 'x');
    ASSERT_HAS_VALUE(tree->insert("a", "value"));
    ASSERT_HAS_VALUE(tree->insert("b", overflow_data));

    // Page Types:     n   p   2   1   B   A
    // Page Contents: [a] [b] [c] [d] [e] [f]
    // Page IDs:       1   2   3   4   5   6
    ASSERT_HAS_VALUE(c.freelist->push(std::move(node_3.page)));
    ASSERT_HAS_VALUE(c.freelist->push(std::move(node_4.page)));

    // Page Types:     n   p   1   A   B
    // Page Contents: [a] [b] [c] [f] [e] [ ]
    // Page IDs:       1   2   3   4   5   6

    // Page Types:     n   p   B   A
    // Page Contents: [a] [b] [e] [f] [ ] [ ]
    // Page IDs:       1   2   3   4   5   6

    // Page Types:     n   p   B   A
    // Page Contents: [a] [b] [e] [f]
    // Page IDs:       1   2   3   4
    vacuum_and_validate(*this, overflow_data);

    const auto head_entry = c.pointers->read_entry(Id {4});
    const auto tail_entry = c.pointers->read_entry(Id {3});

    ASSERT_TRUE(head_entry->back_ptr.is_root());
    ASSERT_EQ(tail_entry->back_ptr, Id {4});
    ASSERT_EQ(head_entry->type, PointerMap::OVERFLOW_HEAD);
    ASSERT_EQ(tail_entry->type, PointerMap::OVERFLOW_LINK);
}

TEST_F(VacuumTests, VacuumOverflowChainSanityCheck)
{
    std::vector<Node> reserved;
    reserved.emplace_back(allocate_node(true));
    reserved.emplace_back(allocate_node(true));
    reserved.emplace_back(allocate_node(true));
    reserved.emplace_back(allocate_node(true));
    reserved.emplace_back(allocate_node(true));
    ASSERT_EQ(reserved.back().page.id().value, 7);

    // Create overflow chains, but don't overflow the root node. Should create 3 chains, 1 of length 1, and 2 of length 2.
    std::vector<std::string> values;
    for (Size i {}; i < 3; ++i) {
        const auto value = random.Generate(PAGE_SIZE * std::min<Size>(i + 1, 2));
        ASSERT_HAS_VALUE(tree->insert(Tools::integral_key<1>(i), value));
        values.emplace_back(value.to_string());
    }

    while (!reserved.empty()) {
        ASSERT_HAS_VALUE(c.freelist->push(std::move(reserved.back().page)));
        reserved.pop_back();
    }

    ASSERT_EQ(pager->page_count(), 12);
    ASSERT_HAS_VALUE(tree->vacuum_one(Id {12}));
    ASSERT_HAS_VALUE(tree->vacuum_one(Id {11}));
    ASSERT_HAS_VALUE(tree->vacuum_one(Id {10}));
    ASSERT_HAS_VALUE(tree->vacuum_one(Id {9}));
    ASSERT_HAS_VALUE(tree->vacuum_one(Id {8}));
    ASSERT_HAS_VALUE(pager->truncate(7));
    ASSERT_EQ(pager->page_count(), 7);

    auto *cursor = CursorInternal::make_cursor(*tree);
    cursor->seek_first();
    for (Size i {}; i < values.size(); ++i) {
        ASSERT_TRUE(cursor->is_valid());
        ASSERT_EQ(cursor->key(), Tools::integral_key<1>(i));
        ASSERT_EQ(cursor->value(), values[i]);
        cursor->next();
    }
    ASSERT_FALSE(cursor->is_valid());
    delete cursor;
}

TEST_F(VacuumTests, VacuumsNodes)
{
    auto node_3 = allocate_node(true);
    auto node_4 = allocate_node(true);
    ASSERT_EQ(node_4.page.id().value, 4);

    std::vector<std::string> values;
    for (Size i {}; i < 5; ++i) {
        const auto key = Tools::integral_key(i);
        const auto value = random.Generate(meta(true).max_local - key.size());
        ASSERT_HAS_VALUE(tree->insert(key, value));
        values.emplace_back(value.to_string());
    }

    // Page Types:     n   p   2   1   n   n
    // Page Contents: [a] [b] [c] [d] [e] [f]
    // Page IDs:       1   2   3   4   5   6
    ASSERT_HAS_VALUE(c.freelist->push(std::move(node_3.page)));
    ASSERT_HAS_VALUE(c.freelist->push(std::move(node_4.page)));

    // Page Types:     n   p   1   n   n
    // Page Contents: [a] [b] [c] [f] [e] [ ]
    // Page IDs:       1   2   3   4   5   6

    // Page Types:     n   p   n   n
    // Page Contents: [a] [b] [e] [f] [ ] [ ]
    // Page IDs:       1   2   3   4   5   6

    // Page Types:     n   p   n   n
    // Page Contents: [a] [b] [e] [f]
    // Page IDs:       1   2   3   4
    ASSERT_EQ(pager->page_count(), 6);
    ASSERT_HAS_VALUE(tree->vacuum_one(Id {6}));
    ASSERT_HAS_VALUE(tree->vacuum_one(Id {5}));
    ASSERT_HAS_VALUE(pager->truncate(4));

    auto *cursor = CursorInternal::make_cursor(*tree);
    cursor->seek_first();
    for (Size i {}; i < values.size(); ++i) {
        ASSERT_TRUE(cursor->is_valid());
        ASSERT_EQ(cursor->key(), Tools::integral_key(i));
        ASSERT_EQ(cursor->value(), values[i]);
        cursor->next();
    }
    ASSERT_FALSE(cursor->is_valid());
    delete cursor;
}

TEST_F(VacuumTests, SanityCheck_Freelist)
{
    sanity_check(0, 50, 10);
}

TEST_F(VacuumTests, SanityCheck_Freelist_OverflowHead)
{
    sanity_check(0, 50, PAGE_SIZE / 2);
}

TEST_F(VacuumTests, SanityCheck_Freelist_OverflowLink)
{
    sanity_check(0, 50, PAGE_SIZE * 2);
}

TEST_F(VacuumTests, SanityCheck_Nodes_1)
{
    sanity_check(50, 50, 10);
}

TEST_F(VacuumTests, SanityCheck_Nodes_2)
{
    sanity_check(200, 50, 10);
}

TEST_F(VacuumTests, SanityCheck_Nodes_Overflow_1)
{
    sanity_check(50, 50, PAGE_SIZE * 2);
}

TEST_F(VacuumTests, SanityCheck_Nodes_Overflow_2)
{
    sanity_check(200, 50, PAGE_SIZE * 2);
}

} // namespace Calico