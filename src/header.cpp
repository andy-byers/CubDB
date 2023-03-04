#include "header.h"
#include "crc.h"
#include "encoding.h"
#include "page.h"

namespace calicodb
{

static auto write_file_header(char *data, const FileHeader &header) -> void
{
    put_u32(data, header.magic_code);
    data += sizeof(std::uint32_t);

    put_u32(data, header.header_crc);
    data += sizeof(std::uint32_t);

    put_u64(data, header.page_count);
    data += sizeof(std::uint64_t);

    put_u64(data, header.record_count);
    data += sizeof(std::uint64_t);

    put_u64(data, header.freelist_head.value);
    data += sizeof(Id);

    put_u64(data, header.commit_lsn.value);
    data += sizeof(Lsn);

    put_u16(data, header.page_size);
}

auto FileHeader::read(const char *data) -> void
{
    magic_code = get_u32(data);
    data += sizeof(std::uint32_t);

    header_crc = get_u32(data);
    data += sizeof(std::uint32_t);

    page_count = get_u64(data);
    data += sizeof(std::uint64_t);

    record_count = get_u64(data);
    data += sizeof(std::uint64_t);

    freelist_head.value = get_u64(data);
    data += sizeof(Id);

    commit_lsn.value = get_u64(data);
    data += sizeof(Lsn);

    page_size = get_u16(data);
}

auto FileHeader::compute_crc() const -> std::uint32_t
{
    char data[FileHeader::kSize];
    write_file_header(data, *this);
    return crc32c::Value(data + 8, FileHeader::kSize - 8);
}

auto FileHeader::write(char *data) const -> void
{
    write_file_header(data, *this);
}

auto TableHeader::write(char *data) const -> void
{
    put_u64(data, commit_lsn.value);
    put_u64(data + sizeof(Lsn), record_count);
}

auto TableHeader::read(const char *data) -> void
{
    commit_lsn.value = get_u64(data);
    record_count = get_u64(data + sizeof(Lsn));
}

auto PageHeader::write(char *data) const -> void
{
    put_u64(data, page_lsn.value);
}

auto PageHeader::read(const char *data) -> void
{
    page_lsn.value = get_u64(data);
}

auto NodeHeader::read(const char *data) -> void
{
    // Flags byte.
    is_external = *data++;

    next_id.value = get_u64(data);
    data += sizeof(Id);

    prev_id.value = get_u64(data);
    data += sizeof(Id);

    cell_count = get_u16(data);
    data += sizeof(PageSize);

    cell_start = get_u16(data);
    data += sizeof(PageSize);

    free_start = get_u16(data);
    data += sizeof(PageSize);

    free_total = get_u16(data);
    data += sizeof(PageSize);

    frag_count = static_cast<std::uint8_t>(*data);
}

auto NodeHeader::write(char *data) const -> void
{
    *data++ = static_cast<char>(is_external);

    put_u64(data, next_id.value);
    data += sizeof(Id);

    put_u64(data, prev_id.value);
    data += sizeof(Id);

    put_u16(data, cell_count);
    data += sizeof(PageSize);

    put_u16(data, cell_start);
    data += sizeof(PageSize);

    put_u16(data, free_start);
    data += sizeof(PageSize);

    put_u16(data, free_total);
    data += sizeof(PageSize);

    *data = static_cast<char>(frag_count);
}

} // namespace calicodb