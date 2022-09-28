#ifndef CALICO_DISABLED_WAL_H
#define CALICO_DISABLED_WAL_H

#include "wal.h"

namespace calico {

class DisabledWriteAheadLog: public WriteAheadLog {
public:
    ~DisabledWriteAheadLog() override = default;

    [[nodiscard]]
    auto is_enabled() const -> bool override
    {
        return false;
    }

    [[nodiscard]]
    auto is_working() const -> bool override
    {
        return false;
    }

    [[nodiscard]]
    auto worker_status() const -> Status override
    {
        return Status::ok();
    }

    [[nodiscard]]
    auto flushed_lsn() const -> SequenceId override
    {
        return SequenceId::null();
    }

    [[nodiscard]]
    auto current_lsn() const -> SequenceId override
    {
        return SequenceId::null();
    }

    [[nodiscard]]
    auto log(WalPayloadIn) -> Status override
    {
        return Status::ok();
    }

    [[nodiscard]]
    auto flush() -> Status override
    {
        return Status::ok();
    }

    [[nodiscard]]
    auto advance() -> Status override
    {
        return Status::ok();
    }

    [[nodiscard]]
    auto stop_workers() -> Status override
    {
        return Status::ok();
    }

    [[nodiscard]]
    auto start_workers() -> Status override
    {
        return Status::ok();
    }

    [[nodiscard]]
    auto roll_forward(SequenceId, const Callback &) -> Status override
    {
        return Status::ok();
    }

    [[nodiscard]]
    auto roll_backward(SequenceId, const Callback &) -> Status override
    {
        return Status::ok();
    }

    [[nodiscard]]
    auto remove_before(SequenceId) -> Status override
    {
        return Status::ok();
    }

    [[nodiscard]]
    auto remove_after(SequenceId) -> Status override
    {
        return Status::ok();
    }
};

} // namespace calico

#endif // CALICO_DISABLED_WAL_H
