
#include "fakes.h"
#include "page/page.h"
#include "pager/basic_pager.h"
#include "pager/framer.h"
#include "pager/registry.h"
#include "unit_tests.h"
#include "utils/info_log.h"
#include "utils/layout.h"
#include "wal/disabled_wal.h"
#include <gtest/gtest.h>
#include <numeric>

namespace calico {

class CacheTests: public testing::Test {
public:
    cache<int, int> target;
};

TEST_F(CacheTests, EmptyCacheBehavior)
{
    calico::cache<int, int> cache;

    ASSERT_TRUE(cache.is_empty());
    ASSERT_EQ(cache.size(), 0);
    ASSERT_EQ(begin(cache), end(cache));
    ASSERT_EQ(cache.get(1), end(cache));
    ASSERT_EQ(cache.evict(), std::nullopt);
}

TEST_F(CacheTests, NonEmptyCacheBehavior)
{
    calico::cache<int, int> cache;

    cache.put(1, 1);
    ASSERT_FALSE(cache.is_empty());
    ASSERT_EQ(cache.size(), 1);
    ASSERT_NE(begin(cache), end(cache));
    ASSERT_NE(cache.get(1), end(cache));
    ASSERT_NE(cache.evict(), std::nullopt);
}

TEST_F(CacheTests, ElementsArePromotedAfterUse)
{
    calico::cache<int, int> cache;

    // 1*, 2, 3, 4, END
    cache.put(4, 4);
    cache.put(3, 3);
    cache.put(2, 2);
    cache.put(1, 1);

    // 3, 4, 1*, 2, END
    cache.put(4, 4);
    cache.put(4, 4);
    ASSERT_EQ(cache.get(3)->value, 3);
    ASSERT_EQ(cache.size(), 4);

    decltype(cache)::entry entry;
    entry = cache.evict().value();
    ASSERT_FALSE(entry.hot);
    ASSERT_EQ(entry.value, 2);
    entry = cache.evict().value();
    ASSERT_FALSE(entry.hot);
    ASSERT_EQ(entry.value, 1);
    entry = cache.evict().value();
    ASSERT_TRUE(entry.hot);
    ASSERT_EQ(entry.value, 4);
    entry = cache.evict().value();
    ASSERT_TRUE(entry.hot);
    ASSERT_EQ(entry.value, 3);
}

TEST_F(CacheTests, IterationRespectsReplacementPolicy)
{
    // 1*, 2, 3, END
    target.put(3, 3);
    target.put(2, 2);
    target.put(1, 1);

    // 1, 2, 3*, END
    target.put(2, 2);
    target.put(1, 1);

    // Hottest -> coldest
    auto itr = begin(target);
    ASSERT_TRUE(itr->hot);
    ASSERT_EQ(itr++->value, 1);
    ASSERT_TRUE(itr->hot);
    ASSERT_EQ(itr++->value, 2);
    ASSERT_FALSE(itr->hot);
    ASSERT_EQ(itr++->value, 3);
    ASSERT_EQ(itr, end(target));

    // Coldest -> hottest
    auto ritr = rbegin(target);
    ASSERT_FALSE(ritr->hot);
    ASSERT_EQ(ritr++->value, 3);
    ASSERT_TRUE(ritr->hot);
    ASSERT_EQ(ritr++->value, 2);
    ASSERT_TRUE(ritr->hot);
    ASSERT_EQ(ritr++->value, 1);
    ASSERT_EQ(ritr, rend(target));
}

TEST_F(CacheTests, QueryDoesNotPromoteElements)
{
    calico::cache<int, int> cache;

    // 1*, 2, 3, END
    cache.put(3, 3);
    cache.put(2, 2);
    cache.put(1, 1);

    ASSERT_EQ(cache.query(1)->value, 1);
    ASSERT_EQ(cache.query(2)->value, 2);

    // Method is const.
    const auto &ref = cache;
    ASSERT_EQ(ref.query(3)->value, 3);

    auto itr = begin(cache);
    ASSERT_EQ(itr++->value, 1);
    ASSERT_EQ(itr++->value, 2);
    ASSERT_EQ(itr++->value, 3);
    ASSERT_EQ(itr, end(cache));
}

TEST_F(CacheTests, ModifyValue)
{
    calico::cache<int, int> cache;

    cache.put(1, 1);
    cache.put(1, 2);

    ASSERT_EQ(cache.size(), 1);
    ASSERT_EQ(cache.get(1)->value, 2);
}

TEST_F(CacheTests, WarmElementsAreFifoOrdered)
{
    calico::cache<int, int> cache;

    // 1*, 2, 3, END
    cache.put(3, 3);
    cache.put(2, 2);
    cache.put(1, 1);

    auto itr = begin(cache);
    ASSERT_EQ(itr++->value, 1);
    ASSERT_EQ(itr++->value, 2);
    ASSERT_EQ(itr++->value, 3);
    ASSERT_EQ(itr, end(cache));

    ASSERT_EQ(cache.evict()->value, 3);
    ASSERT_EQ(cache.evict()->value, 2);
    ASSERT_EQ(cache.evict()->value, 1);
}

TEST_F(CacheTests, HotElementsAreLruOrdered)
{
    calico::cache<int, int> cache;

    // 1*, 2, 3
    cache.put(3, 3);
    cache.put(2, 2);
    cache.put(1, 1);

    // 2, 3, 1*
    ASSERT_EQ(cache.get(3)->value, 3);
    ASSERT_EQ(cache.get(2)->value, 2);
    ASSERT_EQ(cache.get(1)->value, 1);

    auto itr = begin(cache);
    ASSERT_EQ(itr++->value, 1);
    ASSERT_EQ(itr++->value, 2);
    ASSERT_EQ(itr++->value, 3);
    ASSERT_EQ(itr, end(cache));

    ASSERT_EQ(cache.evict()->value, 3);
    ASSERT_EQ(cache.evict()->value, 2);
    ASSERT_EQ(cache.evict()->value, 1);
}

TEST_F(CacheTests, HotElementsAreEncounteredFirst)
{
    calico::cache<int, int> cache;

    // 4*, 3, 2, 1, END
    cache.put(1, 1);
    cache.put(2, 2);
    cache.put(3, 3);
    cache.put(4, 4);

    // 3, 2, 1, 4*, END
    ASSERT_EQ(cache.get(1)->value, 1);
    ASSERT_EQ(cache.get(2)->value, 2);
    ASSERT_EQ(cache.get(3)->value, 3);

    // 3, 2, 1, 5*, 4, END
    cache.put(5, 5);

    auto itr = begin(cache);
    ASSERT_TRUE(itr->hot);
    ASSERT_EQ(itr++->value, 3);
    ASSERT_TRUE(itr->hot);
    ASSERT_EQ(itr++->value, 2);
    ASSERT_TRUE(itr->hot);
    ASSERT_EQ(itr++->value, 1);
    ASSERT_FALSE(itr->hot);
    ASSERT_EQ(itr++->value, 5);
    ASSERT_FALSE(itr->hot);
    ASSERT_EQ(itr++->value, 4);
    ASSERT_EQ(itr, end(cache));
}

TEST_F(CacheTests, SeparatorIsMovedOnInsert)
{
    calico::cache<int, int> cache;

    // 4*, 3, 2, 1, END
    cache.put(1, 1);
    cache.put(2, 2);
    cache.put(3, 3);
    cache.put(4, 4);
    ASSERT_FALSE(begin(cache)->hot);
    ASSERT_EQ(begin(cache)->value, 4);

    // 4, 3*, 2, 1, END
    cache.put(4, 4);
    ASSERT_TRUE(begin(cache)->hot);
    ASSERT_EQ(begin(cache)->value, 4);

    // 3, 4, 2*, 1, END
    cache.put(3, 3);
    ASSERT_TRUE(begin(cache)->hot);
    ASSERT_EQ(begin(cache)->value, 3);

    // 2, 3, 4, 1*, END
    cache.put(2, 2);
    ASSERT_TRUE(begin(cache)->hot);
    ASSERT_EQ(begin(cache)->value, 2);

    // 1, 2, 3, 4, END*
    cache.put(1, 1);
    ASSERT_TRUE(begin(cache)->hot);
    ASSERT_EQ(begin(cache)->value, 1);
}

TEST_F(CacheTests, AddWarmElements)
{
    calico::cache<int, int> cache;

    // 4*, 3, 2, 1, END
    cache.put(1, 1);
    cache.put(2, 2);
    cache.put(3, 3);
    cache.put(4, 4);
    ASSERT_FALSE(begin(cache)->hot);
    ASSERT_EQ(begin(cache)->value, 4);

    // 3, 4, 2*, 1, END
    cache.put(4, 4);
    cache.put(3, 3);

    // 3, 4, 6*, 5, 2, 1, END
    cache.put(5, 5);
    cache.put(6, 6);

    auto itr = begin(cache);
    ASSERT_TRUE(itr->hot);
    ASSERT_EQ(itr++->value, 3);
    ASSERT_TRUE(itr->hot);
    ASSERT_EQ(itr++->value, 4);
    ASSERT_FALSE(itr->hot);
    ASSERT_EQ(itr++->value, 6);
    ASSERT_FALSE(itr->hot);
    ASSERT_EQ(itr++->value, 5);
    ASSERT_FALSE(itr->hot);
    ASSERT_EQ(itr++->value, 2);
    ASSERT_FALSE(itr->hot);
    ASSERT_EQ(itr++->value, 1);
    ASSERT_EQ(itr, end(cache));
}

TEST_F(CacheTests, InsertAfterWarmElementsDepleted)
{
    calico::cache<int, int> cache;

    // 4*, 3, 2, 1, END
    cache.put(1, 1);
    cache.put(2, 2);
    cache.put(3, 3);
    cache.put(4, 4);
    ASSERT_FALSE(begin(cache)->hot);
    ASSERT_EQ(begin(cache)->value, 4);

    // 3, 4, 2*, 1, END
    cache.put(4, 4);
    cache.put(3, 3);

    // 3, 4, 2*, END
    auto entry = cache.evict().value();
    ASSERT_FALSE(entry.hot);
    ASSERT_EQ(entry.value, 1);

    // 3, 4, END*
    entry = cache.evict().value();
    ASSERT_FALSE(entry.hot);
    ASSERT_EQ(entry.value, 2);

    // 4, 3, END*
    cache.put(4, 4);
    ASSERT_TRUE(prev(end(cache))->hot);
    ASSERT_EQ(prev(end(cache))->value, 3);
    ASSERT_TRUE(begin(cache)->hot);
    ASSERT_EQ(begin(cache)->value, 4);

    // 4, 3, 2*, END
    cache.put(2, 2);
    ASSERT_FALSE(prev(end(cache))->hot);
    ASSERT_EQ(prev(end(cache))->value, 2);
}

static auto check_cache_order(int hot_count, int warm_count)
{
    cache<int, int> c;

    for (int i {1}; i <= hot_count + warm_count; ++i)
        c.put(i, i);
    for (int i {1}; i <= hot_count; ++i)
        c.put(i, i);

    // Iteration: Hot elements should be encountered first. In particular, the most-recently-
    // used hot element (if present) should be first.
    auto itr = begin(c);
    ASSERT_EQ(itr->value, hot_count ? hot_count : warm_count);
    for (int i {}; i < hot_count; ++i) {
        ASSERT_TRUE(itr++->hot);
    }
    for (int i {}; i < warm_count; ++i) {
        ASSERT_FALSE(itr++->hot);
    }

    // Eviction: Hot elements should be evicted last.
    for (int i {}; i < warm_count; ++i) {
        ASSERT_FALSE(c.evict()->hot);
    }
    for (int i {}; i < hot_count; ++i) {
        ASSERT_TRUE(c.evict()->hot);
    }
}

TEST(CacheOrderTests, CheckOrder)
{
    check_cache_order(1, 0);
    check_cache_order(0, 1);
    check_cache_order(2, 0);
    check_cache_order(0, 2);
    check_cache_order(2, 1);
    check_cache_order(1, 2);
    check_cache_order(1, 1);
    check_cache_order(2, 2);
}

TEST(MoveOnlyCacheTests, WorksWithMoveOnlyValue)
{
    calico::cache<int, std::unique_ptr<int>> cache;
    cache.put(1, std::make_unique<int>(1));
    ASSERT_EQ(*cache.get(1)->value, 1);
    ASSERT_EQ(*cache.evict()->value, 1);
}

class PageRegistryTests : public testing::Test {
public:
    ~PageRegistryTests() override = default;

