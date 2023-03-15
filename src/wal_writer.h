// Copyright (c) 2022, The CalicoDB Authors. All rights reserved.
// This source code is licensed under the MIT License, which can be found in
// LICENSE.md. See AUTHORS.md for a list of contributor names.

#ifndef CALICODB_WAL_WRITER_H
#define CALICODB_WAL_WRITER_H

#include "wal_record.h"

namespace calicodb
{

class WalWriter
{
public:
    explicit WalWriter(Logger &file, std::string &tail);

    [[nodiscard]] auto block_count() const -> std::size_t
    {
        return m_block;
    }

    // NOTE: If either of these methods return a non-OK status, the state of this object is unspecified, except for the block
    //       count, which remains valid.
    [[nodiscard]] auto write(Lsn lsn, const Slice &payload) -> Status;
    [[nodiscard]] auto flush() -> Status;

    // NOTE: Only valid if the writer has flushed.
    [[nodiscard]] auto flushed_lsn() const -> Lsn;

private:
    std::uint32_t m_type_crc[kMaxRecordType + 1] {};
    std::string *m_tail {};
    Lsn m_flushed_lsn;
    Logger *m_file {};
    Lsn m_last_lsn {};
    std::size_t m_block {};
    std::size_t m_offset {};
};

} // namespace calicodb

#endif // CALICODB_WAL_WRITER_H