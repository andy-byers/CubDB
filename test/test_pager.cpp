// Copyright (c) 2022, The CalicoDB Authors. All rights reserved.
// This source code is licensed under the MIT License, which can be found in
// LICENSE.md. See AUTHORS.md for a list of contributor names.

#include "common.h"
#include "encoding.h"
#include "fake_env.h"
#include "freelist.h"
#include "pager.h"
#include "temp.h"
#include "test.h"
#include "wal.h"

namespace calicodb::test
{

// Buffer manager tests were adapted from LevelDB.
class BufmgrTests : public testing::Test
{
public:
    static constexpr U32 kCacheSize = 1'000;
    Dirtylist m_dirtylist;
    Stat m_stat;
    Bufmgr mgr;

    explicit BufmgrTests()
        : mgr(kMinFrameCount, m_stat)
    {
    }

    ~BufmgrTests() override = default;

    auto insert_and_reference(U32 key, U32 value) -> PageRef *
    {
        auto *ref = mgr.next_victim();
        if (ref == nullptr) {
            ref = mgr.allocate();
            EXPECT_NE(ref, nullptr);
        } else {
            if (ref->get_flag(PageRef::kDirty)) {
                m_dirtylist.remove(*ref);
            }
            mgr.erase(ref->page_id);
        }
        if (ref) {
            ref->page_id.value = key;
            put_u32(ref->get_data(), value);
            mgr.register_page(*ref);
            mgr.ref(*ref);
        } else {
            ADD_FAILURE() << "OOM when allocating a page reference";
        }
        return ref;
    }

    auto insert(U32 key, U32 value) -> void
    {
        if (auto *ref = insert_and_reference(key, value)) {
            mgr.unref(*ref);
        }
    }

    auto erase(U32 key) -> bool
    {
        return mgr.erase(Id(key));
    }

    auto lookup(U32 key) -> int
    {
        if (auto *ref = mgr.lookup(Id(key))) {
            return static_cast<int>(get_u32(ref->get_data()));
        }
        return -1;
    }
};

TEST_F(BufmgrTests, HitAndMiss)
{
    ASSERT_EQ(-1, lookup(100));

    insert(100, 101);
    ASSERT_EQ(101, lookup(100));
    ASSERT_EQ(-1, lookup(200));
    ASSERT_EQ(-1, lookup(300));

    insert(200, 201);
    ASSERT_EQ(101, lookup(100));
    ASSERT_EQ(201, lookup(200));
    ASSERT_EQ(-1, lookup(300));
}

TEST_F(BufmgrTests, Erase)
{
    erase(200);

    insert(100, 101);
    insert(200, 201);
    erase(100);
    ASSERT_EQ(-1, lookup(100));
    ASSERT_EQ(201, lookup(200));

    erase(100);
    ASSERT_EQ(-1, lookup(100));
    ASSERT_EQ(201, lookup(200));
}

TEST_F(BufmgrTests, EvictionPolicy)
{
    insert(100, 101);
    insert(200, 201);
    insert(300, 301);
    auto *h = mgr.lookup(Id(300));
    ASSERT_NE(h, nullptr);
    mgr.ref(*h);

    // Frequently used entry must be kept around,
    // as must things that are still in use.
    for (U32 i = 0; i < kCacheSize + 100; i++) {
        insert(1000 + i, 2000 + i);
        ASSERT_EQ(2000 + i, lookup(1000 + i));
        ASSERT_EQ(101, lookup(100));
    }
    ASSERT_EQ(101, lookup(100));
    ASSERT_EQ(-1, lookup(200));
    ASSERT_EQ(301, lookup(300));
    mgr.unref(*h);
}

TEST_F(BufmgrTests, UseExceedsCacheSize)
{
    // Overfill the cache, keeping handles on all inserted entries.
    std::vector<PageRef *> h;
    for (U32 i = 0; i < kCacheSize + 100; i++) {
        h.push_back(insert_and_reference(1000 + i, 2000 + i));
    }

    // Check that all the entries can be found in the cache.
    for (U32 i = 0; i < h.size(); i++) {
        ASSERT_EQ(2000 + i, lookup(1000 + i));
    }

    for (auto *ref : h) {
        mgr.unref(*ref);
    }
}

#ifndef NDEBUG
TEST_F(BufmgrTests, DeathTests)
{
    auto *ref1 = insert_and_reference(2, 2);
    auto *ref2 = insert_and_reference(3, 2);
    ASSERT_DEATH(insert(2, 2), "");
    ASSERT_DEATH(insert(3, 3), "");
    mgr.unref(*ref1);
    mgr.unref(*ref2);
}
#endif // NDEBUG

class DirtylistTests : public BufmgrTests
{
public:
    auto add(U32 key) -> void
    {
        auto *ref = insert_and_reference(key, key);
        ASSERT_NE(ref, nullptr);
        m_dirtylist.add(*ref);
        mgr.unref(*ref);
    }