    PageRegistry registry;
};

TEST_F(PageRegistryTests, HotEntriesAreFoundLast)
{
    registry.put(identifier {11UL}, {Size {11UL}});
    registry.put(identifier {12UL}, {Size {12UL}});
    registry.put(identifier {13UL}, {Size {13UL}});
    registry.put(identifier {1UL}, {Size {1UL}});
    registry.put(identifier {2UL}, {Size {2UL}});
    registry.put(identifier {3UL}, {Size {3UL}});
    ASSERT_EQ(registry.size(), 6);

    ASSERT_EQ(registry.get(identifier {11UL})->value.frame_index, 11UL);
    ASSERT_EQ(registry.get(identifier {12UL})->value.frame_index, 12UL);
    ASSERT_EQ(registry.get(identifier {13UL})->value.frame_index, 13UL);

    Size i {}, j {};

    const auto callback = [&i, &j](auto page_id, auto entry) {
        EXPECT_EQ(page_id.value, entry.frame_index);
        EXPECT_EQ(page_id.value, i + (j >= 3)*10 + 1) << "The cache entries should have been visited in order {1, 2, 3, 11, 12, 13}";
        j++;
        i = j % 3;
        return false;
    };

    ASSERT_FALSE(registry.evict(callback));
}

class FramerTests : public testing::Test {
public:
    explicit FramerTests()
        : home {std::make_unique<HeapStorage>()}
    {
        std::unique_ptr<RandomEditor> file;
        RandomEditor *temp_file {};
        EXPECT_TRUE(home->open_random_editor(DATA_FILENAME, &temp_file).is_ok());
        file.reset(temp_file);

        Framer *temp;
        auto s = Framer::open(std::move(file), 0x100, 8, &temp);
        EXPECT_TRUE(s.is_ok());
        framer.reset(temp);
    }

