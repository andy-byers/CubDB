// Copyright (c) 2022, The CalicoDB Authors. All rights reserved.
// This source code is licensed under the MIT License, which can be found in
// LICENSE.md. See AUTHORS.md for a list of contributor names.

#ifndef CALICODB_TXN_IMPL_H
#define CALICODB_TXN_IMPL_H

#include "schema.h"
#include "table_impl.h"

namespace calicodb
{

inline auto readonly_transaction() -> Status
{
    // TODO: Status::readonly("...")/Status::is_readonly()?
    return Status::not_supported("transaction is readonly");
}

class TxnImpl : public Txn
{
public:
    explicit TxnImpl(Pager &pager, Status &status, bool write);
    ~TxnImpl() override;
    [[nodiscard]] auto status() const -> Status override;
    [[nodiscard]] auto new_table(const TableOptions &options, const std::string &name, Table *&out) -> Status override;
    [[nodiscard]] auto drop_table(const std::string &name) -> Status override;
    [[nodiscard]] auto vacuum() -> Status override;
    [[nodiscard]] auto commit() -> Status override;
    auto rollback() -> void override;

    auto TEST_validate() const -> void;

private:
    friend class DBImpl;

    [[nodiscard]] auto vacuum_freelist() -> Status;

    mutable Schema m_schema;
    TxnImpl **m_backref = nullptr;
    Pager *m_pager;
    Status *m_status;
    bool m_write = false;
};

inline auto txn_impl(Txn *txn) -> TxnImpl *
{
    return reinterpret_cast<TxnImpl *>(txn);
}
inline auto txn_impl(const Txn *txn) -> const TxnImpl *
{
    return reinterpret_cast<const TxnImpl *>(txn);
}

} // namespace calicodb

#endif // CALICODB_TXN_IMPL_H
