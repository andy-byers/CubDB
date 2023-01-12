#ifndef CALICO_PAGE_CELL_H
#define CALICO_PAGE_CELL_H

#include "page.h"
#include "utils/scratch.h"
#include <optional>

namespace Calico {

class Node;

class Cell {
public:
    struct Parameters {
        Slice key;
        Slice local_value;
        Id overflow_id;
        Size value_size {};
        Size page_size {};
        bool is_external {};
    };

    static auto read_at(Slice, Size, bool) -> Cell;
    static auto read_at(const Node &, Size) -> Cell;
    explicit Cell(const Parameters &);

    ~Cell() = default;
    [[nodiscard]] auto copy() const -> Cell;
    [[nodiscard]] auto key() const -> Slice;
    [[nodiscard]] auto size() const -> Size;
    [[nodiscard]] auto value_size() const -> Size;
    [[nodiscard]] auto overflow_size() const -> Size;
    [[nodiscard]] auto local_value() const -> Slice;
    [[nodiscard]] auto overflow_id() const -> Id;
    [[nodiscard]] auto left_child_id() const -> Id;
    auto set_is_external(bool) -> void;
    auto set_left_child_id(Id) -> void;
    auto set_overflow_id(Id) -> void;
    auto write(Bytes) const -> void;
    auto detach(Bytes, bool = false) -> void;

    [[nodiscard]]
    auto is_external() const -> bool
    {
        return m_is_external;
    }

    [[nodiscard]]
    auto is_attached() const -> bool
    {
        return m_is_attached;
    }

private:
    Cell() = default;

    Slice m_key;
    Slice m_local_value;
    Id m_left_child_id;
    Id m_overflow_id;
    Size m_value_size {};
    Size m_page_size {};
    bool m_is_external {};
    bool m_is_attached {true};
};

auto make_external_cell(Slice, Slice, Size) -> Cell;
auto make_internal_cell(Slice, Size) -> Cell;

} // namespace Calico

#endif // CALICO_PAGE_CELL_H