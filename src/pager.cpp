// Copyright (c) 2022, The CalicoDB Authors. All rights reserved.
// This source code is licensed under the MIT License, which can be found in
// LICENSE.md. See AUTHORS.md for a list of contributor names.

#include "pager.h"
#include "db_impl.h"
#include "encoding.h"
#include "header.h"
#include "logging.h"
#include "page.h"
#include "wal.h"
#include <limits>

namespace calicodb
{

auto Pager::mode() const -> Mode
{
    return m_mode;
}

auto Pager::purge_page(PageRef &victim) -> void
{
    if (victim.dirty) {
        m_dirtylist.remove(victim);
    }
    m_bufmgr.erase(victim.page_id);
}

auto Pager::read_page(PageRef &out) -> Status
{
    char *page = nullptr;
    Status s;

    if (m_state->use_wal) {
        // Try to read the page from the WAL. Nulls out "page" if it cannot find the
        // page.
        page = out.page;
        s = m_wal->read(out.page_id, page);
    }

    if (s.is_ok() && page == nullptr) {
        // Read the page from the DB file.
        s = read_page_from_file(out);
    }

    if (!s.is_ok()) {
        if (!out.page_id.is_root()) {
            m_bufmgr.erase(out.page_id);
        }
        if (m_mode != kOpen) {
            // TODO: Really, an error should only be set if m_mode == kDirty. The tree module will need to be revised a bit
            //       to make sure pages are always released before returning (this always happens on the happy path, but if
            //       there was an error, it may not happen). For now, just be a bit more strict than necessary, setting an
            //       error so the cache can be purged to fix the reference counts.
            set_status(s);
        }
    }
    return s;
}

auto Pager::read_page_from_file(PageRef &ref) const -> Status
{
    Slice slice;
    const auto offset = ref.page_id.as_index() * m_bufmgr.page_size();
    auto s = m_file->read(offset, m_bufmgr.page_size(), ref.page, &slice);
    if (s.is_ok()) {
        m_statistics.bytes_read += slice.size();
        std::memset(ref.page + slice.size(), 0, m_bufmgr.page_size() - slice.size());
    }
    return s;
}

auto Pager::write_page_to_file(const PageRef &ref) const -> Status
{
    const Slice data(ref.page, m_bufmgr.page_size());
    auto s = m_file->write(ref.page_id.as_index() * data.size(), data);
    if (s.is_ok()) {
        m_statistics.bytes_written += data.size();
    }
    return s;
}

auto Pager::open(const Parameters &param, Pager *&out) -> Status
{
    CALICODB_EXPECT_TRUE(is_power_of_two(param.page_size));
    CALICODB_EXPECT_GE(param.page_size, kMinPageSize);
    CALICODB_EXPECT_LE(param.page_size, kMaxPageSize);
    CALICODB_EXPECT_GE(param.frame_count, kMinFrameCount);
    CALICODB_EXPECT_LE(param.page_size * param.frame_count, kMaxCacheSize);

    // TODO: See TODO in db_impl.cpp about passing in the file to this constructor.
    File *file;
    CALICODB_TRY(param.env->new_file(param.filename, Env::kCreate | Env::kReadWrite, file));

    out = new Pager(param, *file);
    //    auto s = out->initialize_root(!exists);
    //    if (!s.is_ok()) {
    //        delete out;
    //        out = nullptr;
    //    }
    return Status::ok();
    //    return s;
}

auto Pager::initialize_root(bool fresh_pager) -> Status
{
    CALICODB_TRY(begin(fresh_pager));

    auto *root = m_bufmgr.root();
    auto s = read_page(*root);
    if (s.is_ok() && fresh_pager) {
        // If this is a new file, the root page is dirty since it was just
        // allocated.
        m_dirtylist.add(*root);
        m_mode = kDirty;
        ++m_page_count;
    }
    return s;
}

Pager::Pager(const Parameters &param, File &file)
    : m_state(param.state),
      m_filename(param.filename),
      m_freelist(*this, m_state->freelist_head),
      m_bufmgr(param.page_size, param.frame_count),
      m_log(param.log),
      m_file(&file),
      m_env(param.env),
      m_wal(param.wal)
{
    CALICODB_EXPECT_NE(m_state, nullptr);
}

Pager::~Pager()
{
    delete m_file;
}

auto Pager::statistics() const -> const Statistics &
{
    return m_statistics;
}

auto Pager::page_count() const -> std::size_t
{
    return m_page_count;
}

auto Pager::page_size() const -> std::size_t
{
    return m_bufmgr.page_size();
}

auto Pager::begin(bool write) -> Status
{
    CALICODB_EXPECT_NE(m_mode, kError);
    if (m_mode == kOpen) {
        CALICODB_TRY(m_file->file_lock(File::kShared));
        Status s;
        bool changed;
        for (;;) {
            s = m_wal->start_reader(changed);
            if (s.is_busy()) {
                continue;
            }
            break;
        }
        // TODO TODO TODO
        if (!s.is_ok()) {
            m_file->file_unlock();
            return s;
        }
        if (changed) {
            purge_all_pages();
        }
        // Refresh the in-memory DB root page.
        CALICODB_TRY(read_page(*m_bufmgr.root()));
        if (write) {
            s = m_wal->start_writer();
            if (!s.is_ok()) {
                m_wal->finish_reader();
                m_file->file_unlock();
                return s;
            }
        }
        m_mode = write ? kWrite : kRead;
        m_save = m_mode;
        return Status::ok();
    }
    return Status::not_supported("transaction already started");
}

auto Pager::commit() -> Status
{
    CALICODB_EXPECT_NE(m_mode, kOpen);

    // Report prior errors again.
    CALICODB_TRY(m_state->status);

    Status s;
    if (m_mode == kDirty) {
        // Write the file header to the root page if anything has changed.
        FileHeader header;
        auto root = acquire_root();
        header.read(root.data());
        const auto needs_new_header =
            header.page_count != m_page_count ||
            header.freelist_head != m_state->freelist_head.value;
        if (needs_new_header) {
            upgrade(root);
            header.page_count = static_cast<U32>(m_page_count);
            header.freelist_head = m_state->freelist_head.value;
            header.write(root.data());
        }
        release(std::move(root));

        if (m_dirtylist.head == nullptr) {
            // Ensure that there is always a WAL frame to store the DB size.
            m_dirtylist.head = m_bufmgr.root();
        }

        s = flush_all_pages();
        if (s.is_ok()) {
            CALICODB_EXPECT_FALSE(m_dirtylist.head);
            m_saved_count = m_page_count;
        }
        set_status(s);
    }
    m_mode = m_save;
    return s;
}

auto Pager::rollback() -> void
{
    CALICODB_EXPECT_TRUE(m_state->use_wal);
    CALICODB_EXPECT_NE(m_mode, kOpen);
    if (m_mode >= kDirty) {
        m_wal->rollback();
        m_page_count = m_saved_count;
        m_state->status = Status::ok();
        purge_all_pages(); // TODO: Need to check a change counter to determine if we need to purge pages
    }                      // TODO: at the start of each transaction.
    m_refresh_root = true;
    m_mode = m_save;
}

auto Pager::finish() -> void
{
    (void)m_wal->finish_reader(); // TODO
    m_file->file_unlock();
    m_mode = kOpen;
    m_save = kOpen;
}

auto Pager::purge_all_pages() -> void
{
    PageRef *victim;
    while ((victim = m_bufmgr.next_victim())) {
        CALICODB_EXPECT_NE(victim, nullptr);
        purge_page(*victim);
    }
    CALICODB_EXPECT_EQ(m_bufmgr.size(), 0);
    if (m_dirtylist.head) {
        CALICODB_EXPECT_EQ(m_dirtylist.head, m_bufmgr.root());
        CALICODB_EXPECT_FALSE(m_dirtylist.head->prev);
        CALICODB_EXPECT_FALSE(m_dirtylist.head->next);
        m_dirtylist.head->dirty = false;
        m_dirtylist.head = nullptr;
    }
}

auto Pager::checkpoint() -> Status
{
    CALICODB_EXPECT_EQ(m_mode, kWrite);
    auto s = wal_checkpoint();
    if (!s.is_ok()) {
        set_status(s);
    }
    return s;
}

auto Pager::wal_checkpoint() -> Status
{
    std::size_t dbsize;

    // Transfer the WAL contents back to the DB. Note that this call will sync the WAL
    // file before it starts transferring any data back.
    CALICODB_TRY(m_wal->checkpoint(*m_file, &dbsize));

    if (dbsize) {
        set_page_count(dbsize);
        CALICODB_TRY(m_env->resize_file(m_filename, dbsize * m_bufmgr.page_size()));
    }
    return m_file->sync();
}

auto Pager::flush_all_pages() -> Status
{
    if (m_state->use_wal) {
        auto *p = m_dirtylist.head;
        for (; p; p = p->next) {
            if (p->page_id.value > m_page_count) {
                // This page is past the current end of the file due to a vacuum operation
                // decreasing the page count. Just remove the page from the dirty list. It
                // wouldn't be transferred back to the DB on checkpoint anyway since it is
                // out of bounds.
                p = m_dirtylist.remove(*p);
            } else {
                p->dirty = false;
            }
        }
        p = m_dirtylist.head;
        m_dirtylist.head = nullptr;

        // The DB page count is specified here. This indicates that the writes are part of
        // a commit, which is always the case if this method is called while the WAL is
        // enabled.
        CALICODB_EXPECT_NE(p, nullptr);
        return m_wal->write(p, m_page_count);
    }

    for (auto *p = m_dirtylist.head; p; p = m_dirtylist.remove(*p)) {
        CALICODB_TRY(write_page_to_file(*p));
    }
    return m_file->sync();
}

auto Pager::set_status(const Status &error) const -> Status
{
    if (m_state->status.is_ok() && (error.is_io_error() || error.is_corruption())) {
        m_state->status = error;
        m_mode = kError;
    }
    return error;
}

auto Pager::set_page_count(std::size_t page_count) -> void
{
    CALICODB_EXPECT_GT(page_count, 0);
    for (auto i = page_count; i < m_page_count; ++i) {
        if (auto *out_of_range = m_bufmgr.query(Id::from_index(i))) {
            purge_page(*out_of_range);
        }
    }
    m_page_count = page_count;
}

auto Pager::ensure_available_buffer() -> Status
{
    Status s;
    if (!m_bufmgr.available()) {
        // There are no available frames, so the cache must be full. "next_victim()" will not find
        // an ref to evict if all pages are referenced, which should never happen.
        auto *victim = m_bufmgr.next_victim();
        CALICODB_EXPECT_NE(victim, nullptr);

        if (victim->dirty) {
            m_dirtylist.remove(*victim);

            if (m_state->use_wal) {
                // Write just this page to the WAL. DB page count is 0 here because this write
                // is not part of a commit.
                s = m_wal->write(&*victim, 0);
            } else {
                // WAL is not enabled, meaning this code is called from either recovery, checkpoint,
                // or initialization.
                s = write_page_to_file(*victim);
            }
            if (!s.is_ok()) {
                // This is an error, regardless of what file_lock the pager is in. Requires a successful
                // rollback and cache purge.
                set_status(s);
            }
        }
        m_bufmgr.erase(victim->page_id);
    }
    return s;
}

auto Pager::allocate(Page &page) -> Status
{
    static constexpr std::size_t kMaxPageCount = 0xFFFFFFFF - 1;
    if (m_page_count == kMaxPageCount) {
        return Status::not_supported("reached the maximum database size");
    }
    if (!m_freelist.is_empty()) {
        return m_freelist.pop(page);
    }

    const auto allocate_upgraded = [&page, this] {
        auto s = acquire(Id::from_index(m_page_count), page);
        if (s.is_ok()) {
            upgrade(page);
        }
        return s;
    };
    CALICODB_TRY(allocate_upgraded());

    // Since this is a fresh page from the end of the file, it could be a pointer map page. If so,
    // it is already blank, so just skip it and allocate another. It'll get filled in as the pages
    // following it are used by the tree layer.
    if (PointerMap::lookup(*this, page.id()) == page.id()) {
        release(std::move(page));
        return allocate_upgraded();
    }
    return Status::ok();
}

auto Pager::acquire(Id page_id, Page &page) -> Status
{
    CALICODB_EXPECT_FALSE(page_id.is_null());

    PageRef *ref;
    if (page_id.is_root()) {
        if (m_refresh_root) {
            CALICODB_TRY(read_page(*m_bufmgr.root()));
            m_refresh_root = false;
        }
        ref = m_bufmgr.root();
    } else {
        ref = m_bufmgr.get(page_id);
        if (!ref) {
            CALICODB_TRY(ensure_available_buffer());
            ref = m_bufmgr.alloc(page_id);
            if (page_id.as_index() < m_page_count) {
                CALICODB_TRY(read_page(*ref));
            } else {
                std::memset(ref->page, 0, m_bufmgr.page_size());
                m_page_count = page_id.value;
            }
        }
    }
    m_bufmgr.ref(*ref);
    page = Page(*this, *ref);
    return Status::ok();
}

auto Pager::destroy(Page page) -> Status
{
    return m_freelist.push(std::move(page));
}

auto Pager::acquire_root() -> Page
{
    m_bufmgr.ref(*m_bufmgr.root());
    return Page(*this, *m_bufmgr.root());
}

auto Pager::upgrade(Page &page) -> void
{
    CALICODB_EXPECT_TRUE(
        !m_state->use_wal || // In initialization routine
        m_mode >= kWrite);   // Transaction has started

    if (!page.m_ref->dirty) {
        m_dirtylist.add(*page.m_ref);
        if (m_mode == kWrite) {
            m_mode = kDirty;
        }
    }
    CALICODB_EXPECT_FALSE(page.m_write);
    page.m_write = true;
}

auto Pager::release(Page page) -> void
{
    CALICODB_EXPECT_GT(page.m_ref->refcount, 0);
    m_bufmgr.unref(*page.m_ref);
    page.m_pager = nullptr;
}

auto Pager::load_state(const FileHeader &header) -> void
{
    m_page_count = header.page_count;
    m_saved_count = header.page_count;
}

auto Pager::TEST_validate() const -> void
{
#ifndef NDEBUG
    // Some caller has a live page.
    CALICODB_EXPECT_EQ(m_bufmgr.refsum(), 0);

    if (m_mode <= kWrite) {
        CALICODB_EXPECT_FALSE(m_dirtylist.head);
    } else {
        if (m_mode == kDirty) {
            CALICODB_EXPECT_TRUE(m_dirtylist.head);
        }
        auto *p = m_dirtylist.head;
        while (p) {
            CALICODB_EXPECT_TRUE(p->dirty);
            p = p->next;
        }
    }
#endif // NDEBUG
}

// The first pointer map page is always on page 2, right after the root page.
static constexpr std::size_t kFirstMapPage = 2;

static constexpr auto kEntrySize =
    sizeof(char) + // Type (1 B)
    Id::kSize;     // Back pointer (4 B)

static auto entry_offset(Id map_id, Id page_id) -> std::size_t
{
    CALICODB_EXPECT_LT(map_id, page_id);
    return (page_id.value - map_id.value - 1) * kEntrySize;
}

static auto decode_entry(const char *data) -> PointerMap::Entry
{
    PointerMap::Entry entry;
    entry.type = PointerMap::Type{*data++};
    entry.back_ptr.value = get_u32(data);
    return entry;
}

auto PointerMap::lookup(const Pager &pager, Id page_id) -> Id
{
    CALICODB_EXPECT_FALSE(page_id.is_null());

    // Root page (1) has no parents, and page 2 is the first pointer map page. If "page_id" is a pointer map
    // page, "page_id" will be returned.
    if (page_id.value < kFirstMapPage) {
        return Id::null();
    }
    const auto inc = pager.page_size() / kEntrySize + 1;
    const auto idx = (page_id.value - kFirstMapPage) / inc;
    return Id(idx * inc + kFirstMapPage);
}

auto PointerMap::is_map(const Pager &pager, Id page_id) -> bool
{
    return lookup(pager, page_id) == page_id;
}

auto PointerMap::read_entry(Pager &pager, Id page_id, Entry &out) -> Status
{
    const auto mid = lookup(pager, page_id);
    CALICODB_EXPECT_LE(kFirstMapPage, mid.value);
    CALICODB_EXPECT_NE(mid, page_id);

    const auto offset = entry_offset(mid, page_id);
    CALICODB_EXPECT_LE(offset + kEntrySize, pager.page_size());

    Page map;
    CALICODB_TRY(pager.acquire(mid, map));
    out = decode_entry(map.data() + offset);
    pager.release(std::move(map));
    return Status::ok();
}

auto PointerMap::write_entry(Pager &pager, Id page_id, Entry entry) -> Status
{
    const auto mid = lookup(pager, page_id);
    CALICODB_EXPECT_LE(kFirstMapPage, mid.value);
    CALICODB_EXPECT_NE(mid, page_id);

    const auto offset = entry_offset(mid, page_id);
    CALICODB_EXPECT_LE(offset + kEntrySize, pager.page_size());

    Page map;
    CALICODB_TRY(pager.acquire(mid, map));
    const auto [back_ptr, type] = decode_entry(map.data() + offset);
    if (entry.back_ptr != back_ptr || entry.type != type) {
        if (!map.is_writable()) {
            pager.upgrade(map);
        }
        auto data = map.data() + offset;
        *data++ = entry.type;
        put_u32(data, entry.back_ptr.value);
    }
    pager.release(std::move(map));
    return Status::ok();
}

Freelist::Freelist(Pager &pager, Id &head)
    : m_pager{&pager},
      m_head{&head}
{
}

[[nodiscard]] auto Freelist::is_empty() const -> bool
{
    return m_head->is_null();
}

auto Freelist::pop(Page &page) -> Status
{
    if (!m_head->is_null()) {
        CALICODB_TRY(m_pager->acquire(*m_head, page));
        m_pager->upgrade(page);
        *m_head = read_next_id(page);

        if (!m_head->is_null()) {
            // Only clear the back pointer for the new freelist head. Callers must make sure to update the returned
            // node's back pointer at some point.
            const PointerMap::Entry entry = {Id::null(), PointerMap::kFreelistLink};
            CALICODB_TRY(PointerMap::write_entry(*m_pager, *m_head, entry));
        }
        return Status::ok();
    }
    return Status::not_supported("free list is empty");
}

auto Freelist::push(Page page) -> Status
{
    CALICODB_EXPECT_FALSE(page.id().is_root());
    write_next_id(page, *m_head);

    // Write the parent of the old head, if it exists.
    PointerMap::Entry entry = {page.id(), PointerMap::kFreelistLink};
    if (!m_head->is_null()) {
        CALICODB_TRY(PointerMap::write_entry(*m_pager, *m_head, entry));
    }
    // Clear the parent of the new head.
    entry.back_ptr = Id::null();
    CALICODB_TRY(PointerMap::write_entry(*m_pager, page.id(), entry));

    *m_head = page.id();
    m_pager->release(std::move(page));
    return Status::ok();
}

} // namespace calicodb
