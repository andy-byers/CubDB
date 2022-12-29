#include "framer.h"
#include "calico/storage.h"
#include "page/page.h"
#include "utils/encoding.h"
#include "utils/expect.h"
#include "utils/header.h"
#include "utils/info_log.h"
#include "utils/layout.h"

namespace calico {

Frame::Frame(Byte *buffer, Size id, Size size)
    : m_bytes {buffer + id*size, size}
{
    CALICO_EXPECT_TRUE(is_power_of_two(size));
    CALICO_EXPECT_GE(size, MINIMUM_PAGE_SIZE);
    CALICO_EXPECT_LE(size, MAXIMUM_PAGE_SIZE);
}

auto Frame::lsn() const -> identifier
{
    const auto offset = PageLayout::header_offset(m_page_id) + PageLayout::LSN_OFFSET;
    return identifier {get_u64(m_bytes.range(offset))};
}

auto Frame::ref(Pager &source, bool is_writable) -> Page
{
    CALICO_EXPECT_FALSE(m_is_writable);

    if (is_writable) {
        CALICO_EXPECT_EQ(m_ref_count, 0);
        m_is_writable = true;
    }
    m_ref_count++;
    return Page {{m_page_id, data(), &source, is_writable}};
}

auto Frame::unref(Page &page) -> void
{
    CALICO_EXPECT_EQ(m_page_id, page.id());
    CALICO_EXPECT_GT(m_ref_count, 0);

    if (page.is_writable()) {
        CALICO_EXPECT_EQ(m_ref_count, 1);
        m_is_writable = false;
    }
    // Make sure the page doesn't get released twice.
    page.m_source.reset();
    m_ref_count--;
}

auto Framer::open(std::unique_ptr<RandomEditor> file, Size page_size, Size frame_count, Framer **out) -> Status
{
    CALICO_EXPECT_TRUE(is_power_of_two(page_size));
    CALICO_EXPECT_GE(page_size, MINIMUM_PAGE_SIZE);
    CALICO_EXPECT_LE(page_size, MAXIMUM_PAGE_SIZE);
    CALICO_EXPECT_GE(frame_count, MINIMUM_FRAME_COUNT);
    CALICO_EXPECT_LE(frame_count, MAXIMUM_FRAME_COUNT);

    // Allocate the frames, i.e. where pages from disk are stored in memory. Aligned to the page size, so it could
    // potentially be used for direct I/O.
    const auto cache_size = page_size * frame_count;
    AlignedBuffer buffer {
        new(static_cast<std::align_val_t>(page_size), std::nothrow) Byte[cache_size],
        AlignedDeleter {static_cast<std::align_val_t>(page_size)}};
    if (buffer == nullptr)
        return Status::system_error("cannot allocate frames: out of memory");

    *out = new(std::nothrow) Framer {std::move(file), std::move(buffer), page_size, frame_count};
    if (*out == nullptr)
        return Status::system_error("cannot allocate framer object: out of memory");
    return Status::ok();
}

Framer::Framer(std::unique_ptr<RandomEditor> file, AlignedBuffer buffer, Size page_size, Size frame_count)
    : m_buffer {std::move(buffer)},
      m_file {std::move(file)},
      m_page_size {page_size}
{
    // The buffer should be aligned to the page size.
    CALICO_EXPECT_EQ(reinterpret_cast<std::uintptr_t>(m_buffer.get()) % page_size, 0);
    mem_clear({m_buffer.get(), page_size * frame_count});

    while (m_frames.size() < frame_count)
        m_frames.emplace_back(m_buffer.get(), m_frames.size(), page_size);
    
    while (m_available.size() < m_frames.size())
        m_available.emplace_back(Size {m_available.size()});
}

auto Framer::ref(Size id, Pager &source, bool is_writable) -> Page
{
    CALICO_EXPECT_LT(id, m_frames.size());
    m_ref_sum++;
    return m_frames[id].ref(source, is_writable);
}

auto Framer::unref(Size id, Page &page) -> void
{
    CALICO_EXPECT_LT(id, m_frames.size());
    m_frames[id].unref(page);
    m_ref_sum--;
}

auto Framer::pin(identifier id) -> Result<Size>
{
    CALICO_EXPECT_FALSE(id.is_null());
    if (m_available.empty()) {
        ThreePartMessage message;
        message.set_primary("could not pin page");
        message.set_detail("unable to find an available frame");
        message.set_hint("unpin a page and try again");
        return Err {message.not_found()};
    }

    auto &frame = frame_at_impl(m_available.back());
    CALICO_EXPECT_EQ(frame.ref_count(), 0);

    if (auto r = read_page_from_file(id, frame.data())) {
        if (!*r) {
            // We just tried to read at or past EOF. This happens when we allocate a new page or roll the WAL forward.
            mem_clear(frame.data());
            m_page_count++;
        }
    } else {
        return Err {r.error()};
    }

    auto fid = m_available.back();
    m_available.pop_back();
    frame.reset(id);
    return fid;
}

auto Framer::discard(Size id) -> void
{
    CALICO_EXPECT_EQ(frame_at_impl(id).ref_count(), 0);
    frame_at_impl(id).reset(identifier::null());
    m_available.emplace_back(id);
}

auto Framer::unpin(Size id) -> void
{
    auto &frame = frame_at_impl(id);
    CALICO_EXPECT_EQ(frame.ref_count(), 0);
    frame.reset(identifier::null());
    m_available.emplace_back(id);
}

auto Framer::write_back(Size id) -> Status
{
    auto &frame = frame_at_impl(id);
    CALICO_EXPECT_LE(frame.ref_count(), 1);

    // If this fails, the caller (buffer pool) will need to roll back the database state or exit.
    auto s = write_page_to_file(frame.pid(), frame.data());
    if (s.is_ok()) {
        const auto lsn = frame.lsn();
        m_flushed_lsn = std::max(lsn, m_flushed_lsn);
    }
    return s;
}

auto Framer::sync() -> Status
{
    return m_file->sync();
}

auto Framer::read_page_from_file(identifier id, Bytes out) const -> Result<bool>
{
    CALICO_EXPECT_EQ(m_page_size, out.size());
    const auto file_size = m_page_count * m_page_size;
    const auto offset = id.as_index() * out.size();

    // Don't even try to call read() if the file isn't large enough. The system call can be pretty slow even if it doesn't read anything.
    // This happens when we are allocating a page from the end of the file.
    if (offset >= file_size)
        return false;

    auto s = m_file->read(out, offset);
    if (!s.is_ok()) return Err {s};

    // We should always read exactly what we requested, unless we are allocating a page during recovery.
    if (out.size() == m_page_size)
        return true;

    // In that case, we will hit EOF here.
    if (out.is_empty())
        return false;

    ThreePartMessage message;
    message.set_primary("could not read page {}", id.value);
    message.set_detail("incomplete read");
    message.set_hint("read {}/{} bytes", out.size(), m_page_size);
    return Err {message.system_error()};
}

auto Framer::write_page_to_file(identifier id, BytesView in) const -> Status
{
    CALICO_EXPECT_EQ(m_page_size, in.size());
    return m_file->write(in, id.as_index() * in.size());
}

auto Framer::load_state(const FileHeader &header) -> void
{
    m_flushed_lsn.value = header.flushed_lsn; // TODO: Consider whether this should be read here... This will cause it to decrease after a roll back.
    m_page_count = header.page_count;         //       Right now, the XactTests are not reloading state between transactions.
}

auto Framer::save_state(FileHeader &header) const -> void
{
    header.flushed_lsn = m_flushed_lsn.value;
    header.page_count = m_page_count;
}

} // namespace calico