    auto remove(U32 key) -> void
    {
        auto *ref = mgr.lookup(Id(key));
        ASSERT_NE(ref, nullptr);
        remove(*ref);
    }
    auto remove(PageRef &ref) -> void
    {
        m_dirtylist.remove(ref);
    }

    // NOTE: This is destructive.
    auto sort_and_check() -> void
    {
        std::vector<U32> pgno;
        auto *list = m_dirtylist.sort();
        for (auto *p = list; p; p = p->dirty) {
            pgno.emplace_back(p->get_page_ref()->page_id.value);
            p->get_page_ref()->clear_flag(PageRef::kDirty);
        }
        ASSERT_TRUE(std::is_sorted(begin(pgno), end(pgno)));
    }
};

TEST_F(DirtylistTests, AddAndRemove)
{
    add(2);
    add(3);
    add(4);

    remove(2);
    remove(3);
    remove(4);
}

TEST_F(DirtylistTests, SortSortedPages)
{
    for (U32 i = 0; i < 1'000; ++i) {
        add(i + 2);
        if (i % kMinFrameCount + 1 == kMinFrameCount) {
            sort_and_check();
        }
    }
}

TEST_F(DirtylistTests, SortUnsortedPages)
{
    std::default_random_engine rng(42);
    std::vector<U32> pgno(1'000);
    std::iota(begin(pgno), end(pgno), 2);
    std::shuffle(begin(pgno), end(pgno), rng);
    for (U32 i = 0; i < 1'000; ++i) {
        add(pgno[i]);
        if (i % kMinFrameCount + 1 == kMinFrameCount) {
            sort_and_check();
        }
    }
}

#ifndef NDEBUG
TEST_F(DirtylistTests, DeathTest)
{
    // An empty dirtylist must not be sorted.
    ASSERT_DEATH(sort_and_check(), "");
}
#endif // NDEBUG

class PagerTests : public testing::Test
{
protected:
    static constexpr std::size_t kManyPages = kMinFrameCount * 100;

    Env *m_env;
    File *m_wal_file = nullptr;
    Pager *m_pager = nullptr;
    File *m_file = nullptr;
    Status m_status;
    Stat m_stat;

    explicit PagerTests()
        : m_env(new FakeEnv())
    {
    }

    ~PagerTests() override
    {
        close();
        delete m_file;
        delete m_wal_file;
        delete m_env;
    }

    auto SetUp() -> void override
    {
        reopen();
    }

    auto reopen(Options::LockMode lock_mode = Options::kLockNormal) -> bool
    {
        close();
        (void)m_env->remove_file("db");
        (void)m_env->remove_file("wal");
        delete m_file;
        m_file = nullptr;

        auto s = m_env->new_file(
            "db",
            Env::kCreate | Env::kReadWrite,
            m_file);
        if (s.is_ok()) {
            const Pager::Parameters param = {
                "db",
                "wal",
                m_file,
                m_env,
                nullptr,
                &m_status,
                &m_stat,
                nullptr,
                kMinFrameCount,
                Options::kSyncNormal,
                lock_mode,
                true,
            };
            s = Pager::open(param, m_pager);
        }
        if (s.is_ok()) {
            s = m_pager->start_reader();
            m_pager->finish();
        }
        if (s.is_ok()) {
            delete m_wal_file;
            m_wal_file = nullptr;
            s = m_env->new_file("wal", Env::kReadWrite, m_wal_file);
        }
        if (!s.is_ok()) {
            ADD_FAILURE() << s.to_string();
            delete m_file;
            return false;
        }
        return true;
    }
    auto close() -> void
    {
        delete m_pager;
        m_pager = nullptr;
    }

    std::vector<Id> m_page_ids;
    auto allocate_page(PageRef *&page_out) -> Id
    {
        EXPECT_OK(m_pager->allocate(page_out));
        if (m_page_ids.empty() || m_page_ids.back() < page_out->page_id) {
            m_page_ids.emplace_back(page_out->page_id);
        }
        std::memset(page_out->get_data(), 0, kPageSize);
        return page_out->page_id;
    }
    auto allocate_page() -> Id
    {
        PageRef *page;
        const auto id = allocate_page(page);
        m_pager->release(page);
        return id;
    }
    auto alter_page(PageRef &page) -> void
    {
        m_pager->mark_dirty(page);
        const auto value = get_u32(page.get_data() + kPageSize - 4);
        put_u32(page.get_data() + kPageSize - 4, value + 1);
    }
    auto alter_page(std::size_t index) -> void
    {
        PageRef *page;
        EXPECT_OK(m_pager->acquire(m_page_ids.at(index), page));
        alter_page(*page);
        m_pager->release(page);
    }
    auto read_page(const PageRef &page) -> U32
    {
        return get_u32(page.get_data() + kPageSize - 4);
    }
    auto read_page(std::size_t index) -> U32
    {
        if (m_page_ids.at(index).value > m_pager->page_count()) {
            return 0;
        }
        PageRef *page;
        EXPECT_OK(m_pager->acquire(m_page_ids.at(index), page));
        const auto value = read_page(*page);
        m_pager->release(page);
        return value;
    }

    template <class Fn>
    auto pager_view(const Fn &fn) -> void
    {
        ASSERT_OK(m_pager->start_reader());
        fn();
        m_pager->finish();
    }
    template <class Fn>
    auto pager_update(const Fn &fn) -> void
    {
        ASSERT_OK(m_pager->start_reader());
        ASSERT_OK(m_pager->start_writer());
        if (m_pager->page_count() == 0) {
            m_pager->initialize_root();
        }
        fn();
        m_pager->finish();
    }
};

TEST_F(PagerTests, AllocatePage)
{
    pager_update([this] {
        // Pager layer skips pointer map pages, and the root already exists.
        ASSERT_EQ(Id(3), allocate_page());
        ASSERT_EQ(Id(4), allocate_page());
        ASSERT_EQ(Id(5), allocate_page());
        ASSERT_EQ(5, m_pager->page_count());
    });
}

TEST_F(PagerTests, AcquirePage)
{
    pager_update([this] {
        allocate_page();
        allocate_page();
        allocate_page();
        ASSERT_EQ(5, m_pager->page_count());

        PageRef *page;
        for (U32 n = 1; n < 5; ++n) {
            ASSERT_OK(m_pager->acquire(Id(n), page));
            m_pager->release(page);
            ASSERT_EQ(5, m_pager->page_count());
        }
        // Attempt to skip page 5.
        ASSERT_TRUE(m_pager->acquire(Id(6), page).is_corruption());
    });
}

TEST_F(PagerTests, NOOP)
{
    pager_update([] {});
    pager_view([] {});
    ASSERT_OK(m_pager->checkpoint(true));
    ASSERT_OK(m_pager->checkpoint(false));
    pager_update([this] {
        m_pager->set_page_count(0);
        m_pager->set_status(Status::ok());
    });

    std::size_t file_size;
    // Database size is 0 before the first checkpoint.
    ASSERT_OK(m_env->file_size("db", file_size));
    ASSERT_EQ(file_size, 0);
}

TEST_F(PagerTests, Commit)
{
    for (int iteration = 0; iteration < 6; ++iteration) {
        reopen(iteration < 3 ? Options::kLockNormal : Options::kLockExclusive);
        pager_update([this] {
            // Alter each page.
            for (std::size_t i = 0; i < kManyPages; ++i) {
                PageRef *page;
                allocate_page(page);
                alter_page(*page);

                m_pager->release(
                    page,
                    // kNoCache should be ignored since the page is dirty.
                    Pager::kNoCache);
            }
            // Alter every other page.
            for (std::size_t i = 0; i < kManyPages; ++i) {
                PageRef *page;
                ASSERT_OK(m_pager->acquire(m_page_ids[i], page));
                alter_page(*page);
                m_pager->release(
                    page,
                    // Drop every other update.
                    i & 1 ? Pager::kNoCache : Pager::kDiscard);
            }
            ASSERT_OK(m_pager->commit());
        });
        if (iteration % 3 > 0) {
            // Make sure we actually have all the data we need in the WAL. The root page is
            // not in the WAL, but it is blank anyway.
            ASSERT_OK(m_file->resize(0));
            // Transfer the lost pages back.
            ASSERT_OK(m_pager->checkpoint(iteration % 3 == 1));
            // Everything should be back in the database file. The next reader shouldn't read
            // any pages from the WAL.
            ASSERT_OK(m_wal_file->resize(0));
        }
        pager_view([this] {
            for (std::size_t i = 0; i < kManyPages; ++i) {
                const auto value = read_page(i);
                ASSERT_EQ(1 + (i & 1), value);
            }
        });
    }
}

TEST_F(PagerTests, Rollback)
{
    for (int iteration = 0; iteration < 6; ++iteration) {
        reopen(iteration < 3 ? Options::kLockNormal : Options::kLockExclusive);
        std::size_t page_count = 0;
        pager_update([this, &page_count] {
            for (std::size_t i = 0; i < kManyPages; ++i) {
                PageRef *page;
                allocate_page(page);
                alter_page(*page);
                m_pager->release(page);

                if (i == kManyPages / 2) {
                    ASSERT_OK(m_pager->commit());
                    page_count = m_pager->page_count();
                }
            }
        });
        if (iteration % 3 > 0) {
            ASSERT_OK(m_file->resize(0));
            ASSERT_OK(m_pager->checkpoint(iteration % 3 == 1));
            ASSERT_OK(m_wal_file->resize(0));
        }
        pager_view([this, page_count] {
            ASSERT_EQ(m_pager->page_count(), page_count);
            for (std::size_t i = 0; i < kManyPages; ++i) {
                ASSERT_EQ(i <= kManyPages / 2, read_page(i) != 0);
            }
        });
    }
}

TEST_F(PagerTests, Truncation)
{
    pager_update([this] {
        for (std::size_t i = 0; i < kManyPages; ++i) {
            allocate_page();
        }
        for (std::size_t i = 0; i < kManyPages; ++i) {
            alter_page(i);
        }
        m_pager->set_page_count(m_page_ids.at(kManyPages / 2).value);
        ASSERT_OK(m_pager->commit());
    });

    ASSERT_OK(m_pager->checkpoint(true));

    std::size_t file_size;
    ASSERT_OK(m_env->file_size("db", file_size));
    ASSERT_EQ(file_size, kPageSize * m_page_ids.at(kManyPages / 2).value);

    pager_view([this] {
        for (std::size_t i = 0; i < kManyPages; ++i) {
            EXPECT_EQ(i <= kManyPages / 2, read_page(i) != 0) << i;
        }
    });
}

TEST_F(PagerTests, Freelist)
{
    pager_update([this] {
        PageRef *page;
        // Fill up several trunk pages.
        for (std::size_t i = 0; i < kPageSize; ++i) {
            allocate_page(page);
            m_pager->release(page);
        }
        for (std::size_t i = 0; i < kPageSize; ++i) {
            ASSERT_OK(m_pager->acquire(m_page_ids.at(i), page));
            ASSERT_OK(m_pager->destroy(page));
        }
        ASSERT_OK(m_pager->commit());
    });
    pager_update([this] {
        ASSERT_EQ(m_page_ids.back().value, m_pager->page_count());
        PageRef *page;
        for (std::size_t i = 0; i < kPageSize; ++i) {
            allocate_page(page);
            m_pager->release(page);
        }
        ASSERT_OK(m_pager->commit());
    });
    pager_view([this] {
        ASSERT_EQ(m_page_ids.back().value, m_pager->page_count());
    });
}

TEST_F(PagerTests, FreelistCorruption)
{
    pager_update([this] {
        PageRef *page;
        allocate_page(page);
        page->page_id.value = m_pager->page_count() + 1;
        ASSERT_NOK(m_pager->destroy(page));
        auto *root = &m_pager->get_root();
        ASSERT_NOK(m_pager->destroy(root));
    });
}

TEST_F(PagerTests, ReportsOutOfRangePages)
{
    pager_update([this] {
        PageRef *page;
        ASSERT_NOK(m_pager->acquire(Id(100), page));
        ASSERT_NOK(m_pager->acquire(Id(200), page));
        ASSERT_NOK(m_pager->acquire(Id(300), page));
    });
}

TEST_F(PagerTests, MovePage)
{
    static constexpr U32 kSpecialValue = 123'456;
    static constexpr U32 kNumPages = 32;
    pager_update([this] {
        for (U32 i = 0; i < kNumPages; ++i) {
            PageRef *pg;
            ASSERT_OK(m_pager->allocate(pg));
            m_pager->mark_dirty(*pg);
            put_u32(pg->get_data(), pg->page_id.value);
            m_pager->release(pg, Pager::kDiscard);
        }
        PageRef *pg;
        ASSERT_OK(m_pager->get_unused_page(pg));
        m_pager->mark_dirty(*pg);
        put_u32(pg->get_data(), kSpecialValue);

        m_pager->move_page(*pg, Id(3));
        for (; pg->page_id.value != kNumPages;) {
            m_pager->move_page(*pg, Id(pg->page_id.value + 1));
        }
        ASSERT_EQ(get_u32(pg->get_data()), kSpecialValue);

        m_pager->release(pg);
        ASSERT_OK(m_pager->commit());
    });
    pager_view([this] {
        PageRef *pg;
        ASSERT_OK(m_pager->acquire(Id(kNumPages), pg));
        ASSERT_EQ(get_u32(pg->get_data()), kSpecialValue);
        m_pager->release(pg);
    });
}

#ifndef NDEBUG
TEST_F(PagerTests, DeathTest)
{
    ASSERT_EQ(m_pager->mode(), Pager::kOpen);
    ASSERT_DEATH((void)m_pager->commit(), "");

    ASSERT_DEATH((void)m_pager->start_writer(), "");
    ASSERT_OK(m_pager->start_reader());
    ASSERT_DEATH((void)m_pager->checkpoint(true), "");

    pager_update([this] {
        PageRef *a, *b;
        ASSERT_OK(m_pager->allocate(a));
        ASSERT_OK(m_pager->allocate(b));
        ASSERT_DEATH(m_pager->move_page(*a, b->page_id), "");
        m_pager->release(a);
        m_pager->release(b);
    });
}
#endif // NDEBUG

class HashIndexTestBase
{
protected:
    explicit HashIndexTestBase()
    {
        EXPECT_OK(m_env.new_file("shm", Env::kCreate, m_shm));
        m_index = new HashIndex(m_header, m_shm);
    }

    ~HashIndexTestBase()
    {
        delete m_shm;
        delete m_index;
    }

    auto append(U32 key)
    {
        ASSERT_OK(m_index->assign(key, ++m_header.max_frame));
    }

    FakeEnv m_env;
    File *m_shm = nullptr;
    HashIndexHdr m_header = {};
    HashIndex *m_index = nullptr;
};

class HashIndexTests
    : public HashIndexTestBase,
      public testing::Test
{
protected:
    ~HashIndexTests() override = default;
};

TEST_F(HashIndexTests, FirstSegmentFrameBounds)
{
    append(1);
    append(2);
    append(3);
    append(4);

    const U32 min_frame(2);
    m_header.max_frame = 3;

    U32 value;
    ASSERT_OK(m_index->lookup(1, min_frame, value));
    ASSERT_FALSE(value);
    ASSERT_OK(m_index->lookup(2, min_frame, value));
    ASSERT_EQ(value, 2);
    ASSERT_OK(m_index->lookup(3, min_frame, value));
    ASSERT_EQ(value, 3);
    ASSERT_OK(m_index->lookup(4, min_frame, value));
    ASSERT_FALSE(value);
}

TEST_F(HashIndexTests, SecondSegmentFrameBounds)
{
    for (U32 i = 1; i <= 6'000; ++i) {
        append(i);
    }

    const U32 min_frame = 5'000;
    m_header.max_frame = 5'500;

    U32 value;
    ASSERT_OK(m_index->lookup(1, min_frame, value));
    ASSERT_FALSE(value);
    ASSERT_OK(m_index->lookup(4'999, min_frame, value));
    EXPECT_FALSE(value);
    ASSERT_OK(m_index->lookup(5'000, min_frame, value));
    ASSERT_EQ(value, 5'000);
    ASSERT_OK(m_index->lookup(5'500, min_frame, value));
    ASSERT_EQ(value, 5'500);
    ASSERT_OK(m_index->lookup(5'501, min_frame, value));
    ASSERT_FALSE(value);
    ASSERT_OK(m_index->lookup(10'000, min_frame, value));
    ASSERT_FALSE(value);
}

TEST_F(HashIndexTests, Cleanup)
{
    U32 value;
    append(1);
    append(2);
    append(3);
    append(4);

    // Performing cleanup when there are no valid frames is a NOOP. The next person to write the
    // WAL index will do so at frame 1, which automatically causes the WAL index to clear itself.
    m_header.max_frame = 0;
    m_index->cleanup();
    m_header.max_frame = 4;

    ASSERT_OK(m_index->lookup(1, 1, value));
    ASSERT_EQ(value, 1);
    ASSERT_OK(m_index->lookup(2, 1, value));
    ASSERT_EQ(value, 2);
    ASSERT_OK(m_index->lookup(3, 1, value));
    ASSERT_EQ(value, 3);
    ASSERT_OK(m_index->lookup(4, 1, value));
    ASSERT_EQ(value, 4);

    m_header.max_frame = 2;
    m_index->cleanup();
    m_header.max_frame = 4;

    ASSERT_OK(m_index->lookup(1, 1, value));
    ASSERT_EQ(value, 1);
    ASSERT_OK(m_index->lookup(2, 1, value));
    ASSERT_EQ(value, 2);
    ASSERT_OK(m_index->lookup(3, 1, value));
    ASSERT_FALSE(value);
    ASSERT_OK(m_index->lookup(4, 1, value));
    ASSERT_FALSE(value);
}

TEST_F(HashIndexTests, ReadsAndWrites)
{
    std::vector<U32> keys;
    // Write 2 full index buckets + a few extra entries.
    for (U32 i = 0; i < 4'096 * 2; ++i) {
        keys.emplace_back(i);
    }
    std::default_random_engine rng(42);
    std::shuffle(begin(keys), end(keys), rng);

    for (const auto &id : keys) {
        append(id);
    }

    const U32 lower = 1'234;
    m_header.max_frame = 5'000;

    U32 value = 1;
    for (const auto &key : keys) {
        ASSERT_EQ(m_index->fetch(value), key);
        U32 current;
        ASSERT_OK(m_index->lookup(key, lower, current));
        if (m_header.max_frame < value || value < lower) {
            ASSERT_FALSE(current);
        } else {
            ASSERT_EQ(current, value);
        }
        ++value;
    }
}

TEST_F(HashIndexTests, SimulateUsage)
{
    static constexpr std::size_t kNumTestFrames = 10'000;

    RandomGenerator random;
    std::map<U32, U32> simulated;

    for (std::size_t iteration = 0; iteration < 2; ++iteration) {
        U32 lower = 1;
        for (std::size_t frame = 1; frame <= kNumTestFrames; ++frame) {
            if (const auto r = random.Next(10); r == 0) {
                // Run a commit. The calls that validate the page-frame mapping below
                // will ignore frames below "lower". This is not exactly how the WAL works,
                // we actually use 3 index headers, 2 in the index, and 1 in memory. The
                // in-index header's max_frame is used as the position of the last commit.
                lower = m_header.max_frame + 1;
                simulated.clear();
            } else {
                // Perform a write, but only if the page does not already exist in a frame
                // in the range "lower" to "m_header.max_frame", inclusive.
                U32 value;
                const U32 key = static_cast<U32>(random.Next(1, kNumTestFrames));
                ASSERT_OK(m_index->lookup(key, lower, value));
                if (value < lower) {
                    append(key);
                    simulated.insert_or_assign(key, m_header.max_frame);
                }
            }
        }
        U32 result;
        for (const auto &[key, value] : simulated) {
            ASSERT_OK(m_index->lookup(key, lower, result));
            ASSERT_EQ(result, value);
        }
        // Reset the WAL index.
        m_header.max_frame = 0;
        simulated.clear();
    }
}

class HashIteratorTests
    : public HashIndexTestBase,
      public testing::Test
{
protected:
    ~HashIteratorTests() override = default;
};

#ifndef NDEBUG
TEST_F(HashIteratorTests, EmptyIndexDeathTest)
{
    HashIterator itr(*m_index);
    ASSERT_DEATH((void)itr.init(), "");
}
#endif // NDEBUG

class HashIteratorParamTests
    : public HashIndexTestBase,
      public testing::TestWithParam<std::tuple<std::size_t, std::size_t>>
{
protected:
    HashIteratorParamTests()
        : m_num_pages(std::get<1>(GetParam())),
          m_num_copies(std::get<0>(GetParam()))
    {
    }

    ~HashIteratorParamTests() override = default;

    auto test_reordering_and_deduplication()
    {
        m_header.max_frame = 0;
        m_index->cleanup();

        for (std::size_t d = 0; d < m_num_copies; ++d) {
            for (std::size_t i = 0; i < m_num_pages; ++i) {
                append(static_cast<U32>(m_num_pages - i));
            }
        }
        HashIterator itr(*m_index);
        ASSERT_OK(itr.init());
        HashIterator::Entry entry;

        for (std::size_t i = 0;; ++i) {
            if (itr.read(entry)) {
                // Keys (page IDs) are always read in order. Values (frame IDs) should be
                // the most-recent values set for the associated key.
                ASSERT_EQ(entry.key, i + 1);
                ASSERT_EQ(entry.value, m_num_pages * m_num_copies - i);
            } else {
                ASSERT_EQ(i, m_num_pages);
                break;
            }
        }
    }

    std::size_t m_num_pages = 0;
    std::size_t m_num_copies = 0;
};

TEST_P(HashIteratorParamTests, ReorderingAndDeduplication)
{
    test_reordering_and_deduplication();
}

INSTANTIATE_TEST_SUITE_P(
    HashIteratorParamTests,
    HashIteratorParamTests,
    ::testing::Values(
        std::make_tuple(1, 1),
        std::make_tuple(1, 2),
        std::make_tuple(1, 3),
        std::make_tuple(1, 10),
        std::make_tuple(1, 100),
        std::make_tuple(1, 10'000),
        std::make_tuple(1, 100'000),
        std::make_tuple(5, 1),
        std::make_tuple(5, 2),
        std::make_tuple(5, 3),
        std::make_tuple(5, 10),
        std::make_tuple(5, 100),
        std::make_tuple(5, 10'000),
        std::make_tuple(5, 100'000)));

} // namespace calicodb::test