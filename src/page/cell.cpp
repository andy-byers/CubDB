#include "cell.h"

#include "node.h"
#include "utils/encoding.h"
#include "utils/expect.h"
#include "utils/layout.h"

namespace Calico {

auto Cell::read_at(BytesView in, Size page_size, bool is_external) -> Cell
{
    Cell cell;
    cell.m_page_size = page_size;

    if (!is_external) {
        cell.m_left_child_id.value = get_u64(in);
        in.advance(PAGE_ID_SIZE);
    }
    const auto key_size = get_u16(in);
    in.advance(sizeof(std::uint16_t));

    if (is_external) {
        cell.m_value_size = get_u32(in);
        in.advance(sizeof(std::uint32_t));
    }

    cell.m_key = in;
    cell.m_key.truncate(key_size);

    if (is_external) {
        in.advance(cell.m_key.size());
        const auto local_value_size = get_local_value_size(key_size, cell.m_value_size, page_size);
        cell.m_local_value = in;
        cell.m_local_value.truncate(local_value_size);

        if (local_value_size < cell.m_value_size) {
            in.advance(local_value_size);
            cell.m_overflow_id.value = get_u64(in);
        }
    }
    cell.m_is_external = is_external;
    return cell;
}

auto Cell::read_at(const Node &node, Size offset) -> Cell
{
    return read_at(node.page().view(offset), node.size(), node.is_external());
}

Cell::Cell(const Parameters &param)
    : m_key {param.key},
      m_local_value {param.local_value},
      m_overflow_id {param.overflow_id},
      m_value_size {param.value_size},
      m_page_size {param.page_size},
      m_is_external {param.is_external}
{}

auto Cell::copy() const -> Cell
{
    return Cell {{
        m_key,
        m_local_value,
        m_overflow_id,
        m_value_size,
        m_page_size,
        m_is_external,
    }};
}

auto Cell::size() const -> Size
{
    const auto is_internal = !m_is_external;
    const auto size_fields {sizeof(std::uint16_t) + sizeof(std::uint32_t) * m_is_external};
    const auto has_overflow_id = !m_overflow_id.is_null();
    return PAGE_ID_SIZE * static_cast<Size>(is_internal + has_overflow_id) +
           size_fields + m_key.size() + m_local_value.size();
}

auto Cell::left_child_id() const -> Id
{
    CALICO_EXPECT_FALSE(m_is_external);
    return m_left_child_id;
}

auto Cell::set_left_child_id(Id left_child_id) -> void
{
    CALICO_EXPECT_FALSE(m_is_external);
    m_left_child_id = left_child_id;
}

auto Cell::set_overflow_id(Id id) -> void
{
    CALICO_EXPECT_TRUE(m_is_external);
    m_overflow_id = id;
}

auto Cell::key() const -> BytesView
{
    return m_key;
}

auto Cell::local_value() const -> BytesView
{
    CALICO_EXPECT_TRUE(m_is_external);
    return m_local_value;
}

auto Cell::value_size() const -> Size
{
    return m_value_size;
}

auto Cell::overflow_size() const -> Size
{
    return m_value_size - m_local_value.size();
}

auto Cell::overflow_id() const -> Id
{
    // Internal cells have a zero-length value field, so they cannot overflow.
    CALICO_EXPECT_TRUE(m_is_external);
    return m_overflow_id;
}

auto Cell::write(Bytes out) const -> void
{
    if (!m_is_external) {
        CALICO_EXPECT_FALSE(m_left_child_id.is_root());
        put_u64(out, m_left_child_id.value);
        out.advance(PAGE_ID_SIZE);
    }
    put_u16(out, static_cast<std::uint16_t>(m_key.size()));
    out.advance(sizeof(std::uint16_t));

    if (m_is_external) {
        put_u32(out, static_cast<std::uint32_t>(m_value_size));
        out.advance(sizeof(std::uint32_t));
    }

    mem_copy(out, m_key, m_key.size());
    out.advance(m_key.size());

    if (m_is_external) {
        const auto local = local_value();
        mem_copy(out, local, local.size());

        if (!m_overflow_id.is_null()) {
            CALICO_EXPECT_FALSE(m_left_child_id.is_root());
            CALICO_EXPECT_LT(local.size(), m_value_size);
            out.advance(local.size());
            put_u64(out, m_overflow_id.value);
        }
    }
}

auto Cell::detach(Bytes scratch, bool ensure_internal) -> void
{
    if (ensure_internal && m_is_external)
        set_is_external(false);

    write(scratch);
    *this = read_at(scratch, m_page_size, m_is_external);
    m_is_attached = false;
}

auto Cell::set_is_external(bool is_external) -> void
{
    m_is_external = is_external;

    if (!m_is_external) {
        m_local_value.clear();
        m_value_size = 0;
        m_overflow_id = Id::null();
    }
}

auto make_external_cell(BytesView key, BytesView value, Size page_size) -> Cell
{
    CALICO_EXPECT_FALSE(key.is_empty());
    const auto local_value_size = get_local_value_size(key.size(), value.size(), page_size);
    Cell::Parameters param;
    param.key = key;
    param.local_value = value;
    param.value_size = value.size();
    param.page_size = page_size;
    param.is_external = true;

    if (local_value_size != value.size()) {
        CALICO_EXPECT_LT(local_value_size, value.size());
        param.local_value.truncate(local_value_size);
        // Set to an arbitrary value.
        param.overflow_id = Id::root();
    }
    return Cell {param};
}

auto make_internal_cell(BytesView key, Size page_size) -> Cell
{
    CALICO_EXPECT_FALSE(key.is_empty());
    Cell::Parameters param;
    param.key = key;
    param.page_size = page_size;
    param.is_external = false;
    return Cell {param};
}

} // namespace Calico
