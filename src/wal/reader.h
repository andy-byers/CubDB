#ifndef CALICO_WAL_READER_H
#define CALICO_WAL_READER_H

#include "helpers.h"
#include "record.h"
#include "utils/crc.h"
#include "wal.h"
#include <optional>

namespace Calico {

class WalIterator {
    Span m_tail;
    Reader *m_file {};
    Size m_block {};
    Size m_offset {};

public:
    explicit WalIterator(Reader &file, Span tail);
    [[nodiscard]] auto read(Span &payload) -> Status;
    [[nodiscard]] auto offset() const -> Size;
};

class WalReader {
    [[nodiscard]] auto reopen() -> Status;

    std::optional<WalIterator> m_itr;
    std::unique_ptr<Reader> m_file {};
    std::string m_prefix;
    Span m_tail;
    Span m_data;
    Storage *m_storage {};
    WalSet *m_set {};
    Id m_id {};

public:
    struct Position {
        Id id;
        Size offset {};
    };

    struct Parameters {
        std::string prefix;
        Span tail;
        Span data;
        Storage *storage {};
        WalSet *set {};
    };

    [[nodiscard]] auto id() const -> Id
    {
        // Not valid until reader has had skip() or read() called once.
        return m_id;
    }

    [[nodiscard]] static auto open(const Parameters &param, WalReader **out) -> Status;
    [[nodiscard]] auto read(WalPayloadOut &payload) -> Status;
    [[nodiscard]] auto seek(Lsn lsn) -> Status;
    [[nodiscard]] auto skip() -> Status;
};

} // namespace Calico

#endif // CALICO_WAL_READER_H