#include "tree.h"
#include "logging.h"
#include "pager.h"
#include "utils.h"
#include <functional>

namespace calicodb
{

struct SeekResult {
    unsigned index {};
    bool exact {};
};

using FetchKey = std::function<Slice(std::size_t)>;

static auto seek_binary(unsigned n, const Slice &key, const FetchKey &fetch) -> SeekResult
{
    auto upper {n};
    unsigned lower {};

    while (lower < upper) {
        const auto mid = (lower + upper) / 2;
        const auto rhs = fetch(mid);
        if (const auto cmp = key.compare(rhs); cmp < 0) {
            upper = mid;
        } else if (cmp > 0) {
            lower = mid + 1;
        } else {
            return {mid, true};
        }
    }
    return {lower, false};
}

NodeIterator::NodeIterator(Node &node, const Parameters &param)
    : m_overflow {param.overflow},
      m_lhs_key {param.lhs_key},
      m_rhs_key {param.rhs_key},
      m_node {&node}
{
    CDB_EXPECT_NE(m_overflow, nullptr);
    CDB_EXPECT_NE(m_lhs_key, nullptr);
    CDB_EXPECT_NE(m_rhs_key, nullptr);
}

// NOTE: "buffer" is only used if the key is fragmented.
auto NodeIterator::fetch_key(std::string &buffer, const Cell &cell, Slice &out) const -> Status
{
    if (!cell.has_remote || cell.key_size <= cell.local_size) {
        out = Slice {cell.key, cell.key_size};
        return Status::ok();
    }

    if (buffer.size() < cell.key_size) {
        buffer.resize(cell.key_size);
    }
    Span key {buffer.data(), cell.key_size};
    mem_copy(key, {cell.key, cell.local_size});
    key.advance(cell.local_size);

    CDB_TRY(m_overflow->read_chain(key, read_overflow_id(cell)));
    out = Slice {buffer}.truncate(cell.key_size);
    return Status::ok();
}

auto NodeIterator::index() const -> std::size_t
{
    return m_index;
}

auto NodeIterator::seek(const Slice &key, bool *found) -> Status
{
    Status s;
    const auto fetch = [&s, this](auto index) {
        Slice out;
        if (s.is_ok()) {
            s = fetch_key(*m_lhs_key, read_cell(*m_node, index), out);
        }
        return out;
    };

    const auto [index, exact] = seek_binary(
        m_node->header.cell_count, key, fetch);

    m_index = index;
    if (found != nullptr) {
        *found = exact;
    }
    return s;
}

auto NodeIterator::seek(const Cell &cell, bool *found) -> Status
{
    if (!cell.has_remote) {
        return seek({cell.key, cell.key_size});
    }
    Slice key;
    CDB_TRY(fetch_key(*m_rhs_key, cell, key));
    return seek(key, found);
}

[[nodiscard]] static auto is_overflowing(const Node &node) -> bool
{
    return node.overflow.has_value();
}

[[nodiscard]] static auto is_underflowing(const Node &node) -> bool
{
    return node.header.cell_count == 0;
}

BPlusTreeInternal::BPlusTreeInternal(BPlusTree &tree)
    : m_tree {&tree},
      m_pointers {&tree.m_pointers},
      m_overflow {&tree.m_overflow},
      m_payloads {&tree.m_payloads},
      m_freelist {&tree.m_freelist},
      m_pager {tree.m_pager}
{
}

auto BPlusTreeInternal::node_iterator(Node &node) const -> NodeIterator
{
    const NodeIterator::Parameters param {
        &m_tree->m_overflow,
        &m_tree->m_lhs_key,
        &m_tree->m_rhs_key,
    };
    return NodeIterator {node, param};
}

auto BPlusTreeInternal::is_pointer_map(Id pid) const -> bool
{
    return m_pointers->lookup(pid) == pid;
}

auto BPlusTreeInternal::find_external_slot(const Slice &key, SearchResult &out) const -> Status
{
    Node root;
    CDB_TRY(acquire(root, Id::root()));
    return find_external_slot(key, std::move(root), out);
}

auto BPlusTreeInternal::find_external_slot(const Slice &key, Node node, SearchResult &out) const -> Status
{
    for (;;) {
        bool exact;
        auto itr = node_iterator(node);
        CDB_TRY(itr.seek(key, &exact));

        if (node.header.is_external) {
            out.node = std::move(node);
            out.index = itr.index();
            out.exact = exact;
            return Status::ok();
        }
        const auto next_id = read_child_id(node, itr.index() + exact);
        CDB_EXPECT_NE(next_id, node.page.id()); // Infinite loop.
        release(std::move(node));
        CDB_TRY(acquire(node, next_id));
    }
}

auto BPlusTreeInternal::find_parent_id(Id pid, Id &out) const -> Status
{
    PointerMap::Entry entry;
    CDB_TRY(m_pointers->read_entry(pid, entry));
    out = entry.back_ptr;
    return Status::ok();
}

auto BPlusTreeInternal::fix_parent_id(Id pid, Id parent_id, PointerMap::Type type) -> Status
{
    PointerMap::Entry entry {parent_id, type};
    return m_pointers->write_entry(pid, entry);
}

auto BPlusTreeInternal::maybe_fix_overflow_chain(const Cell &cell, Id parent_id) -> Status
{
    if (cell.has_remote) {
        return fix_parent_id(read_overflow_id(cell), parent_id, PointerMap::OverflowHead);
    }
    return Status::ok();
}

auto BPlusTreeInternal::allocate_root(Node &out) -> Status
{
    CDB_EXPECT_EQ(m_pager->page_count(), 0);
    CDB_TRY(allocate(out, true));
    CDB_EXPECT_EQ(m_pager->page_count(), 1);
    return Status::ok();
}

auto BPlusTreeInternal::make_fresh_node(Node &out, bool is_external) const -> void
{
    out.header = NodeHeader {};
    out.header.is_external = is_external;
    out.scratch = m_tree->m_node_scratch.data();
    if (out.header.is_external) {
        out.meta = &m_tree->m_external_meta;
    } else {
        out.meta = &m_tree->m_internal_meta;
    }
    out.initialize();
}

auto BPlusTreeInternal::make_existing_node(Node &out) const -> void
{
    out.scratch = m_tree->m_node_scratch.data();
    out.header.read(out.page);
    if (out.header.is_external) {
        out.meta = &m_tree->m_external_meta;
    } else {
        out.meta = &m_tree->m_internal_meta;
    }
    out.initialize();
}

auto BPlusTreeInternal::allocate(Node &out, bool is_external) -> Status
{
    const auto fetch_unused_page = [this](Page &page) {
        if (m_freelist->is_empty()) {
            CDB_TRY(m_pager->allocate(page));
            // Since this is a fresh page from the end of the file, it could be a pointer map page. If so,
            // it is already blank, so just skip it and allocate another. It'll get filled in as the pages
            // following it are used.
            if (is_pointer_map(page.id())) {
                m_pager->release(std::move(page));
                CDB_TRY(m_pager->allocate(page));
            }
            return Status::ok();
        } else {
            return m_freelist->pop(page);
        }
    };
    CDB_TRY(fetch_unused_page(out.page));
    CDB_EXPECT_FALSE(is_pointer_map(out.page.id()));
    make_fresh_node(out, is_external);
    return Status::ok();
}

auto BPlusTreeInternal::acquire(Node &out, Id pid, bool needs_upgrade) const -> Status
{
    CDB_EXPECT_FALSE(is_pointer_map(pid));
    CDB_TRY(m_pager->acquire(pid, out.page));
    make_existing_node(out);

    if (needs_upgrade) {
        upgrade(out);
    }
    return Status::ok();
}

auto BPlusTreeInternal::upgrade(Node &node) const -> void
{
    m_pager->upgrade(node.page);

    // Ensure that the fragment count byte doesn't overflow. We have to account for the possible addition of
    // 2 fragments.
    if (node.header.frag_count + 6 >= 0xFF) {
        manual_defragment(node);
    }
}

auto BPlusTreeInternal::release(Node node) const -> void
{
    m_pager->release(std::move(node).take());
}

auto BPlusTreeInternal::destroy(Node node) -> Status
{
    // Pointer map pages should never be explicitly destroyed.
    CDB_EXPECT_FALSE(is_pointer_map(node.page.id()));
    return m_freelist->push(std::move(node.page));
}

auto BPlusTreeInternal::insert_cell(Node &node, std::size_t index, const Cell &cell) -> Status
{
    write_cell(node, index, cell);
    if (!node.header.is_external) {
        CDB_TRY(fix_parent_id(read_child_id(cell), node.page.id()));
    }
    return maybe_fix_overflow_chain(cell, node.page.id());
}

auto BPlusTreeInternal::remove_cell(Node &node, std::size_t index) -> Status
{
    const auto cell = read_cell(node, index);
    if (cell.has_remote) {
        CDB_TRY(m_overflow->erase_chain(read_overflow_id(cell)));
    }
    erase_cell(node, index, cell.size);
    return Status::ok();
}

auto BPlusTreeInternal::fix_links(Node &node) -> Status
{
    for (std::size_t index {}; index < node.header.cell_count; ++index) {
        const auto cell = read_cell(node, index);
        CDB_TRY(maybe_fix_overflow_chain(cell, node.page.id()));
        if (!node.header.is_external) {
            CDB_TRY(fix_parent_id(read_child_id(cell), node.page.id()));
        }
    }
    if (!node.header.is_external) {
        CDB_TRY(fix_parent_id(node.header.next_id, node.page.id()));
    }
    if (node.overflow) {
        CDB_TRY(maybe_fix_overflow_chain(*node.overflow, node.page.id()));
        if (!node.header.is_external) {
            CDB_TRY(fix_parent_id(read_child_id(*node.overflow), node.page.id()));
        }
    }
    return Status::ok();
}

auto BPlusTreeInternal::resolve_overflow(Node node) -> Status
{
    Node next;
    while (is_overflowing(node)) {
        if (node.page.id().is_root()) {
            CDB_TRY(split_root(std::move(node), next));
        } else {
            CDB_TRY(split_non_root(std::move(node), next));
        }
        node = std::move(next);
    }
    release(std::move(node));
    return Status::ok();
}

auto BPlusTreeInternal::split_root(Node root, Node &out) -> Status
{
    Node child;
    CDB_TRY(allocate(child, root.header.is_external));

    // Copy the cells.
    static constexpr auto after_root_headers = FileHeader::SIZE + NodeHeader::SIZE;
    auto data = child.page.span(after_root_headers, root.page.size() - after_root_headers);
    mem_copy(data, root.page.view(after_root_headers, data.size()));

    // Copy the header and cell pointers. Doesn't copy the page LSN.
    child.header = root.header;
    data = child.page.span(NodeHeader::SIZE, root.header.cell_count * sizeof(PageSize));
    mem_copy(data, root.page.view(after_root_headers, data.size()));

    CDB_EXPECT_TRUE(is_overflowing(root));
    std::swap(child.overflow, root.overflow);
    child.overflow_index = root.overflow_index;
    child.gap_size = root.gap_size + FileHeader::SIZE;

    root.header = NodeHeader {};
    root.header.is_external = false;
    root.header.next_id = child.page.id();
    root.initialize();

    CDB_TRY(fix_parent_id(child.page.id(), root.page.id()));
    release(std::move(root));

    CDB_TRY(fix_links(child));
    out = std::move(child);
    return Status::ok();
}

auto BPlusTreeInternal::transfer_left(Node &left, Node &right) -> Status
{
    CDB_EXPECT_EQ(left.header.is_external, right.header.is_external);
    const auto cell = read_cell(right, 0);
    CDB_TRY(insert_cell(left, left.header.cell_count, cell));
    CDB_EXPECT_FALSE(is_overflowing(left));
    erase_cell(right, 0, cell.size);
    return Status::ok();
}

auto BPlusTreeInternal::split_non_root(Node right, Node &out) -> Status
{
    CDB_EXPECT_FALSE(right.page.id().is_root());
    CDB_EXPECT_TRUE(is_overflowing(right));
    const auto &header = right.header;

    Id parent_id;
    CDB_TRY(find_parent_id(right.page.id(), parent_id));
    CDB_EXPECT_FALSE(parent_id.is_null());

    Node parent, left;
    CDB_TRY(acquire(parent, parent_id, true));
    CDB_TRY(allocate(left, header.is_external));

    const auto overflow_index = right.overflow_index;
    auto overflow = *right.overflow;
    right.overflow.reset();

    if (overflow_index == header.cell_count) {
        // Note the reversal of the "left" and "right" parameters. We are splitting the other way.
        return split_non_root_fast(
            std::move(parent),
            std::move(right),
            std::move(left),
            overflow,
            out);
    }

    /* Fix the overflow. The overflow cell should fit in either "left" or "right". This routine
     * works by transferring cells, one-by-one, from "right" to "left", and trying to insert the
     * overflow cell. Where the overflow cell is written depends on how many cells we have already
     * transferred. If "overflow_index" is 0, we definitely have enough room in "left". Otherwise,
     * we transfer a cell and try to write the overflow cell to "right". If this isn't possible,
     * then the left node must have enough room, since the maximum cell size is limited to roughly
     * 1/4 of a page. If "right" is more than 3/4 full, then "left" must be less than 1/4 full, so
     * it must be able to accept the overflow cell without overflowing.
     */
    for (std::size_t i {}, n = header.cell_count; i < n; ++i) {
        if (i == overflow_index) {
            CDB_TRY(insert_cell(left, left.header.cell_count, overflow));
            break;
        }
        CDB_TRY(transfer_left(left, right));

        if (usable_space(right) >= overflow.size + 2) {
            CDB_TRY(insert_cell(right, overflow_index - i - 1, overflow));
            break;
        }
        CDB_EXPECT_NE(i + 1, n);
    }
    CDB_EXPECT_FALSE(is_overflowing(left));
    CDB_EXPECT_FALSE(is_overflowing(right));

    auto separator = read_cell(right, 0);
    detach_cell(separator, m_tree->cell_scratch());

    if (header.is_external) {
        if (!header.prev_id.is_null()) {
            Node left_sibling;
            CDB_TRY(acquire(left_sibling, header.prev_id, true));
            left_sibling.header.next_id = left.page.id();
            left.header.prev_id = left_sibling.page.id();
            release(std::move(left_sibling));
        }
        right.header.prev_id = left.page.id();
        left.header.next_id = right.page.id();
        CDB_TRY(m_payloads->promote(nullptr, separator, parent_id));
    } else {
        left.header.next_id = read_child_id(separator);
        CDB_TRY(fix_parent_id(left.header.next_id, left.page.id()));
        erase_cell(right, 0);
    }

    auto itr = node_iterator(parent);
    CDB_TRY(itr.seek(separator));

    // Post the separator into the parent node. This call will fix the sibling's parent pointer.
    write_child_id(separator, left.page.id());
    CDB_TRY(insert_cell(parent, itr.index(), separator));

    release(std::move(left));
    release(std::move(right));
    out = std::move(parent);
    return Status::ok();
}

auto BPlusTreeInternal::split_non_root_fast(Node parent, Node left, Node right, const Cell &overflow, Node &out) -> Status
{
    const auto &header = left.header;
    CDB_TRY(insert_cell(right, 0, overflow));

    CDB_EXPECT_FALSE(is_overflowing(left));
    CDB_EXPECT_FALSE(is_overflowing(right));

    Cell separator;
    if (header.is_external) {
        if (!header.next_id.is_null()) {
            Node right_sibling;
            CDB_TRY(acquire(right_sibling, header.next_id, true));
            right_sibling.header.prev_id = right.page.id();
            right.header.next_id = right_sibling.page.id();
            release(std::move(right_sibling));
        }
        right.header.prev_id = left.page.id();
        left.header.next_id = right.page.id();

        separator = read_cell(right, 0);
        CDB_TRY(m_payloads->promote(m_tree->cell_scratch(), separator, parent.page.id()));
    } else {
        separator = read_cell(left, header.cell_count - 1);
        detach_cell(separator, m_tree->cell_scratch());
        erase_cell(left, header.cell_count - 1);

        right.header.next_id = left.header.next_id;
        left.header.next_id = read_child_id(separator);
        CDB_TRY(fix_parent_id(right.header.next_id, right.page.id()));
        CDB_TRY(fix_parent_id(left.header.next_id, left.page.id()));
    }

    auto itr = node_iterator(parent);
    CDB_TRY(itr.seek(separator));

    // Post the separator into the parent node. This call will fix the sibling's parent pointer.
    write_child_id(separator, left.page.id());
    CDB_TRY(insert_cell(parent, itr.index(), separator));

    const auto offset = !is_overflowing(parent);
    write_child_id(parent, itr.index() + offset, right.page.id());
    CDB_TRY(fix_parent_id(right.page.id(), parent.page.id()));

    release(std::move(left));
    release(std::move(right));
    out = std::move(parent);
    return Status::ok();
}

auto BPlusTreeInternal::resolve_underflow(Node node, const Slice &anchor) -> Status
{
    while (is_underflowing(node)) {
        if (node.page.id().is_root()) {
            return fix_root(std::move(node));
        }
        Id parent_id;
        CDB_TRY(find_parent_id(node.page.id(), parent_id));
        CDB_EXPECT_FALSE(parent_id.is_null());

        Node parent;
        CDB_TRY(acquire(parent, parent_id, true));
        // NOTE: Searching for the anchor key from the node we took from should always give us the correct index
        //       due to the B+-tree ordering rules.
        bool exact;
        auto itr = node_iterator(parent);
        CDB_TRY(itr.seek(anchor, &exact));
        CDB_TRY(fix_non_root(std::move(node), parent, itr.index() + exact));
        node = std::move(parent);
    }
    release(std::move(node));
    return Status::ok();
}

auto BPlusTreeInternal::internal_merge_left(Node &left, Node &right, Node &parent, std::size_t index) -> Status
{
    CDB_EXPECT_TRUE(is_underflowing(left));
    CDB_EXPECT_FALSE(left.header.is_external);
    CDB_EXPECT_FALSE(right.header.is_external);
    CDB_EXPECT_FALSE(parent.header.is_external);

    auto separator = read_cell(parent, index);
    write_cell(left, left.header.cell_count, separator);
    write_child_id(left, left.header.cell_count - 1, left.header.next_id);
    CDB_TRY(fix_parent_id(left.header.next_id, left.page.id()));
    CDB_TRY(maybe_fix_overflow_chain(separator, left.page.id()));
    erase_cell(parent, index, separator.size);

    while (right.header.cell_count) {
        CDB_TRY(transfer_left(left, right));
    }
    left.header.next_id = right.header.next_id;
    write_child_id(parent, index, left.page.id());
    return Status::ok();
}

auto BPlusTreeInternal::external_merge_left(Node &left, Node &right, Node &parent, std::size_t index) -> Status
{
    CDB_EXPECT_TRUE(is_underflowing(left));
    CDB_EXPECT_TRUE(left.header.is_external);
    CDB_EXPECT_TRUE(right.header.is_external);
    CDB_EXPECT_FALSE(parent.header.is_external);

    left.header.next_id = right.header.next_id;

//    const auto separator = read_cell(parent, index);
//    erase_cell(parent, index, separator.size);
    CDB_TRY(remove_cell(parent, index));

    while (right.header.cell_count) {
        CDB_TRY(transfer_left(left, right));
    }
    write_child_id(parent, index, left.page.id());

    if (!right.header.next_id.is_null()) {
        Node right_sibling;
        CDB_TRY(acquire(right_sibling, right.header.next_id, true));
        right_sibling.header.prev_id = left.page.id();
        release(std::move(right_sibling));
    }
    return Status::ok();
}

auto BPlusTreeInternal::merge_left(Node &left, Node right, Node &parent, std::size_t index) -> Status
{
    if (left.header.is_external) {
        CDB_TRY(external_merge_left(left, right, parent, index));
    } else {
        CDB_TRY(internal_merge_left(left, right, parent, index));
    }
    CDB_TRY(fix_links(left));
    return destroy(std::move(right));
}

auto BPlusTreeInternal::internal_merge_right(Node &left, Node &right, Node &parent, std::size_t index) -> Status
{
    CDB_EXPECT_TRUE(is_underflowing(right));
    CDB_EXPECT_FALSE(left.header.is_external);
    CDB_EXPECT_FALSE(right.header.is_external);
    CDB_EXPECT_FALSE(parent.header.is_external);

    auto separator = read_cell(parent, index);
    write_cell(left, left.header.cell_count, separator);
    write_child_id(left, left.header.cell_count - 1, left.header.next_id);
    CDB_TRY(fix_parent_id(left.header.next_id, left.page.id()));
    CDB_TRY(maybe_fix_overflow_chain(separator, left.page.id()));
    left.header.next_id = right.header.next_id;

    CDB_EXPECT_EQ(read_child_id(parent, index + 1), right.page.id());
    write_child_id(parent, index + 1, left.page.id());
    erase_cell(parent, index, separator.size);

    // Transfer the rest of the cells. left shouldn't overflow.
    while (right.header.cell_count) {
        CDB_TRY(transfer_left(left, right));
    }
    return Status::ok();
}

auto BPlusTreeInternal::external_merge_right(Node &left, Node &right, Node &parent, std::size_t index) -> Status
{
    CDB_EXPECT_TRUE(is_underflowing(right));
    CDB_EXPECT_TRUE(left.header.is_external);
    CDB_EXPECT_TRUE(right.header.is_external);
    CDB_EXPECT_FALSE(parent.header.is_external);

    left.header.next_id = right.header.next_id;
//    const auto separator = read_cell(parent, index);
    CDB_EXPECT_EQ(read_child_id(parent, index + 1), right.page.id());
    write_child_id(parent, index + 1, left.page.id());
//    erase_cell(parent, index, separator.size);
    CDB_TRY(remove_cell(parent, index));

    while (right.header.cell_count) {
        CDB_TRY(transfer_left(left, right));
    }
    if (!right.header.next_id.is_null()) {
        Node right_sibling;
        CDB_TRY(acquire(right_sibling, right.header.next_id, true));
        right_sibling.header.prev_id = left.page.id();
        release(std::move(right_sibling));
    }
    return Status::ok();
}

auto BPlusTreeInternal::merge_right(Node &left, Node right, Node &parent, std::size_t index) -> Status
{
    if (left.header.is_external) {
        CDB_TRY(external_merge_right(left, right, parent, index));
    } else {
        CDB_TRY(internal_merge_right(left, right, parent, index));
    }
    CDB_TRY(fix_links(left));
    return destroy(std::move(right));
}

auto BPlusTreeInternal::fix_non_root(Node node, Node &parent, std::size_t index) -> Status
{
    CDB_EXPECT_FALSE(node.page.id().is_root());
    CDB_EXPECT_TRUE(is_underflowing(node));
    CDB_EXPECT_FALSE(is_overflowing(parent));

    if (index > 0) {
        Node left;
        CDB_TRY(acquire(left, read_child_id(parent, index - 1), true));
        if (left.header.cell_count == 1) {
            CDB_TRY(merge_right(left, std::move(node), parent, index - 1));
            release(std::move(left));
            CDB_EXPECT_FALSE(is_overflowing(parent));
            return Status::ok();
        }
        CDB_TRY(rotate_right(parent, left, node, index - 1));
        release(std::move(left));
    } else {
        Node right;
        CDB_TRY(acquire(right, read_child_id(parent, index + 1), true));
        if (right.header.cell_count == 1) {
            CDB_TRY(merge_left(node, std::move(right), parent, index));
            release(std::move(node));
            CDB_EXPECT_FALSE(is_overflowing(parent));
            return Status::ok();
        }
        CDB_TRY(rotate_left(parent, node, right, index));
        release(std::move(right));
    }

    CDB_EXPECT_FALSE(is_overflowing(node));
    release(std::move(node));

    if (is_overflowing(parent)) {
        const auto saved_id = parent.page.id();
        CDB_TRY(resolve_overflow(std::move(parent)));
        CDB_TRY(acquire(parent, saved_id, true));
    }
    return Status::ok();
}

auto BPlusTreeInternal::fix_root(Node root) -> Status
{
    CDB_EXPECT_TRUE(root.page.id().is_root());

    // If the root is external here, the whole tree must be empty.
    if (!root.header.is_external) {
        Node child;
        CDB_TRY(acquire(child, root.header.next_id, true));

        // We don't have enough room to transfer the child contents into the root, due to the space occupied by
        // the file header. In this case, we'll just split the child and insert the median cell into the root.
        // Note that the child needs an overflow cell for the split routine to work. We'll just fake it by
        // extracting an arbitrary cell and making it the overflow cell.
        if (usable_space(child) < FileHeader::SIZE) {
            child.overflow_index = child.header.cell_count / 2;
            child.overflow = read_cell(child, child.overflow_index);
            detach_cell(*child.overflow, m_tree->cell_scratch());
            erase_cell(child, child.overflow_index);
            release(std::move(root));
            Node parent;
            CDB_TRY(split_non_root(std::move(child), parent));
            release(std::move(parent));
            CDB_TRY(acquire(root, Id::root(), true));
        } else {
            merge_root(root, child);
            CDB_TRY(destroy(std::move(child)));
        }
        CDB_TRY(fix_links(root));
    }
    release(std::move(root));
    return Status::ok();
}

auto BPlusTreeInternal::rotate_left(Node &parent, Node &left, Node &right, std::size_t index) -> Status
{
    if (left.header.is_external) {
        return external_rotate_left(parent, left, right, index);
    } else {
        return internal_rotate_left(parent, left, right, index);
    }
}

auto BPlusTreeInternal::external_rotate_left(Node &parent, Node &left, Node &right, std::size_t index) -> Status
{
    CDB_EXPECT_TRUE(left.header.is_external);
    CDB_EXPECT_TRUE(right.header.is_external);
    CDB_EXPECT_FALSE(parent.header.is_external);
    CDB_EXPECT_GT(parent.header.cell_count, 0);
    CDB_EXPECT_GT(right.header.cell_count, 1);

    auto lowest = read_cell(right, 0);
    CDB_TRY(insert_cell(left, left.header.cell_count, lowest));
    CDB_EXPECT_FALSE(is_overflowing(left));
    erase_cell(right, 0);

    auto separator = read_cell(right, 0);
    CDB_TRY(m_payloads->promote(m_tree->cell_scratch(), separator, parent.page.id()));
    write_child_id(separator, left.page.id());

    CDB_TRY(remove_cell(parent, index));
    return insert_cell(parent, index, separator);
}

auto BPlusTreeInternal::internal_rotate_left(Node &parent, Node &left, Node &right, std::size_t index) -> Status
{
    CDB_EXPECT_FALSE(parent.header.is_external);
    CDB_EXPECT_FALSE(left.header.is_external);
    CDB_EXPECT_FALSE(right.header.is_external);
    CDB_EXPECT_GT(parent.header.cell_count, 0);
    CDB_EXPECT_GT(right.header.cell_count, 1);

    Node child;
    CDB_TRY(acquire(child, read_child_id(right, 0), true));
    const auto saved_id = left.header.next_id;
    left.header.next_id = child.page.id();
    CDB_TRY(fix_parent_id(child.page.id(), left.page.id()));
    release(std::move(child));

    auto separator = read_cell(parent, index);
    CDB_TRY(insert_cell(left, left.header.cell_count, separator));
    CDB_EXPECT_FALSE(is_overflowing(left));
    write_child_id(left, left.header.cell_count - 1, saved_id);
    erase_cell(parent, index, separator.size);

    auto lowest = read_cell(right, 0);
    detach_cell(lowest, m_tree->cell_scratch());
    erase_cell(right, 0);
    write_child_id(lowest, left.page.id());
    return insert_cell(parent, index, lowest);
}

auto BPlusTreeInternal::rotate_right(Node &parent, Node &left, Node &right, std::size_t index) -> Status
{
    if (left.header.is_external) {
        return external_rotate_right(parent, left, right, index);
    } else {
        return internal_rotate_right(parent, left, right, index);
    }
}

auto BPlusTreeInternal::external_rotate_right(Node &parent, Node &left, Node &right, std::size_t index) -> Status
{
    CDB_EXPECT_TRUE(left.header.is_external);
    CDB_EXPECT_TRUE(right.header.is_external);
    CDB_EXPECT_FALSE(parent.header.is_external);
    CDB_EXPECT_GT(parent.header.cell_count, 0);
    CDB_EXPECT_GT(left.header.cell_count, 1);

    auto highest = read_cell(left, left.header.cell_count - 1);
    CDB_TRY(insert_cell(right, 0, highest));
    CDB_EXPECT_FALSE(is_overflowing(right));

    auto separator = highest;
    CDB_TRY(m_payloads->promote(m_tree->cell_scratch(), separator, parent.page.id()));
    write_child_id(separator, left.page.id());

    // Don't erase the cell until it has been detached.
    erase_cell(left, left.header.cell_count - 1);

    CDB_TRY(remove_cell(parent, index));
    CDB_TRY(insert_cell(parent, index, separator));
    return Status::ok();
}

auto BPlusTreeInternal::internal_rotate_right(Node &parent, Node &left, Node &right, std::size_t index) -> Status
{
    CDB_EXPECT_FALSE(parent.header.is_external);
    CDB_EXPECT_FALSE(left.header.is_external);
    CDB_EXPECT_FALSE(right.header.is_external);
    CDB_EXPECT_GT(parent.header.cell_count, 0);
    CDB_EXPECT_GT(left.header.cell_count, 1);

    Node child;
    CDB_TRY(acquire(child, left.header.next_id, true));
    const auto child_id = child.page.id();
    CDB_TRY(fix_parent_id(child.page.id(), right.page.id()));
    left.header.next_id = read_child_id(left, left.header.cell_count - 1);
    release(std::move(child));

    auto separator = read_cell(parent, index);
    CDB_TRY(insert_cell(right, 0, separator));
    CDB_EXPECT_FALSE(is_overflowing(right));
    write_child_id(right, 0, child_id);
    erase_cell(parent, index, separator.size);

    auto highest = read_cell(left, left.header.cell_count - 1);
    detach_cell(highest, m_tree->cell_scratch());
    write_child_id(highest, left.page.id());
    erase_cell(left, left.header.cell_count - 1, highest.size);
    CDB_TRY(insert_cell(parent, index, highest));
    return Status::ok();
}

PayloadManager::PayloadManager(const NodeMeta &meta, OverflowList &overflow)
    : m_meta {&meta},
      m_overflow {&overflow}
{
}

auto PayloadManager::emplace(char *scratch, Node &node, const Slice &key, const Slice &value, std::size_t index) -> Status
{
    CDB_EXPECT_TRUE(node.header.is_external);

    auto k = key.size();
    auto v = value.size();
    const auto local_size = compute_local_size(k, v, node.meta->min_local, node.meta->max_local);
    const auto has_remote = k + v > local_size;

    if (k > local_size) {
        k = local_size;
        v = 0;
    } else if (has_remote) {
        v = local_size - k;
    }

    CDB_EXPECT_EQ(k + v, local_size);
    auto total_size = local_size + varint_length(key.size()) + varint_length(value.size());

    Id overflow_id;
    if (has_remote) {
        CDB_TRY(m_overflow->write_chain(overflow_id, node.page.id(), key.range(k), value.range(v)));
        total_size += sizeof(overflow_id);
    }

    const auto emplace = [&](auto *out) {
        ::calicodb::emplace_cell(out, key.size(), value.size(), key.range(0, k), value.range(0, v), overflow_id);
    };

    if (const auto offset = allocate_block(node, static_cast<PageSize>(index), static_cast<PageSize>(total_size))) {
        // Write directly into the node.
        emplace(node.page.data() + offset);
    } else {
        // The node has overflowed. Write the cell to scratch memory.
        emplace(scratch);
        node.overflow = parse_external_cell(*node.meta, scratch);
        node.overflow->is_free = true;
    }
    return Status::ok();
}

auto PayloadManager::promote(char *scratch, Cell &cell, Id parent_id) -> Status
{
    detach_cell(cell, scratch);

    // "scratch" should have enough room before its "m_data" member to write the left child ID.
    const auto header_size = sizeof(Id) + varint_length(cell.key_size);
    cell.ptr = cell.key - header_size;
    cell.local_size = compute_local_size(cell.key_size, 0, m_meta->min_local, m_meta->max_local);
    cell.size = header_size + cell.local_size;
    cell.has_remote = false;

    if (cell.key_size > cell.local_size) {
        // Part of the key is on an overflow page. No value is stored locally in this case, so the local size computation is still correct.
        Id overflow_id;
        CDB_TRY(m_overflow->copy_chain(overflow_id, parent_id, read_overflow_id(cell), cell.key_size - cell.local_size));
        write_overflow_id(cell, overflow_id);
        cell.size += sizeof(Id);
        cell.has_remote = true;
    }
    return Status::ok();
}

auto PayloadManager::collect_key(std::string &scratch, const Cell &cell, Slice &out) const -> Status
{
    if (scratch.size() < cell.key_size) {
        scratch.resize(cell.key_size);
    }
    if (!cell.has_remote || cell.key_size <= cell.local_size) {
        mem_copy(scratch, {cell.key, cell.key_size});
        out = Slice {scratch.data(), cell.key_size};
        return Status::ok();
    }
    Span span {scratch};
    span.truncate(cell.key_size);
    mem_copy(span, {cell.key, cell.local_size});

    CDB_TRY(m_overflow->read_chain(span.range(cell.local_size), read_overflow_id(cell)));
    out = span.range(0, cell.key_size);
    return Status::ok();
}

auto PayloadManager::collect_value(std::string &scratch, const Cell &cell, Slice &out) const -> Status
{
    std::size_t value_size;
    decode_varint(cell.ptr, value_size);
    if (scratch.size() < value_size) {
        scratch.resize(value_size);
    }
    if (!cell.has_remote) {
        mem_copy(scratch, {cell.key + cell.key_size, value_size});
        out = Slice {scratch.data(), value_size};
        return Status::ok();
    }
    std::size_t remote_key_size {};
    if (cell.key_size > cell.local_size) {
        remote_key_size = cell.key_size - cell.local_size;
    }
    Span span {scratch};
    span.truncate(value_size);

    if (remote_key_size == 0) {
        const auto local_value_size = cell.local_size - cell.key_size;
        mem_copy(span, {cell.key + cell.key_size, local_value_size});
        span.advance(local_value_size);
    }

    CDB_TRY(m_overflow->read_chain(span, read_overflow_id(cell), remote_key_size));
    out = Span {scratch}.truncate(value_size);
    return Status::ok();
}

BPlusTree::BPlusTree(Pager &pager)
    : m_node_scratch(pager.page_size(), '\0'),
      m_cell_scratch(pager.page_size(), '\0'),
      m_external_meta {
          external_cell_size, parse_external_cell,
          compute_min_local(pager.page_size()),
          compute_max_local(pager.page_size())},
      m_internal_meta {
          internal_cell_size,
          parse_internal_cell,
          m_external_meta.min_local,
          m_external_meta.max_local},
      m_pointers {pager},
      m_freelist {pager, m_pointers},
      m_overflow {pager, m_freelist, m_pointers},
      m_payloads {m_external_meta, m_overflow},
      m_pager {&pager}
{
}

auto BPlusTree::cell_scratch() -> char *
{
    // Leave space for a child ID (maximum difference between the size of a varint and an Id).
    return m_cell_scratch.data() + sizeof(Id) - 1;
}

auto BPlusTree::insert(const Slice &key, const Slice &value, bool *exists) -> Status
{
    CDB_EXPECT_FALSE(key.is_empty());
    BPlusTreeInternal internal {*this};

    SearchResult slot;
    CDB_TRY(internal.find_external_slot(key, slot));
    auto [node, index, exact] = std::move(slot);
    internal.upgrade(node);

    if (exact) {
        CDB_TRY(internal.remove_cell(node, index));
    }

    CDB_TRY(m_payloads.emplace(cell_scratch(), node, key, value, index));
    CDB_TRY(internal.resolve_overflow(std::move(node)));
    if (exists != nullptr) {
        *exists = exact;
    }
    return Status::ok();
}

auto BPlusTree::erase(const Slice &key) -> Status
{
    BPlusTreeInternal internal {*this};
    SearchResult slot;

    CDB_TRY(internal.find_external_slot(key, slot));
    auto [node, index, exact] = std::move(slot);

    if (exact) {
        Slice anchor;
        const auto cell = read_cell(node, index);
        CDB_TRY(collect_key(m_anchor, cell, anchor));

        internal.upgrade(node);
        CDB_TRY(internal.remove_cell(node, index));
        return internal.resolve_underflow(std::move(node), anchor);
    }
    internal.release(std::move(node));
    return Status::not_found("not found");
}

auto BPlusTree::lowest(Node &out) -> Status
{
    BPlusTreeInternal internal {*this};
    CDB_TRY(internal.acquire(out, Id::root()));
    while (!out.header.is_external) {
        const auto next_id = read_child_id(out, 0);
        internal.release(std::move(out));
        CDB_TRY(internal.acquire(out, next_id));
    }
    return Status::ok();
}

auto BPlusTree::highest(Node &out) -> Status
{
    BPlusTreeInternal internal {*this};
    CDB_TRY(internal.acquire(out, Id::root()));
    while (!out.header.is_external) {
        const auto next_id = out.header.next_id;
        internal.release(std::move(out));
        CDB_TRY(internal.acquire(out, next_id));
    }
    return Status::ok();
}

auto BPlusTree::collect_key(std::string &scratch, const Cell &cell, Slice &out) const -> Status
{
    return m_payloads.collect_key(scratch, cell, out);
}

auto BPlusTree::collect_value(std::string &scratch, const Cell &cell, Slice &out) const -> Status
{
    return m_payloads.collect_value(scratch, cell, out);
}

auto BPlusTree::search(const Slice &key, SearchResult &out) -> Status
{
    return BPlusTreeInternal {*this}.find_external_slot(key, out);
}

auto BPlusTree::vacuum_step(Page &free, Id last_id) -> Status
{
    BPlusTreeInternal internal {*this};

    PointerMap::Entry entry;
    CDB_TRY(m_pointers.read_entry(last_id, entry));

    const auto fix_basic_link = [&entry, &free, this]() -> Status {
        Page parent;
        CDB_TRY(m_pager->acquire(entry.back_ptr, parent));
        m_pager->upgrade(parent);
        write_next_id(parent, free.id());
        m_pager->release(std::move(parent));
        return Status::ok();
    };

    switch (entry.type) {
    case PointerMap::FreelistLink: {
        if (last_id == free.id()) {

        } else if (last_id == m_freelist.m_head) {
            m_freelist.m_head = free.id();
        } else {
            // Back pointer points to another freelist page.
            CDB_EXPECT_FALSE(entry.back_ptr.is_null());
            CDB_TRY(fix_basic_link());
            Page last;
            CDB_TRY(m_pager->acquire(last_id, last));
            if (const auto next_id = read_next_id(last); !next_id.is_null()) {
                CDB_TRY(internal.fix_parent_id(next_id, free.id(), PointerMap::FreelistLink));
            }
            m_pager->release(std::move(last));
        }
        break;
    }
    case PointerMap::OverflowLink: {
        // Back pointer points to another overflow chain link, or the head of the chain.
        CDB_TRY(fix_basic_link());
        break;
    }
    case PointerMap::OverflowHead: {
        // Back pointer points to the node that the overflow chain is rooted in. Search through that nodes cells
        // for the target overflowing cell.
        Node parent;
        CDB_TRY(internal.acquire(parent, entry.back_ptr, true));
        bool found {};
        for (std::size_t i {}; i < parent.header.cell_count; ++i) {
            auto cell = read_cell(parent, i);
            found = cell.has_remote && read_overflow_id(cell) == last_id;
            if (found) {
                write_overflow_id(cell, free.id());
                break;
            }
        }
        CDB_EXPECT_TRUE(found);
        internal.release(std::move(parent));
        break;
    }
    case PointerMap::Node: {
        // Back pointer points to another node. Search through that node for the target child pointer.
        Node parent;
        CDB_TRY(internal.acquire(parent, entry.back_ptr, true));
        CDB_EXPECT_FALSE(parent.header.is_external);
        bool found {};
        for (std::size_t i {}; !found && i <= parent.header.cell_count; ++i) {
            const auto child_id = read_child_id(parent, i);
            found = child_id == last_id;
            if (found) {
                write_child_id(parent, i, free.id());
            }
        }
        CDB_EXPECT_TRUE(found);
        internal.release(std::move(parent));
        // Update references.
        Node last;
        CDB_TRY(internal.acquire(last, last_id, true));
        for (std::size_t i {}; i < last.header.cell_count; ++i) {
            const auto cell = read_cell(last, i);
            CDB_TRY(internal.maybe_fix_overflow_chain(cell, free.id()));
            if (!last.header.is_external) {
                CDB_TRY(internal.fix_parent_id(read_child_id(last, i), free.id()));
            }
        }
        if (!last.header.is_external) {
            CDB_TRY(internal.fix_parent_id(last.header.next_id, free.id()));
        } else {
            if (!last.header.prev_id.is_null()) {
                Node prev;
                CDB_TRY(internal.acquire(prev, last.header.prev_id, true));
                prev.header.next_id = free.id();
                internal.release(std::move(prev));
            }
            if (!last.header.next_id.is_null()) {
                Node next;
                CDB_TRY(internal.acquire(next, last.header.next_id, true));
                next.header.prev_id = free.id();
                internal.release(std::move(next));
            }
        }
        internal.release(std::move(last));
    }
    }
    CDB_TRY(m_pointers.write_entry(last_id, {}));
    CDB_TRY(m_pointers.write_entry(free.id(), entry));
    Page last;
    CDB_TRY(m_pager->acquire(last_id, last));
    // We need to upgrade the last node, even though we aren't writing to it. This causes a full image to be written,
    // which we will need if we crash during vacuum and need to roll back.
    m_pager->upgrade(last);
    if (entry.type != PointerMap::Node) {
        if (const auto next_id = read_next_id(last); !next_id.is_null()) {
            PointerMap::Entry next_entry;
            CDB_TRY(m_pointers.read_entry(next_id, next_entry));
            next_entry.back_ptr = free.id();
            CDB_TRY(m_pointers.write_entry(next_id, next_entry));
        }
    }
    mem_copy(free.span(sizeof(Lsn), free.size() - sizeof(Lsn)),
             last.view(sizeof(Lsn), last.size() - sizeof(Lsn)));
    m_pager->release(std::move(last));
    return Status::ok();
}

auto BPlusTree::vacuum_one(Id target, bool &success) -> Status
{
    BPlusTreeInternal internal {*this};

    if (internal.is_pointer_map(target)) {
        success = true;
        return Status::ok();
    }
    if (target.is_root() || m_freelist.is_empty()) {
        success = false;
        return Status::ok();
    }

    // Swap the head of the freelist with the last page in the file.
    Page head;
    CDB_TRY(m_freelist.pop(head));
    if (target != head.id()) {
        // Swap the last page with the freelist head.
        CDB_TRY(vacuum_step(head, target));
    } else {
        // TODO: May not really be necessary...
        CDB_TRY(internal.fix_parent_id(target, Id::null(), {}));
    }
    m_pager->release(std::move(head));
    success = true;
    return Status::ok();
}

auto BPlusTree::save_state(FileHeader &header) const -> void
{
    header.freelist_head = m_freelist.m_head;
}

auto BPlusTree::load_state(const FileHeader &header) -> void
{
    m_freelist.m_head = header.freelist_head;
}

#define CHECK_OK(expr)                                                                                                            \
    do {                                                                                                                          \
        if (const auto check_s = (expr); !check_s.is_ok()) {                                                                      \
            std::fprintf(stderr, "error: encountered %s status \"%s\"\n", get_status_name(check_s), check_s.to_string().c_str()); \
            std::abort();                                                                                                         \
        }                                                                                                                         \
    } while (0)

#define CHECK_TRUE(expr)                                              \
    do {                                                              \
        if (!(expr)) {                                                \
            std::fprintf(stderr, "error: \"%s\" was false\n", #expr); \
            std::abort();                                             \
        }                                                             \
    } while (0)

class BPlusTreeValidator
{
public:
    using Callback = std::function<void(Node &, std::size_t)>;
    using PageCallback = std::function<void(const Page &)>;

    explicit BPlusTreeValidator(BPlusTree &tree)
        : m_tree {&tree}
    {
    }

    struct PrintData {
        std::vector<std::string> levels;
        std::vector<std::size_t> spaces;
    };

    auto collect_levels(PrintData &data, Node node, std::size_t level) -> void
    {
        BPlusTreeInternal internal {*m_tree};
        const auto &header = node.header;
        ensure_level_exists(data, level);
        for (std::size_t cid {}; cid < header.cell_count; ++cid) {
            const auto is_first = cid == 0;
            const auto not_last = cid + 1 < header.cell_count;
            auto cell = read_cell(node, cid);

            if (!header.is_external) {
                Node next;
                CHECK_OK(internal.acquire(next, read_child_id(cell), false));
                collect_levels(data, std::move(next), level + 1);
            }

            if (is_first) {
                add_to_level(data, std::to_string(node.page.id().value) + ":[", level);
            }

            const auto key = Slice {cell.key, std::min<std::size_t>(3, cell.key_size)}.to_string();
            CHECK_TRUE(!key.empty());
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
            CHECK_OK(internal.acquire(next, node.header.next_id, false));
            collect_levels(data, std::move(next), level + 1);
        }

        internal.release(std::move(node));
    }

    auto traverse_inorder(const Callback &callback) -> void
    {
        BPlusTreeInternal internal {*m_tree};
        Node root;
        CHECK_OK(internal.acquire(root, Id::root(), false));
        traverse_inorder_helper(std::move(root), callback);
    }

    auto validate_freelist(Id head) -> void
    {
        BPlusTreeInternal internal {*m_tree};
        auto &pager = *m_tree->m_pager;
        auto &freelist = m_tree->m_freelist;
        if (freelist.is_empty()) {
            return;
        }
        CHECK_TRUE(!head.is_null());
        Page page;
        CHECK_OK(pager.acquire(head, page));

        Id parent_id;
        traverse_chain(std::move(page), [&](const auto &link) {
            Id found_id;
            CHECK_OK(internal.find_parent_id(link.id(), found_id));
            CHECK_TRUE(found_id == parent_id);
            parent_id = link.id();
        });
    }

    auto validate_overflow(Id overflow_id, Id parent_id, std::size_t overflow_size) -> void
    {
        BPlusTreeInternal internal {*m_tree};
        auto &pager = *m_tree->m_pager;

        Page page;
        CHECK_OK(pager.acquire(overflow_id, page));

        Id last_id;
        std::size_t n {};
        traverse_chain(std::move(page), [&](const auto &link) {
            Id found_id;
            CHECK_OK(internal.find_parent_id(link.id(), found_id));
            CHECK_TRUE(found_id == (n ? last_id : parent_id));
            n += pager.page_size() - sizeof(Lsn) - sizeof(Id);
            last_id = link.id();
        });
        CHECK_TRUE(n >= overflow_size);
    }

    auto validate_siblings() -> void
    {
        BPlusTreeInternal internal {*m_tree};

        const auto validate_possible_overflows = [this](auto &node) {
            for (std::size_t i {}; i < node.header.cell_count; ++i) {
                const auto cell = read_cell(node, i);
                if (cell.has_remote) {
                    std::size_t value_size;
                    decode_varint(cell.ptr, value_size);
                    const auto remote_size = cell.key_size + value_size - cell.local_size;
                    validate_overflow(read_overflow_id(cell), node.page.id(), remote_size);
                }
            }
        };

        // Find the leftmost external node.
        Node node;
        CHECK_OK(internal.acquire(node, Id::root(), false));
        while (!node.header.is_external) {
            const auto id = read_child_id(node, 0);
            internal.release(std::move(node));
            CHECK_OK(internal.acquire(node, id, false));
        }
        std::size_t i {}; // Traverse across the sibling chain to the right.
        while (!node.header.next_id.is_null()) {
            ++i;
            validate_possible_overflows(node);
            Node right;
            CHECK_OK(internal.acquire(right, node.header.next_id, false));
            std::string lhs_buffer, rhs_buffer;
            Slice lhs_key;
            CHECK_OK(m_tree->collect_key(lhs_buffer, read_cell(node, 0), lhs_key));
            Slice rhs_key;
            CHECK_OK(m_tree->collect_key(rhs_buffer, read_cell(right, 0), rhs_key));
            CHECK_TRUE(lhs_key < rhs_key);
            CHECK_TRUE(right.header.prev_id == node.page.id());
            internal.release(std::move(node));
            node = std::move(right);
        }
        validate_possible_overflows(node);
        internal.release(std::move(node));
    }

    auto validate_parent_child() -> void
    {
        BPlusTreeInternal internal {*m_tree};
        auto check = [&](auto &node, auto index) -> void {
            Node child;
            CHECK_OK(internal.acquire(child, read_child_id(node, index), false));

            Id parent_id;
            CHECK_OK(internal.find_parent_id(child.page.id(), parent_id));
            CHECK_TRUE(parent_id == node.page.id());

            internal.release(std::move(child));
        };
        traverse_inorder([f = std::move(check)](const auto &node, auto index) -> void {
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
    }

private:
    auto traverse_inorder_helper(Node node, const Callback &callback) -> void
    {
        BPlusTreeInternal internal {*m_tree};
        for (std::size_t index {}; index <= node.header.cell_count; ++index) {
            if (!node.header.is_external) {
                const auto saved_id = node.page.id();
                const auto next_id = read_child_id(node, index);

                // "node" must be released while we traverse, otherwise we are limited in how long of a traversal we can
                // perform by the number of pager frames.
                internal.release(std::move(node));

                Node next;
                CHECK_OK(internal.acquire(next, next_id, false));
                traverse_inorder_helper(std::move(next), callback);

                CHECK_OK(internal.acquire(node, saved_id, false));
            }
            if (index < node.header.cell_count) {
                callback(node, index);
            }
        }
        internal.release(std::move(node));
    }

    auto traverse_chain(Page page, const PageCallback &callback) -> void
    {
        for (; ; ) {
            callback(page);

            const auto next_id = read_next_id(page);
            m_tree->m_pager->release(std::move(page));
            if (next_id.is_null()) {
                break;
            }
            CHECK_OK(m_tree->m_pager->acquire(next_id, page));
        }
    }

    auto add_to_level(PrintData &data, const std::string &message, std::size_t target) -> void
    {
        // If target is equal to levels.size(), add spaces to all levels.
        CHECK_TRUE(target <= data.levels.size());
        std::size_t i {};

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

    auto ensure_level_exists(PrintData &data, std::size_t level) -> void
    {
        while (level >= data.levels.size()) {
            data.levels.emplace_back();
            data.spaces.emplace_back();
        }
        CHECK_TRUE(data.levels.size() > level);
        CHECK_TRUE(data.levels.size() == data.spaces.size());
    }

    BPlusTree *m_tree {};
};

auto BPlusTree::TEST_to_string() -> std::string
{
    std::string repr;
    BPlusTreeValidator::PrintData data;
    BPlusTreeValidator validator {*this};
    BPlusTreeInternal internal {*this};

    Node root;
    CHECK_OK(internal.acquire(root, Id::root()));
    validator.collect_levels(data, std::move(root), 0);
    for (const auto &level : data.levels) {
        repr.append(level + '\n');
//        repr.append(level.substr(0, 200) + '\n');
    }

    return repr;
}

auto BPlusTree::TEST_check_order() -> void
{
    BPlusTreeValidator validator {*this};
    std::string last_key;
    auto is_first = true;

    validator.traverse_inorder([&](auto &node, auto index) -> void {
        std::string buffer;
        Slice key;
        CHECK_OK(collect_key(buffer, read_cell(node, index), key));
        if (is_first) {
            is_first = false;
        } else {
            CHECK_TRUE(!key.is_empty());
            CHECK_TRUE(last_key <= key);
        }
        last_key = key.to_string();
    });
}

auto BPlusTree::TEST_check_links() -> void
{
    BPlusTreeValidator validator {*this};
    validator.validate_siblings();
    validator.validate_parent_child();
    validator.validate_freelist(m_freelist.m_head);
}

auto BPlusTree::TEST_check_nodes() -> void
{
    BPlusTreeValidator validator {*this};
    validator.traverse_inorder([](auto &node, auto index) -> void {
        // Only validate once per node.
        if (index == 0) {
            node.TEST_validate();
        }
    });
}

#undef CHECK_TRUE
#undef CHECK_OK

} // namespace calicodb