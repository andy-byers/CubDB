#ifndef CUB_PAGE_NODE_H
#define CUB_PAGE_NODE_H

#include "cell.h"
#include "common.h"
#include "page.h"
#include "utils/slice.h"

namespace cub {

class NodeHeader {
public:
    NodeHeader(PID, MutBytes);
    [[nodiscard]] auto parent_id() const -> PID;
    [[nodiscard]] auto right_sibling_id() const -> PID;
    [[nodiscard]] auto rightmost_child_id() const -> PID;
    [[nodiscard]] auto cell_count() const -> Size;
    [[nodiscard]] auto free_count() const -> Size;
    [[nodiscard]] auto cell_start() const -> Index;
    [[nodiscard]] auto free_start() const -> Index;
    [[nodiscard]] auto frag_count() const -> Size;
    auto set_parent_id(PID) -> void;
    auto set_right_sibling_id(PID) -> void;
    auto set_rightmost_child_id(PID) -> void;
    auto set_cell_count(Size) -> void;
    auto set_free_count(Size) -> void;
    auto set_cell_start(Index) -> void;
    auto set_free_start(Index) -> void;
    auto set_frag_count(Size) -> void;

private:
    MutBytes m_header;
};

class Node final {
public:
    struct SearchResult {
        Index index{};
        bool found_eq{};
    };
    Node(Page, bool);
    ~Node() = default;

    auto take() -> Page {return std::move(m_page);}

    [[nodiscard]] auto page() const -> const Page&
    {
        return m_page;
    }

    auto page() -> Page&
    {
        return m_page;
    }

    auto read_key(Index) const -> RefBytes;
    auto read_cell(Index) const -> Cell;
    auto detach_cell(Index, Scratch) const -> Cell;
    auto extract_cell(Index, Scratch) -> Cell;
    auto find_ge(RefBytes) const -> SearchResult;
    auto insert(Cell) -> void;
    auto insert_at(Index, Cell) -> void;
    auto remove(RefBytes) -> bool;
    auto remove_at(Index, Size) -> void;
    auto defragment() -> void;

    auto overflow_cell() const -> const Cell&;
    auto set_overflow_cell(Cell) -> void;
    auto take_overflow_cell() -> Cell;

    auto is_overflowing() const -> bool;
    auto is_underflowing() const -> bool;
    auto is_external() const -> bool;

    // Public header fields.
    auto cell_count() const -> Size;
    auto parent_id() const -> PID;
    auto right_sibling_id() const -> PID;
    auto child_id(Index) const -> PID;
    auto rightmost_child_id() const -> PID;
    auto set_parent_id(PID) -> void;
    auto set_right_sibling_id(PID) -> void;
    auto set_rightmost_child_id(PID) -> void;
    auto set_child_id(Index, PID) -> void;

    auto reset(bool = false) -> void;
    auto usable_space() const -> Size;
    auto cell_area_offset() const -> Size;
    auto cell_pointers_offset() const -> Size;
    auto header_offset() const -> Index;

    Node(Node&&) = default;
    auto operator=(Node&&) -> Node& = default;

private:
    auto recompute_usable_space() -> void;
    auto gap_size() const -> Size;
    auto cell_pointer(Index) const -> Index;
    auto set_cell_pointer(Index, Index) -> void;
    auto insert_cell_pointer(Index, Index) -> void;
    auto remove_cell_pointer(Index) -> void;
    auto defragment(std::optional<Index>) -> void;
    auto allocate(Size, std::optional<Index>) -> Index;
    auto allocate_from_gap(Size) -> Index;
    auto allocate_from_free(Size) -> Index;
    auto take_free_space(Index, Index, Size) -> Index;
    auto give_free_space(Index, Size) -> void;

    NodeHeader m_header;
    Page m_page;
    std::optional<Cell> m_overflow{};
    Size m_usable_space{};
};

} // cub

#endif // CUB_PAGE_NODE_H