    ~FramerTests() override = default;

    std::unique_ptr<HeapStorage> home;
    std::unique_ptr<Framer> framer;
};

TEST_F(FramerTests, NewFramerIsSetUpCorrectly)
{
    ASSERT_EQ(framer->available(), 8);
    ASSERT_EQ(framer->page_count(), 0);
    ASSERT_TRUE(framer->flushed_lsn().is_null());
}

TEST_F(FramerTests, KeepsTrackOfAvailableFrames)
{
    auto frame_id = framer->pin(identifier::root()).value();
    ASSERT_EQ(framer->available(), 7);
    framer->discard(frame_id);
    ASSERT_EQ(framer->available(), 8);
}

TEST_F(FramerTests, PinFailsWhenNoFramesAreAvailable)
{
    for (Size i {1}; i <= 8; i++)
        ASSERT_TRUE(framer->pin(identifier {i}));
    const auto r = framer->pin(identifier {9UL});
    ASSERT_FALSE(r.has_value());
    ASSERT_TRUE(r.error().is_not_found()) << "Unexpected Error: " << r.error().what();

    framer->unpin(Size {1UL});
    ASSERT_TRUE(framer->pin(identifier {9UL}));
}

auto write_to_page(Page &page, const std::string &message) -> void
{
    const auto offset = PageLayout::content_offset(page.id());
    CALICO_EXPECT_LE(offset + message.size(), page.size());
    page.write(stob(message), offset);
}

[[nodiscard]]
auto read_from_page(const Page &page, Size size) -> std::string
{
    const auto offset = PageLayout::content_offset(page.id());
    CALICO_EXPECT_LE(offset + size, page.size());
    auto message = std::string(size, '\x00');
    page.read(stob(message), offset);
    return message;
}

class PagerTests : public TestOnHeap {
public:
    static constexpr Size frame_count {8};//TODO 32};
    static constexpr Size page_size {0x100};
    std::string test_message {"Hello, world!"};

