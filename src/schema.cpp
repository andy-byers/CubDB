// Copyright (c) 2022, The CalicoDB Authors. All rights reserved.
// This source code is licensed under the MIT License, which can be found in
// LICENSE.md. See AUTHORS.md for a list of contributor names.

#include "schema.h"
#include "encoding.h"
#include "logging.h"
#include "scope_guard.h"
#include "table_impl.h"
#include "tree.h"

namespace calicodb
{

Schema::Schema(Pager &pager)
    : m_pager(&pager),
      m_map(new Tree(pager, nullptr))
{
}

Schema::~Schema()
{
    delete m_map;
}

auto Schema::new_table(const TableOptions &options, const std::string &name, Table *&out) -> Status
{
    std::string value;
    auto s = m_map->get(name, &value);

    Id root_id;
    if (s.is_ok()) {
        if (options.error_if_exists) {
            return Status::invalid_argument("table \"" + name + "\" already exists");
        }
        if (value.size() != Id::kSize) {
            std::string message("root entry for table \"" + name + "\" is corrupted: ");
            message.append(escape_string(value));
            return Status::corruption(message);
        }
        root_id.value = get_u32(value);
    } else if (s.is_not_found()) {
        if (!options.create_if_missing) {
            return Status::invalid_argument("table \"" + name + "\" does not exist");
        }
        CALICODB_TRY(Tree::create(*m_pager, false, &root_id));
        value.resize(Id::kSize);
        put_u32(value.data(), root_id.value);
        CALICODB_TRY(m_map->put(name, value));
    }
    auto itr = m_trees.find(root_id);
    if (itr == end(m_trees) || itr->second.tree == nullptr) {
        itr = m_trees.insert(itr, {root_id, {}});
        itr->second.root = root_id;
        itr->second.tree = new Tree(*m_pager, &itr->second.root);
    }
    out = new TableImpl(itr->second.tree);
    return Status::ok();
}

auto Schema::drop_table(const std::string &name) -> Status
{
    std::string value;
    CALICODB_TRY(m_map->get(name, &value));
    if (value.size() != Id::kSize) {
        std::string message("table \"" + name + "\" has a corrupted root ID: ");
        append_escaped_string(message, value);
        return Status::corruption(message);
    }

    Id root_id(get_u32(value));
    auto itr = m_trees.find(root_id);
    if (itr != end(m_trees) && itr->second.tree) {
        return Status::invalid_argument(
            "table \"" + name + "\" is still open");
    }
    Tree drop(*m_pager, &root_id);
    CALICODB_TRY(Tree::destroy(drop));
    CALICODB_TRY(m_map->erase(name));
    m_trees.erase(root_id);
    return Status::ok();
}

auto Schema::vacuum_reroot(Id old_id, Id new_id) -> void
{
    auto tree = m_trees.find(old_id);
    if (tree == end(m_trees)) {
        RootedTree rooted;
        rooted.root = old_id;
        m_trees.insert(tree, {old_id, rooted});
    }
    // Rekey the tree. Leave the root ID stored in the tree state set
    // to the original root.
    auto node = m_trees.extract(old_id);
    CALICODB_EXPECT_FALSE(!node);
    node.key() = new_id;
    const auto root_id = node.mapped().root;
    m_trees.insert(std::move(node));

    // Map the original root ID to the newest root ID.
    m_reroot.insert_or_assign(root_id, new_id);
}

auto Schema::vacuum_finish() -> Status
{
    auto *cursor = CursorInternal::make_cursor(*m_map);
    cursor->seek_first();
    ScopeGuard guard = [cursor] {
        delete cursor;
    };

    Status s;
    while (cursor->is_valid()) {
        if (cursor->value().size() != sizeof(U32)) {
            std::string message("root entry for table \"");
            message.append(cursor->key().to_string());
            message.append("\" is corrupted: ");
            message.append(escape_string(cursor->value()));
            return Status::corruption(message);
        }
        const Id old_id(get_u32(cursor->value()));
        const auto root = m_reroot.find(old_id);
        if (root != end(m_reroot)) {
            char buffer[sizeof(U32)];
            // Update the database schema with the new root page ID for this tree.
            put_u32(buffer, root->second.value);
            const Slice value(buffer, sizeof(U32));
            s = m_map->put(cursor->key(), value);
            if (!s.is_ok()) {
                break;
            }
            // Update the in-memory root stored by each Tree.
            auto tree = m_trees.find(root->second);
            CALICODB_EXPECT_NE(tree, end(m_trees));
            if (tree->second.tree) {
                tree->second.root = root->second;
            } else {
                // This tree is not actually open. The RootedTree entry exists
                // so that vacuum_reroot() could find the original root ID (or
                // the table was just closed by the user, either way, the entry
                // should be removed).
                m_trees.erase(tree);
            }
            m_reroot.erase(root);
        }
        cursor->next();
    }
    const auto success = m_reroot.empty();
    m_reroot.clear();

    if (s.is_ok() && !success) {
        std::string message("missing ");
        append_number(message, m_reroot.size());
        message.append(" root entries");
        return Status::corruption(message);
    }
    inform_live_cursors();
    return s;
}

auto Schema::vacuum_page(Id page_id, bool &success) -> Status
{
    return m_map->vacuum_one(page_id, *this, &success);
}

auto Schema::inform_live_cursors() -> void
{
    for (auto &[_, tree] : m_trees) {
        if (tree.tree) {
            tree.tree->inform_cursors();
        }
    }
}

auto Schema::TEST_validate() const -> void
{
    for (const auto &[_, tree] : m_trees) {
        if (tree.tree) {
            tree.tree->TEST_validate();

            // Make sure the last vacuum didn't miss any roots.
            CALICODB_EXPECT_EQ(tree.root, tree.tree->root());
        }
    }
}

} // namespace calicodb