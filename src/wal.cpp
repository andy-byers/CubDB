// Copyright (c) 2022, The CalicoDB Authors. All rights reserved.
// This source code is licensed under the MIT License, which can be found in
// LICENSE.md. See AUTHORS.md for a list of contributor names.

#include "wal.h"
#include "bufmgr.h"
#include "calicodb/env.h"
#include "encoding.h"
#include "logging.h"
#include <atomic>

namespace calicodb
{

// TODO: See https://stackoverflow.com/questions/8759429/atomic-access-to-shared-memory
//       May need to construct atomic integers for some variables, like the CkptInfo
//       struct. SQLite WAL index seems to work fine without atomic intrinsics though
//       so we'll see.
static_assert(std::atomic<U32>::is_always_lock_free);

using Key = HashIndex::Key;
using Value = HashIndex::Value;
using Hash = U16;

using Ptr = char *const;
using ConstPtr = const char *const;

static constexpr std::size_t kReadmarkNotUsed = 0xFFFFFFFF;
static constexpr std::size_t kWriteLock = 0;
static constexpr std::size_t kNotWriteLock = 1;
static constexpr std::size_t kCkptLock = 1;
// static constexpr std::size_t kRecoverLock = 2; TODO
static constexpr std::size_t kReaderCount = File::kShmLockCount - 3;
#define READ_LOCK(i) ((i) + 3)

struct CkptInfo {
    U32 backfill;
    U32 readmark[kReaderCount];
    U8 locks[File::kShmLockCount];
    U32 backfill_attempted;
    U32 reserved;
};

// static constexpr std::size_t kIndexLockOffset = sizeof(HashIndexHdr) * 2 + offsetof(CkptInfo, locks);
static constexpr std::size_t kIndexHeaderSize = sizeof(HashIndexHdr) * 2 + sizeof(CkptInfo);

// Header is stored at the start of the first index group. std::memcpy() is used
// on the struct, so it needs to be a POD (or at least trivially copiable). Its
// size should be a multiple of 4 to prevent misaligned accesses.
static_assert(std::is_pod_v<HashIndexHdr>);
static_assert((kIndexHeaderSize & 0b11) == 0);

static constexpr U32 kHashPrime = 383;
static constexpr U32 kNIndexHashes = 8192;
static constexpr U32 kNIndexKeys = 4096;
static constexpr U32 kNIndexKeys0 =
    kNIndexKeys - kIndexHeaderSize / sizeof(U32);
static constexpr U32 kIndexPageSize =
    kNIndexKeys * sizeof(U32) +
    kNIndexHashes * sizeof(U16);

static constexpr auto index_group_number(Value value) -> U32
{
    return (value - 1 + kNIndexKeys - kNIndexKeys0) / kNIndexKeys;
}

static auto index_hash(Key key) -> Hash
{
    return key * kHashPrime & (kNIndexHashes - 1);
}

static constexpr auto next_index_hash(Hash hash) -> Hash
{
    return (hash + 1) & (kNIndexHashes - 1);
}

static auto too_many_collisions(Key key) -> Status
{
    std::string message;
    append_fmt_string(message, "too many WAL index collisions for page %u", key);
    return Status::corruption(message);
}

struct HashGroup {
    explicit HashGroup(U32 group_number, volatile char *data)
    {
        keys = reinterpret_cast<volatile Key *>(data);
        hash = reinterpret_cast<volatile Hash *>(keys + kNIndexKeys);
        if (group_number) {
            base = kNIndexKeys0 + (kNIndexKeys * (group_number - 1));
        } else {
            keys += kIndexHeaderSize / sizeof *keys;
        }
    }

