// Copyright (c) 2022, The CalicoDB Authors. All rights reserved.
// This source code is licensed under the MIT License, which can be found in
// LICENSE.md. See AUTHORS.md for a list of contributor names.

#ifndef CALICODB_PAGER_H
#define CALICODB_PAGER_H

#include "calicodb/env.h"
#include "frames.h"
#include <unordered_set>

namespace calicodb
{

class Env;
class FrameManager;
class TableSet;
class Wal;

class Pager final
{
public:
    friend class DBImpl;

    enum Mode {
        kOpen,
        kWrite,
        kDirty,
        kError,
    };

    struct Parameters {
        std::string filename;
        Env *env = nullptr;
        Wal *wal = nullptr;
        LogFile *log = nullptr;
        DBState *state = nullptr;
        std::size_t frame_count = 0;
        std::size_t page_size = 0;
    };

    ~Pager();
    [[nodiscard]] static auto open(const Parameters &param, Pager *&out) -> Status;
    [[nodiscard]] auto mode() const -> Mode;
    [[nodiscard]] auto page_count() const -> std::size_t;
    [[nodiscard]] auto page_size() const -> std::size_t;
    [[nodiscard]] auto bytes_read() const -> std::size_t;
    [[nodiscard]] auto bytes_written() const -> std::size_t;
    [[nodiscard]] auto begin_txn() -> bool;
    [[nodiscard]] auto rollback_txn() -> Status;
    [[nodiscard]] auto commit_txn() -> Status;
    [[nodiscard]] auto checkpoint() -> Status;
    [[nodiscard]] auto allocate(Page &page) -> Status;
    [[nodiscard]] auto acquire(Id page_id, Page &page) -> Status;
    auto upgrade(Page &page) -> void;
    auto release(Page page) -> void;
    auto set_page_count(std::size_t page_count) -> void;
    auto load_state(const FileHeader &header) -> void;

    [[nodiscard]] auto acquire_root() -> Page;

    [[nodiscard]] auto hits() const -> U64
    {
        return m_cache.hits();
    }

    [[nodiscard]] auto misses() const -> U64
    {
        return m_cache.misses();
    }

    auto TEST_validate() const -> void;

private:
    explicit Pager(const Parameters &param, File &file, AlignedBuffer buffer);
    [[nodiscard]] auto flush_to_disk() -> Status;
    [[nodiscard]] auto populate_entry(CacheEntry &out) -> Status;
    [[nodiscard]] auto cache_entry(Id page_id, CacheEntry *&out) -> Status;
    [[nodiscard]] auto is_dirty(const CacheEntry &entry) const -> bool;
    [[nodiscard]] auto read_page_from_file(CacheEntry &entry) const -> Status;
    [[nodiscard]] auto write_page_to_file(const CacheEntry &entry) const -> Status;
    [[nodiscard]] auto ensure_available_frame() -> Status;
    [[nodiscard]] auto wal_checkpoint() -> Status;
    auto purge_cache() -> void;
    auto purge_entry(CacheEntry &victim) -> void;
    auto dirty_page(CacheEntry &entry) -> void;
    auto clean_page(CacheEntry &entry) -> CacheEntry *;

    mutable std::size_t m_bytes_read = 0;
    mutable std::size_t m_bytes_written = 0;

    std::string m_filename;
    FrameManager m_frames;
    PageCache m_cache;

    // The root page is always acquired. Keep info about it here.
    CacheEntry m_root;

    // List of dirty page cache entries. Linked by the "prev" and "next"
    // CacheEntry members.
    CacheEntry *m_dirty = nullptr;

    // True if a checkpoint operation is being run, false otherwise. Used
    // to indicate failure during a checkpoint.
    bool m_in_ckpt = false;

    int m_txn = 0;

    LogFile *m_log = nullptr;
    File *m_file = nullptr;
    Env *m_env = nullptr;
    Wal *m_wal = nullptr;
    DBState *m_state = nullptr;
    std::size_t m_page_count = 0;
    Mode m_mode = kOpen;
};

} // namespace calicodb

#endif // CALICODB_PAGER_H