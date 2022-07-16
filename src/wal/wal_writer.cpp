#include "wal_writer.h"
#include "storage/interface.h"
#include "utils/identifier.h"
#include "utils/logging.h"
#include "wal_record.h"
#include <optional>

namespace cco {

using namespace page;
using namespace utils;

auto WALWriter::open(const WALParameters &param) -> Result<std::unique_ptr<IWALWriter>>
{
    CCO_EXPECT_GE(param.page_size, MINIMUM_PAGE_SIZE);
    CCO_EXPECT_LE(param.page_size, MAXIMUM_PAGE_SIZE);
    CCO_EXPECT_TRUE(is_power_of_two(param.page_size));

    CCO_TRY_CREATE(file, param.directory.open_file(WAL_NAME, Mode::CREATE | Mode::WRITE_ONLY | Mode::APPEND, 0666));
    CCO_TRY_CREATE(file_size, file->size());
    auto writer = std::unique_ptr<WALWriter> {new(std::nothrow) WALWriter {std::move(file), param}};
    if (!writer) {
        ThreePartMessage message;
        message.set_primary("cannot open WAL writer");
        message.set_detail("out of memory");
        return Err {message.system_error()};
    }
    writer->m_has_committed = file_size > 0;
    return writer;
}

WALWriter::WALWriter(std::unique_ptr<IFile> file, const WALParameters &param):
      m_file {std::move(file)},
      m_block(param.page_size, '\x00') {}

auto WALWriter::append(WALRecord record) -> Result<void>
{
    const auto next_lsn {record.lsn()};
    CCO_EXPECT_EQ(next_lsn.value, m_last_lsn.value + 1);
    std::optional<WALRecord> temp {std::move(record)};

    while (temp) {
        const auto remaining = m_block.size() - m_cursor;

        // Each record must contain at least 1 payload byte.
        const auto can_fit_some = remaining > WALRecord::HEADER_SIZE;
        const auto can_fit_all = remaining >= temp->size();

        if (can_fit_some) {
            WALRecord rest;

            if (!can_fit_all)
                rest = temp->split(remaining - WALRecord::HEADER_SIZE);

            auto destination = stob(m_block).range(m_cursor, temp->size());
            temp->write(destination);

            m_cursor += temp->size();

            if (can_fit_all) {
                temp.reset();
            } else {
                temp = rest;
            }
            continue;
        }
        CCO_TRY(flush());
    }
    m_last_lsn = next_lsn;
    return {};
}

auto WALWriter::truncate() -> Result<void>
{
    return m_file->resize(0)
        .and_then([this]() -> Result<void> {
            return m_file->sync();
        })
        .and_then([this]() -> Result<void> {
            m_cursor = 0;
            m_has_committed = false;
            mem_clear(stob(m_block));
            return {};
        });
}

auto WALWriter::flush() -> Result<void>
{
    if (m_cursor) {
        // The unused part of the block should be zero-filled.
        auto block = stob(m_block);
        mem_clear(block.range(m_cursor));

        CCO_TRY(m_file->write(block));
        CCO_TRY(m_file->sync());

        m_cursor = 0;
        m_flushed_lsn = m_last_lsn;
        m_has_committed = true;
    }
    return {};
}

} // cco