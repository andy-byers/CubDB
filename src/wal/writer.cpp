#include "writer.h"
#include "utils/logging.h"
#include "utils/types.h"
#include <optional>

namespace calico {

auto LogWriter::write(SequenceId lsn, BytesView payload) -> Status
{
    CALICO_EXPECT_FALSE(lsn.is_null());
    WalRecordHeader lhs {};
    lhs.type = WalRecordHeader::Type::FULL;
    lhs.size = static_cast<std::uint16_t>(payload.size());
    lhs.crc = crc_32(payload);

    while (!payload.is_empty()) {
        auto rest = m_tail;
        // Note that this modifies rest to point to [<m_offset>, <end>) in the tail buffer.
        const auto space_remaining = rest.advance(m_offset).size();
        const auto can_fit_some = space_remaining > sizeof(WalRecordHeader);
        const auto can_fit_all = space_remaining >= sizeof(WalRecordHeader) + payload.size();

        if (can_fit_some) {
            WalRecordHeader rhs {};

            if (!can_fit_all)
                rhs = split_record(lhs, payload, space_remaining);

            // We must have room for the whole header and at least 1 payload byte.
            write_wal_record_header(rest, lhs);
            rest.advance(sizeof(lhs));
            mem_copy(rest, payload.range(0, lhs.size));

            m_offset += sizeof(lhs) + lhs.size;
            payload.advance(lhs.size);
            rest.advance(lhs.size);

            if (!can_fit_all)
                lhs = rhs;

            // The new value of m_offset must be less than or equal to the start of the next block. If it is exactly
            // at the start of the next block, we should fall through and read it into the tail buffer.
            if (m_offset != m_tail.size()) continue;
        }
        CALICO_EXPECT_LE(m_tail.size() - m_offset, sizeof(lhs));
        auto s = flush();
        if (!s.is_ok()) return s;
    }
    // Record is fully in the tail buffer and maybe partially on disk. Next time we flush, this record is guaranteed
    // to be all the way on disk.
    m_last_lsn = lsn;
    return Status::ok();
}

auto LogWriter::flush() -> Status
{
    // Already flushed, just return OK.
    if (m_offset == 0)
        return Status::ok();

    // Clear unused bytes at the end of the tail buffer.
    mem_clear(m_tail.range(m_offset));

    auto s = m_file->write(m_tail);
    if (s.is_ok()) {
        m_flushed_lsn->store(m_last_lsn);
        m_offset = 0;
        m_number++;
    }
    return s;
}

auto WalWriter::open() -> Status
{
    return open_segment(++m_segments->most_recent_id());
}

auto WalWriter::write(SequenceId lsn, NamedScratch payload) -> void
{
    m_worker.dispatch(Event {lsn, payload}, false);
}

auto WalWriter::advance() -> void
{
    m_worker.dispatch(std::nullopt, true);
}

auto WalWriter::destroy() && -> Status
{
    return std::move(m_worker).destroy();
}

auto WalWriter::on_event(const EventWrapper &event) -> Status
{
    // std::nullopt means we need to advance to the next segment.
    if (!event) return advance_segment();

    auto [lsn, buffer, size] = *event;
    auto s = m_writer->write(lsn, buffer->truncate(size));

    m_scratch->put(buffer);
    if (s.is_ok() && m_writer->block_count() >= m_wal_limit)
        return advance_segment();
    return s;
}

auto WalWriter::on_cleanup(const Status &) -> Status
{
    return close_segment();
}

auto WalWriter::open_segment(SegmentId id) -> Status
{
    CALICO_EXPECT_EQ(m_writer, std::nullopt);
    AppendWriter *file {};
    auto s = m_store->open_append_writer(m_prefix + id.to_name(), &file);
    if (s.is_ok()) {
        m_file.reset(file);
        m_writer = LogWriter {*m_file, m_tail, *m_flushed_lsn};
    }
    return s;
}

auto WalWriter::close_segment() -> Status
{
    CALICO_EXPECT_NE(m_writer, std::nullopt);

    // This is a NOOP if the tail buffer was empty.
    auto s = m_writer->flush();
    m_writer.reset();
    m_file.reset();

    // We want to do this part, even if the flush failed.
    auto id = ++m_segments->most_recent_id();
    if (m_writer->block_count()) {
        m_segments->add_segment({id});
    } else {
        auto t = m_store->remove_file(m_prefix + id.to_name());
        s = s.is_ok() ? t : s;
    }
    return s;
}

auto WalWriter::advance_segment() -> Status
{
    auto s = close_segment();
    if (s.is_ok()) {
        auto id = ++m_segments->most_recent_id();
        return open_segment(id);
    }
    return s;
}





auto BackgroundWriter::handle_error(SegmentGuard &guard, Status e) -> void
{
    CALICO_EXPECT_FALSE(e.is_ok());
    m_logger->error(e.what());

    if (guard.is_started()) {
        e = guard.finish(false);
        if (!e.is_ok()) {
            m_logger->error("(1/2) cannot complete segment after error");
            m_logger->error("(2/2) {}", e.what());
        }
    }
}

auto BackgroundWriter::emit_payload(SequenceId lsn, BytesView payload) -> Status
{
    return m_writer.write(lsn, payload, [this](auto flushed_lsn) {
        m_flushed_lsn->store(flushed_lsn);
    });
}

auto BackgroundWriter::emit_commit(SequenceId lsn) -> Status
{
    static constexpr char payload[] {WalPayloadType::COMMIT, '\x00'};
    return emit_payload(lsn, stob(payload));
}

auto BackgroundWriter::advance_segment(SegmentGuard &guard, bool has_commit) -> Status
{
    if (!guard.id().is_null()) {
        auto s = guard.finish(has_commit);
        if (!s.is_ok()) return s;
    }
    return guard.start();
}

auto BasicWalWriter::stop() -> Status
{
    return std::move(m_background).destroy();
}

auto BasicWalWriter::flush_block() -> void
{
    m_background.dispatch(BackgroundWriter::Event {
        BackgroundWriter::EventType::FLUSH_BLOCK,
        m_last_lsn,
        std::nullopt,
        0,
    }, true);
}

auto BasicWalWriter::log_full_image(PageId page_id, BytesView image) -> void
{
    auto buffer = m_scratch.get();
    const auto size = encode_full_image_payload(page_id, image, *buffer);

    m_background.dispatch({
        BackgroundWriter::EventType::LOG_FULL_IMAGE,
        ++m_last_lsn,
        buffer,
        size,
    });
}

auto BasicWalWriter::log_deltas(PageId page_id, BytesView image, const std::vector<PageDelta> &deltas) -> void
{
    auto buffer = m_scratch.get();
    const auto size = encode_deltas_payload(page_id, image, deltas, *buffer);

    m_background.dispatch({
        BackgroundWriter::EventType::LOG_DELTAS,
        ++m_last_lsn,
        buffer,
        size,
    });
}

auto BasicWalWriter::log_commit() -> void
{
    m_background.dispatch({
        BackgroundWriter::EventType::LOG_COMMIT,
        ++m_last_lsn,
        std::nullopt,
        0,
    }, true);
}

} // namespace calico