    volatile Key *keys = nullptr;
    volatile Hash *hash = nullptr;
    U32 base = 0;
};

HashIndex::HashIndex(HashIndexHdr &header, File *file)
    : m_hdr(&header),
      m_file(file)
{
}

HashIndex::~HashIndex()
{
    if (!m_file) {
        for (const auto *group : m_groups) {
            delete[] group;
        }
    }
}

auto HashIndex::header() -> volatile HashIndexHdr *
{
    CALICODB_EXPECT_FALSE(m_groups.empty());
    CALICODB_EXPECT_TRUE(m_groups[0]);
    return reinterpret_cast<volatile HashIndexHdr *>(m_groups[0]);
}

auto HashIndex::lookup(Key key, Value lower, Value &out) -> Status
{
    out = 0;

    const auto upper = m_hdr->max_frame;
    if (upper == 0) {
        return Status::ok();
    }
    if (lower == 0) {
        lower = 1;
    }
    const auto min_group_number = index_group_number(lower);

    for (auto n = index_group_number(m_hdr->max_frame);; --n) {
        CALICODB_TRY(map_group(n));
        HashGroup group(n, m_groups[n]);
        // The guard above prevents considering groups that haven't been allocated yet.
        // Such groups would start past the current "max_frame".
        CALICODB_EXPECT_LE(group.base, m_hdr->max_frame);
        auto collisions = kNIndexHashes;
        auto key_hash = index_hash(key);
        Hash relative;

        // Find the WAL frame containing the given page. Limit the search to the set of
        // valid frames (in the range "lower" to "m_hdr->max_frame", inclusive).
        while ((relative = group.hash[key_hash])) {
            if (collisions-- == 0) {
                return too_many_collisions(key);
            }
            const auto absolute = relative + group.base;
            const auto found =
                absolute >= lower &&
                absolute <= upper &&
                group.keys[relative - 1] == key;
            if (found) {
                out = absolute;
            }
            key_hash = next_index_hash(key_hash);
        }
        if (out || n <= min_group_number) {
            break;
        }
    }
    return Status::ok();
}

auto HashIndex::fetch(Value value) -> Key
{
    const auto n = index_group_number(value);
    if (n >= m_groups.size()) {
        return 0;
    }
    CALICODB_EXPECT_LT(n, m_groups.size());
    CALICODB_EXPECT_TRUE(m_groups[n]);
    const HashGroup group(n, m_groups[n]);
    if (group.base) {
        return group.keys[(value - kNIndexKeys0 - 1) % kNIndexKeys];
    } else {
        return group.keys[value - 1];
    }
}

auto HashIndex::assign(Key key, Value value) -> Status
{
    const auto group_number = index_group_number(value);
    const auto key_capacity = group_number ? kNIndexKeys : kNIndexKeys0;

    CALICODB_TRY(map_group(group_number));
    HashGroup group(group_number, m_groups[group_number]);

    CALICODB_EXPECT_LT(group.base, value);
    const auto relative = value - group.base;
    CALICODB_EXPECT_LE(relative, key_capacity);
    if (relative == 1) {
        // Clear the whole group when the first entry is inserted.
        const auto group_size =
            key_capacity * sizeof *group.keys +
            kNIndexHashes * sizeof *group.hash;
        std::memset(const_cast<Key *>(group.keys), 0, group_size);
    }

    if (group.keys[relative - 1]) {
        cleanup();
        CALICODB_EXPECT_EQ(group.keys[relative - 1], 0);
    }

    CALICODB_EXPECT_LE(relative, key_capacity);

    auto key_hash = index_hash(key);
    // Use the relative frame index as the number of allowed collisions. This value is always
    // 1 more than the number of entries, so the worst case will succeed. Note that this only
    // works because frames are written in monotonically increasing order.
    auto collisions = relative;

    // Find the first unused hash slot. Collisions are handled by incrementing the key until an
    // unused slot is found, wrapping back to the start if the end is hit. There are always more
    // hash slots than frames, so this search will always terminate.
    for (; group.hash[key_hash]; key_hash = next_index_hash(key_hash)) {
        if (collisions-- == 0) {
            return too_many_collisions(key);
        }
    }

    group.hash[key_hash] = static_cast<Hash>(relative);
    group.keys[relative - 1] = key;
    return Status::ok();
}

auto HashIndex::map_group(std::size_t group_number) -> Status
{
    while (group_number >= m_groups.size()) {
        m_groups.emplace_back();
    }
    if (m_groups[group_number] == nullptr) {
        volatile void *ptr;
        if (m_file) {
            CALICODB_TRY(m_file->shm_map(group_number, ptr));
        } else {
            ptr = new char[kIndexPageSize]();
        }
        m_groups[group_number] = Ptr(ptr);
    }
    return Status::ok();
}

auto HashIndex::groups() const -> const std::vector<volatile char *> &
{
    return m_groups;
}

auto HashIndex::cleanup() -> void
{
    if (m_hdr->max_frame != 0) {
        const auto n = index_group_number(m_hdr->max_frame);
        CALICODB_EXPECT_TRUE(m_groups[n]); // Must already be mapped
        HashGroup group(n, m_groups[n]);
        const auto max_hash = m_hdr->max_frame - group.base;
        for (std::size_t i = 0; i < kNIndexHashes; ++i) {
            if (group.hash[i] > max_hash) {
                group.hash[i] = 0;
            }
        }
        // Clear the keys that correspond to cleared hash slots.
        const auto rest_size = static_cast<std::uintptr_t>(
            ConstPtr(group.hash) -
            ConstPtr(group.keys + max_hash));
        std::memset(const_cast<Key *>(group.keys + max_hash), 0, rest_size);
    }
}

// Merge 2 sorted lists.
static auto merge_lists(
    const Key *keys,
    Hash *left,
    U32 left_size,
    Hash *&right,
    U32 &right_size,
    Hash *scratch)
{
    U32 L = 0;
    U32 r = 0;
    U32 i = 0;

    while (L < left_size || r < right_size) {
        Hash hash;
        if (L < left_size && (r >= right_size || keys[left[L]] < keys[right[r]])) {
            hash = left[L++];
        } else {
            hash = right[r++];
        }
        scratch[i++] = hash;
        if (L < left_size && keys[left[L]] == keys[hash]) {
            ++L;
        }
    }

    right = left;
    right_size = i;
    std::memcpy(left, scratch, i * sizeof *scratch);
}

static auto mergesort(
    const Key *keys,
    Hash *hashes,
    Hash *scratch,
    U32 &size)
{
    struct SubList {
        Hash *ptr;
        U32 len;
    };

    static constexpr std::size_t kMaxBits = 13;

    SubList sublists[kMaxBits] = {};
    U32 sub_index = 0;
    U32 n_merge;
    Hash *p_merge;

    CALICODB_EXPECT_EQ(kNIndexKeys, 1 << (ARRAY_SIZE(sublists) - 1));
    CALICODB_EXPECT_LE(size, kNIndexKeys);
    CALICODB_EXPECT_GT(size, 0);

    for (U32 L = 0; L < size; ++L) {
        p_merge = hashes + L;
        n_merge = 1;

        for (sub_index = 0; L & (1 << sub_index); ++sub_index) {
            auto *sub = &sublists[sub_index];
            CALICODB_EXPECT_LT(sub_index, ARRAY_SIZE(sublists));
            CALICODB_EXPECT_NE(sub->ptr, nullptr);
            CALICODB_EXPECT_LE(sub->len, 1U << sub_index);
            CALICODB_EXPECT_EQ(sub->ptr, hashes + (L & ~U32((2 << sub_index) - 1)));
            merge_lists(keys, sub->ptr, sub->len, p_merge, n_merge, scratch);
        }
        sublists[sub_index].ptr = p_merge;
        sublists[sub_index].len = n_merge;
    }
    for (++sub_index; sub_index < kMaxBits; ++sub_index) {
        if (size & (1 << sub_index)) {
            auto *sub = sublists + sub_index;
            merge_lists(keys, sub->ptr, sub->len, p_merge, n_merge, scratch);
        }
    }
    CALICODB_EXPECT_EQ(p_merge, hashes);
    size = n_merge;

#ifndef NDEBUG
    for (std::size_t i = 1; i < size; ++i) {
        CALICODB_EXPECT_GT(keys[hashes[i]], keys[hashes[i - 1]]);
    }
#endif // NDEBUG
}

HashIterator::HashIterator(HashIndex &source)
    : m_source(&source)
{
}

HashIterator::~HashIterator()
{
    operator delete(m_state, std::align_val_t{alignof(State)});
}

auto HashIterator::init() -> Status
{
    const auto *const hdr = m_source->m_hdr;

    // This method should not be called on an empty index.
    const auto last_value = hdr->max_frame;
    CALICODB_EXPECT_GT(last_value, 0);

    // TODO: Hopefully this makes this struct hack thing OK in C++... I would have tried to
    //       use std::aligned_storage, but there is that buffer of U16s allocated right
    //       after the array of State::Groups.
    static_assert(std::is_pod_v<State>);
    static_assert(std::is_pod_v<State::Group>);
    static_assert(0 == (alignof(State) & (alignof(Hash) - 1)));

    // Allocate internal buffers.
    m_num_groups = index_group_number(last_value) + 1;
    const auto state_size =
        sizeof(State) +                             // Includes storage for 1 group ("groups[1]").
        (m_num_groups - 1) * sizeof(State::Group) + // Additional groups.
        last_value * sizeof(Hash);                  // Indices to sort.
    m_state = reinterpret_cast<State *>(
        operator new(state_size, std::align_val_t{alignof(State)}));
    std::memset(m_state, 0, state_size);

    // Temporary buffer for the mergesort routine. Freed before returning from this routine.
    auto *temp = new Hash[last_value < kNIndexKeys ? last_value : kNIndexKeys]();

    for (U32 i = 0; i < m_num_groups; ++i) {
        CALICODB_TRY(m_source->map_group(i));
        HashGroup group(i, m_source->m_groups[i]);

        U32 group_size = kNIndexKeys;
        if (i + 1 == m_num_groups) {
            group_size = last_value - group.base;
        } else if (i == 0) {
            group_size = kNIndexKeys0;
        }

        // Pointer into the special index buffer located right after the group array.
        auto *index = reinterpret_cast<Hash *>(&m_state->groups[m_num_groups]) + group.base;

        for (std::size_t j = 0; j < group_size; ++j) {
            index[j] = static_cast<Hash>(j);
        }
        mergesort(const_cast<const Key *>(group.keys), index, temp, group_size);
        m_state->groups[i].base = group.base + 1;
        m_state->groups[i].size = group_size;
        m_state->groups[i].keys = const_cast<Key *>(group.keys);
        m_state->groups[i].index = index;
    }
    delete[] temp;
    return Status::ok();
}

auto HashIterator::read(Entry &out) -> bool
{
    static constexpr U32 kBadResult = 0xFFFFFFFF;
    CALICODB_EXPECT_LT(m_prior, kBadResult);
    auto result = kBadResult;

    auto *last_group = m_state->groups + m_num_groups - 1;
    for (std::size_t i = 0; i < m_num_groups; ++i) {
        auto *group = last_group - i;
        while (group->next < group->size) {
            const auto key = group->keys[group->index[group->next]];
            if (key > m_prior) {
                if (key < result) {
                    result = key;
                    out.value = group->base + group->index[group->next];
                }
                break;
            }
            ++group->next;
        }
    }
    m_prior = result;
    out.key = m_prior;
    return result != kBadResult;
}

// WAL header layout:
//     Offset  Size  Purpose
//    ---------------------------------------
//     0       4     Magic number (1559861749)
//     4       4     WAL version (1)
//     8       4     DB page size
//     12      4     Checkpoint number
//     16      4     Salt-1
//     20      4     Salt-2
//     24      4     Checksum-1
//     28      4     Checksum-2
//
static constexpr std::size_t kWalHdrSize = 32;
static constexpr U32 kWalMagic = 1559861749;
static constexpr U32 kWalVersion = 1;

// WAL frame header layout:
//     Offset  Size  Purpose
//    ---------------------------------------
//     0       4     Page number
//     4       4     DB size in pages (> 0 if commit frame)
//     8       4     Salt-1
//     12      4     Salt-2
//     16      4     Checksum-1
//     20      4     Checksum-2
//
struct WalFrameHdr {
    static constexpr std::size_t kSize = 24;