    explicit PagerTests()
        : wal {std::make_unique<DisabledWriteAheadLog>()},
          scratch {wal_scratch_size(page_size)}
    {
        BasicPager *temp;
        const auto s = BasicPager::open({
            PREFIX,
            store.get(),
            &scratch,
            &images,
            wal.get(),
            &status,
            &has_xact,
            &commit_lsn,
            create_sink(),
            frame_count,
            page_size,
        }, &temp);
        EXPECT_TRUE(s.is_ok());
        pager.reset(temp);
    }

    ~PagerTests() override = default;

    [[nodiscard]]
    auto allocate_write(const std::string &message) const
    {
        auto r = pager->allocate();
        EXPECT_TRUE(r.has_value()) << "Error: " << r.error().what();
        write_to_page(*r, message);
        return std::move(*r);
    }

    [[nodiscard]]
    auto allocate_write_release(const std::string &message) const
    {
        auto page = allocate_write(message);
        const auto id = page.id();
        const auto s = pager->release(std::move(page));
        EXPECT_TRUE(s.is_ok()) << "Error: " << s.what();
        return id;
    }

    [[nodiscard]]
    auto acquire_write(identifier id, const std::string &message) const
    {
        auto r = pager->acquire(id, false);
        EXPECT_TRUE(r.has_value()) << "Error: " << r.error().what();
        write_to_page(*r, message);
        return std::move(*r);
    }

    auto acquire_write_release(identifier id, const std::string &message) const
    {
        auto page = acquire_write(id, message);
        const auto s = pager->release(std::move(page));
        EXPECT_TRUE(s.is_ok()) << "Error: " << s.what();
    }

