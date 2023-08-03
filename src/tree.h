// Copyright (c) 2022, The CalicoDB Authors. All rights reserved.
// This source code is licensed under the MIT License, which can be found in
// LICENSE.md. See AUTHORS.md for a list of contributor names.

#ifndef CALICODB_TREE_H
#define CALICODB_TREE_H

#include "calicodb/cursor.h"
#include "header.h"
#include "node.h"
#include "pager.h"

namespace calicodb
{

class Schema;
class Tree;
class TreeCursor;

[[nodiscard]] inline auto truncate_suffix(const Slice &lhs, const Slice &rhs, Slice &prefix_out) -> int
{
    const auto end = std::min(
        lhs.size(), rhs.size());

    size_t n = 0;
    for (; n < end; ++n) {
        const auto u = static_cast<uint8_t>(lhs[n]);
        const auto v = static_cast<uint8_t>(rhs[n]);
        if (u < v) {
            break;
        } else if (u > v) {
            return -1;
        }
    }
    if (n >= rhs.size()) {
        return -1;
    }
    // `lhs` < result <= `rhs`
    prefix_out = rhs.range(0, n + 1);
    return 0;
}

class Tree final
{
public:
    static constexpr size_t kRequiredBufferSize = 3 * kPageSize;

    ~Tree();
    auto release_nodes() const -> void;

    explicit Tree(Pager &pager, Stat &stat, char *scratch, const Id *root_id);
    static auto create(Pager &pager, Id *out) -> Status;
    static auto destroy(Tree &tree) -> Status;
    static auto get_tree(Cursor &c) -> Tree *;

    auto new_cursor() -> Cursor *;
    auto put(const Slice &key, const Slice &value) -> Status;
    auto get(const Slice &key, std::string *value) const -> Status;
    auto erase(const Slice &key) -> Status;
    auto vacuum(Schema &schema) -> Status;

    auto put(Cursor &c, const Slice &key, const Slice &value) -> Status;
    auto erase(Cursor &c) -> Status;

    auto allocate(bool is_external, Node &node_out) -> Status
    {
        PageRef *ref;
        auto s = m_pager->allocate(ref);
        if (s.is_ok()) {
            if (ref->refs == 1) {
                CALICODB_EXPECT_FALSE(PointerMap::is_map(ref->page_id));
                node_out = Node::from_new_page(*ref, m_node_scratch, is_external);
            } else {
                m_pager->release(ref);
                s = corrupted_node(ref->page_id);
            }
        }
        return s;
    }

    auto acquire(Id page_id, Node &node_out, bool write = false) const -> Status
    {
        PageRef *ref;
        auto s = m_pager->acquire(page_id, ref);
        if (s.is_ok()) {
            if (Node::from_existing_page(*ref, m_node_scratch, node_out)) {
                m_pager->release(ref);
                return corrupted_node(page_id);
            }
            if (write) {
                upgrade(node_out);
            }
        }
        return s;
    }

    auto free(Node node) -> Status
    {
        return m_pager->destroy(node.ref);
    }

    auto upgrade(Node &node) const -> void
    {
        m_pager->mark_dirty(*node.ref);
    }

    auto release(Node node, Pager::ReleaseAction action = Pager::kKeep) const -> void
    {
        m_pager->release(node.ref, action);
    }

    [[nodiscard]] auto root() const -> Id
    {
        return m_root_id ? *m_root_id : Id::root();
    }

    auto print_structure(std::string &repr_out) const -> Status;
    auto print_nodes(std::string &repr_out) const -> Status;

    auto TEST_validate() const -> void;

private:
    friend class DBImpl;
    friend class InorderTraversal;
    friend class Schema;
    friend class SchemaCursor;
    friend class TreeCursor;
    friend class TreePrinter;
    friend class TreeValidator;
    friend class UserCursor;

    auto put(TreeCursor &c, const Slice &key, const Slice &value) -> Status;
    auto erase(TreeCursor &c) -> Status;

