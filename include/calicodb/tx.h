// Copyright (c) 2022, The CalicoDB Authors. All rights reserved.
// This source code is licensed under the MIT License, which can be found in
// LICENSE.md. See AUTHORS.md for a list of contributor names.

#ifndef CALICODB_TX_H
#define CALICODB_TX_H

#include "status.h"

namespace calicodb
{

struct BucketOptions;
class Cursor;

// TODO: Comments are outdated!
// Transaction on a CalicoDB database
// The lifetime of a transaction is the same as that of the Tx object representing it (see
// DB::new_tx()).
class Tx
{
public:
    explicit Tx();
    virtual ~Tx();

    Tx(Tx &) = delete;
    void operator=(Tx &) = delete;

    // Return the status associated with this transaction
    // On creation, a Tx will always have an OK status. Only read-write transactions
    // can have a non-OK status. The status is set when a routine on this object fails
    // such that the consistency of the underlying data store becomes questionable, or
    // corruption is detected in one of the files.
    virtual auto status() const -> Status = 0;

    // Return a reference to a cursor that iterates over the database schema
    // The database schema is a special bucket that stores the name and location of every
    // other bucket in the database. Calling Cursor::key() on a valid schema cursor gives a
    // bucket name. Calling Cursor::value() gives a bucket descriptor: a human-readable
    // description of options that the bucket was created with. This cursor must not be
    // used after the Tx that created it has been destroyed.
    // SEE: cursor.h (for additional Cursor usage requirements)
    [[nodiscard]] virtual auto schema() const -> Cursor & = 0;

    // Create a new bucket
    // Returns an OK status on success and a non-OK status on failure. If c_out is nonnull,
    // opens a cursor over the bucket contents and stores it in `*c_out`. The bucket can
    // accessed via this cursor until the transaction is finished, or until `name` is dropped
    // using Tx::drop_bucket(). Returns a non-OK status on failure. Note that the bucket will
    // not persist in the database unless Tx::commit() is called after the bucket has been
    // created but before the transaction is finished.
    virtual auto create_bucket(const BucketOptions &options, const Slice &name, Cursor **c_out) -> Status = 0;

    // Open an existing bucket
    // On success, stores a cursor over the bucket contents in `c_out` and returns an OK
    // status. If the bucket named `name` does not exist already, a status for which
    // Status::is_invalid_argument() evaluates to true is returned.
    virtual auto open_bucket(const Slice &name, Cursor *&c_out) const -> Status = 0;

    // Remove a bucket from the database
    // If a bucket named `name` exists, this method will attempt to remove it. If `name`
    // does not exist, returns a status for which Status::is_invalid_argument() evaluates
    // to true. If a cursor was obtained for `name` during this transaction, it must be
    // delete'd before this routine is called.
    virtual auto drop_bucket(const Slice &name) -> Status = 0;

    // Defragment the database
    // This routine reclaims all unused pages in the database. The database file will be
    // truncated the next time a checkpoint is run.
    virtual auto vacuum() -> Status = 0;

    // Commit pending changes to the database
    // Returns an OK status if the commit operation was successful, and a non-OK status
    // on failure. If this method is not called before the Tx object is destroyed, all
    // pending changes will be dropped. This method can be called more than once for a
    // given Tx: file locks are held until the Tx handle is delete'd.
    virtual auto commit() -> Status = 0;

    // Get the `value` associated with the given `key` from the bucket referenced by `c`
    // If a record with the given `key` exists, assigns to `*value` its associated value and
    // returns an OK status. If the `key` does not exist, sets `*value` to nullptr and returns
    // a "not found" status. If an error is encountered, returns a non-OK status as appropriate.
    // Leaves the cursor on the target record on success, and invalidates it on failure.
    virtual auto get(Cursor &c, const Slice &key, std::string *value) const -> Status = 0;

    // Create a mapping between `key` and `value` in the bucket referenced to by `c`
    // If a record with key `key` already exists, sets its value to `value`. Otherwise, a
    // new record is created. Returns an OK status on success, and a non-OK status on
    // failure.
    // Also adjusts the cursor `c` to point to the newly-created record. The following
    // expressions evaluate to true on success:
    //     `c->is_valid()`
    //     `c->status().is_ok()`
    //     `c->key()` == `key`
    //     `c->value()` == `value`
    // It is safe to use both `c->key()` and `c->value()` as parameters to this
    // routine. On failure, the cursor is left in an unspecified state (possibly
    // invalidated, or placed on a nearby record).
    virtual auto put(Cursor &c, const Slice &key, const Slice &value) -> Status = 0;

    // Erase a record from the bucket referenced by `c`
    // On success, leaves `c` on the record following the erased record and returns an
    // OK status. Returns a non-OK status if an error was encountered. It is not an error
    // if `key` does not exist.
    virtual auto erase(Cursor &c, const Slice &key) -> Status = 0;

    // Erase the record pointed to by `c`
    // On success, the record is erased, `c` is placed on the following record, and an
    // OK status is returned. Otherwise, a non-OK status is returned and the cursor is
    // left in an unspecified state.
    virtual auto erase(Cursor &c) -> Status = 0;
};

} // namespace calicodb

#endif // CALICODB_TX_H
