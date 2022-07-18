#include "wal_manager.h"
#include "wal_reader.h"
#include "wal_record.h"
#include "wal_writer.h"
#include "page/file_header.h"
#include "pool/interface.h"
#include "storage/interface.h"
#include "utils/logging.h"
#include <optional>

namespace cco {

using namespace page;
using namespace utils;

auto WALManager::open(const WALParameters &param) -> Result<std::unique_ptr<IWALManager>>
{
    CCO_EXPECT_GE(param.page_size, MINIMUM_PAGE_SIZE);
    CCO_EXPECT_LE(param.page_size, MAXIMUM_PAGE_SIZE);
    CCO_EXPECT_TRUE(is_power_of_two(param.page_size));

    CCO_TRY_CREATE(reader, WALReader::open(param));
    CCO_TRY_CREATE(writer, WALWriter::open(param));
    auto manager = std::unique_ptr<WALManager> {new(std::nothrow) WALManager {std::move(reader), std::move(writer), param}};
    if (!manager) {
        ThreePartMessage message;
        message.set_primary("cannot open WAL manager");
        message.set_detail("out of memory");
        return Err {message.system_error()};
    }
    return manager;
}

WALManager::WALManager(std::unique_ptr<IWALReader> reader, std::unique_ptr<IWALWriter> writer, const WALParameters &param):
      m_tracker {param.page_size},
      m_reader {std::move(reader)},
      m_writer {std::move(writer)},
      m_logger {create_logger(param.log_sink, "wal")},
      m_pool {param.pool} {}

auto WALManager::close() -> Result<void>
{
    const auto rr = m_reader->close();
    if (!rr.has_value()) {
        m_logger->error("cannot close WAL reader");
        m_logger->error("(reason) {}", rr.error().what());
    }
    const auto wr = m_writer->close();
    if (!wr.has_value()) {
        m_logger->error("cannot close WAL writer");
        m_logger->error("(reason) {}", wr.error().what());
    }
    // If both close() calls produced an error, we'll lose one of them. It'll end up in the
    // log though.
    return !rr.has_value() ? rr : wr;
}

auto WALManager::has_records() const -> bool
{
    return m_writer->has_committed() || m_writer->has_pending();
}

auto WALManager::flushed_lsn() const -> LSN
{
    return m_writer->flushed_lsn();
}

auto WALManager::track(Page &page) -> void
{
    m_tracker.track(page);
}

auto WALManager::discard(Page &page) -> void
{
    m_tracker.discard(page);
}

auto WALManager::append(Page &page) -> Result<void>
{
    return m_writer->append(WALRecord {m_tracker.collect(page, ++m_writer->last_lsn())});
}

auto WALManager::truncate() -> Result<void>
{
    m_tracker.reset();
    return m_writer->truncate();
}

auto WALManager::flush() -> Result<void>
{
    return m_writer->flush();
}

auto WALManager::recover() -> Result<void>
{
    if (m_writer->has_committed()) {
        CCO_EXPECT_TRUE(m_pool->status().is_ok()); // TODO
        CCO_EXPECT_FALSE(m_writer->has_pending());
        CCO_TRY_CREATE(found_commit, roll_forward());
        if (!found_commit)
            CCO_TRY(roll_backward());
        CCO_TRY(m_pool->flush());
        CCO_TRY(truncate());
    }
    return {};
}

auto WALManager::abort() -> Result<void>
{
    CCO_TRY(flush());
    CCO_TRY(roll_forward());
    CCO_TRY(roll_backward());
    return flush();
}

auto WALManager::commit() -> Result<void>
{
    auto next_lsn = ++m_writer->last_lsn();
    CCO_TRY_CREATE(root, m_pool->acquire(PID::root(), true));
    auto header = get_file_header_writer(root);
    header.set_flushed_lsn(next_lsn++);
    header.update_header_crc();
    CCO_TRY(m_pool->release(std::move(root)));
    CCO_TRY(m_writer->append(WALRecord::commit(next_lsn)));
    CCO_TRY(flush());
    return {};
}

auto WALManager::roll_forward() -> Result<bool>
{
    CCO_TRY(m_reader->reset());

    if (!m_reader->record()) {
        if (m_writer->has_committed()) {
            ThreePartMessage message;
            message.set_primary("cannot roll forward");
            message.set_detail("WAL contains unreadable records");
            return Err {message.corruption()};
        }
        return false;
    }

    for (auto should_continue = true; should_continue; ) {
        CCO_EXPECT_NE(m_reader->record(), std::nullopt);
        const auto record = *m_reader->record();

        if (m_writer->flushed_lsn() < record.lsn())
            m_writer->set_flushed_lsn(record.lsn());

        // Stop at the commit record.
        if (record.is_commit())
            return true;

        const auto update = record.decode();
        CCO_TRY_CREATE(page, m_pool->fetch(update.page_id, true));
        CCO_EXPECT_FALSE(page.has_manager());

        if (page.lsn() < record.lsn())
            page.redo(record.lsn(), update.changes);

        CCO_TRY(m_pool->release(std::move(page)));
        CCO_TRY_STORE(should_continue, m_reader->increment());
    }
    return false;
}

auto WALManager::roll_backward() -> Result<void>
{
    if (!m_reader->record())
        return {};

    if (m_reader->record()->is_commit()) {
        CCO_TRY_CREATE(decremented, m_reader->decrement());
        if (!decremented) {
            ThreePartMessage message;
            message.set_primary("cannot roll back");
            message.set_detail("transaction is empty");
            return Err {message.corruption()};
        }
        CCO_EXPECT_NE(m_reader->record(), std::nullopt);
        CCO_EXPECT_FALSE(m_reader->record()->is_commit());
    }

    for (auto should_continue = true; should_continue; ) {
        CCO_EXPECT_NE(m_reader->record(), std::nullopt);
        const auto record = *m_reader->record();
        CCO_EXPECT_FALSE(record.is_commit());

        const auto update = record.decode();
        CCO_TRY_CREATE(page, m_pool->fetch(update.page_id, true));
        CCO_EXPECT_FALSE(page.has_manager());

        if (page.lsn() >= record.lsn())
            page.undo(update.previous_lsn, update.changes);

        CCO_TRY(m_pool->release(std::move(page)));
        CCO_TRY_STORE(should_continue, m_reader->decrement());
    }
    return {};
}

auto WALManager::save_header(page::FileHeaderWriter &header) -> void
{
    header.set_flushed_lsn(m_writer->flushed_lsn());
}

auto WALManager::load_header(const page::FileHeaderReader &header) -> void
{
    m_writer->set_flushed_lsn(header.flushed_lsn());
}

} // cco