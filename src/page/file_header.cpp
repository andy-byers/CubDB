#include "file_header.h"

#include "node.h"
#include "utils/crc.h"
#include "utils/encoding.h"
#include "utils/identifier.h"
#include "utils/layout.h"

namespace cco::page {

using namespace utils;

FileHeader::FileHeader():
      m_backing(FileLayout::HEADER_SIZE, '\x00'),
      m_header {stob(m_backing)} {}

FileHeader::FileHeader(Bytes data):
      m_header{data.range(FileLayout::header_offset(), FileLayout::HEADER_SIZE)} {}

FileHeader::FileHeader(Node &root):
      FileHeader {root.page()} {}

FileHeader::FileHeader(Page &root):
    m_header {root.bytes(FileLayout::header_offset(), FileLayout::HEADER_SIZE)}
{
    CCO_EXPECT_TRUE(root.id().is_root());
}

auto FileHeader::data() -> Bytes
{
    return m_header;
}

auto FileHeader::data() const -> BytesView
{
    return m_header;
}

auto FileHeader::magic_code() const -> Index
{
    return utils::get_u32(m_header.range(FileLayout::MAGIC_CODE_OFFSET));
}

auto FileHeader::header_crc() const -> Index
{
    return utils::get_u32(m_header.range(FileLayout::HEADER_CRC_OFFSET));
}

auto FileHeader::page_count() const -> Size
{
    return utils::get_u32(m_header.range(FileLayout::PAGE_COUNT_OFFSET));
}

auto FileHeader::node_count() const -> Size
{
    return utils::get_u32(m_header.range(FileLayout::NODE_COUNT_OFFSET));
}

auto FileHeader::free_count() const -> Size
{
    return utils::get_u32(m_header.range(FileLayout::FREE_COUNT_OFFSET));
}

auto FileHeader::free_start() const -> PID
{
    return PID {utils::get_u32(m_header.range(FileLayout::FREE_START_OFFSET))};
}

auto FileHeader::page_size() const -> Size
{
    return utils::get_u16(m_header.range(FileLayout::PAGE_SIZE_OFFSET));
}

auto FileHeader::block_size() const -> Size
{
    return utils::get_u16(m_header.range(FileLayout::BLOCK_SIZE_OFFSET));
}

auto FileHeader::record_count() const -> Size
{
    return utils::get_u32(m_header.range(FileLayout::KEY_COUNT_OFFSET));
}

auto FileHeader::flushed_lsn() const -> LSN
{
    return LSN {utils::get_u32(m_header.range(FileLayout::FLUSHED_LSN_OFFSET))};
}

auto FileHeader::update_magic_code() -> void
{
    utils::put_u32(m_header.range(FileLayout::MAGIC_CODE_OFFSET), MAGIC_CODE);
}

auto FileHeader::update_header_crc() -> void
{
    const auto offset = FileLayout::HEADER_CRC_OFFSET;
    utils::put_u32(m_header.range(offset), crc_32(m_header.range(offset + sizeof(uint32_t))));
}

auto FileHeader::set_page_count(Size page_count) -> void
{
    CCO_EXPECT_BOUNDED_BY(uint32_t, page_count);
    utils::put_u32(m_header.range(FileLayout::PAGE_COUNT_OFFSET), static_cast<uint32_t>(page_count));
}

auto FileHeader::set_node_count(Size node_count) -> void
{
    CCO_EXPECT_BOUNDED_BY(uint32_t, node_count);
    utils::put_u32(m_header.range(FileLayout::NODE_COUNT_OFFSET), static_cast<uint32_t>(node_count));
}

auto FileHeader::set_free_count(Size free_count) -> void
{
    CCO_EXPECT_BOUNDED_BY(uint32_t, free_count);
    utils::put_u32(m_header.range(FileLayout::FREE_COUNT_OFFSET), static_cast<uint32_t>(free_count));
}

auto FileHeader::set_free_start(PID free_start) -> void
{
    utils::put_u32(m_header.range(FileLayout::FREE_START_OFFSET), free_start.value);
}

auto FileHeader::set_page_size(Size page_size) -> void
{
    CCO_EXPECT_BOUNDED_BY(uint16_t, page_size);
    utils::put_u16(m_header.range(FileLayout::PAGE_SIZE_OFFSET), static_cast<uint16_t>(page_size));
}

auto FileHeader::set_block_size(Size block_size) -> void
{
    CCO_EXPECT_BOUNDED_BY(uint16_t, block_size);
    utils::put_u16(m_header.range(FileLayout::BLOCK_SIZE_OFFSET), static_cast<uint16_t>(block_size));
}

auto FileHeader::set_key_count(Size key_count) -> void
{
    CCO_EXPECT_BOUNDED_BY(uint32_t, key_count);
    utils::put_u32(m_header.range(FileLayout::KEY_COUNT_OFFSET), static_cast<uint32_t>(key_count));
}

auto FileHeader::set_flushed_lsn(LSN flushed_lsn) -> void
{
    utils::put_u32(m_header.range(FileLayout::FLUSHED_LSN_OFFSET), flushed_lsn.value);
}

auto FileHeader::is_magic_code_consistent() const -> bool
{
    return magic_code() == MAGIC_CODE;
}

auto FileHeader::is_header_crc_consistent() const -> bool
{
    const auto offset = FileLayout::HEADER_CRC_OFFSET + sizeof(uint32_t);
    return header_crc() == crc_32(m_header.range(offset));
}

} // calico::page