    [[nodiscard]]
    auto acquire_read_release(identifier id, Size size) const
    {
        auto r = pager->acquire(id, false);
        EXPECT_TRUE(r.has_value()) << "Error: " << r.error().what();
        auto message = read_from_page(*r, size);
        EXPECT_TRUE(pager->release(std::move(*r)).is_ok());
        return message;
    }

    Status status {Status::ok()};
    bool has_xact {};
    identifier commit_lsn;
    std::unordered_set<identifier, identifier::hash> images;
    std::unique_ptr<WriteAheadLog> wal;
    std::unique_ptr<Pager> pager;
    LogScratchManager scratch;
};

TEST_F(PagerTests, NewPagerIsSetUpCorrectly)
{
    ASSERT_EQ(pager->page_count(), 0);
    ASSERT_EQ(pager->flushed_lsn(), identifier::null());
    ASSERT_TRUE(pager->status().is_ok());
}

TEST_F(PagerTests, AllocationInceasesPageCount)
{
    [[maybe_unused]] const auto a = allocate_write_release("a");
    ASSERT_EQ(pager->page_count(), 1);
    [[maybe_unused]] const auto b = allocate_write_release("b");
    ASSERT_EQ(pager->page_count(), 2);
    [[maybe_unused]] const auto c = allocate_write_release("c");
    ASSERT_EQ(pager->page_count(), 3);
}

TEST_F(PagerTests, FirstAllocationCreatesRootPage)
{
    auto id = allocate_write_release(test_message);
    ASSERT_EQ(id, identifier::root());
}

TEST_F(PagerTests, AcquireReturnsCorrectPage)
{
    const auto id = allocate_write_release(test_message);
    auto r = pager->acquire(id, false);
    ASSERT_EQ(id, r->id());
    ASSERT_EQ(id, identifier::root());
    EXPECT_TRUE(pager->release(std::move(*r)).is_ok());
}

TEST_F(PagerTests, MultipleWritersDeathTest)
{
    const auto page = allocate_write(test_message);
    ASSERT_DEATH(const auto same_page = pager->acquire(page.id(), true), EXPECTATION_MATCHER);
}

TEST_F(PagerTests, ReaderAndWriterDeathTest)
{
    const auto page = allocate_write(test_message);
    ASSERT_DEATH(const auto same_page = pager->acquire(page.id(), false), EXPECTATION_MATCHER);
}

TEST_F(PagerTests, MultipleReaders)
{
    const auto id = allocate_write_release(test_message);
    auto page_1a = pager->acquire(id, false).value();
    auto page_1b = pager->acquire(id, false).value();
    ASSERT_TRUE(pager->release(std::move(page_1a)).is_ok());
    ASSERT_TRUE(pager->release(std::move(page_1b)).is_ok());
}

TEST_F(PagerTests, PagesAreAutomaticallyReleased)
{
    // This line allocates a page, writes to it, then lets it go out of scope. The page should release itself in its destructor using the pointer it
    // stores back to the pager object. If it doesn't, we would not be able to acquire the same page as writable again (see MultipleWritersDeathTest).
    const auto id = allocate_write(test_message).id();
    ASSERT_EQ(acquire_read_release(id, test_message.size()), test_message);
}

template<class T>
static auto run_root_persistence_test(T &test, Size n)
{
    const auto id = test.allocate_write_release(test.test_message);

    // Cause the root page to be evicted and written back, along with some other pages.
    while (test.pager->page_count() < n)
        [[maybe_unused]] auto unused = test.allocate_write_release("...");

    // Read the root page back from the file.
    ASSERT_EQ(test.acquire_read_release(id, test.test_message.size()), test.test_message);
}

TEST_F(PagerTests, RootDataPersistsInFrame)
{
    run_root_persistence_test(*this, frame_count);
}

TEST_F(PagerTests, RootDataPersistsInStorage)
{
    run_root_persistence_test(*this, frame_count * 2);
}

[[nodiscard]]
auto generate_id_strings(Size n)
{
    std::vector<Size> id_ints(n);
    std::iota(begin(id_ints), end(id_ints), 1);

    std::vector<std::string> id_strs;
    std::transform(cbegin(id_ints), cend(id_ints), back_inserter(id_strs), [](auto id) {
        return fmt::format("{:06}", id);
    });
    return id_strs;
}

TEST_F(PagerTests, SanityCheck)
{
    const auto ids = generate_id_strings(500);

    for (const auto &id: ids)
        [[maybe_unused]] const auto unused = allocate_write_release(id);

    for (const auto &id: ids) { // NOTE: gtest assertion macros sometimes complain if braces are omitted.
        ASSERT_EQ(id, acquire_read_release(identifier {std::stoull(id)}, id.size()));
    }
}

} // namespace calico