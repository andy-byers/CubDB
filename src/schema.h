// Copyright (c) 2022, The CalicoDB Authors. All rights reserved.
// This source code is licensed under the MIT License, which can be found in
// LICENSE.md. See AUTHORS.md for a list of contributor names.

#ifndef CALICODB_SCHEMA_H
#define CALICODB_SCHEMA_H

#include "calicodb/db.h"
#include "page.h"
#include <unordered_map>

namespace calicodb
{

class TableImpl;
class Tree;

// Representation of the database schema
class Schema final
{
public:
    explicit Schema(Pager &pager);
    ~Schema();

    [[nodiscard]] auto new_table(const TableOptions &options, const std::string &name, Table *&out) -> Status;
    [[nodiscard]] auto drop_table(const std::string &name) -> Status;

    [[nodiscard]] auto vacuum_page(Id page_id, bool &success) -> Status;

    // Write updated root page IDs for tables that were closed during vacuum, if any
    // tables were rerooted
    [[nodiscard]] auto vacuum_finish() -> Status;

    auto inform_live_cursors() -> void;

    auto TEST_validate() const -> void;

private:
    template <class T>
    using HashMap = std::unordered_map<Id, T, Id::Hash>;

    // Change the root page of a table from "old_id" to "new_id" during vacuum
    friend class Tree;
    auto vacuum_reroot(Id old_id, Id new_id) -> void;

    struct RootedTree {
        Tree *tree = nullptr;
        Id root;
    };

    HashMap<RootedTree> m_trees;
    HashMap<Id> m_reroot;
    Pager *m_pager;
    Tree *m_map;
};

} // namespace calicodb

#endif // CALICODB_SCHEMA_H