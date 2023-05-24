// Copyright (c) 2022, The CalicoDB Authors. All rights reserved.
// This source code is licensed under the MIT License, which can be found in
// LICENSE.md. See AUTHORS.md for a list of contributor names.

#include "tree.h"
#include "db_impl.h"
#include "encoding.h"
#include "logging.h"
#include "pager.h"
#include "schema.h"
#include "utils.h"
#include <array>
#include <functional>
#include <numeric>

namespace calicodb
{

[[nodiscard]] static auto default_cursor_status() -> Status
{
    return Status::not_found();
}

static constexpr auto kMaxCellHeaderSize =
    kVarintMaxLength + // Value size  (10 B)
    kVarintMaxLength + // Key size    (10 B)
    Id::kSize;         // Overflow ID (4 B)

static constexpr std::size_t kPointerSize = 2;

inline constexpr auto compute_local_size(std::size_t key_size, std::size_t value_size) -> std::size_t
{
    constexpr const std::size_t kMinLocal =
        (kPageSize - NodeHeader::kSize) * 32 / 256 - kMaxCellHeaderSize - kPointerSize;
    constexpr const std::size_t kMaxLocal =
        (kPageSize - NodeHeader::kSize) * 64 / 256 - kMaxCellHeaderSize - kPointerSize;
    if (key_size + value_size <= kMaxLocal) {
        return key_size + value_size;
    } else if (key_size > kMaxLocal) {
        return kMaxLocal;
    }
    // Try to prevent the key from being split.
    return std::max(kMinLocal, key_size);
}

static auto node_header_offset(const Node &node)
{
    return page_offset(node.page.id());
}

static auto cell_slots_offset(const Node &node)
{
    return node_header_offset(node) + NodeHeader::kSize;
}

static auto cell_area_offset(const Node &node)
{
    return cell_slots_offset(node) + node.header.cell_count * kPointerSize;
}

// TODO: May want to cache the total number of free block bytes.
static auto usable_space(const Node &node) -> std::size_t
{
    // Number of bytes not occupied by cells or cell pointers.
    return node.gap_size + node.header.frag_count + BlockAllocator::accumulate_free_bytes(node);
}

static auto detach_cell(Cell &cell, char *backing) -> void
{
    if (cell.is_free) {
        return;
    }
    std::memcpy(backing, cell.ptr, cell.size);
    const auto diff = cell.key - cell.ptr;
    cell.ptr = backing;
    cell.key = backing + diff;
    cell.is_free = true;
}

static auto read_child_id_at(const Node &node, std::size_t offset) -> Id
{
    return Id(get_u32(node.page.data() + offset));
}

static auto write_child_id_at(Node &node, std::size_t offset, Id child_id) -> void
{
    put_u32(node.page.data() + offset, child_id.value);
}

static auto read_child_id(const Node &node, std::size_t index) -> Id
{
    const auto &header = node.header;
    CALICODB_EXPECT_LE(index, header.cell_count);
    CALICODB_EXPECT_FALSE(header.is_external);
    if (index == header.cell_count) {
        return header.next_id;
    }
    return read_child_id_at(node, node.get_slot(index));
}

static auto read_child_id(const Cell &cell) -> Id
{
    return Id(get_u32(cell.ptr));
}

static auto read_overflow_id(const Cell &cell) -> Id
{
    return Id(get_u32(cell.key + cell.local_size));
}

static auto write_overflow_id(Cell &cell, Id overflow_id) -> void
{
    put_u32(cell.key + cell.local_size, overflow_id.value);
}

static auto write_child_id(Node &node, std::size_t index, Id child_id) -> void
{
    auto &header = node.header;
    CALICODB_EXPECT_LE(index, header.cell_count);
    CALICODB_EXPECT_FALSE(header.is_external);
    if (index == header.cell_count) {
        header.next_id = child_id;
    } else {
        write_child_id_at(node, node.get_slot(index), child_id);
    }
}

static auto write_child_id(Cell &cell, Id child_id) -> void
{
    put_u32(cell.ptr, child_id.value);
}

static auto parse_external_cell(char *data, const char *limit) -> Cell
{
    U64 key_size, value_size;
    const auto *ptr = data;
    if ((ptr = decode_varint(ptr, limit, value_size))) {
        if ((ptr = decode_varint(ptr, limit, key_size))) {
            const auto header_size = static_cast<std::uintptr_t>(ptr - data);

            Cell cell;
            cell.ptr = data;
            cell.key = data + header_size;
            cell.key_size = key_size;
            cell.total_size = key_size + value_size;
            cell.local_size = compute_local_size(key_size, value_size);
            cell.has_remote = cell.local_size < key_size + value_size;
            cell.size = header_size + cell.local_size + cell.has_remote * Id::kSize;
            return cell;
        }
    }
    // TODO: Allow this function to fail. Should report corruption if it is clear that a
    //       varint is messed up. Other indicators of corruption should also be used!
    CALICODB_EXPECT_TRUE(false && "not implemented");
    return Cell{};
}

static auto parse_internal_cell(char *data, const char *limit) -> Cell
{
    U64 key_size;
    if (const auto *ptr = decode_varint(data + Id::kSize, limit, key_size)) {
        const auto header_size = static_cast<std::uintptr_t>(ptr - data);

        Cell cell;
        cell.ptr = data;
        cell.key = data + header_size;
        cell.key_size = key_size;
        cell.total_size = key_size;
        cell.local_size = compute_local_size(key_size, 0);
        cell.has_remote = cell.local_size < key_size;
        cell.size = header_size + cell.local_size + cell.has_remote * Id::kSize;
        return cell;
    }
    // TODO: See parse_external_cell().
    CALICODB_EXPECT_TRUE(false && "not implemented");
    return Cell{};
}

static auto read_cell_at(Node &node, std::size_t offset)
{
    return node.meta->parse_cell(node.page.data() + offset, node.page.data() + kPageSize);
}

auto read_cell(Node &node, std::size_t index) -> Cell
{
    return read_cell_at(node, node.get_slot(index));
}

[[nodiscard]] static auto get_next_pointer(const Node &node, std::size_t offset) -> unsigned
{
    return get_u16(node.page.data() + offset);
}

[[nodiscard]] static auto get_block_size(const Node &node, std::size_t offset) -> unsigned
{
    return get_u16(node.page.data() + offset + kPointerSize);
}

static auto set_next_pointer(Node &node, std::size_t offset, std::size_t value) -> void
{
    CALICODB_EXPECT_LT(value, kPageSize);
    return put_u16(node.page.data() + offset, static_cast<U16>(value));
}

static auto set_block_size(Node &node, std::size_t offset, std::size_t value) -> void
{
    CALICODB_EXPECT_GE(value, 4);
    CALICODB_EXPECT_LT(value, kPageSize);
    return put_u16(node.page.data() + offset + kPointerSize, static_cast<U16>(value));
}

static auto take_free_space(Node &node, std::size_t ptr0, std::size_t ptr1, std::size_t needed_size) -> std::size_t
{
    CALICODB_EXPECT_LT(ptr0, kPageSize);
    CALICODB_EXPECT_LT(ptr1, kPageSize);
    CALICODB_EXPECT_LT(needed_size, kPageSize);

    const auto ptr2 = get_next_pointer(node, ptr1);
    const auto free_size = get_block_size(node, ptr1);
    auto &header = node.header;

    CALICODB_EXPECT_GE(free_size, needed_size);
    const auto diff = static_cast<unsigned>(free_size - needed_size);

    if (diff < 4) {
        header.frag_count = header.frag_count + diff;
        if (ptr0 == 0) {
            header.free_start = ptr2;
        } else {
            set_next_pointer(node, ptr0, ptr2);
        }
    } else {
        set_block_size(node, ptr1, diff);
    }
    return ptr1 + diff;
}

static auto allocate_from_free_list(Node &node, std::size_t needed_size) -> std::size_t
{
    unsigned prev_ptr = 0;
    auto curr_ptr = node.header.free_start;

    while (curr_ptr) {
        if (needed_size <= get_block_size(node, curr_ptr)) {
            return take_free_space(node, prev_ptr, curr_ptr, needed_size);
        }
        prev_ptr = curr_ptr;
        curr_ptr = get_next_pointer(node, curr_ptr);
    }
    return 0;
}

static auto allocate_from_gap(Node &node, std::size_t needed_size) -> std::size_t
{
    if (node.gap_size >= needed_size) {
        node.gap_size -= static_cast<unsigned>(needed_size);
        node.header.cell_start -= static_cast<unsigned>(needed_size);
        return node.header.cell_start;
    }
    return 0;
}

auto BlockAllocator::accumulate_free_bytes(const Node &node) -> std::size_t
{
    unsigned total = 0;
    for (auto ptr = node.header.free_start; ptr != 0;) {
        total += get_block_size(node, ptr);
        ptr = get_next_pointer(node, ptr);
    }
    return total;
}

auto BlockAllocator::allocate(Node &node, std::size_t needed_size) -> std::size_t
{
    CALICODB_EXPECT_LT(needed_size, kPageSize);

    if (const auto offset = allocate_from_gap(node, needed_size)) {
        return offset;
    }
    return allocate_from_free_list(node, needed_size);
}

auto BlockAllocator::release(Node &node, std::size_t block_start, std::size_t block_size) -> void
{
    auto &header = node.header;

    // Largest possible fragment that can be reclaimed in this process. Based on
    // the fact that external cells must be at least 3 bytes. Internal cells are
    // always larger than fragments.
    const std::size_t fragment_cutoff = 2 + !header.is_external;
    CALICODB_EXPECT_NE(block_size, 0);

    // Blocks of less than 4 bytes are too small to hold the free block header,
    // so they must become fragment bytes.
    if (block_size < 4) {
        header.frag_count += static_cast<unsigned>(block_size);
        return;
    }
    // The free block list is sorted by start position. Find where the
    // new block should go.
    unsigned prev = 0;
    auto next = header.free_start;
    while (next && next < block_start) {
        prev = next;
        next = get_next_pointer(node, next);
    }

    if (prev != 0) {
        // Merge with the predecessor block.
        const auto before_end = prev + get_block_size(node, prev);
        if (before_end + fragment_cutoff >= block_start) {
            const auto diff = block_start - before_end;
            block_start = prev;
            block_size += get_block_size(node, prev) + diff;
            header.frag_count -= static_cast<unsigned>(diff);
        }
    }
    if (block_start != prev) {
        // There was no left merge. Point the "before" pointer to where the new free
        // block will be inserted.
        if (prev == 0) {
            header.free_start = static_cast<unsigned>(block_start);
        } else {
            set_next_pointer(node, prev, block_start);
        }
    }

    if (next != 0) {
        // Merge with the successor block.
        const auto current_end = block_start + block_size;
        if (current_end + fragment_cutoff >= next) {
            const auto diff = next - current_end;
            block_size += get_block_size(node, next) + diff;
            header.frag_count -= static_cast<unsigned>(diff);
            next = get_next_pointer(node, next);
        }
    }
    // If there was a left merge, this will set the next pointer and block size of
    // the free block at "prev".
    set_next_pointer(node, block_start, next);
    set_block_size(node, block_start, block_size);
}

auto BlockAllocator::defragment(Node &node, int skip) -> void
{
    auto &header = node.header;
    const auto n = header.cell_count;
    const auto to_skip = skip >= 0 ? static_cast<std::size_t>(skip) : n;
    auto end = kPageSize;
    auto ptr = node.page.data();
    std::vector<std::size_t> ptrs(n);

    for (std::size_t index = 0; index < n; ++index) {
        if (index == to_skip) {
            continue;
        }
        const auto offset = node.get_slot(index);
        const auto size = read_cell_at(node, offset).size;

        end -= size;
        std::memcpy(node.scratch + end, ptr + offset, size);
        ptrs[index] = end;
    }
    for (std::size_t index = 0; index < n; ++index) {
        if (index == to_skip) {
            continue;
        }
        node.set_slot(index, ptrs[index]);
    }
    const auto offset = cell_area_offset(node);
    const auto size = kPageSize - offset;
    std::memcpy(node.page.data() + offset, node.scratch + offset, size);

    header.cell_start = static_cast<unsigned>(end);
    header.frag_count = 0;
    header.free_start = 0;
    node.gap_size = static_cast<unsigned>(end - cell_area_offset(node));
}

static constexpr NodeMeta kExternalMeta = {
    parse_external_cell};

static constexpr NodeMeta kInternalMeta = {
    parse_internal_cell};

static auto setup_node(Node &node) -> void
{
    node.meta = node.header.is_external ? &kExternalMeta : &kInternalMeta;
    node.slots_offset = static_cast<unsigned>(cell_slots_offset(node));

    if (node.header.cell_start == 0) {
        node.header.cell_start = static_cast<unsigned>(kPageSize);
    }

    const auto bottom = cell_area_offset(node);
    const auto top = node.header.cell_start;

    CALICODB_EXPECT_GE(top, bottom);
    node.gap_size = static_cast<unsigned>(top - bottom);
}

static auto allocate_block(Node &node, unsigned index, unsigned size) -> std::size_t
{
    CALICODB_EXPECT_LE(index, node.header.cell_count);

    if (size + kPointerSize > usable_space(node)) {
        node.overflow_index = index;
        return 0;
    }

    // We don't have room to insert the cell pointer.
    if (node.gap_size < kPointerSize) {
        BlockAllocator::defragment(node);
    }
    // Insert a dummy cell pointer to save the slot.
    node.insert_slot(index, kPageSize - 1);

    auto offset = BlockAllocator::allocate(node, size);
    if (offset == 0) {
        BlockAllocator::defragment(node, static_cast<int>(index));
        offset = BlockAllocator::allocate(node, size);
    }
    // We already made sure we had enough room to fulfill the request. If we had to defragment, the call
    // to allocate() following defragmentation should succeed.
    CALICODB_EXPECT_NE(offset, 0);
    node.set_slot(index, offset);

    return offset;
}

static auto free_block(Node &node, unsigned index, unsigned size) -> void
{
    BlockAllocator::release(node, static_cast<unsigned>(node.get_slot(index)), size);
    node.remove_slot(index);
}

auto write_cell(Node &node, std::size_t index, const Cell &cell) -> std::size_t
{
    if (const auto offset = allocate_block(node, static_cast<unsigned>(index), static_cast<unsigned>(cell.size))) {
        std::memcpy(node.page.data() + offset, cell.ptr, cell.size);
        return offset;
    }
    node.overflow_index = static_cast<unsigned>(index);
    node.overflow = cell;
    return 0;
}

static auto erase_cell(Node &node, std::size_t index, std::size_t size_hint) -> void
{
    CALICODB_EXPECT_LT(index, node.header.cell_count);
    free_block(node, static_cast<unsigned>(index), static_cast<unsigned>(size_hint));
}

auto erase_cell(Node &node, std::size_t index) -> void
{
    erase_cell(node, index, read_cell(node, index).size);
}

auto Node::get_slot(std::size_t index) const -> std::size_t
{
    CALICODB_EXPECT_LT(index, header.cell_count);
    return get_u16(page.data() + slots_offset + index * kPointerSize);
}

auto Node::set_slot(std::size_t index, std::size_t pointer) -> void
{
    CALICODB_EXPECT_LT(index, header.cell_count);
    return put_u16(page.data() + slots_offset + index * kPointerSize, static_cast<U16>(pointer));
}

auto Node::insert_slot(std::size_t index, std::size_t pointer) -> void
{
    CALICODB_EXPECT_LE(index, header.cell_count);
    CALICODB_EXPECT_GE(gap_size, kPointerSize);
    const auto offset = slots_offset + index * kPointerSize;
    const auto size = (header.cell_count - index) * kPointerSize;
    auto *data = page.data() + offset;

    std::memmove(data + kPointerSize, data, size);
    put_u16(data, static_cast<U16>(pointer));

    gap_size -= static_cast<unsigned>(kPointerSize);
    ++header.cell_count;
}

auto Node::remove_slot(std::size_t index) -> void
{
    CALICODB_EXPECT_LT(index, header.cell_count);
    const auto offset = slots_offset + index * kPointerSize;
    const auto size = (header.cell_count - index) * kPointerSize;
    auto *data = page.data() + offset;

    std::memmove(data, data + kPointerSize, size);

    gap_size += kPointerSize;
    --header.cell_count;
}

auto Node::take() && -> Page
{
    if (page.is_writable()) {
        if (header.frag_count > 0xFF) {
            // Fragment count overflow.
            BlockAllocator::defragment(*this);
        }
        header.write(page.data() + node_header_offset(*this));
    }
    return std::move(page);
}

static auto merge_root(Node &root, Node &child) -> void
{
    CALICODB_EXPECT_EQ(root.header.next_id, child.page.id());
    const auto &header = child.header;
    if (header.free_start) {
        BlockAllocator::defragment(child);
    }

    // Copy the cell content area.
    CALICODB_EXPECT_GE(header.cell_start, cell_slots_offset(root));
    auto memory_size = kPageSize - header.cell_start;
    auto memory = root.page.data() + header.cell_start;
    std::memcpy(memory, child.page.data() + header.cell_start, memory_size);

    // Copy the header and cell pointers.
    memory_size = header.cell_count * kPointerSize;
    memory = root.page.data() + cell_slots_offset(root);
    std::memcpy(memory, child.page.data() + cell_slots_offset(child), memory_size);
    root.header = header;
    root.meta = child.meta;
}

[[nodiscard]] static auto is_overflowing(const Node &node) -> bool
{
    return node.overflow.has_value();
}

[[nodiscard]] static auto is_underflowing(const Node &node) -> bool
{
    return node.header.cell_count == 0;
}

static constexpr auto kLinkContentOffset = Id::kSize;
static constexpr auto kLinkContentSize = kPageSize - kLinkContentOffset;

[[nodiscard]] static auto get_readable_content(const Page &page, std::size_t size_limit) -> Slice
{
    return page.view().range(kLinkContentOffset, std::min(size_limit, kLinkContentSize));
}

auto Tree::create(Pager &pager, bool is_root, Id *out) -> Status
{
    std::string scratch;
    Id root_id;
    Node node;

    if (!is_root) {
        CALICODB_TRY(NodeManager::allocate(pager, node, scratch, true));
        root_id = node.page.id();
        NodeManager::release(pager, std::move(node));

        CALICODB_EXPECT_FALSE(root_id.is_root());
        // No back pointer necessary for root pages.
        PointerMap::Entry entry = {Id::null(), PointerMap::kTreeRoot};
        CALICODB_TRY(PointerMap::write_entry(pager, root_id, entry));
    } else {
        CALICODB_TRY(NodeManager::acquire(pager, Id::root(), node, scratch, true));
        node.header.is_external = true;
        NodeManager::release(pager, std::move(node));
        root_id = Id::root();
    }
    if (out != nullptr) {
        *out = root_id;
    }
    return Status::ok();
}

auto Tree::find_external(const Slice &key, bool write, bool &exact) const -> Status
{
    m_cursor.seek_root(write);
    exact = false;

    while (m_cursor.is_valid()) {
        const auto found = m_cursor.seek(key);
        if (m_cursor.is_valid()) {
            if (m_cursor.node().header.is_external) {
                exact = found;
                break;
            }
            const auto next_id = read_child_id(m_cursor.node(), m_cursor.index());
            CALICODB_EXPECT_NE(next_id, m_cursor.node().page.id()); // Infinite loop.
            m_cursor.move_down(next_id);
        }
    }
    return m_cursor.status();
}

auto Tree::read_key(Node &node, std::size_t index, std::string &scratch, Slice &key_out) const -> Status
{
    const auto cell = read_cell(node, index);
    if (scratch.size() < cell.key_size) {
        scratch.resize(cell.key_size);
    }
    auto s = PayloadManager::access(*m_pager, cell, 0, cell.key_size, nullptr, scratch.data());
    if (s.is_ok()) {
        key_out = Slice(scratch).truncate(cell.key_size);
    }
    return s;
}

auto Tree::read_value(Node &node, std::size_t index, std::string &scratch, Slice &value_out) const -> Status
{
    const auto cell = read_cell(node, index);
    const auto value_size = cell.total_size - cell.key_size;
    if (scratch.size() < value_size) {
        scratch.resize(value_size);
    }
    auto s = PayloadManager::access(*m_pager, cell, cell.key_size, value_size, nullptr, scratch.data());
    if (s.is_ok()) {
        value_out = Slice(scratch).truncate(value_size);
    }
    return s;
}

auto Tree::write_key(Node &node, std::size_t index, const Slice &key) -> Status
{
    const auto cell = read_cell(node, index);
    return PayloadManager::access(*m_pager, cell, 0, key.size(), key.data(), nullptr);
}

auto Tree::write_value(Node &node, std::size_t index, const Slice &value) -> Status
{
    const auto cell = read_cell(node, index);
    return PayloadManager::access(*m_pager, cell, cell.key_size, value.size(), value.data(), nullptr);
}

auto Tree::find_parent_id(Id page_id, Id &out) const -> Status
{
    PointerMap::Entry entry;
    CALICODB_TRY(PointerMap::read_entry(*m_pager, page_id, entry));
    out = entry.back_ptr;
    return Status::ok();
}

auto Tree::fix_parent_id(Id page_id, Id parent_id, PointerMap::Type type) -> Status
{
    PointerMap::Entry entry = {parent_id, type};
    return PointerMap::write_entry(*m_pager, page_id, entry);
}

auto Tree::maybe_fix_overflow_chain(const Cell &cell, Id parent_id) -> Status
{
    if (cell.has_remote) {
        return fix_parent_id(read_overflow_id(cell), parent_id, PointerMap::kOverflowHead);
    }
    return Status::ok();
}

auto Tree::insert_cell(Node &node, std::size_t index, const Cell &cell) -> Status
{
    write_cell(node, index, cell);
    if (!node.header.is_external) {
        CALICODB_TRY(fix_parent_id(read_child_id(cell), node.page.id(), PointerMap::kTreeNode));
    }
    return maybe_fix_overflow_chain(cell, node.page.id());
}

auto Tree::remove_cell(Node &node, std::size_t index) -> Status
{
    const auto cell = read_cell(node, index);
    if (cell.has_remote) {
        CALICODB_TRY(free_overflow(read_overflow_id(cell)));
    }
    erase_cell(node, index, cell.size);
    return Status::ok();
}

auto Tree::free_overflow(Id head_id) -> Status
{
    while (!head_id.is_null()) {
        Page page;
        CALICODB_TRY(m_pager->acquire(head_id, page));
        head_id = read_next_id(page);
        m_pager->mark_dirty(page);
        CALICODB_TRY(m_pager->destroy(std::move(page)));
    }
    return Status::ok();
}

auto Tree::fix_links(Node &node) -> Status
{
    for (std::size_t index = 0; index < node.header.cell_count; ++index) {
        const auto cell = read_cell(node, index);
        CALICODB_TRY(maybe_fix_overflow_chain(cell, node.page.id()));
        if (!node.header.is_external) {
            CALICODB_TRY(fix_parent_id(read_child_id(cell), node.page.id(), PointerMap::kTreeNode));
        }
    }
    if (!node.header.is_external) {
        CALICODB_TRY(fix_parent_id(node.header.next_id, node.page.id(), PointerMap::kTreeNode));
    }
    if (node.overflow) {
        CALICODB_TRY(maybe_fix_overflow_chain(*node.overflow, node.page.id()));
        if (!node.header.is_external) {
            CALICODB_TRY(fix_parent_id(read_child_id(*node.overflow), node.page.id(), PointerMap::kTreeNode));
        }
    }
    return Status::ok();
}

auto Tree::allocate(bool is_external, Node &out) -> Status
{
    return NodeManager::allocate(*m_pager, out, m_node_scratch, is_external);
}

auto Tree::acquire(Id page_id, bool upgrade, Node &out) const -> Status
{
    return NodeManager::acquire(*m_pager, page_id, out, m_node_scratch, upgrade);
}

auto Tree::free(Node node) -> Status
{
    return NodeManager::destroy(*m_pager, std::move(node));
}

auto Tree::upgrade(Node &node) const -> void
{
    NodeManager::upgrade(*m_pager, node);
}

auto Tree::release(Node node) const -> void
{
    NodeManager::release(*m_pager, std::move(node));
}

auto Tree::resolve_overflow() -> Status
{
    CALICODB_EXPECT_TRUE(m_cursor.is_valid());

    Status s;
    while (is_overflowing(m_cursor.node())) {
        if (m_cursor.node().page.id() == root()) {
            s = split_root();
        } else {
            s = split_nonroot();
        }
        if (s.is_ok()) {
            report_stats(kSMOCount, 1);
        } else {
            break;
        }
    }
    m_cursor.clear();
    return s;
}

auto Tree::split_root() -> Status
{
    auto &root = m_cursor.node();
    CALICODB_EXPECT_EQ(Tree::root(), root.page.id());

    Node child;
    auto s = allocate(root.header.is_external, child);
    if (s.is_ok()) {
        // Copy the cell content area.
        const auto after_root_headers = cell_area_offset(root);
        auto memory_size = kPageSize - after_root_headers;
        auto memory = child.page.data() + after_root_headers;
        std::memcpy(memory, root.page.data() + after_root_headers, memory_size);

        // Copy the header and cell pointers. Doesn't copy the page LSN.
        memory_size = root.header.cell_count * kPointerSize;
        memory = child.page.data() + cell_slots_offset(child);
        std::memcpy(memory, root.page.data() + cell_slots_offset(root), memory_size);
        child.header = root.header;

        CALICODB_EXPECT_TRUE(is_overflowing(root));
        std::swap(child.overflow, root.overflow);
        child.overflow_index = root.overflow_index;
        child.gap_size = root.gap_size;
        if (root.page.id().is_root()) {
            child.gap_size += FileHeader::kSize;
        }

        root.header = NodeHeader{};
        root.header.is_external = false;
        root.header.next_id = child.page.id();
        setup_node(root);

        s = fix_parent_id(child.page.id(), root.page.id(), PointerMap::kTreeNode);
        if (s.is_ok()) {
            s = fix_links(child);
        }
        m_cursor.history[0].index = 0;
        m_cursor.move_to(std::move(child), 1);
    }
    return s;
}

auto Tree::transfer_left(Node &left, Node &right) -> Status
{
    CALICODB_EXPECT_EQ(left.header.is_external, right.header.is_external);
    const auto cell = read_cell(right, 0);
    CALICODB_TRY(insert_cell(left, left.header.cell_count, cell));
    CALICODB_EXPECT_FALSE(is_overflowing(left));
    erase_cell(right, 0, cell.size);
    return Status::ok();
}

auto Tree::split_nonroot() -> Status
{
    auto &node = m_cursor.node();
    CALICODB_EXPECT_NE(node.page.id(), root());
    CALICODB_EXPECT_TRUE(is_overflowing(node));
    const auto &header = node.header;

    CALICODB_EXPECT_LT(0, m_cursor.level);
    const auto last_loc = m_cursor.history[m_cursor.level - 1];
    const auto parent_id = last_loc.page_id;
    CALICODB_EXPECT_FALSE(parent_id.is_null());

    Node parent, left;
    CALICODB_TRY(acquire(parent_id, true, parent));
    CALICODB_TRY(allocate(header.is_external, left));

    const auto overflow_index = node.overflow_index;
    auto overflow = *node.overflow;
    node.overflow.reset();

    if (overflow_index == header.cell_count) {
        // Note the reversal of the "left" and "right" parameters. We are splitting the other way.
        // This can greatly improve the performance of sequential writes.
        return split_nonroot_fast(
            std::move(parent),
            std::move(left),
            overflow);
    }

    // Fix the overflow. The overflow cell should fit in either "left" or "right". This routine
    // works by transferring cells, one-by-one, from "right" to "left", and trying to insert the
    // overflow cell. Where the overflow cell is written depends on how many cells we have already
    // transferred. If "overflow_index" is 0, we definitely have enough room in "left". Otherwise,
    // we transfer a cell and try to write the overflow cell to "right". If this isn't possible,
    // then the left node must have enough room, since the maximum cell size is limited to roughly
    // 1/4 of a page. If "right" is more than 3/4 full, then "left" must be less than 1/4 full, so
    // it must be able to accept the overflow cell without overflowing.
    for (std::size_t i = 0, n = header.cell_count; i < n; ++i) {
        if (i == overflow_index) {
            CALICODB_TRY(insert_cell(left, left.header.cell_count, overflow));
            break;
        }
        CALICODB_TRY(transfer_left(left, node));

        if (usable_space(node) >= overflow.size + 2) {
            CALICODB_TRY(insert_cell(node, overflow_index - i - 1, overflow));
            break;
        }
        CALICODB_EXPECT_NE(i + 1, n);
    }
    CALICODB_EXPECT_FALSE(is_overflowing(left));
    CALICODB_EXPECT_FALSE(is_overflowing(node));

    auto separator = read_cell(node, 0);
    detach_cell(separator, cell_scratch());

    if (header.is_external) {
        if (!header.prev_id.is_null()) {
            Node left_sibling;
            CALICODB_TRY(acquire(header.prev_id, true, left_sibling));
            left_sibling.header.next_id = left.page.id();
            left.header.prev_id = left_sibling.page.id();
            release(std::move(left_sibling));
        }
        node.header.prev_id = left.page.id();
        left.header.next_id = node.page.id();
        CALICODB_TRY(PayloadManager::promote(
            *m_pager,
            nullptr,
            separator,
            parent_id));
    } else {
        left.header.next_id = read_child_id(separator);
        CALICODB_TRY(fix_parent_id(left.header.next_id, left.page.id(), PointerMap::kTreeNode));
        erase_cell(node, 0);
    }

    // Post the separator into the parent node. This call will fix the sibling's parent pointer.
    write_child_id(separator, left.page.id());
    CALICODB_TRY(insert_cell(parent, last_loc.index, separator));

    release(std::move(left));
    m_cursor.move_to(std::move(parent), -1);
    return Status::ok();
}

auto Tree::split_nonroot_fast(Node parent, Node right, const Cell &overflow) -> Status
{
    auto &left = m_cursor.node();
    const auto &header = left.header;
    CALICODB_TRY(insert_cell(right, 0, overflow));

    CALICODB_EXPECT_FALSE(is_overflowing(left));
    CALICODB_EXPECT_FALSE(is_overflowing(right));

    Cell separator;
    if (header.is_external) {
        if (!header.next_id.is_null()) {
            Node right_sibling;
            CALICODB_TRY(acquire(header.next_id, true, right_sibling));
            right_sibling.header.prev_id = right.page.id();
            right.header.next_id = right_sibling.page.id();
            release(std::move(right_sibling));
        }
        right.header.prev_id = left.page.id();
        left.header.next_id = right.page.id();

        separator = read_cell(right, 0);
        CALICODB_TRY(PayloadManager::promote(
            *m_pager,
            cell_scratch(),
            separator,
            parent.page.id()));
    } else {
        separator = read_cell(left, header.cell_count - 1);
        detach_cell(separator, cell_scratch());
        erase_cell(left, header.cell_count - 1);

        right.header.next_id = left.header.next_id;
        left.header.next_id = read_child_id(separator);
        CALICODB_TRY(fix_parent_id(right.header.next_id, right.page.id(), PointerMap::kTreeNode));
        CALICODB_TRY(fix_parent_id(left.header.next_id, left.page.id(), PointerMap::kTreeNode));
    }

    CALICODB_EXPECT_LT(0, m_cursor.level);
    const auto last_loc = m_cursor.history[m_cursor.level - 1];

    // Post the separator into the parent node. This call will fix the sibling's parent pointer.
    write_child_id(separator, left.page.id());
    CALICODB_TRY(insert_cell(parent, last_loc.index, separator));

    const auto offset = !is_overflowing(parent);
    write_child_id(parent, last_loc.index + offset, right.page.id());
    CALICODB_TRY(fix_parent_id(right.page.id(), parent.page.id(), PointerMap::kTreeNode));

    release(std::move(right));
    m_cursor.move_to(std::move(parent), -1);
    return Status::ok();
}

auto Tree::resolve_underflow() -> Status
{
    while (m_cursor.is_valid() && is_underflowing(m_cursor.node())) {
        if (m_cursor.node().page.id() == root()) {
            return fix_root();
        }
        CALICODB_EXPECT_LT(0, m_cursor.level);
        const auto last_loc = m_cursor.history[m_cursor.level - 1];

        Node parent;
        CALICODB_TRY(acquire(last_loc.page_id, true, parent));
        CALICODB_TRY(fix_nonroot(std::move(parent), last_loc.index));

        report_stats(kSMOCount, 1);
    }
    return Status::ok();
}

auto Tree::merge_left(Node &left, Node right, Node &parent, std::size_t index) -> Status
{
    CALICODB_EXPECT_FALSE(parent.header.is_external);
    CALICODB_EXPECT_TRUE(is_underflowing(left));

    if (left.header.is_external) {
        CALICODB_EXPECT_TRUE(right.header.is_external);
        left.header.next_id = right.header.next_id;
        CALICODB_TRY(remove_cell(parent, index));

        while (right.header.cell_count) {
            CALICODB_TRY(transfer_left(left, right));
        }
        write_child_id(parent, index, left.page.id());

        if (!right.header.next_id.is_null()) {
            Node right_sibling;
            CALICODB_TRY(acquire(right.header.next_id, true, right_sibling));
            right_sibling.header.prev_id = left.page.id();
            release(std::move(right_sibling));
        }
    } else {
        CALICODB_EXPECT_FALSE(right.header.is_external);
        auto separator = read_cell(parent, index);
        write_cell(left, left.header.cell_count, separator);
        write_child_id(left, left.header.cell_count - 1, left.header.next_id);
        CALICODB_TRY(fix_parent_id(left.header.next_id, left.page.id(), PointerMap::kTreeNode));
        CALICODB_TRY(maybe_fix_overflow_chain(separator, left.page.id()));
        erase_cell(parent, index, separator.size);

        while (right.header.cell_count) {
            CALICODB_TRY(transfer_left(left, right));
        }
        left.header.next_id = right.header.next_id;
        write_child_id(parent, index, left.page.id());
    }
    CALICODB_TRY(fix_links(left));
    return free(std::move(right));
}

auto Tree::merge_right(Node &left, Node right, Node &parent, std::size_t index) -> Status
{
    CALICODB_EXPECT_FALSE(parent.header.is_external);
    CALICODB_EXPECT_TRUE(is_underflowing(right));
    if (left.header.is_external) {
        CALICODB_EXPECT_TRUE(right.header.is_external);

        left.header.next_id = right.header.next_id;
        CALICODB_EXPECT_EQ(read_child_id(parent, index + 1), right.page.id());
        write_child_id(parent, index + 1, left.page.id());
        CALICODB_TRY(remove_cell(parent, index));

        while (right.header.cell_count) {
            CALICODB_TRY(transfer_left(left, right));
        }
        if (!right.header.next_id.is_null()) {
            Node right_sibling;
            CALICODB_TRY(acquire(right.header.next_id, true, right_sibling));
            right_sibling.header.prev_id = left.page.id();
            release(std::move(right_sibling));
        }
    } else {
        CALICODB_EXPECT_FALSE(right.header.is_external);

        auto separator = read_cell(parent, index);
        write_cell(left, left.header.cell_count, separator);
        write_child_id(left, left.header.cell_count - 1, left.header.next_id);
        CALICODB_TRY(fix_parent_id(left.header.next_id, left.page.id(), PointerMap::kTreeNode));
        CALICODB_TRY(maybe_fix_overflow_chain(separator, left.page.id()));
        left.header.next_id = right.header.next_id;

        CALICODB_EXPECT_EQ(read_child_id(parent, index + 1), right.page.id());
        write_child_id(parent, index + 1, left.page.id());
        erase_cell(parent, index, separator.size);

        // Transfer the rest of the cells. left shouldn't overflow.
        while (right.header.cell_count) {
            CALICODB_TRY(transfer_left(left, right));
        }
    }
    CALICODB_TRY(fix_links(left));
    return free(std::move(right));
}

auto Tree::fix_nonroot(Node parent, std::size_t index) -> Status
{
    auto &node = m_cursor.node();
    CALICODB_EXPECT_NE(node.page.id(), root());
    CALICODB_EXPECT_TRUE(is_underflowing(node));
    CALICODB_EXPECT_FALSE(is_overflowing(parent));

    if (index > 0) {
        Node left;
        CALICODB_TRY(acquire(read_child_id(parent, index - 1), true, left));
        if (left.header.cell_count == 1) {
            const auto save_id = node.page.id();
            CALICODB_TRY(merge_right(left, m_cursor.take(), parent, index - 1));
            release(std::move(left));
            CALICODB_EXPECT_FALSE(is_overflowing(parent));
            m_cursor.move_to(std::move(parent), -1);
            return Status::ok();
        }
        CALICODB_TRY(rotate_right(parent, left, node, index - 1));
        release(std::move(left));
    } else {
        Node right;
        CALICODB_TRY(acquire(read_child_id(parent, index + 1), true, right));
        if (right.header.cell_count == 1) {
            CALICODB_TRY(merge_left(node, std::move(right), parent, index));
            CALICODB_EXPECT_FALSE(is_overflowing(parent));
            m_cursor.move_to(std::move(parent), -1);
            return Status::ok();
        }
        CALICODB_TRY(rotate_left(parent, node, right, index));
        release(std::move(right));
    }

    CALICODB_EXPECT_FALSE(is_overflowing(node));
    m_cursor.move_to(std::move(parent), -1);
    if (is_overflowing(m_cursor.node())) {
        CALICODB_TRY(resolve_overflow());
    }
    return Status::ok();
}

auto Tree::fix_root() -> Status
{
    auto &node = m_cursor.node();
    CALICODB_EXPECT_EQ(node.page.id(), root());

    // If the root is external here, the whole tree must be empty.
    if (!node.header.is_external) {
        Node child;
        CALICODB_TRY(acquire(node.header.next_id, true, child));

        // We don't have enough room to transfer the child contents into the root, due to the space occupied by
        // the file header. In this case, we'll just split the child and insert the median cell into the root.
        // Note that the child needs an overflow cell for the split routine to work. We'll just fake it by
        // extracting an arbitrary cell and making it the overflow cell.
        if (node.page.id().is_root() && usable_space(child) < FileHeader::kSize) {
            child.overflow_index = child.header.cell_count / 2;
            child.overflow = read_cell(child, child.overflow_index);
            detach_cell(*child.overflow, cell_scratch());
            erase_cell(child, child.overflow_index);
            m_cursor.clear();
            m_cursor.move_to(std::move(child), 0);
            CALICODB_TRY(split_nonroot());
        } else {
            merge_root(node, child);
            CALICODB_TRY(free(std::move(child)));
        }
        CALICODB_TRY(fix_links(node));
    }
    return Status::ok();
}

auto Tree::rotate_left(Node &parent, Node &left, Node &right, std::size_t index) -> Status
{
    CALICODB_EXPECT_FALSE(parent.header.is_external);
    CALICODB_EXPECT_GT(parent.header.cell_count, 0);
    CALICODB_EXPECT_GT(right.header.cell_count, 1);
    if (left.header.is_external) {
        CALICODB_EXPECT_TRUE(right.header.is_external);

        auto lowest = read_cell(right, 0);
        CALICODB_TRY(insert_cell(left, left.header.cell_count, lowest));
        CALICODB_EXPECT_FALSE(is_overflowing(left));
        erase_cell(right, 0);

        auto separator = read_cell(right, 0);
        CALICODB_TRY(PayloadManager::promote(
            *m_pager,
            cell_scratch(),
            separator,
            parent.page.id()));
        write_child_id(separator, left.page.id());

        CALICODB_TRY(remove_cell(parent, index));
        return insert_cell(parent, index, separator);
    } else {
        CALICODB_EXPECT_FALSE(right.header.is_external);

        Node child;
        CALICODB_TRY(acquire(read_child_id(right, 0), true, child));
        const auto saved_id = left.header.next_id;
        left.header.next_id = child.page.id();
        CALICODB_TRY(fix_parent_id(child.page.id(), left.page.id(), PointerMap::kTreeNode));
        release(std::move(child));

        const auto separator = read_cell(parent, index);
        CALICODB_TRY(insert_cell(left, left.header.cell_count, separator));
        CALICODB_EXPECT_FALSE(is_overflowing(left));
        write_child_id(left, left.header.cell_count - 1, saved_id);
        erase_cell(parent, index, separator.size);

        auto lowest = read_cell(right, 0);
        detach_cell(lowest, cell_scratch());
        erase_cell(right, 0);
        write_child_id(lowest, left.page.id());
        return insert_cell(parent, index, lowest);
    }
}

auto Tree::rotate_right(Node &parent, Node &left, Node &right, std::size_t index) -> Status
{
    CALICODB_EXPECT_FALSE(parent.header.is_external);
    CALICODB_EXPECT_GT(parent.header.cell_count, 0);
    CALICODB_EXPECT_GT(left.header.cell_count, 1);

    if (left.header.is_external) {
        CALICODB_EXPECT_TRUE(right.header.is_external);

        auto highest = read_cell(left, left.header.cell_count - 1);
        CALICODB_TRY(insert_cell(right, 0, highest));
        CALICODB_EXPECT_FALSE(is_overflowing(right));

        auto separator = highest;
        CALICODB_TRY(PayloadManager::promote(
            *m_pager,
            cell_scratch(),
            separator,
            parent.page.id()));
        write_child_id(separator, left.page.id());

        // Don't erase the cell until it has been detached.
        erase_cell(left, left.header.cell_count - 1);

        CALICODB_TRY(remove_cell(parent, index));
        CALICODB_TRY(insert_cell(parent, index, separator));
    } else {
        CALICODB_EXPECT_FALSE(right.header.is_external);

        Node child;
        CALICODB_TRY(acquire(left.header.next_id, true, child));
        const auto child_id = child.page.id();
        CALICODB_TRY(fix_parent_id(child.page.id(), right.page.id(), PointerMap::kTreeNode));
        left.header.next_id = read_child_id(left, left.header.cell_count - 1);
        release(std::move(child));

        auto separator = read_cell(parent, index);
        CALICODB_TRY(insert_cell(right, 0, separator));
        CALICODB_EXPECT_FALSE(is_overflowing(right));
        write_child_id(right, 0, child_id);
        erase_cell(parent, index, separator.size);

        auto highest = read_cell(left, left.header.cell_count - 1);
        detach_cell(highest, cell_scratch());
        write_child_id(highest, left.page.id());
        erase_cell(left, left.header.cell_count - 1, highest.size);
        CALICODB_TRY(insert_cell(parent, index, highest));
    }
    return Status::ok();
}

Tree::Tree(Pager &pager, const Id *root_id)
    : m_node_scratch(kPageSize, '\0'),
      m_cell_scratch(kPageSize, '\0'),
      m_pager(&pager),
      m_cursor(*this),
      m_root_id(root_id)
{
}

Tree::~Tree()
{
    m_cursor.clear();
}

auto Tree::report_stats(ReportType type, std::size_t increment) const -> void
{
    if (type == kBytesRead) {
        m_stats.bytes_read += increment;
    } else if (type == kBytesWritten) {
        m_stats.bytes_written += increment;
    } else {
        m_stats.smo_count += increment;
    }
}

auto Tree::cell_scratch() -> char *
{
    // Leave space for a child ID (maximum difference between the size of a varint and an Id).
    return m_cell_scratch.data() + Id::kSize - 1;
}

auto Tree::get(const Slice &key, std::string *value) const -> Status
{
    bool found;
    auto s = find_external(key, false, found);
    if (!s.is_ok()) {
        // Do nothing. A low-level I/O error has occurred.
    } else if (!found) {
        s = Status::not_found();
    } else if (value) {
        Slice slice;
        s = read_value(m_cursor.node(), m_cursor.index(), *value, slice);
        value->resize(slice.size());
        if (s.is_ok()) {
            report_stats(kBytesRead, slice.size());
        }
    }
    m_cursor.clear();
    return s;
}

auto Tree::put(const Slice &key, const Slice &value) -> Status
{
    if (key.is_empty()) {
        return Status::invalid_argument("key is empty");
    }
    bool exact;
    auto s = find_external(key, true, exact);
    if (s.is_ok()) {
        if (exact) {
            s = remove_cell(m_cursor.node(), m_cursor.index());
        }
        if (s.is_ok()) {
            bool overflow;
            // Create a cell representing the `key` and `value`. This routine also populates any
            // overflow pages necessary to hold a `key` and/or `value` that won't fit on a single
            // node page. If the cell cannot fit in `node`, it will be written to scratch memory.
            s = emplace(m_cursor.node(), key, value, m_cursor.index(), overflow);

            if (s.is_ok()) {
                if (overflow) {
                    // There wasn't enough room in `node` to hold the cell. Get the node back and
                    // perform a split.
                    m_cursor.node().overflow = parse_external_cell(
                        cell_scratch(), cell_scratch() + kPageSize);
                    m_cursor.node().overflow->is_free = true;
                    s = resolve_overflow();
                }
                report_stats(kBytesWritten, key.size() + value.size());
            }
        }
    }
    m_cursor.clear();
    return s;
}

auto Tree::emplace(Node &node, const Slice &key, const Slice &value, std::size_t index, bool &overflow) -> Status
{
    CALICODB_EXPECT_TRUE(node.header.is_external);
    auto k = key.size();
    auto v = value.size();
    const auto local_size = compute_local_size(k, v);
    const auto has_remote = k + v > local_size;

    if (k > local_size) {
        k = local_size;
        v = 0;
    } else if (has_remote) {
        v = local_size - k;
    }

    CALICODB_EXPECT_EQ(k + v, local_size);
    // Serialize the cell header for the external cell and determine the number
    // of bytes needed for the cell.
    char header[kVarintMaxLength * 2];
    auto *ptr = header;
    ptr = encode_varint(ptr, value.size());
    ptr = encode_varint(ptr, key.size());
    const auto hdr_size = std::uintptr_t(ptr - header);
    const auto cell_size = local_size + hdr_size + Id::kSize * has_remote;

    // Attempt to allocate space for the cell in the node. If this is not possible,
    // write the cell to scratch memory.
    ptr = cell_scratch();
    const auto local_offset = allocate_block(node, U32(index), U32(cell_size));
    if (local_offset) {
        ptr = node.page.data() + local_offset;
        overflow = false;
    } else {
        overflow = true;
    }
    // Write the cell header.
    std::memcpy(ptr, header, hdr_size);
    ptr += hdr_size;

    std::optional<Page> prev;
    auto payload_left = key.size() + value.size();
    auto prev_pgno = node.page.id();
    auto prev_type = PointerMap::kOverflowHead;
    auto *next_ptr = ptr + local_size;
    auto len = local_size;
    auto src = key;

    Status s;
    while (s.is_ok()) {
        const auto n = std::min(len, src.size());
        std::memcpy(ptr, src.data(), n);
        src.advance(n);
        payload_left -= n;
        if (payload_left == 0) {
            break;
        }
        ptr += n;
        len -= n;
        if (src.is_empty()) {
            src = value;
        }
        if (src.is_empty()) {
            break;
        } else if (len == 0) {
            Page ovfl;
            s = m_pager->allocate(ovfl);
            if (s.is_ok()) {
                put_u32(next_ptr, ovfl.id().value);
                put_u32(ovfl.data(), 0);
                len = kLinkContentSize;
                ptr = ovfl.data() + Id::kSize;
                next_ptr = ovfl.data();
                if (prev) {
                    m_pager->release(std::move(*prev));
                }
                s = PointerMap::write_entry(
                    *m_pager, ovfl.id(), {prev_pgno, prev_type});
                prev_type = PointerMap::kOverflowLink;
                prev_pgno = ovfl.id();
                prev = std::move(ovfl);
            }
        }
    }
    if (prev) {
        m_pager->release(std::move(*prev));
    }
    return s;
}

auto Tree::erase(const Slice &key) -> Status
{
    bool exact;
    auto s = find_external(key, true, exact);
    if (s.is_ok() && exact) {
        s = remove_cell(m_cursor.node(), m_cursor.index());
        if (s.is_ok() && is_underflowing(m_cursor.node())) {
            s = resolve_underflow();
        }
    }
    m_cursor.clear();
    return s;

    //    SearchResult slot;
    //
    //    CALICODB_TRY(find_external(key, slot));
    //    auto [node, index, exact] = std::move(slot);
    //
    //    if (exact) {
    //        Slice anchor;
    //        CALICODB_TRY(read_key(node, index, m_anchor, anchor));
    //
    //        upgrade(node);
    //        CALICODB_TRY(remove_cell(node, index));
    //
    //        return resolve_underflow(std::move(node), anchor);
    //    }
    //    release(std::move(node));
    //    return Status::ok();
}

auto Tree::find_lowest(Node &out) const -> Status
{
    auto s = acquire(root(), false, out);
    while (s.is_ok() && !out.header.is_external) {
        const auto next_id = read_child_id(out, 0);
        release(std::move(out));
        s = acquire(next_id, false, out);
    }
    return s;
}

auto Tree::find_highest(Node &out) const -> Status
{
    auto s = acquire(root(), false, out);
    while (s.is_ok() && !out.header.is_external) {
        const auto next_id = out.header.next_id;
        release(std::move(out));
        s = acquire(next_id, false, out);
    }
    return s;
}

auto Tree::vacuum_step(Page &free, Schema &schema, Id last_id) -> Status
{
    CALICODB_EXPECT_NE(free.id(), last_id);
    auto &freelist = m_pager->m_freelist;

    PointerMap::Entry entry;
    CALICODB_TRY(PointerMap::read_entry(*m_pager, last_id, entry));

    const auto fix_basic_link = [&entry, &free, this]() -> Status {
        Page parent;
        CALICODB_TRY(m_pager->acquire(entry.back_ptr, parent));
        m_pager->mark_dirty(parent);
        write_next_id(parent, free.id());
        m_pager->release(std::move(parent));
        return Status::ok();
    };

    switch (entry.type) {
        case PointerMap::kFreelistLink: {
            if (last_id == freelist.m_head) {
                freelist.m_head = free.id();
            } else if (last_id != free.id()) {
                // Back pointer points to another freelist page.
                CALICODB_EXPECT_FALSE(entry.back_ptr.is_null());
                CALICODB_TRY(fix_basic_link());
                Page last;
                CALICODB_TRY(m_pager->acquire(last_id, last));
                if (const auto next_id = read_next_id(last); !next_id.is_null()) {
                    CALICODB_TRY(fix_parent_id(next_id, free.id(), PointerMap::kFreelistLink));
                }
                m_pager->release(std::move(last));
            }
            break;
        }
        case PointerMap::kOverflowLink: {
            // Back pointer points to another overflow chain link, or the head of the chain.
            CALICODB_TRY(fix_basic_link());
            break;
        }
        case PointerMap::kOverflowHead: {
            // Back pointer points to the node that the overflow chain is rooted in. Search through that node's cells
            // for the target overflowing cell.
            Node parent;
            CALICODB_TRY(acquire(entry.back_ptr, true, parent));
            bool found = false;
            for (std::size_t i = 0; i < parent.header.cell_count; ++i) {
                auto cell = read_cell(parent, i);
                found = cell.has_remote && read_overflow_id(cell) == last_id;
                if (found) {
                    write_overflow_id(cell, free.id());
                    break;
                }
            }
            CALICODB_EXPECT_TRUE(found);
            release(std::move(parent));
            break;
        }
        case PointerMap::kTreeRoot: {
            schema.vacuum_reroot(last_id, free.id());
            // Tree root pages are also node pages (with no parent page). Handle them the same, but
            // note the guard against updating the parent page's child pointers below.
            [[fallthrough]];
        }
        case PointerMap::kTreeNode: {
            if (entry.type != PointerMap::kTreeRoot) {
                // Back pointer points to another node, i.e. this is not a root. Search through the
                // parent for the target child pointer and overwrite it with the new page ID.
                Node parent;
                CALICODB_TRY(acquire(entry.back_ptr, true, parent));
                CALICODB_EXPECT_FALSE(parent.header.is_external);
                bool found = false;
                for (std::size_t i = 0; !found && i <= parent.header.cell_count; ++i) {
                    const auto child_id = read_child_id(parent, i);
                    found = child_id == last_id;
                    if (found) {
                        write_child_id(parent, i, free.id());
                    }
                }
                CALICODB_EXPECT_TRUE(found);
                release(std::move(parent));
            }
            // Update references.
            Node last;
            CALICODB_TRY(acquire(last_id, true, last));
            for (std::size_t i = 0; i < last.header.cell_count; ++i) {
                const auto cell = read_cell(last, i);
                CALICODB_TRY(maybe_fix_overflow_chain(cell, free.id()));
                if (!last.header.is_external) {
                    CALICODB_TRY(fix_parent_id(read_child_id(last, i), free.id(), PointerMap::kTreeNode));
                }
            }
            if (!last.header.is_external) {
                CALICODB_TRY(fix_parent_id(last.header.next_id, free.id(), PointerMap::kTreeNode));
            } else {
                if (!last.header.prev_id.is_null()) {
                    Node prev;
                    CALICODB_TRY(acquire(last.header.prev_id, true, prev));
                    prev.header.next_id = free.id();
                    release(std::move(prev));
                }
                if (!last.header.next_id.is_null()) {
                    Node next;
                    CALICODB_TRY(acquire(last.header.next_id, true, next));
                    next.header.prev_id = free.id();
                    release(std::move(next));
                }
            }
            release(std::move(last));
            break;
        }
        default:
            return Status::corruption("pointer map page is corrupted");
    }
    CALICODB_TRY(PointerMap::write_entry(*m_pager, last_id, {}));
    CALICODB_TRY(PointerMap::write_entry(*m_pager, free.id(), entry));

    Page last;
    CALICODB_TRY(m_pager->acquire(last_id, last));

    const auto is_link =
        entry.type != PointerMap::kTreeNode &&
        entry.type != PointerMap::kTreeRoot;
    if (is_link) {
        if (const auto next_id = read_next_id(last); !next_id.is_null()) {
            PointerMap::Entry next_entry;
            CALICODB_TRY(PointerMap::read_entry(*m_pager, next_id, next_entry));
            next_entry.back_ptr = free.id();
            CALICODB_TRY(PointerMap::write_entry(*m_pager, next_id, next_entry));
        }
    }
    std::memcpy(free.data(), last.data(), kPageSize);
    m_pager->release(std::move(last));
    return Status::ok();
}

auto Tree::vacuum_one(Id target, Schema &schema, bool *success) -> Status
{
    if (PointerMap::is_map(target)) {
        *success = true;
        return Status::ok();
    }
    auto &freelist = m_pager->m_freelist;
    if (target.is_root() || freelist.is_empty()) {
        *success = false;
        return Status::ok();
    }

    // Swap the head of the freelist with the last page in the file.
    Page head;
    CALICODB_TRY(freelist.pop(head));
    if (target != head.id()) {
        // Swap the last page with the freelist head.
        CALICODB_TRY(vacuum_step(head, schema, target));
    } else {
        CALICODB_TRY(fix_parent_id(target, Id::null(), {}));
    }
    m_pager->release(std::move(head));
    *success = true;
    return Status::ok();
}

auto Tree::destroy_impl(Node node) -> Status
{
    for (std::size_t i = 0; i <= node.header.cell_count; ++i) {
        if (i < node.header.cell_count) {
            if (const auto cell = read_cell(node, i); cell.has_remote) {
                CALICODB_TRY(free_overflow(read_overflow_id(cell)));
            }
        }
        if (!node.header.is_external) {
            const auto save_id = node.page.id();
            const auto next_id = read_child_id(node, i);
            release(std::move(node));

            Node next;
            CALICODB_TRY(acquire(next_id, false, next));
            CALICODB_TRY(destroy_impl(std::move(next)));
            CALICODB_TRY(acquire(save_id, false, node));
        }
    }

    if (!node.page.id().is_root()) {
        return free(std::move(node));
    }
    return Status::ok();
}

auto Tree::destroy(Tree &tree) -> Status
{
    Node root;
    CALICODB_TRY(tree.acquire(tree.root(), false, root));
    return tree.destroy_impl(std::move(root));
}

auto NodeManager::allocate(Pager &pager, Node &out, std::string &scratch, bool is_external) -> Status
{
    auto s = pager.allocate(out.page);
    if (s.is_ok()) {
        CALICODB_EXPECT_FALSE(PointerMap::is_map(out.page.id()));
        out.header.is_external = is_external;
        out.scratch = scratch.data();
        setup_node(out);
    }
    return s;
}

auto NodeManager::acquire(Pager &pager, Id page_id, Node &out, std::string &scratch, bool upgrade_node) -> Status
{
    CALICODB_EXPECT_FALSE(PointerMap::is_map(page_id));
    auto s = pager.acquire(page_id, out.page);
    if (s.is_ok()) {
        out.scratch = scratch.data();
        out.header.read(out.page.data() + node_header_offset(out));
        setup_node(out);
        if (upgrade_node) {
            upgrade(pager, out);
        }
    }
    return s;
}

auto NodeManager::upgrade(Pager &pager, Node &node) -> void
{
    pager.mark_dirty(node.page);
}

auto NodeManager::release(Pager &pager, Node node) -> void
{
    pager.release(std::move(node).take());
}

auto NodeManager::destroy(Pager &pager, Node node) -> Status
{
    return pager.destroy(std::move(node.page));
}

auto PayloadManager::promote(Pager &pager, char *scratch, Cell &cell, Id parent_id) -> Status
{
    detach_cell(cell, scratch);

    // The buffer that `scratch` points into should have enough room before `scratch` to write
    // the left child ID.
    const auto header_size = Id::kSize + varint_length(cell.key_size);
    cell.ptr = cell.key - header_size;
    cell.local_size = compute_local_size(cell.key_size, 0);
    cell.size = header_size + cell.local_size;
    cell.has_remote = false;

    if (cell.key_size > cell.local_size) {
        // Part of the key is on an overflow page. No value is stored locally in this case, so
        // the local size computation is still correct. Copy the overflow key, page-by-page,
        // to a new overflow chain.
        Id ovfl_id;
        auto rest = cell.key_size - cell.local_size;
        auto pgno = read_overflow_id(cell);
        std::optional<Page> prev;
        while (rest && !pgno.is_null()) {
            Page src, dst;
            CALICODB_TRY(pager.allocate(dst));
            CALICODB_TRY(pager.acquire(pgno, src));
            if (ovfl_id.is_null()) {
                // Record the new overflow ID to write to the promoted cell.
                ovfl_id = dst.id();
            }
            std::memcpy(dst.data(), src.data(), kPageSize);
            if (prev) {
                const PointerMap::Type type = ovfl_id.is_null() ? PointerMap::kOverflowHead : PointerMap::kOverflowLink;
                const PointerMap::Entry entry = {prev->id(), type};
                CALICODB_TRY(PointerMap::write_entry(pager, dst.id(), entry));
                put_u32(prev->data(), dst.id().value);
                pager.release(std::move(*prev));
            }
            rest -= std::min(rest, kLinkContentSize);
            prev = std::move(dst);
            pgno = read_next_id(src);
            pager.release(std::move(src));
        }
        put_u32(prev->data(), 0);
        pager.release(std::move(*prev));

        const PointerMap::Entry entry = {parent_id, PointerMap::kOverflowHead};
        CALICODB_TRY(PointerMap::write_entry(pager, ovfl_id, entry));
        write_overflow_id(cell, ovfl_id);
        cell.size += Id::kSize;
        cell.has_remote = true;
    }
    return Status::ok();
}

auto PayloadManager::access(
    Pager &pager,
    const Cell &cell,   // The `cell` containing the payload being accessed
    std::size_t offset, // `offset` within the payload being accessed
    std::size_t length, // Number of bytes to access
    const char *in_buf, // Write buffer of size at least `length` bytes, or nullptr if not a write
    char *out_buf       // Read buffer of size at least `length` bytes, or nullptr if not a read
    ) -> Status
{
    CALICODB_EXPECT_TRUE(in_buf || out_buf);
    if (offset <= cell.local_size) {
        const auto n = std::min(length, cell.local_size - offset);
        if (in_buf) {
            std::memcpy(cell.key + offset, in_buf, n);
            in_buf += n;
        } else {
            std::memcpy(out_buf, cell.key + offset, n);
            out_buf += n;
        }
        length -= n;
        offset = 0;
    } else {
        offset -= cell.local_size;
    }

    Status s;
    if (length) {
        auto pgno = read_overflow_id(cell);
        while (!pgno.is_null()) {
            Page ovfl;
            s = pager.acquire(pgno, ovfl);
            if (!s.is_ok()) {
                break;
            }
            std::size_t len;
            if (offset >= kLinkContentSize) {
                offset -= kLinkContentSize;
                len = 0;
            } else {
                auto *ptr = ovfl.data() + kLinkContentOffset + offset;
                len = std::min(length, kLinkContentSize - offset);
                if (in_buf) {
                    std::memcpy(ptr, in_buf, len);
                    in_buf += len;
                } else {
                    std::memcpy(out_buf, ptr, len);
                    out_buf += len;
                }
                offset = 0;
            }
            pgno = read_next_id(ovfl);
            pager.release(std::move(ovfl));
            length -= len;
            if (length == 0) {
                break;
            }
        }
    }
    return s;
}

#if CALICODB_TEST

#define CHECK_OK(expr)                                           \
    do {                                                         \
        if (const auto check_s = (expr); !check_s.is_ok()) {     \
            std::fprintf(stderr, "error(line %d): %s\n",         \
                         __LINE__, check_s.to_string().c_str()); \
            std::abort();                                        \
        }                                                        \
    } while (0)

#define CHECK_TRUE(expr)                                                                   \
    do {                                                                                   \
        if (!(expr)) {                                                                     \
            std::fprintf(stderr, "error: \"%s\" was false on line %d\n", #expr, __LINE__); \
            std::abort();                                                                  \
        }                                                                                  \
    } while (0)

#define CHECK_EQ(lhs, rhs)                                                                         \
    do {                                                                                           \
        if ((lhs) != (rhs)) {                                                                      \
            std::fprintf(stderr, "error: \"" #lhs " != " #rhs "\" failed on line %d\n", __LINE__); \
            std::abort();                                                                          \
        }                                                                                          \
    } while (0)

auto Node::TEST_validate() -> void
{
    std::vector<char> used(kPageSize);
    const auto account = [&x = used](auto from, auto size) {
        auto lower = begin(x) + long(from);
        auto upper = begin(x) + long(from) + long(size);
        CHECK_TRUE(!std::any_of(lower, upper, [](auto byte) {
            return byte != '\x00';
        }));

        std::fill(lower, upper, 1);
    };
    // Header(s) and cell pointers.
    {
        account(0, cell_area_offset(*this));

        // Make sure the header fields are not obviously wrong.
        CALICODB_EXPECT_LT(header.frag_count, static_cast<U8>(-1) * 2);
        CALICODB_EXPECT_LT(header.cell_count, static_cast<U16>(-1));
        CALICODB_EXPECT_LT(header.free_start, static_cast<U16>(-1));
    }
    // Gap space.
    {
        account(cell_area_offset(*this), gap_size);
    }
    // Free list blocks.
    {
        std::vector<unsigned> offsets;
        auto i = header.free_start;
        const char *data = page.data();
        while (i) {
            const auto size = get_u16(data + i + kPointerSize);
            account(i, size);
            offsets.emplace_back(i);
            i = get_u16(data + i);
        }
        const auto offsets_copy = offsets;
        std::sort(begin(offsets), end(offsets));
        CALICODB_EXPECT_EQ(offsets, offsets_copy);
    }
    // Cell bodies. Also makes sure the cells are in order where possible.
    for (std::size_t n = 0; n < header.cell_count; ++n) {
        const auto lhs_ptr = get_slot(n);
        const auto lhs_cell = read_cell_at(*this, lhs_ptr);
        CHECK_TRUE(lhs_cell.size >= 3);
        account(lhs_ptr, lhs_cell.size);

        if (n + 1 < header.cell_count) {
            const auto rhs_ptr = get_slot(n + 1);
            const auto rhs_cell = read_cell_at(*this, rhs_ptr);
            if (!lhs_cell.has_remote && !rhs_cell.has_remote) {
                const Slice lhs_key(lhs_cell.key, lhs_cell.key_size);
                const Slice rhs_key(rhs_cell.key, rhs_cell.key_size);
                CHECK_TRUE(lhs_key < rhs_key);
            }
        }
    }

    // Every byte should be accounted for, except for fragments.
    const auto total_bytes = std::accumulate(
        begin(used),
        end(used),
        static_cast<int>(header.frag_count),
        [](auto accum, auto next) {
            return accum + next;
        });
    CHECK_EQ(kPageSize, std::size_t(total_bytes));
}

class TreeValidator
{
    using NodeCallback = std::function<void(Node &, std::size_t)>;
    using PageCallback = std::function<void(const Page &)>;

    struct PrinterData {
        std::vector<std::string> levels;
        std::vector<std::size_t> spaces;
    };

    static auto traverse_inorder_helper(const Tree &tree, Node node, const NodeCallback &callback) -> void
    {
        for (std::size_t index = 0; index <= node.header.cell_count; ++index) {
            if (!node.header.is_external) {
                const auto saved_id = node.page.id();
                const auto next_id = read_child_id(node, index);

                // "node" must be released while we traverse, otherwise we are limited in how long of a traversal we can
                // perform by the number of pager frames.
                tree.release(std::move(node));

                Node next;
                CHECK_OK(tree.acquire(next_id, false, next));
                traverse_inorder_helper(tree, std::move(next), callback);
                CHECK_OK(tree.acquire(saved_id, false, node));
            }
            if (index < node.header.cell_count) {
                callback(node, index);
            }
        }
        tree.release(std::move(node));
    }

    static auto traverse_inorder(const Tree &tree, const NodeCallback &callback) -> void
    {
        Node root;
        CHECK_OK(tree.acquire(tree.root(), false, root));
        traverse_inorder_helper(tree, std::move(root), callback);
    }

    static auto traverse_chain(Pager &pager, Page page, const PageCallback &callback) -> void
    {
        for (;;) {
            callback(page);

            const auto next_id = read_next_id(page);
            pager.release(std::move(page));
            if (next_id.is_null()) {
                break;
            }
            CHECK_OK(pager.acquire(next_id, page));
        }
    }

    static auto add_to_level(PrinterData &data, const std::string &message, std::size_t target) -> void
    {
        // If target is equal to levels.size(), add spaces to all levels.
        CHECK_TRUE(target <= data.levels.size());
        std::size_t i = 0;

        auto s_itr = begin(data.spaces);
        auto L_itr = begin(data.levels);
        while (s_itr != end(data.spaces)) {
            CHECK_TRUE(L_itr != end(data.levels));
            if (i++ == target) {
                // Don't leave trailing spaces. Only add them if there will be more text.
                L_itr->resize(L_itr->size() + *s_itr, ' ');
                L_itr->append(message);
                *s_itr = 0;
            } else {
                *s_itr += message.size();
            }
            ++L_itr;
            ++s_itr;
        }
    }

    static auto ensure_level_exists(PrinterData &data, std::size_t level) -> void
    {
        while (level >= data.levels.size()) {
            data.levels.emplace_back();
            data.spaces.emplace_back();
        }
        CHECK_TRUE(data.levels.size() > level);
        CHECK_TRUE(data.levels.size() == data.spaces.size());
    }

    static auto collect_levels(const Tree &tree, PrinterData &data, Node node, std::size_t level) -> void
    {
        const auto &header = node.header;
        ensure_level_exists(data, level);
        for (std::size_t cid = 0; cid < header.cell_count; ++cid) {
            const auto is_first = cid == 0;
            const auto not_last = cid + 1 < header.cell_count;
            auto cell = read_cell(node, cid);

            if (!header.is_external) {
                Node next;
                CHECK_OK(tree.acquire(read_child_id(cell), false, next));
                collect_levels(tree, data, std::move(next), level + 1);
            }

            if (is_first) {
                add_to_level(data, std::to_string(node.page.id().value) + ":[", level);
            }

            const auto key = Slice(cell.key, std::min<std::size_t>(3, cell.key_size)).to_string();
            add_to_level(data, escape_string(key), level);
            if (cell.has_remote) {
                add_to_level(data, "(" + number_to_string(read_overflow_id(cell).value) + ")", level);
            }

            if (not_last) {
                add_to_level(data, ",", level);
            } else {
                add_to_level(data, "]", level);
            }
        }
        if (!node.header.is_external) {
            Node next;
            CHECK_OK(tree.acquire(node.header.next_id, false, next));
            collect_levels(tree, data, std::move(next), level + 1);
        }

        tree.release(std::move(node));
    }

public:
    //    static auto validate_freelist(Tree &tree, Id head) -> void
    //    {
    //        auto &pager = *tree.m_pager;
    //        auto &freelist = tree.freelist;
    //        if (freelist.is_empty()) {
    //            return;
    //        }
    //        CHECK_TRUE(!head.is_null());
    //        Page page;
    //        CHECK_OK(pager.acquire(head, page));
    //
    //        Id parent_id;
    //        traverse_chain(pager, std::move(page), [&](const auto &link) {
    //            Id found_id;
    //            CHECK_OK(tree.find_parent_id(link.id(), found_id));
    //            CHECK_TRUE(found_id == parent_id);
    //            parent_id = link.id();
    //        });
    //    }

    static auto validate_tree(const Tree &tree) -> void
    {
        auto check_parent_child = [&tree](auto &node, auto index) -> void {
            Node child;
            CHECK_OK(tree.acquire(read_child_id(node, index), false, child));

            Id parent_id;
            CHECK_OK(tree.find_parent_id(child.page.id(), parent_id));
            CHECK_TRUE(parent_id == node.page.id());

            tree.release(std::move(child));
        };
        traverse_inorder(tree, [f = std::move(check_parent_child)](const auto &node, auto index) {
            const auto count = node.header.cell_count;
            CHECK_TRUE(index < count);

            if (!node.header.is_external) {
                f(node, index);
                // Rightmost child.
                if (index + 1 == count) {
                    f(node, index + 1);
                }
            }
        });

        traverse_inorder(tree, [&tree](auto &node, auto index) {
            const auto cell = read_cell(node, index);

            auto accumulated = cell.local_size;
            auto requested = cell.key_size;
            if (node.header.is_external) {
                U64 value_size;
                decode_varint(cell.ptr, value_size);
                requested += value_size;
            }

            if (cell.has_remote) {
                const auto overflow_id = read_overflow_id(cell);
                Page head;
                CHECK_OK(tree.m_pager->acquire(overflow_id, head));
                traverse_chain(*tree.m_pager, std::move(head), [&](auto &page) {
                    CHECK_TRUE(requested > accumulated);
                    const auto size_limit = std::min(kPageSize, requested - accumulated);
                    accumulated += get_readable_content(page, size_limit).size();
                });
                CHECK_EQ(requested, accumulated);
            }

            if (index == 0) {
                node.TEST_validate();

                if (node.header.is_external && !node.header.next_id.is_null()) {
                    Node next;
                    CHECK_OK(tree.acquire(node.header.next_id, false, next));

                    tree.release(std::move(next));
                }
            }
        });

        // Find the leftmost external node.
        Node node;
        CHECK_OK(tree.acquire(tree.root(), false, node));
        while (!node.header.is_external) {
            const auto id = read_child_id(node, 0);
            tree.release(std::move(node));
            CHECK_OK(tree.acquire(id, false, node));
        }
        while (!node.header.next_id.is_null()) {
            Node right;
            CHECK_OK(tree.acquire(node.header.next_id, false, right));
            std::string lhs_buffer, rhs_buffer;
            Slice lhs_key;
            CHECK_OK(const_cast<Tree &>(tree).read_key(node, 0, lhs_buffer, lhs_key));
            Slice rhs_key;
            CHECK_OK(const_cast<Tree &>(tree).read_key(right, 0, rhs_buffer, rhs_key));
            CHECK_TRUE(lhs_key < rhs_key);
            CHECK_EQ(right.header.prev_id, node.page.id());
            tree.release(std::move(node));
            node = std::move(right);
        }
        tree.release(std::move(node));
    }

    [[nodiscard]] static auto to_string(const Tree &tree) -> std::string
    {
        std::string repr;
        PrinterData data;

        Node root;
        CHECK_OK(tree.acquire(tree.root(), false, root));
        collect_levels(tree, data, std::move(root), 0);
        for (const auto &level : data.levels) {
            repr.append(level + '\n');
        }
        return repr;
    }
};

auto Tree::TEST_validate() const -> void
{
    //    TreeValidator::validate_freelist(*this, *freelist.m_head);
    TreeValidator::validate_tree(*this);
}

auto Tree::TEST_to_string() const -> std::string
{
    return TreeValidator::to_string(*this);
}

#undef CHECK_TRUE
#undef CHECK_EQ
#undef CHECK_OK

#else

auto Node::TEST_validate() -> void
{
}

auto Tree::TEST_to_string() const -> std::string
{
    return "";
}

auto Tree::TEST_validate() -> void
{
}

#endif // CALICODB_TEST

InternalCursor::~InternalCursor()
{
    clear();
}

auto InternalCursor::clear() -> void
{
    if (is_valid()) {
        m_tree->release(std::move(m_node));
        m_node.overflow.reset();
    }
    // When m_status.is_ok() is false, none of the other variables besides m_tree may
    // be trusted. Call seek_root() to reposition the cursor on the root node.
    m_status = Status::not_found();
}

auto InternalCursor::seek_root(bool write) -> void
{
    clear();
    level = 0;
    history[0] = {m_tree->root(), 0};
    m_status = m_tree->acquire(m_tree->root(), write, m_node);
    m_write = write;
}

auto InternalCursor::seek(const Slice &key) -> bool
{
    CALICODB_EXPECT_TRUE(is_valid());

    const auto n = m_node.header.cell_count;
    auto exact = false;
    unsigned lower = 0;
    auto upper = n;

    while (lower < upper) {
        Slice rhs;
        const auto mid = (lower + upper) / 2;
        m_status = m_tree->read_key(m_node, mid, m_buffer, rhs);
        if (!is_valid()) {
            break;
        }
        const auto cmp = key.compare(rhs);
        if (cmp <= 0) {
            exact = cmp == 0;
            upper = mid;
        } else if (cmp > 0) {
            lower = mid + 1;
        }
    }
    history[level].index = lower;
    if (!m_node.header.is_external) {
        history[level].index += exact;
    }
    return exact;
}

auto InternalCursor::move_down(Id child_id) -> void
{
    CALICODB_EXPECT_TRUE(is_valid());
    clear();
    history[++level] = {child_id, 0};
    m_status = m_tree->acquire(child_id, m_write, m_node);
}

auto InternalCursor::move_up() -> void
{
    CALICODB_EXPECT_TRUE(is_valid());
    clear();
    m_status = m_tree->acquire(history[--level].page_id, m_write, m_node);
}

Cursor::Cursor() = default;
Cursor::~Cursor() = default;

CursorImpl::~CursorImpl() = default;

auto CursorImpl::is_valid() const -> bool
{
    return m_status.is_ok();
}

auto CursorImpl::status() const -> Status
{
    return m_status;
}

auto CursorImpl::fetch_payload() -> Status
{
    CALICODB_EXPECT_EQ(m_key_size, 0);
    CALICODB_EXPECT_EQ(m_value_size, 0);

    Node node;
    CALICODB_TRY(m_tree->acquire(m_loc.page_id, false, node));

    Slice key, value;
    auto s = m_tree->read_key(node, m_loc.index, m_key, key);
    m_key_size = key.size();
    if (s.is_ok()) {
        s = m_tree->read_value(node, m_loc.index, m_value, value);
        m_value_size = value.size();
    }
    m_tree->release(std::move(node));
    return s;
}

auto CursorImpl::key() const -> Slice
{
    CALICODB_EXPECT_TRUE(is_valid());
    return Slice(m_key).truncate(m_key_size);
}

auto CursorImpl::value() const -> Slice
{
    CALICODB_EXPECT_TRUE(is_valid());
    return Slice(m_value).truncate(m_value_size);
}

auto CursorImpl::seek_first() -> void
{
    m_key_size = 0;
    m_value_size = 0;

    Node lowest;
    auto s = m_tree->find_lowest(lowest);
    if (!s.is_ok()) {
        m_status = s;
        return;
    }
    if (lowest.header.cell_count) {
        seek_to(std::move(lowest), 0);
    } else {
        m_tree->release(std::move(lowest));
        m_status = Status::not_found();
    }
}

auto CursorImpl::seek_last() -> void
{
    m_key_size = 0;
    m_value_size = 0;

    Node highest;
    auto s = m_tree->find_highest(highest);
    if (!s.is_ok()) {
        m_status = s;
        return;
    }
    if (const auto count = highest.header.cell_count) {
        seek_to(std::move(highest), count - 1);
    } else {
        m_tree->release(std::move(highest));
        m_status = Status::not_found();
    }
}

auto CursorImpl::next() -> void
{
    CALICODB_EXPECT_TRUE(is_valid());
    m_key_size = 0;
    m_value_size = 0;

    Node node;
    auto s = m_tree->acquire(m_loc.page_id, false, node);
    if (!s.is_ok()) {
        m_status = s;
        return;
    }
    if (++m_loc.index < m_loc.count) {
        seek_to(std::move(node), m_loc.index);
        return;
    }
    const auto next_id = node.header.next_id;
    m_tree->release(std::move(node));

    if (next_id.is_null()) {
        m_status = default_cursor_status();
        return;
    }
    s = m_tree->acquire(next_id, false, node);
    if (!s.is_ok()) {
        m_status = s;
        return;
    }
    seek_to(std::move(node), 0);
}

auto CursorImpl::previous() -> void
{
    CALICODB_EXPECT_TRUE(is_valid());
    m_key_size = 0;
    m_value_size = 0;

    Node node;
    auto s = m_tree->acquire(m_loc.page_id, false, node);
    if (!s.is_ok()) {
        m_status = s;
        return;
    }
    if (m_loc.index != 0) {
        seek_to(std::move(node), m_loc.index - 1);
        return;
    }
    const auto prev_id = node.header.prev_id;
    m_tree->release(std::move(node));

    if (prev_id.is_null()) {
        m_status = default_cursor_status();
        return;
    }
    s = m_tree->acquire(prev_id, false, node);
    if (!s.is_ok()) {
        m_status = s;
        return;
    }
    const auto count = node.header.cell_count;
    seek_to(std::move(node), count - 1);
}

auto CursorImpl::seek_to(Node node, std::size_t index) -> void
{
    const auto *hdr = &node.header;
    CALICODB_EXPECT_TRUE(hdr->is_external);
    m_status = default_cursor_status();
    if (!hdr->cell_count) {
        // Table is empty.
        return;
    }

    if (index == hdr->cell_count && !hdr->next_id.is_null()) {
        m_tree->release(std::move(node));
        auto s = m_tree->acquire(hdr->next_id, false, node);
        if (!s.is_ok()) {
            m_status = s;
            return;
        }
        hdr = &node.header;
        index = 0;
    }
    if (index < hdr->cell_count) {
        m_loc.index = static_cast<unsigned>(index);
        m_loc.count = hdr->cell_count;
        m_loc.page_id = node.page.id();
        m_status = fetch_payload();
    }
    m_tree->release(std::move(node));
}

auto CursorImpl::seek(const Slice &key) -> void
{
    m_key_size = 0;
    m_value_size = 0;

    bool exact;
    auto s = m_tree->find_external(key, false, exact);
    if (!s.is_ok()) {
        m_status = s;
        return;
    }
    const auto index = m_tree->m_cursor.index();
    seek_to(m_tree->m_cursor.take(), index);
    m_tree->m_cursor.clear();
}

auto CursorInternal::make_cursor(Tree &tree) -> Cursor *
{
    auto *cursor = new CursorImpl(tree);
    invalidate(*cursor, default_cursor_status());
    return cursor;
}

auto CursorInternal::invalidate(const Cursor &cursor, Status error) -> void
{
    CALICODB_EXPECT_FALSE(error.is_ok());
    reinterpret_cast<const CursorImpl &>(cursor).m_status = std::move(error);
}

} // namespace calicodb
