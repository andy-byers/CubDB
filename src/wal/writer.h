#ifndef CALICO_WAL_WAL_WRITER_H
#define CALICO_WAL_WAL_WRITER_H

#include "helpers.h"
#include "utils/crc.h"
#include "wal.h"
#include <memory>
#include <optional>
#include <queue>
#include <spdlog/logger.h>
#include <thread>

#include "utils/worker.h"

namespace Calico {

class LogWriter {
public:
    // NOTE: LogWriter must always be created on an empty segment file.
    LogWriter(AppendWriter &file, Bytes tail, std::atomic<Id> &flushed_lsn)
        : m_tail {tail},
          m_flushed_lsn {&flushed_lsn},
          m_file {&file}
    {}

    [[nodiscard]]
    auto block_count() const -> Size
    {
        return m_number;
    }

    // NOTE: If either of these methods return a non-OK status, the state of this object is unspecified, except for the block
    //       count, which remains valid.
    [[nodiscard]] auto write(WalPayloadIn payload) -> Status;
    [[nodiscard]] auto flush() -> Status;

private:
    Bytes m_tail;
    std::atomic<Id> *m_flushed_lsn {};
    AppendWriter *m_file {};
    Id m_last_lsn {};
    Size m_number {};
    Size m_offset {};
};

class WalWriter {
public:
    struct Parameters {
        Slice prefix;
        Bytes tail;
        Storage *storage {};
        System *system {};
        WalSet *set {};
        std::atomic<Id> *flushed_lsn {};
        Size wal_limit {};
    };

    explicit WalWriter(const Parameters &param);

    auto destroy() && -> void;
    auto write(WalPayloadIn payload) -> void;
    auto advance() -> void;
    auto flush() -> void;

private:
    [[nodiscard]] auto advance_segment() -> Status;
    [[nodiscard]] auto open_segment(SegmentId) -> Status;
    auto close_segment() -> Status;

    std::string m_prefix;
    std::optional<LogWriter> m_writer;
    std::unique_ptr<AppendWriter> m_file;
    std::atomic<Id> *m_flushed_lsn {};
    Storage *m_storage {};
    System *m_system {};
    WalSet *m_set {};
    SegmentId m_current;
    Bytes m_tail;
    Size m_wal_limit {};
};

} // namespace Calico

#endif // CALICO_WAL_WAL_WRITER_H