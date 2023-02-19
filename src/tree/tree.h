#ifndef CALICO_TREE_H
#define CALICO_TREE_H

#include <array>
#include "memory.h"
#include "node.h"

namespace Calico {

struct FileHeader;
class BPlusTree;

struct SearchResult {
    Node node;
    Size index {};
    bool exact {};
};

// TODO: This implementation takes a shortcut and reads fragmented keys into a temporary buffer.
//       This isn't necessary: we could iterate through, page by page, and compare bytes as we encounter
//       them. It's just a bit more complicated.
class NodeIterator {
    mutable Status *m_status {};
    OverflowList *m_overflow {};
    std::string *m_lhs_key {};
    std::string *m_rhs_key {};
    Node *m_node {};
    Size m_index {};

    [[nodiscard]] auto fetch_key(std::string &buffer, const Cell &cell) const -> Slice;

public:
    struct Parameters {
        OverflowList *overflow {};
        std::string *lhs_key {};
        std::string *rhs_key {};
        Status *status {};
    };
    explicit NodeIterator(Node &node, const Parameters &param);
    [[nodiscard]] auto is_valid() const -> bool;
    [[nodiscard]] auto index() const -> Size;
    auto seek(const Slice &key) -> bool;
    auto seek(const Cell &cell) -> bool;
};

class PayloadManager {
    const NodeMeta *m_meta {};
    OverflowList *m_overflow {};
    std::string m_scratch {}; // TODO: share

public:
    explicit PayloadManager(const NodeMeta &meta, OverflowList &overflow);

    /* Create a cell in an external node. If the node overflows, the cell will be created in scratch
     * memory and set as the node's overflow cell. May allocate an overflow chain with its back pointer
     * pointing to "node".
     */
    [[nodiscard]] auto emplace(Node &node, const Slice &key, const Slice &value, Size index) -> tl::expected<void, Status>;

    /* Prepare a cell read from an external node to be posted into its parent as a separator. Will
     * allocate a new overflow chain for overflowing keys, pointing back to "parent_id".
     */
    [[nodiscard]] auto promote(Cell &cell, Id parent_id) -> tl::expected<void, Status>;

    [[nodiscard]] auto collect_key(const Cell &cell) -> tl::expected<std::string, Status>;
    [[nodiscard]] auto collect_value(const Cell &cell) -> tl::expected<std::string, Status>;
};

class BPlusTree {
    friend class BPlusTreeInternal;
    friend class BPlusTreeValidator;
    friend class CursorInternal;

    Status m_status;
    std::array<std::string, 4> m_scratch;
    std::string m_lhs_key;
    std::string m_rhs_key;

    NodeMeta m_external_meta;
    NodeMeta m_internal_meta;
    PointerMap m_pointers;
    FreeList m_freelist;
    OverflowList m_overflow;
    PayloadManager m_payloads;

    Pager *m_pager {};

    [[nodiscard]] auto vacuum_step(Page &head, Id last_id) -> tl::expected<void, Status>;
    [[nodiscard]] auto write_external_cell(Node &node, Size index, const Cell &cell) -> tl::expected<void, Status>;
    [[nodiscard]] auto make_existing_node(Page page) -> Node;
    [[nodiscard]] auto make_fresh_node(Page page, bool is_external) -> Node;
    [[nodiscard]] auto scratch(Size index) -> Byte *;
    [[nodiscard]] auto allocate(bool is_external) -> tl::expected<Node, Status>;
    [[nodiscard]] auto acquire(Id pid, bool upgrade = false) -> tl::expected<Node, Status>;
    [[nodiscard]] auto lowest() -> tl::expected<Node, Status>;
    [[nodiscard]] auto highest() -> tl::expected<Node, Status>;
    [[nodiscard]] auto destroy(Node node) -> tl::expected<void, Status>;
    auto release(Node node) const -> void;

public:
    explicit BPlusTree(Pager &pager);

    [[nodiscard]] auto setup() -> tl::expected<Node, Status>;
    [[nodiscard]] auto collect_key(const Cell &cell) -> tl::expected<std::string, Status>;
    [[nodiscard]] auto collect_value(const Cell &cell) -> tl::expected<std::string, Status>;
    [[nodiscard]] auto search(const Slice &key) -> tl::expected<SearchResult, Status>;
    [[nodiscard]] auto insert(const Slice &key, const Slice &value) -> tl::expected<bool, Status>;
    [[nodiscard]] auto erase(const Slice &key) -> tl::expected<void, Status>;
    [[nodiscard]] auto vacuum_one(Id target) -> tl::expected<bool, Status>;

    auto save_state(FileHeader &header) const -> void;
    auto load_state(const FileHeader &header) -> void;

    struct Components {
        FreeList *freelist {};
        OverflowList *overflow {};
        PointerMap *pointers {};
    };

    auto TEST_to_string() -> std::string;
    auto TEST_components() -> Components;
    auto TEST_check_order() -> void;
    auto TEST_check_links() -> void;
    auto TEST_check_nodes() -> void;
};

} // namespace Calico

#endif // CALICO_TREE_H
