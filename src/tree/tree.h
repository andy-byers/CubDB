
#ifndef CALICO_TREE_TREE_H
#define CALICO_TREE_TREE_H

#include <spdlog/spdlog.h>
#include "node_pool.h"
#include "interface.h"
#include "utils/scratch.h"

namespace calico {

class Cell;
class IBufferPool;
class Node;

class Tree: public ITree {
public:
    struct Parameters {
        IBufferPool *buffer_pool {};
        spdlog::sink_ptr log_sink;
        PID free_start {};
        Size free_count {};
        Size cell_count {};
        Size node_count {};
    };
    
    explicit Tree(Parameters);
    ~Tree() override = default;

    [[nodiscard]] auto node_count() const -> Size override
    {
        return m_pool.node_count();
    }

    [[nodiscard]] auto cell_count() const -> Size override
    {
        return m_cell_count;
    }

    using Predicate = std::function<bool(const Node&, const Node::SearchResult&)>;

    [[nodiscard]] auto collect_value(const Node&, Index) const -> std::string override;
    auto find_root(bool) -> Node override;
    auto find(BytesView, const Predicate&, bool) -> Result;
    auto find_external(BytesView, bool) -> Result override;
    auto find_ge(BytesView, bool) -> Result override;
    auto find_local_min(Node) -> Position override;
    auto find_local_max(Node) -> Position override;
    auto save_header(FileHeader&) -> void override;
    auto load_header(const FileHeader&) -> void override;
    auto insert(BytesView, BytesView) -> bool override;
    auto remove(BytesView) -> bool override;
    auto allocate_node(PageType) -> Node override;
    auto acquire_node(PID, bool) -> Node override;
    auto destroy_node(Node) -> void override;
    auto make_cell(BytesView, BytesView, bool) -> Cell;

    struct NextStep {
        PID next_id;
        bool should_stop {};
    };
    using Stepper = std::function<NextStep(Node&)>;
    auto step(const Stepper&, bool) -> Node;
    auto find_for_remove(BytesView) -> void;


//    auto validate_children(const Node&, const Node&, const Node&, Index);

protected: // TODO
    auto positioned_insert(Position, BytesView, BytesView) -> void;
    auto positioned_modify(Position, BytesView) -> void;
    auto positioned_remove(Position) -> void;

    auto allocate_overflow_chain(BytesView) -> PID;
    auto destroy_overflow_chain(PID, Size) -> void;
    auto collect_overflow_chain(PID, Bytes) const -> void;

    auto balance_after_overflow(Node) -> void;
    auto split_non_root(Node) -> Node;
    auto split_root(Node) -> Node;


    auto rotate_left(Node&, Node&, Node&, Index) -> void;
    auto rotate_right(Node&, Node&, Node&, Index) -> void;
    auto external_rotate_left(Node&, Node&, Node&, Index) -> void;
    auto external_rotate_right(Node&, Node&, Node&, Index) -> void;
    auto internal_rotate_left(Node&, Node&, Node&, Index) -> void;
    auto internal_rotate_right(Node&, Node&, Node&, Index) -> void;

    auto maybe_balance_after_underflow(Node, BytesView) -> void;
    auto fix_non_root(Node, Node&, Index) -> bool;
    auto fix_root(Node) -> void;

    auto maybe_fix_child_parent_connections(Node &node) -> void;

    ScratchManager m_scratch;
    NodePool m_pool {};
    std::shared_ptr<spdlog::logger> m_logger;
    Size m_cell_count {};
};

} // calico

#endif // CALICO_TREE_TREE_H
