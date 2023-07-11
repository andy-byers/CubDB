// Copyright (c) 2022, The CalicoDB Authors. All rights reserved.
// This source code is licensed under the MIT License, which can be found in
// LICENSE.md. See AUTHORS.md for a list of contributor names.

#ifndef CALICODB_DB_H
#define CALICODB_DB_H

#include "options.h"
#include "tx.h"

namespace calicodb
{

// Tag for starting a transaction that has writer capabilities
// SEE: DB::new_tx()
struct WriteTag {
};

// On-disk collection of buckets
class DB
{
public:
    // Open or create a CalicoDB database with the given filename
    // On success, stores a pointer to the heap-allocated database in `*db` and returns OK. On
    // failure, sets `*db` to nullptr and returns a non-OK status. The user is responsible for
    // calling delete on the database handle when it is no longer needed.
    static auto open(const Options &options, const std::string &filename, DB *&db_out) -> Status;

    // Delete the contents of the specified database from stable storage
    // Deletes every file associated with the database named `filename` and returns OK on
    // success. Returns a non-OK status on failure. `options` should hold the same options
    // that were used to create the database (`options` must at least specify the WAL and
    // info log paths, if non-default values were used).
    static auto destroy(const Options &options, const std::string &filename) -> Status;

    explicit DB();
    virtual ~DB();

    // Prevent move/copy
    DB(DB &) = delete;
    void operator=(DB &) = delete;

    // Get a human-readable string describing a named database property
    // If the property named `name` exists, returns true and stores the property value in
    // `*value_out`. Otherwise, false is returned and `value_out->clear()` is called. The
    // `value_out` parameter is optional: if passed a nullptr, this method performs an
    // existence check.
    virtual auto get_property(const Slice &name, std::string *value_out) const -> bool = 0;

    // Write modified pages from the write-ahead log (WAL) back to the database file
    // If `reset` is true, steps are taken to make sure that the next writer will reset the WAL
    // (start writing from the beginning of the file again). This includes blocking until all
    // other connections are finished using the WAL. Additional checkpoints are run (a) when
    // the database is closed, and (b) when a database is opened that has a WAL on disk. Note
    // that in the case of (b), `reset` is false.
    virtual auto checkpoint(bool reset) -> Status = 0;

    // Run a read-only transaction
    // REQUIRES: Status Fn::operator()(const Tx &) is implemented.
    // Forwards the Status returned by the callable `fn`. Note that the callable accepts a const
    // Tx reference, meaning methods that modify the database state cannot be called on it.
    template <class Fn>
    auto view(Fn &&fn) const -> Status;

    // Run a read-write transaction
    // REQUIRES: Status Fn::operator()(Tx &) is implemented.
    // If the callable `fn` returns an OK status, the transaction is committed. Otherwise,
    // the transaction is rolled back.
    template <class Fn>
    auto update(Fn &&fn) -> Status;

    // Start a transaction manually
    // Stores a pointer to the heap-allocated transaction object in `tx_out` and returns OK on
    // success. Stores nullptr in `tx_out` and returns a non-OK status on failure. If the WriteTag
    // overload is used, then the transaction is a read-write transaction, otherwise it is a
    // readonly transaction. The caller is responsible for calling delete on the Tx pointer when
    // it is no longer needed.
    // NOTE: Consider using the DB::view()/DB::update() API instead.
    virtual auto new_tx(const Tx *&tx_out) const -> Status = 0;
    virtual auto new_tx(WriteTag, Tx *&tx_out) -> Status = 0;
};

template <class Fn>
auto DB::view(Fn &&fn) const -> Status
{
    const Tx *tx;
    auto s = new_tx(tx);
    if (s.is_ok()) {
        s = fn(*tx);
        delete tx;
    }
    return s;
}

template <class Fn>
auto DB::update(Fn &&fn) -> Status
{
    Tx *tx;
    auto s = new_tx(WriteTag(), tx);
    if (s.is_ok()) {
        s = fn(*tx);
        if (s.is_ok()) {
            s = tx->commit();
        }
        // Implicit rollback of all uncommitted changes.
        delete tx;
    }
    return s;
}

} // namespace calicodb

#endif // CALICODB_DB_H
