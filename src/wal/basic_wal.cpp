#include "basic_wal.h"
#include "utils/logging.h"

namespace calico {

#define MAYBE_FORWARD(expr, message) \
    do { \
        const auto &calico_s = (expr); \
        if (!calico_s.is_ok()) return forward_status(calico_s, message); \
    } while (0)

[[nodiscard]] static auto handle_writer_error(spdlog::logger &logger, BasicWalWriter &writer, Status &out)
{
    auto s = writer.status();
    if (!s.is_ok()) {
        logger.error("(1/2) background writer encountered an error");
        logger.error("(2/2) {}", s.what());
        out = s;
    }
    return s;
}

static auto handle_not_started_error(spdlog::logger &logger, bool is_working, const std::string &primary)
{
    if (!is_working) {
        LogMessage message {logger};
        message.set_primary(primary);
        message.set_detail("background workers are not running");
        message.set_hint("start the background workers and try again");
        return message.logic_error();
    }
    return Status::ok();
}

BasicWriteAheadLog::BasicWriteAheadLog(const Parameters &param)
    : m_logger {create_logger(param.sink, "wal")},
      m_prefix {param.prefix},
      m_store {param.store},
      m_page_size {param.page_size},
      m_wal_limit {param.wal_limit},
      m_reader {
          *m_store,
          param.prefix,
          param.page_size}
{
    m_logger->info("constructing BasicWriteAheadLog object");
}

auto BasicWriteAheadLog::open(const Parameters &param, WriteAheadLog **out) -> Status
{
    auto *temp = new (std::nothrow) BasicWriteAheadLog {param};

    if (!temp) {
        ThreePartMessage message;
        message.set_primary("cannot open write-ahead log");
        message.set_detail("out of memory");
        return message.system_error();
    }
    *out = temp;
    return Status::ok();
}

BasicWriteAheadLog::~BasicWriteAheadLog()
{
    m_logger->info("destroying BasicWriteAheadLog object");

    // TODO: Move this into a close() method. We probably want to know if the writer shut down okay.

    if (m_is_working) {
        auto s = handle_writer_error(*m_logger, *m_writer, m_status);
        forward_status(s, "cannot clean up WAL writer");

        s = stop_workers_impl();
        forward_status(s, "cannot stop workers");
    }
}

auto BasicWriteAheadLog::status() const -> Status
{
    if (m_is_working) { // TODO: Error model needs some work!
        auto s = m_writer->status();
        if (!s.is_ok()) return s;

//        s = m_cleaner->status();
//        if (!s.is_ok()) return s;
    }
    return m_status;
}

auto BasicWriteAheadLog::flushed_lsn() const -> std::uint64_t
{
    return m_flushed_lsn.load().value;
}

auto BasicWriteAheadLog::current_lsn() const -> std::uint64_t
{
    CALICO_EXPECT_TRUE(m_is_working);
    return m_writer->last_lsn().value + 1;
}

#define HANDLE_WRITER_ERRORS \
    do { \
        auto s = handle_not_started_error(*m_logger, m_is_working, MSG); \
        MAYBE_FORWARD(s, MSG); \
        s = handle_writer_error(*m_logger, *m_writer, m_status); \
        MAYBE_FORWARD(s, MSG); \
    } while (0)

auto BasicWriteAheadLog::log_image(std::uint64_t page_id, BytesView image) -> Status
{
    static constexpr auto MSG = "could not log full image";
    HANDLE_WRITER_ERRORS;

    // Skip writing this full image if one has already been written for this page during this transaction. If so, we can
    // just use the old one to undo changes made to this page during the entire transaction.
    const auto itr = m_images.find(PageId {page_id});
    if (itr != cend(m_images)) {
        m_logger->info("skipping full image for page {}", page_id);
        return Status::ok();
    }

    m_writer->log_full_image(PageId {page_id}, image);
    m_images.emplace(PageId {page_id});
    return m_writer->status();
}

auto BasicWriteAheadLog::log_deltas(std::uint64_t page_id, BytesView image, const std::vector<PageDelta> &deltas) -> Status
{
    static constexpr auto MSG = "could not log deltas";
    HANDLE_WRITER_ERRORS;

    m_writer->log_deltas(PageId {page_id}, image, deltas);
    return m_writer->status();
}

auto BasicWriteAheadLog::log_commit() -> Status
{
    m_logger->info("logging commit");

    static constexpr auto MSG = "could not log commit";
    HANDLE_WRITER_ERRORS;

    m_writer->log_commit();
    m_images.clear();
    return m_writer->status();
}

#undef HANDLE_WRITER_ERRORS

auto BasicWriteAheadLog::stop_workers() -> Status
{
    return stop_workers_impl();
}

auto BasicWriteAheadLog::stop_workers_impl() -> Status
{
    static constexpr auto MSG = "could not stop background writer";
    m_logger->info("received stop request");
    CALICO_EXPECT_TRUE(m_is_working);

    m_images.clear();
    m_is_working = false;
    auto s = m_writer->stop();
    m_writer.reset();

    auto t = Status::ok(); // m_cleaner->stop();
//    m_cleaner.reset();

    if (m_status.is_ok()) {
        if (!s.is_ok()) {
            m_status = s;
        } else if (!t.is_ok()) {
            m_status = t;
        }
    }

    MAYBE_FORWARD(s, MSG);
    MAYBE_FORWARD(t, MSG);

    m_logger->info("background writer is stopped");
    return s;
}

auto BasicWriteAheadLog::start_workers() -> Status
{
    static constexpr auto MSG = "could not start workers";
    m_logger->info("received start request: next segment ID is {}", m_collection.most_recent_id().value);
    CALICO_EXPECT_FALSE(m_is_working);

    m_writer = std::make_unique<BasicWalWriter>(BasicWalWriter::Parameters {
        m_store,
        &m_collection,
        &m_flushed_lsn,
        &m_pager_lsn,
        m_logger,
        m_prefix,
        m_page_size,
        m_wal_limit,
    });

    auto s = m_writer->status();
    auto t = Status::ok(); // m_cleaner->status();
    if (s.is_ok() && t.is_ok()) {
        m_is_working = true;
        m_logger->info("workers are started");
    } else {
        m_writer.reset();
//        m_cleaner.reset();
        MAYBE_FORWARD(s, MSG);
        MAYBE_FORWARD(t, MSG);
    }
    return s;
}

auto BasicWriteAheadLog::setup_and_recover(const RedoCallback &redo_cb, const UndoCallback &undo_cb) -> Status
{
    static constexpr auto MSG = "cannot recover";
    m_logger->info("received recovery request");
    CALICO_EXPECT_FALSE(m_is_working);

    std::vector<std::string> child_names;
    const auto path_prefix = m_prefix + WAL_PREFIX;
    auto s = m_store->get_children(m_prefix, child_names);
    MAYBE_FORWARD(s, MSG);

    // TODO: Not a great way to validate paths...
    std::vector<std::string> segment_names;
    std::copy_if(cbegin(child_names), cend(child_names), back_inserter(segment_names), [&path_prefix](const auto &path) {
        return stob(path).starts_with(path_prefix) && path.size() - path_prefix.size() == SegmentId::DIGITS_SIZE;
    });

    std::vector<SegmentId> segment_ids;
    std::transform(cbegin(segment_names), cend(segment_names), back_inserter(segment_ids), [](const auto &name) {
        return SegmentId::from_name(stob(name));
    });
    std::sort(begin(segment_ids), end(segment_ids));

    std::vector<RecordPosition> uncommitted;
    for (const auto id: segment_ids) {
        m_logger->info("rolling segment {} forward", id.value);

        s = m_reader.open(id);
        // Allow segments to be empty.
        if (s.is_logic_error())
            continue;
        MAYBE_FORWARD(s, MSG);

        WalSegment segment {id};
        SequenceId last_lsn;
        s = m_reader.redo(uncommitted, [&](const auto &info) {
            last_lsn.value = info.page_lsn;
            segment.has_commit = info.is_commit;
            return redo_cb(info);
        });
        if (!s.is_ok()) {
            s = forward_status(s, "could not roll WAL forward");
            break;
        }

        if (segment.has_commit)
            uncommitted.clear();

        m_collection.add_segment(segment);
        m_flushed_lsn.store(last_lsn);
    }
    // Return on fatal errors. Otherwise, we'll try to roll back the most-recent transaction.
    if (s.is_system_error())
        return s;

    for (auto itr = crbegin(uncommitted); itr != crend(uncommitted); ) {
        m_logger->info("rolling segment {} backward", itr->id.value);

        const auto end = std::find_if(next(itr), crend(uncommitted), [itr](const auto &position) {
            return position.id != itr->id;
        });
        s = m_reader.open(itr->id);
        if (s.is_logic_error()) continue;
        MAYBE_FORWARD(s, MSG);

        s = m_reader.undo(itr, end, [&undo_cb](const auto &info) {
            return undo_cb(info);
        });
        MAYBE_FORWARD(s, MSG);

        s = m_reader.close();
        MAYBE_FORWARD(s, MSG);
        itr = end;
    }

    if (!uncommitted.empty()) {
        s = m_collection.remove_from_right(uncommitted.front().id, [this](const auto &info) {
            CALICO_EXPECT_FALSE(info.has_commit);
            return m_store->remove_file(m_prefix + info.id.to_name());
        });
    }

    m_logger->info("finished recovery");
    return s;
}

auto BasicWriteAheadLog::abort_last(const UndoCallback &callback) -> Status
{
    static constexpr auto MSG = "could not abort last transaction";
    m_logger->info("received abort request");
    CALICO_EXPECT_FALSE(m_is_working);

    auto s = Status::ok();
    SegmentId obsolete;

    for (auto itr = crbegin(m_collection.segments()); itr != crend(m_collection.segments()); itr++) {
        const auto [id, has_commit] = *itr;
        m_logger->info("rolling segment {} backward", id.value);

        if (has_commit) {
            LogMessage message {*m_logger};
            message.set_primary("finished rolling backward");
            message.set_detail("found segment containing commit record");
            message.log(spdlog::level::info);
            break;
        }

        s = m_reader.open(id);
        if (s.is_logic_error()) {
            LogMessage message {*m_logger};
            message.set_primary("skipping segment");
            message.set_detail("segment is empty");
            message.log(spdlog::level::info);
            continue;
        }
        MAYBE_FORWARD(s, MSG);
        std::vector<RecordPosition> positions;

        // TODO: Would be nice to avoid this by saving the positions...
        s = m_reader.redo(positions, [](auto) {return Status::ok();});
        if (s.is_system_error()) {
            m_logger->error("(1/2) {}", MSG);
            m_logger->error("(2/2) {}", s.what());
            return s;
        }

        s = m_reader.undo(crbegin(positions), crend(positions), [&callback](const auto &info) {
            return callback(info);
        });
        MAYBE_FORWARD(s, MSG);

        s = m_reader.close();
        MAYBE_FORWARD(s, MSG);

        obsolete = id;
    }
    if (obsolete.is_null()) return s;

    // Remove obsolete WAL segments.
    s = m_collection.remove_from_right(obsolete, [this](auto info) {
        CALICO_EXPECT_FALSE(info.has_commit);
        return m_store->remove_file(m_prefix + info.id.to_name());
    });
    m_logger->info("finished undo");
    return s;
}

auto BasicWriteAheadLog::flush_pending() -> Status
{
    m_writer->flush_block();
    return status();
}

#undef MAYBE_FORWARD

} // namespace calico