    auto corrupted_node(Id page_id) const -> Status;

    auto redistribute_cells(Node &left, Node &right, Node &parent, uint32_t pivot_idx) -> Status;
    auto resolve_overflow(TreeCursor &c) -> Status;
    auto split_root(TreeCursor &c) -> Status;
    auto split_nonroot(TreeCursor &c) -> Status;
    auto split_nonroot_fast(TreeCursor &c, Node &parent, Node right) -> Status;
    auto resolve_underflow(TreeCursor &c) -> Status;
    auto fix_root(TreeCursor &c) -> Status;
    auto fix_nonroot(TreeCursor &c, Node &parent, uint32_t index) -> Status;

    auto read_key(const Cell &cell, std::string &scratch, Slice *key_out, uint32_t limit = 0) const -> Status;
    auto read_value(const Cell &cell, std::string &scratch, Slice *value_out) const -> Status;
    auto read_key(Node &node, uint32_t index, std::string &scratch, Slice *key_out, uint32_t limit = 0) const -> Status;
    auto read_value(Node &node, uint32_t index, std::string &scratch, Slice *value_out) const -> Status;
    auto write_key(Node &node, uint32_t index, const Slice &key) -> Status;
    auto write_value(Node &node, uint32_t index, const Slice &value) -> Status;

    auto emplace(Node &node, const Slice &key, const Slice &value, uint32_t index, bool &overflow) -> Status;
    auto free_overflow(Id head_id) -> Status;
    auto vacuum_step(PageRef *&free, PointerMap::Entry entry, Schema &schema, Id last_id) -> Status;

    struct PivotOptions {
        const Cell *cells[2];
        char *scratch;
        Id parent_id;
    };
    auto make_pivot(const PivotOptions &opt, Cell &pivot_out) -> Status;
    auto post_pivot(Node &node, uint32_t idx, Cell &cell, Id child_id) -> Status;
    auto insert_cell(Node &node, uint32_t idx, const Cell &cell) -> Status;
    auto remove_cell(Node &node, uint32_t idx) -> Status;
    auto find_parent_id(Id page_id, Id &out) const -> Status;
    auto fix_parent_id(Id page_id, Id parent_id, PointerMap::Type type, Status &s) -> void;
    auto maybe_fix_overflow_chain(const Cell &cell, Id parent_id, Status &s) -> void;
    auto fix_links(Node &node, Id parent_id = Id::null()) -> Status;

    // Internal cursor used to traverse the tree structure
    TreeCursor *const m_cursor;

    auto use_cursor(Cursor *c) const -> void;
    mutable Cursor *m_last_c = nullptr;

    // Various tree operation counts are tracked in this variable.
    Stat *m_stat;

    // When the node pointed at by m_c overflows, store the cell that couldn't fit on the page here. The
    // location that the overflow cell should be placed is copied into the pid and idx fields from m_c.
    // The overflow cell is usually backed by one of the cell scratch buffers.
    struct {
        // Return true if an overflow cell exists, false otherwise
        [[nodiscard]] auto exists() const -> bool
        {
            return !pid.is_null();
        }

        // Discard the overflow cell
        auto clear() -> void
        {
            pid = Id::null();
        }

        Cell cell;
        Id pid;
        uint32_t idx;
    } m_ovfl;

    // Scratch memory for manipulating nodes.
    char *const m_node_scratch;
    char *const m_split_scratch;

    // Scratch memory for cells that aren't embedded in nodes. Use m_cell_scratch[n] to get a pointer to
    // the start of cell scratch buffer n, where n < kNumCellBuffers.
    static constexpr size_t kNumCellBuffers = 4;
    static constexpr auto kCellBufferLen = kPageSize / kNumCellBuffers;
    char *const m_cell_scratch[kNumCellBuffers];

    Pager *const m_pager;
    const Id *const m_root_id;
};

} // namespace calicodb

#endif // CALICODB_TREE_H
