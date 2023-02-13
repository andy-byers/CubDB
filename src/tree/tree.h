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

class BPlusTree {
    friend class BPlusTreeInternal;
    friend class BPlusTreeValidator;
    friend class CursorInternal;

    std::array<std::string, 4> m_scratch;

    NodeMeta m_external_meta;
    NodeMeta m_internal_meta;
    FreeList m_freelist;
    PointerMap m_pointers;

    Pager *m_pager {};

    [[nodiscard]] auto make_existing_node(Page page) -> Node;
    [[nodiscard]] auto make_fresh_node(Page page, bool is_external) -> Node;
    [[nodiscard]] auto scratch(Size index) -> Byte *;
    [[nodiscard]] auto allocate(bool is_external) -> tl::expected<Node, Status>;
    [[nodiscard]] auto acquire(Id pid, bool upgrade = false) -> tl::expected<Node, Status>;
    [[nodiscard]] auto lowest() -> tl::expected<Node, Status>;
    [[nodiscard]] auto highest() -> tl::expected<Node, Status>;
    auto release(Node node) const -> void;
    auto destroy(Node node) -> void;
public:

    explicit BPlusTree(Pager &pager);

    [[nodiscard]] auto setup() -> tl::expected<Node, Status>;
    [[nodiscard]] auto collect(Node node, Size index) -> tl::expected<std::string, Status>;
    [[nodiscard]] auto search(const Slice &key) -> tl::expected<SearchResult, Status>;
    [[nodiscard]] auto insert(const Slice &key, const Slice &value) -> tl::expected<bool, Status>;
    [[nodiscard]] auto erase(const Slice &key) -> tl::expected<void, Status>;

    auto save_state(FileHeader &header) const -> void;
    auto load_state(const FileHeader &header) -> void;

    auto TEST_to_string() -> std::string;
    auto TEST_check_order() -> void;
    auto TEST_check_links() -> void;
    auto TEST_check_nodes() -> void;
};

} // namespace Calico

#endif // CALICO_TREE_H
