#include "basic_wal.h"
#include "cleanup.h"
#include "utils/system.h"

namespace Calico {

BasicWriteAheadLog::BasicWriteAheadLog(const Parameters &param)
    : m_log {param.system->create_log("wal")},
      m_prefix {param.prefix},
      m_store {param.store},
      m_system {param.system},
      m_reader_data(wal_scratch_size(param.page_size), '\x00'),
      m_reader_tail(wal_block_size(param.page_size), '\x00'),
      m_writer_tail(wal_block_size(param.page_size), '\x00'),
      m_wal_limit {param.wal_limit},
      m_writer_capacity {param.writer_capacity}
{
    m_log->trace("BasicWriteAheadLog");

    m_log->info("page_size = {}", param.page_size);
    m_log->info("wal_limit = {}", param.wal_limit);
    m_log->info("writer_capacity = {}", param.writer_capacity);

    CALICO_EXPECT_NE(m_store, nullptr);
    CALICO_EXPECT_NE(m_system, nullptr);
    CALICO_EXPECT_NE(m_writer_capacity, 0);
    CALICO_EXPECT_NE(m_wal_limit, 0);
}

auto BasicWriteAheadLog::open(const Parameters &param) -> tl::expected<WriteAheadLog::Ptr, Status>
{
    // Get the name of every file in the database directory.
    std::vector<std::string> child_names;
    const auto path_prefix = param.prefix + WAL_PREFIX;
    auto s = param.store->get_children(param.prefix, child_names);
    if (!s.is_ok()) return tl::make_unexpected(s);

    // Filter out the segment file names.
    std::vector<std::string> segment_names;
    std::copy_if(cbegin(child_names), cend(child_names), back_inserter(segment_names), [&path_prefix](const auto &path) {
        return Slice {path}.starts_with(path_prefix);
    });

    // Convert to segment IDs.
    std::vector<SegmentId> segment_ids;
    std::transform(cbegin(segment_names), cend(segment_names), back_inserter(segment_ids), [param](const auto &name) {
        return SegmentId::from_name(Slice {name}.advance(param.prefix.size()));
    });
    std::sort(begin(segment_ids), end(segment_ids));

    std::unique_ptr<BasicWriteAheadLog> wal {new (std::nothrow) BasicWriteAheadLog {param}};
    if (wal == nullptr)
        return tl::make_unexpected(system_error("cannot allocate WAL object: out of memory"));

    // Keep track of the segment files.
    for (const auto &id: segment_ids)
        wal->m_set.add_segment(id);

    return wal;
}

BasicWriteAheadLog::~BasicWriteAheadLog()
{
    // m_log->trace("~BasicWriteAheadLog");
    // m_log->info("flushed_lsn: {}", m_flushed_lsn.load().value);

    if (m_writer != nullptr)
        CALICO_ERROR_IF(std::move(*m_writer).destroy());
}

auto BasicWriteAheadLog::start_workers() -> Status
{
    m_writer = std::unique_ptr<WalWriter> {new(std::nothrow) WalWriter {{
        m_prefix,
        m_writer_tail,
        m_store,
        m_system,
        &m_set,
        &m_flushed_lsn,
        m_wal_limit,
    }}};
    if (m_writer == nullptr)
        return system_error("cannot allocate writer object: out of memory");

    m_cleanup = std::unique_ptr<WalCleanup> {new(std::nothrow) WalCleanup {{
        m_prefix,
        &m_recovery_lsn,
        m_store,
        m_system,
        &m_set,
    }}};
    if (m_cleanup == nullptr)
        return system_error("cannot allocate cleanup object: out of memory");

    m_tasks = std::unique_ptr<Worker<Event>> {new(std::nothrow) Worker<Event> {[this](auto event) {
        run_task(std::move(event));
    }, m_writer_capacity}};
    if (m_tasks == nullptr)
        return system_error("cannot allocate task manager object: out of memory");

    return ok();
}

auto BasicWriteAheadLog::run_task(Event event) -> void
{
    if (std::holds_alternative<WalPayloadIn>(event)) {
        m_writer->write(std::get<WalPayloadIn>(event));
    } else if (std::holds_alternative<FlushToken>(event)) {
        m_writer->flush();
    } else {
        CALICO_EXPECT_TRUE((std::holds_alternative<AdvanceToken>(event)));
        m_writer->advance();
    }

    m_cleanup->cleanup();
}

auto BasicWriteAheadLog::flushed_lsn() const -> Id
{
    return m_flushed_lsn.load();
}

auto BasicWriteAheadLog::current_lsn() const -> Id
{
    return Id {m_last_lsn.value + 1};
}

auto BasicWriteAheadLog::log(WalPayloadIn payload) -> void
{
    CALICO_EXPECT_NE(m_writer, nullptr);
    m_last_lsn.value++;
    m_tasks->dispatch(payload);
}

auto BasicWriteAheadLog::flush() -> void
{
    CALICO_EXPECT_NE(m_writer, nullptr);
    m_tasks->dispatch(FlushToken {}, true);
}

auto BasicWriteAheadLog::advance() -> void
{
    CALICO_EXPECT_NE(m_writer, nullptr);
    m_tasks->dispatch(AdvanceToken {}, true);
}

auto BasicWriteAheadLog::open_reader() -> tl::expected<WalReader, Status>
{
    auto reader = WalReader {
        *m_store,
        m_set,
        m_prefix,
        Bytes {m_reader_tail},
        Bytes {m_reader_data}};
    if (auto s = reader.open(); !s.is_ok())
        return tl::make_unexpected(s);
    return reader;
}

auto BasicWriteAheadLog::roll_forward(Id begin_lsn, const Callback &callback) -> Status
{
    // m_log->trace("roll_forward");

    if (m_set.first().is_null())
        return ok();

    if (m_last_lsn.is_null()) {
        m_last_lsn = begin_lsn;
        m_flushed_lsn.store(m_last_lsn);
    }
    // Open the reader on the first (oldest) WAL segment file.
    auto reader = open_reader();
    if (!reader.has_value())
        return reader.error();

    // We should be on the first segment.
    CALICO_EXPECT_TRUE(reader->seek_previous().is_not_found());

    auto s = ok();
    const auto on_seek = [&] {
        const auto id = reader->segment_id();

        Id first_lsn;
        s = reader->read_first_lsn(first_lsn);

        // This indicates an empty file. Try to seek back to the last segment.
        if (s.is_not_found()) {
            if (id != m_set.last()) {
                s = corruption("missing WAL data in segment {}", reader->segment_id().value);
            } else if (id != m_set.first()) {
                s = reader->seek_previous();
            } else {
                s = ok();
            }
            return Id::null();
        }
        if (s.is_ok())
            m_set.set_first_lsn(reader->segment_id(), first_lsn);

        return first_lsn;
    };

    // Find the segment containing the first update that hasn't been applied yet.
    while (s.is_ok()) {
        const auto first_lsn = on_seek();
        CALICO_TRY_S(s);

        if (first_lsn >= begin_lsn) {
            if (first_lsn > begin_lsn)
                s = reader->seek_previous();
            break;
        } else {
            s = reader->seek_next();
        }
    }

    if (s.is_not_found())
        s = ok();

    while (s.is_ok()) {
        if (const auto first_lsn = on_seek(); first_lsn.is_null())
            break;
        CALICO_TRY_S(s);

        s = reader->roll([&callback, begin_lsn, this](auto payload) {
            m_last_lsn = payload.lsn();
            if (m_last_lsn >= begin_lsn)
                return callback(payload);
            return ok();
        });
        m_flushed_lsn.store(m_last_lsn);

        // We found an empty segment. This happens when the program aborted before the writer could either
        // write a block or delete the empty file. This is OK if we are on the last segment.
        if (s.is_not_found())
            s = corruption(s.what().data());

        if (s.is_ok())
            s = reader->seek_next();
    }
    const auto last_id = reader->segment_id();

    // Translate the error status if needed. Note that we allow an incomplete record at the end of the
    // most-recently-written segment.
    if (!s.is_ok()) {
        if (s.is_corruption()) {
            if (last_id != m_set.last())
                return s;
        } else if (!s.is_not_found()) {
            return s;
        }
        s = ok();
    }
    return s;
}

auto BasicWriteAheadLog::roll_backward(Id end_lsn, const Callback &callback) -> Status
{
    // m_log->trace("roll_backward");
    if (m_set.first().is_null())
        return ok();

    auto reader = open_reader();
    if (!reader.has_value())
        return reader.error();

    // Find the most-recent segment. TODO: Just seek all the way to the end at once.
    for (; ; ) {
        auto s = reader->seek_next();
        if (s.is_not_found()) break;
        CALICO_TRY_S(s);
    }

    auto s = ok();
    for (Size i {}; s.is_ok(); i++) {
        Id first_lsn;
        s = reader->read_first_lsn(first_lsn);

        if (s.is_ok()) {
            // Found the segment containing the end_lsn.
            if (first_lsn <= end_lsn)
                break;

            // Read all full image records. We can read them forward, since the pages are disjoint
            // within each transaction.
            s = reader->roll(callback);
        } else if (s.is_not_found()) {
            // The segment file is empty.
            s = corruption(s.what().data());
        }

        // Most-recent segment can have an incomplete record at the end.
        if (s.is_corruption() && i == 0)
            s = ok();
        CALICO_TRY_S(s);

        s = reader->seek_previous();
    }
    return s.is_not_found() ? ok() : s;
}

auto BasicWriteAheadLog::cleanup(Id recovery_lsn) -> void
{
    m_log->trace("cleanup({})", recovery_lsn.value);
    m_recovery_lsn.store(recovery_lsn);
}

auto BasicWriteAheadLog::truncate(Id lsn) -> Status
{
    // m_log->trace("truncate");
    auto current = m_set.last();

    while (!current.is_null()) {
        auto r = read_first_lsn(
            *m_store, m_prefix, current, m_set);

        if (r.has_value()) {
            if (*r <= lsn)
                break;
        } else if (auto s = r.error(); !s.is_not_found()) {
            return s;
        }
        if (!current.is_null()) {
            const auto name = m_prefix + current.to_name();
            CALICO_TRY_S(m_store->remove_file(name));
            m_set.remove_after(SegmentId {current.value - 1});
            // m_log->info("removed segment {} with first LSN {}", name, first_lsn.value);
        }
        current = m_set.id_before(current);
    }
    return ok();
}

} // namespace Calico