    U32 pgno = 0;

    // DB header page count after a commit (nonzero for commit frames, 0 for
    // all other frames).
    U32 db_size = 0;
};

static auto compute_checksum(const Slice &in, const U32 *initial, U32 *out)
{
    CALICODB_EXPECT_NE(out, nullptr);
    CALICODB_EXPECT_EQ(std::uintptr_t(in.data()) & 3, 0);
    CALICODB_EXPECT_LE(in.size(), 65'536);
    CALICODB_EXPECT_EQ(in.size() & 7, 0);
    CALICODB_EXPECT_GT(in.size(), 0);

    U32 s1 = 0;
    U32 s2 = 0;
    if (initial) {
        s1 = initial[0];
        s2 = initial[1];
    }

    const auto *ptr = reinterpret_cast<const U32 *>(in.data());
    const auto *end = ptr + in.size() / sizeof *ptr;

    do {
        s1 += *ptr++ + s2;
        s2 += *ptr++ + s1;
    } while (ptr < end);

    out[0] = s1;
    out[1] = s2;
}

class WalImpl : public Wal
{
public:
    friend auto TEST_print_wal(const Wal &) -> void;

    explicit WalImpl(const Parameters &param, File &wal_file, File &shm_file);
    ~WalImpl() override;

    [[nodiscard]] auto open() -> Status;

    [[nodiscard]] auto read(Id page_id, char *&page) -> Status override;
    [[nodiscard]] auto write(const PageRef *dirty, std::size_t db_size) -> Status override;
    [[nodiscard]] auto needs_checkpoint() const -> bool override;
    [[nodiscard]] auto checkpoint(File &db_file, std::size_t *db_size) -> Status override;

    [[nodiscard]] auto sync() -> Status override
    {
        return m_wal_file->sync();
    }

    auto rollback() -> void override
    {
        CALICODB_EXPECT_TRUE(m_writer_lock);
        const auto max_frame = m_hdr.max_frame;
        m_hdr = *const_cast<HashIndexHdr *>(m_index.header());
        if (max_frame != m_hdr.max_frame) {
            m_index.cleanup();
        }
    }

    [[nodiscard]] auto close() -> Status override
    {
        m_shm_file->shm_unmap(true);
        delete m_shm_file;
        m_shm_file = nullptr;
        if (m_hdr.max_frame == 0) {
            return m_env->remove_file(m_filename);
        }
        return sync();
    }

    [[nodiscard]] auto start_reader(bool &changed) -> Status override
    {
        Status s;
        unsigned tries = 0;
        do {
            s = try_reader(false, tries++, changed);
        } while (s.is_busy());
        return s;
    }
    auto finish_reader() -> void override
    {
        finish_writer();
        if (m_reader_lock >= 0) {
            unlock_shared(READ_LOCK(m_reader_lock));
            m_reader_lock = -1;
        }
    }

    [[nodiscard]] auto start_writer() -> Status override
    {
        if (m_writer_lock) {
            return Status::ok();
        }
        CALICODB_EXPECT_GE(m_reader_lock, 0);
        CALICODB_EXPECT_EQ(m_redo_cksum, 0);

        CALICODB_TRY(lock_exclusive(kWriteLock, 1));
        m_writer_lock = 1;

        if (0 != std::memcmp(&m_hdr, ConstPtr(m_index.header()), sizeof(HashIndexHdr))) {
            unlock_exclusive(kWriteLock, 1);
            m_writer_lock = 0;
            return Status::busy("retry");
        }
        return Status::ok();
    }
    auto finish_writer() -> void override
    {
        if (m_writer_lock) {
            unlock_exclusive(kWriteLock, 1);
            m_redo_cksum = 0;
            m_writer_lock = 0;
        }
    }

    [[nodiscard]] auto statistics() const -> WalStatistics override
    {
        return m_stats;
    }

private:
    auto shm_barrier() const -> void
    {
        if (m_shm_file) {
            m_shm_file->shm_barrier();
        }
    }
    [[nodiscard]] auto lock_shared(std::size_t r) -> Status
    {
        return m_shm_file->shm_lock(r, 1, File::kLock | File::kReader);
    }
    auto unlock_shared(std::size_t r) -> void
    {
        (void)m_shm_file->shm_lock(r, 1, File::kUnlock | File::kReader);
    }

    [[nodiscard]] auto lock_exclusive(std::size_t r, std::size_t n) -> Status
    {
        return m_shm_file->shm_lock(r, n, File::kLock | File::kWriter);
    }
    auto unlock_exclusive(std::size_t r, std::size_t n) -> void
    {
        (void)m_shm_file->shm_lock(r, n, File::kUnlock | File::kWriter);
    }

    [[nodiscard]] auto get_ckpt_info() -> volatile CkptInfo *
    {
        CALICODB_EXPECT_FALSE(m_index.groups().empty());
        CALICODB_EXPECT_TRUE(m_index.groups().front());
        return reinterpret_cast<volatile CkptInfo *>(
            &m_index.groups().front()[sizeof(HashIndexHdr) * 2]);
    }
    [[nodiscard]] auto try_index_header(bool &changed) -> bool
    {
        HashIndexHdr h1, h2;
        U32 cksum[2];

        // TODO: SQLite has this marked as a TSan false positive. May need to tag this method with some attribute for the compiler.
        const auto *hdr = m_index.header();
        std::memcpy(&h1, ConstPtr(&hdr[0]), sizeof(h1));
        shm_barrier();
        std::memcpy(&h2, ConstPtr(&hdr[1]), sizeof(h2));

        if (0 != std::memcmp(&h1, &h2, sizeof(h1))) {
            return false;
        }
        if (h1.flags == 0) {
            return false;
        }
        const Slice target(ConstPtr(&h1), sizeof(h1) - sizeof(h1.cksum));
        compute_checksum(target, nullptr, cksum);

        if (cksum[0] != h1.cksum[0] || cksum[1] != h1.cksum[1]) {
            return false;
        }
        if (0 != std::memcmp(&m_hdr, &h1, sizeof(m_hdr))) {
            std::memcpy(&m_hdr, &h1, sizeof(m_hdr));
            changed = true;
        }
        return true;
    }
    [[nodiscard]] auto read_index_header(bool &changed) -> Status
    {
        CALICODB_TRY(m_index.map_group(0));

        Status s;
        auto success = try_index_header(changed);
        if (!success) {
            const auto prev_lock = m_writer_lock;
            if (prev_lock || (s = lock_exclusive(kWriteLock, 1)).is_ok()) {
                m_writer_lock = 1;
                if ((s = m_index.map_group(0)).is_ok()) {
                    success = try_index_header(changed);
                    if (!success) {
                        s = recover_index();
                        changed = true;
                    }
                }
                if (prev_lock == 0) {
                    unlock_exclusive(kWriteLock, 1);
                    m_writer_lock = 0;
                }
            }
        }
        if (success && m_hdr.version != kWalVersion) {
            std::string message;
            append_fmt_string(
                message,
                "version mismatch (encountered %u but expected %u)",
                m_hdr.version,
                kWalVersion);
            return Status::not_supported(message);
        }
        return s;
    }
    auto write_index_header() -> void
    {
        m_hdr.flags = HashIndexHdr::INITIALIZED;
        m_hdr.version = kWalVersion;

        const Slice target(Ptr(&m_hdr), offsetof(HashIndexHdr, cksum));
        compute_checksum(target, nullptr, m_hdr.cksum);

        volatile auto *hdr = m_index.header();
        std::memcpy(const_cast<HashIndexHdr *>(&hdr[1]), &m_hdr, sizeof(m_hdr));
        shm_barrier();
        std::memcpy(const_cast<HashIndexHdr *>(&hdr[0]), &m_hdr, sizeof(m_hdr));
    }

    auto restart_header(U32 salt_1) -> void
    {
        ++m_ckpt_number;
        m_hdr.max_frame = 0;
        auto *salt = reinterpret_cast<char *>(m_hdr.salt);
        put_u32(salt, get_u32(salt) + 1);
        std::memcpy(salt + sizeof(U32), &salt_1, sizeof(salt_1));
        write_index_header();

        auto *info = get_ckpt_info();
        CALICODB_EXPECT_EQ(info->readmark[0], 0);
        info->backfill_attempted = 0;
        info->backfill = 0;
        info->readmark[1] = 0;
        for (std::size_t i = 2; i < kReaderCount; ++i) {
            info->readmark[i] = kReadmarkNotUsed;
        }
    }
    [[nodiscard]] auto restart_log() -> Status
    {
        Status s;
        if (m_reader_lock == 0) {
            const auto *info = get_ckpt_info();
            CALICODB_EXPECT_EQ(info->backfill, m_hdr.max_frame);
            if (info->backfill) {
                const auto salt_1 = m_env->rand();
                s = lock_exclusive(READ_LOCK(1), kReaderCount - 1);
                if (s.is_ok()) {
                    restart_header(salt_1);
                    unlock_exclusive(READ_LOCK(1), kReaderCount - 1);
                } else if (!s.is_busy()) {
                    return s;
                }
            }
            unlock_shared(READ_LOCK(0));
            m_reader_lock = -1;
            unsigned tries = 0;
            do {
                bool unused;
                s = try_reader(true, tries, unused);
            } while (s.is_busy());
        }
        return s;
    }

    [[nodiscard]] auto try_reader(bool use_wal, unsigned tries, bool &changed) -> Status
    {
        CALICODB_EXPECT_LT(m_reader_lock, 0);
        Status s;
        if (tries > 5) {
            if (tries > 100) {
                m_lock_error = true;
                return Status::corruption("protocol error");
            }
            unsigned delay = 1;
            if (tries >= 10) {
                delay = (tries - 9) * (tries - 9) * 39;
            }
            (void)delay; // TODO: m_env->sleep(delay);
        }

        if (!use_wal) {
            CALICODB_TRY(read_index_header(changed));
        }

        CALICODB_EXPECT_FALSE(m_index.groups().empty());
        CALICODB_EXPECT_TRUE(m_index.groups().front());
        volatile auto *info = get_ckpt_info();
        if (!use_wal && info->backfill == m_hdr.max_frame) {
            s = lock_shared(READ_LOCK(0));
            shm_barrier();
            if (s.is_ok()) {
                if (0 != std::memcmp(ConstPtr(m_index.header()), &m_hdr, sizeof(m_hdr))) {
                    unlock_shared(READ_LOCK(0));
                    return Status::busy("retry");
                }
                m_reader_lock = 0;
                return Status::ok();
            } else if (!s.is_busy()) {
                return s;
            }
        }

        std::size_t max_readmark = 0;
        std::size_t max_index = 0;
        std::size_t max_frame = m_hdr.max_frame;

        for (std::size_t i = 1; i < kReaderCount; i++) {
            const auto mark = info->readmark[i];
            if (max_readmark <= mark && mark <= max_frame) {
                CALICODB_EXPECT_NE(mark, kReadmarkNotUsed);
                max_readmark = mark;
                max_index = i;
            }
        }
        if (max_readmark < max_frame || max_index == 0) {
            for (std::size_t i = 1; i < kReaderCount; ++i) {
                s = lock_exclusive(READ_LOCK(i), 1);
                if (s.is_ok()) {
                    info->readmark[i] = max_frame;
                    max_readmark = max_frame;
                    max_index = i;
                    unlock_exclusive(READ_LOCK(i), 1);
                    break;
                } else if (!s.is_busy()) {
                    return s;
                }
            }
        }
        if (max_index == 0) {
            return Status::busy("retry");
        }

        CALICODB_TRY(lock_shared(READ_LOCK(max_index)));

        m_min_frame = info->backfill + 1;
        shm_barrier();
        const auto busy =
            info->readmark[max_index] != max_readmark ||
            0 != std::memcmp(ConstPtr(m_index.header()), &m_hdr, sizeof(HashIndexHdr));
        if (busy) {
            unlock_shared(READ_LOCK(max_index));
            return Status::busy("retry");
        } else {
            CALICODB_EXPECT_LE(max_readmark, m_hdr.max_frame);
            m_reader_lock = static_cast<int>(max_index);
        }
        return s;
    }

    [[nodiscard]] auto frame_offset(U32 frame) const -> std::size_t
    {
        CALICODB_EXPECT_GT(frame, 0);
        return kWalHdrSize + (frame - 1) * (WalFrameHdr::kSize + m_page_size);
    }

    [[nodiscard]] auto rewrite_checksums(U32 end) -> Status;
    [[nodiscard]] auto recover_index() -> Status;
    [[nodiscard]] auto decode_frame(const char *frame, WalFrameHdr &out) -> bool;
    auto encode_frame(const WalFrameHdr &hdr, const char *page, char *out) -> void;

    WalStatistics m_stats;
    HashIndexHdr m_hdr = {};
    HashIndex m_index;

    std::string m_filename;

    // Storage for a single WAL frame.
    std::string m_frame;

    U32 m_page_size = 0;
    U32 m_redo_cksum = 0;
    U32 m_backfill = 0;
    U32 m_ckpt_number = 0;

    Env *m_env = nullptr;
    File *m_shm_file = nullptr;
    File *m_wal_file = nullptr;

    U32 m_min_frame = 0;

    int m_reader_lock = -1;
    int m_writer_lock = 0;
    int m_ckpt_lock = 0;
    bool m_lock_error = false;

    //    bool m_readonly = false; // TODO: readonly connections
    //    bool m_shm_unreliable = false; // TODO: specific situation where this connection is readonly
    //       and no writer is connected that can keep the shm file up-to-date
};

auto Wal::open(const Parameters &param, Wal *&out) -> Status
{
    File *wal_file, *shm_file;
    CALICODB_TRY(param.env->new_file(param.wal_filename, Env::kCreate | Env::kReadWrite, wal_file));
    auto s = param.env->new_file(param.shm_filename, Env::kCreate | Env::kReadWrite, shm_file);
    if (!s.is_ok()) {
        delete wal_file;
        return s;
    }
    auto *wal = new WalImpl(param, *wal_file, *shm_file);
    out = wal;
    return wal->open();
}

auto Wal::close(Wal *&wal) -> Status
{
    Status s;
    if (wal) {
        s = wal->close();
        delete wal;
        wal = nullptr;
    }
    return s;
}

Wal::~Wal() = default;

WalImpl::WalImpl(const Parameters &param, File &wal_file, File &shm_file)
    : m_index(m_hdr, &shm_file),
      m_filename(param.wal_filename),
      m_frame(WalFrameHdr::kSize + param.page_size, '\0'),
      m_page_size(param.page_size),
      m_env(param.env),
      m_shm_file(&shm_file),
      m_wal_file(&wal_file)
{
    (void)m_backfill;
}

WalImpl::~WalImpl()
{
    delete m_wal_file;
}

auto WalImpl::open() -> Status
{
    return Status::ok();
}

auto WalImpl::rewrite_checksums(U32 end) -> Status
{
    CALICODB_EXPECT_GT(m_redo_cksum, 0);

    // Find the offset of the previous checksum in the WAL file. If we are starting at
    // the first frame, get the previous checksum from the WAL header.
    std::size_t cksum_offset = 24;
    if (m_redo_cksum > 1) {
        cksum_offset = frame_offset(m_redo_cksum - 1) + 16;
    }

    CALICODB_TRY(m_wal_file->read_exact(cksum_offset, 2 * sizeof(U32), m_frame.data()));
    m_hdr.frame_cksum[0] = get_u32(&m_frame[0]);
    m_hdr.frame_cksum[1] = get_u32(&m_frame[sizeof(U32)]);

    auto redo = m_redo_cksum;
    m_redo_cksum = 0;

    for (; redo < end; ++redo) {
        const auto offset = frame_offset(redo);
        CALICODB_TRY(m_wal_file->read_exact(offset, WalFrameHdr::kSize + m_page_size, m_frame.data()));

        WalFrameHdr hdr;
        hdr.pgno = get_u32(&m_frame[0]);
        hdr.db_size = get_u32(&m_frame[4]);
        encode_frame(hdr, &m_frame[WalFrameHdr::kSize], m_frame.data());
        CALICODB_TRY(m_wal_file->write(offset, Slice(m_frame).truncate(WalFrameHdr::kSize)));
    }
    return Status::ok();
}

auto WalImpl::recover_index() -> Status
{
    CALICODB_EXPECT_TRUE(m_ckpt_lock == 0 || m_ckpt_lock == 1);
    CALICODB_EXPECT_EQ(kNotWriteLock, kWriteLock + 1);
    CALICODB_EXPECT_EQ(kCkptLock, kNotWriteLock);
    CALICODB_EXPECT_TRUE(m_writer_lock);

    const auto lock = kNotWriteLock + m_ckpt_lock;
    CALICODB_TRY(lock_exclusive(lock, READ_LOCK(0) - lock));

    U32 frame_cksum[2] = {};
    Status s;

    const auto cleanup = [&] {
        unlock_exclusive(lock, READ_LOCK(0) - lock);
        return s;
    };

    const auto finish = [&] {
        if (s.is_ok()) {
            m_hdr.frame_cksum[0] = frame_cksum[0];
            m_hdr.frame_cksum[1] = frame_cksum[1];
            write_index_header();

            auto *info = get_ckpt_info();
            info->backfill_attempted = m_hdr.max_frame;
            info->backfill = 0;
            info->readmark[0] = 0;
            for (std::size_t i = 1; i < kReaderCount; ++i) {
                s = lock_exclusive(READ_LOCK(i), 1);
                if (s.is_ok()) {
                    if (i == 1 && m_hdr.max_frame) {
                        info->readmark[i] = m_hdr.max_frame;
                    } else {
                        info->readmark[i] = kReadmarkNotUsed;
                    }
                    unlock_exclusive(READ_LOCK(i), 1);
                }
            }
        }
        return cleanup();
    };

    std::memset(&m_hdr, 0, sizeof(m_hdr));

    std::size_t file_size;
    CALICODB_TRY(m_env->file_size(m_filename, file_size));
    if (file_size > kWalHdrSize) {
        char header[kWalHdrSize];
        CALICODB_TRY(m_wal_file->read_exact(0, sizeof(header), header));

        const auto magic = get_u32(&header[0]);
        const auto page_size = get_u32(&header[8]);
        const auto is_valid =
            magic == kWalMagic &&
            is_power_of_two(page_size) &&
            page_size >= kMinPageSize &&
            page_size <= kMaxPageSize;
        if (!is_valid) {
            s = Status::corruption("WAL header is corrupted");
            return cleanup();
        }
        if (page_size != m_page_size) {
            std::string message;
            append_fmt_string(message, "WAL and DB page size mismatch (%u != %u)", page_size, m_page_size);
            s = Status::invalid_argument(message);
            return cleanup();
        }
        m_ckpt_number = get_u32(&header[12]);
        std::memcpy(m_hdr.salt, &header[16], sizeof(m_hdr.salt));

        compute_checksum(
            Slice(header, kWalHdrSize - 8),
            nullptr,
            m_hdr.frame_cksum);
        if (m_hdr.frame_cksum[0] != get_u32(&header[24]) ||
            m_hdr.frame_cksum[1] != get_u32(&header[28])) {
            return finish();
        }
        const auto version = get_u32(&header[4]);
        if (version != kWalVersion) {
            s = Status::corruption("unrecognized WAL version");
            return cleanup();
        }

        const auto last_frame = static_cast<U32>((file_size - kWalHdrSize) / m_frame.size());
        for (U32 n_group = 0; n_group <= index_group_number(last_frame); ++n_group) {
            const auto last = std::min(last_frame, kNIndexKeys0 + n_group * kNIndexKeys);
            const auto first = 1 + (n_group == 0 ? 0 : kNIndexKeys0 + (n_group - 1) * kNIndexKeys);
            for (auto n_frame = first; n_frame <= last; ++n_frame) {
                const auto offset = frame_offset(n_frame);
                CALICODB_TRY(m_wal_file->read_exact(offset, m_frame.size(), m_frame.data()));
                // Decode the frame.
                WalFrameHdr hdr;
                if (!decode_frame(m_frame.data(), hdr)) {
                    break;
                }
                CALICODB_TRY(m_index.assign(hdr.pgno, n_frame));
                if (hdr.db_size) {
                    m_hdr.max_frame = n_frame;
                    m_hdr.page_count = hdr.db_size;
                    frame_cksum[0] = m_hdr.cksum[0];
                    frame_cksum[1] = m_hdr.cksum[1];
                }
            }
        }
    }

    return finish();
}

auto WalImpl::encode_frame(const WalFrameHdr &hdr, const char *page, char *out) -> void
{
    put_u32(&out[0], hdr.pgno);
    put_u32(&out[4], hdr.db_size);

    if (m_redo_cksum) {
        std::memset(&out[8], 0, 16);
    } else {
        auto *cksum = m_hdr.frame_cksum;
        std::memcpy(&out[8], m_hdr.salt, 8);
        compute_checksum(Slice(out, 8), cksum, cksum);
        compute_checksum(Slice(page, m_page_size), cksum, cksum);
        put_u32(&out[16], cksum[0]);
        put_u32(&out[20], cksum[1]);
    }
    std::memcpy(&out[24], page, m_page_size);
}

auto WalImpl::decode_frame(const char *frame, WalFrameHdr &out) -> bool
{
    static constexpr std::size_t kDataFields = sizeof(U32) * 2;
    if (std::memcmp(m_hdr.salt, &frame[kDataFields], 8) != 0) {
        return false;
    }
    const auto pgno = get_u32(&frame[0]);
    if (pgno == 0) {
        return false;
    }
    auto *cksum = m_hdr.frame_cksum;
    compute_checksum(Slice(frame, kDataFields), cksum, cksum);
    compute_checksum(Slice(frame + WalFrameHdr::kSize, m_page_size), cksum, cksum);
    if (cksum[0] != get_u32(&frame[16]) ||
        cksum[1] != get_u32(&frame[20])) {
        return false;
    }
    out.pgno = pgno;
    out.db_size = get_u32(&frame[4]);
    return true;
}

auto WalImpl::read(Id page_id, char *&page) -> Status
{
    U32 frame;
    CALICODB_TRY(m_index.lookup(page_id.value, m_min_frame, frame));

    if (frame) {
        CALICODB_TRY(m_wal_file->read_exact(
            frame_offset(frame) + WalFrameHdr::kSize,
            m_page_size,
            m_frame.data()));

        std::memcpy(page, m_frame.data(), m_page_size);
        m_stats.bytes_read += m_page_size;
    } else {
        page = nullptr;
    }
    return Status::ok();
}

auto WalImpl::write(const PageRef *dirty, std::size_t db_size) -> Status
{
    const auto is_commit = db_size > 0;
    volatile auto *live = m_index.header();
    auto first_frame = m_min_frame;

    // Check if the WAL's copy of the index header differs from what is on the first index page.
    // If it is different, then the WAL has been written since the last commit, with the first
    // record located at frame number "first_frame".
    if (0 != std::memcmp(&m_hdr, ConstPtr(live), sizeof(m_hdr))) {
        first_frame = live->max_frame + 1;
    }

    CALICODB_TRY(restart_log());

    if (m_hdr.max_frame == 0) {
        // This is the first frame written to the WAL. Write the WAL header.
        char header[kWalHdrSize];
        U32 cksum[2];

        put_u32(&header[0], kWalMagic);
        put_u32(&header[4], kWalVersion);
        put_u32(&header[8], m_page_size);
        put_u32(&header[12], m_ckpt_number);
        if (m_ckpt_number == 0) {
            m_hdr.salt[0] = m_env->rand();
            m_hdr.salt[1] = m_env->rand();
        }
        std::memcpy(&header[16], m_hdr.salt, sizeof(m_hdr.salt));
        compute_checksum(Slice(header, sizeof(header) - sizeof(cksum)), nullptr, cksum);
        put_u32(&header[24], cksum[0]);
        put_u32(&header[28], cksum[1]);

        m_hdr.frame_cksum[0] = cksum[0];
        m_hdr.frame_cksum[1] = cksum[1];

        CALICODB_TRY(m_wal_file->write(0, Slice(header, sizeof(header))));
        CALICODB_TRY(m_wal_file->sync());
    }

    // Write each dirty page to the WAL.
    auto next_frame = m_hdr.max_frame + 1;
    for (auto *p = dirty; p; p = p->next) {
        U32 frame;

        // Condition ensures that if this set of pages completes a transaction, then
        // the last frame will always be appended, even if another copy of the page
        // exists in the WAL for this transaction. This frame needs to have its
        // "db_size" field set to mark that it is a commit frame.
        if (first_frame && (p->next || !is_commit)) {
            // Check to see if the page has been written to the WAL already by the
            // current transaction. If so, overwrite it and indicate that checksums
            // need to be recomputed from here on commit.
            CALICODB_TRY(m_index.lookup(p->page_id.value, first_frame, frame));
            if (frame) {
                if (m_redo_cksum == 0 || frame < m_redo_cksum) {
                    m_redo_cksum = frame;
                }
                const Slice page(p->page, m_page_size);
                CALICODB_TRY(m_wal_file->write(frame_offset(frame) + WalFrameHdr::kSize, page));
                continue;
            }
        }
        // Page has not been written during the current transaction. Create a new
        // WAL frame for it.
        WalFrameHdr header;
        header.pgno = p->page_id.value;
        header.db_size = p->next == nullptr ? static_cast<U32>(db_size) : 0;
        encode_frame(header, p->page, m_frame.data());
        CALICODB_TRY(m_wal_file->write(frame_offset(next_frame), m_frame));
        m_stats.bytes_written += m_frame.size();

        // TODO: SQLite does this after the loop finishes, in another loop. They pad out remaining frames to make a full sector
        //       if some option is set, to guard against torn writes.
        CALICODB_TRY(m_index.assign(header.pgno, next_frame));
        ++next_frame;
    }

    if (is_commit && m_redo_cksum) {
        CALICODB_TRY(rewrite_checksums(next_frame));
    }

    m_hdr.max_frame = next_frame - 1;
    if (is_commit) {
        // If this is a commit, then at least 1 frame (the commit frame) must be written. The
        // pager has logic to make sure of this (the root page is forcibly written if no pages
        // are dirty).
        CALICODB_EXPECT_TRUE(dirty);
        m_hdr.page_count = static_cast<U32>(db_size);
        ++m_hdr.change;
        write_index_header();
    }

    return Status::ok();
}

auto WalImpl::needs_checkpoint() const -> bool
{
    return m_hdr.max_frame > 1'000;
}

auto WalImpl::checkpoint(File &db_file, std::size_t *db_size) -> Status
{
    CALICODB_EXPECT_FALSE(m_ckpt_lock);
    CALICODB_EXPECT_FALSE(m_writer_lock);

    CALICODB_TRY(m_wal_file->sync());
    if (db_size) {
        *db_size = m_hdr.page_count;
    }
    volatile auto *info = get_ckpt_info();
    auto max_safe_frame = m_hdr.max_frame;
    if (info->backfill < max_safe_frame) {
        for (std::size_t i = 1; i < kReaderCount; ++i) {
            const auto y = info->readmark[i];
            if (y < max_safe_frame) {
                CALICODB_EXPECT_LE(y, m_hdr.max_frame);
                // TODO: Should busy wait w/ a callback
                auto s = lock_exclusive(READ_LOCK(i), 1);
                if (s.is_ok()) {
                    info->readmark[i] =
                        i == 1 ? max_safe_frame : kReadmarkNotUsed;
                    unlock_exclusive(READ_LOCK(i), 1);
                } else if (s.is_busy()) {
                    max_safe_frame = y;
                } else {
                    return s;
                }
            }
        }

        if (info->backfill < max_safe_frame) {
            HashIterator itr(m_index);
            CALICODB_TRY(itr.init());

            info->backfill_attempted = max_safe_frame;

            for (;;) {
                HashIterator::Entry entry;
                if (!itr.read(entry)) {
                    break;
                }

                CALICODB_TRY(m_wal_file->read_exact(
                    frame_offset(entry.value) + WalFrameHdr::kSize,
                    m_page_size,
                    m_frame.data()));

                CALICODB_TRY(db_file.write(
                    (entry.key - 1) * m_page_size,
                    Slice(m_frame.data(), m_page_size)));
            }

            info->backfill = max_safe_frame;
        }
        ++m_ckpt_number;
        m_min_frame = 0;
        m_hdr.max_frame = 0;
        m_index.cleanup();
        restart_header(m_env->rand());
    }
    return Status::ok();
}

auto TEST_print_wal(const Wal &wal) -> void
{
    auto &impl = reinterpret_cast<WalImpl &>(const_cast<Wal &>(wal));
    const auto *hdr1 = &reinterpret_cast<volatile HashIndexHdr *>(impl.m_index.groups()[0])[0];
    const auto *hdr2 = &reinterpret_cast<volatile HashIndexHdr *>(impl.m_index.groups()[0])[1];
    std::fputs("Max frame values:\n", stderr);
    std::fputs("  Location   Value\n", stderr);
    std::fprintf(stderr, "  local      %u\n", impl.m_hdr.max_frame);
    std::fprintf(stderr, "  shared[0]  %u\n", hdr1->max_frame);
    std::fprintf(stderr, "  shared[1]  %u\n", hdr2->max_frame);
    auto *info = impl.get_ckpt_info();
    std::fputs("WAL locks:", stderr);
    std::fputs("  Write ", stderr);
    if (impl.m_writer_lock) {
        std::fputc('*', stderr);
    }
    std::fputc('\n', stderr);
    std::fputs("  Ckpt ", stderr);
    if (impl.m_ckpt_lock) {
        std::fputs(" *", stderr);
    }
    std::fputc('\n', stderr);
    std::fputs("  Rcvr ?\n", stderr);
    for (std::size_t i = 0; i < kReaderCount; ++i) {
        std::fprintf(stderr, "  Read%zu (%d)", i, info->readmark[i]);
        if (impl.m_reader_lock == int(i)) {
            std::fputs(" *", stderr);
        }
        std::fputc('\n', stderr);
    }
    std::fputc('\n', stderr);
}

#undef READ_LOCK

} // namespace calicodb