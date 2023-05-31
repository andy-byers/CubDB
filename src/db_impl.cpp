// Copyright (c) 2022, The CalicoDB Authors. All rights reserved.
// This source code is licensed under the MIT License, which can be found in
// LICENSE.md. See AUTHORS.md for a list of contributor names.

#include "db_impl.h"
#include "calicodb/env.h"
#include "encoding.h"
#include "logging.h"
#include "scope_guard.h"
#include "tx_impl.h"

namespace calicodb
{

auto DBImpl::open(const Options &sanitized) -> Status
{
    File *file = nullptr;
    ScopeGuard guard = [&file, this] {
        if (m_pager == nullptr) {
            delete file;
        }
    };

    auto s = m_env->new_file(m_db_filename, Env::kReadWrite, file);
    if (s.is_ok()) {
        if (sanitized.error_if_exists) {
            return Status::invalid_argument(
                "database \"" + m_db_filename + "\" already exists");
        }
    } else if (s.is_io_error()) {
        if (!sanitized.create_if_missing) {
            return Status::invalid_argument(
                "database \"" + m_db_filename + "\" does not exist");
        }
        if (m_env->remove_file(m_wal_filename).is_ok()) {
            log(m_log, R"(removed old WAL file "%s")", m_wal_filename.c_str());
        }
        log(m_log, R"(creating missing database "%s")", m_db_filename.c_str());
        s = m_env->new_file(m_db_filename, Env::kCreate, file);
    }
    if (s.is_ok()) {
        s = busy_wait(m_busy, [file] {
            return file->file_lock(kLockShared);
        });
    }
    if (!s.is_ok()) {
        return s;
    }
    const Pager::Parameters pager_param = {
        m_db_filename.c_str(),
        m_wal_filename.c_str(),
        file,
        m_env,
        m_log,
        &m_status,
        m_busy,
        (sanitized.cache_size + kPageSize - 1) / kPageSize,
        sanitized.sync,
    };
    m_pager = new Pager(pager_param);

    const auto needs_ckpt = m_env->file_exists(m_wal_filename);
    s = m_pager->open_wal();
    if (s.is_ok() && needs_ckpt) {
        s = m_pager->checkpoint(false);
        if (s.is_busy()) {
            s = Status::ok();
        }
    }
    std::move(guard).cancel();
    return s;
}

DBImpl::DBImpl(const Options &options, const Options &sanitized, std::string filename)
    : m_env(sanitized.env),
      m_log(sanitized.info_log),
      m_busy(sanitized.busy),
      m_db_filename(std::move(filename)),
      m_wal_filename(sanitized.wal_filename),
      m_owns_log(options.info_log == nullptr)
{
}

DBImpl::~DBImpl()
{
    if (m_pager) {
        const auto s = m_pager->close();
        if (!s.is_ok()) {
            log(m_log, "failed to close pager: %s", s.to_string().c_str());
        }
    }
    delete m_pager;

    if (m_owns_log) {
        delete m_log;
    }
}

auto DBImpl::destroy(const Options &options, const std::string &filename) -> Status
{
    auto copy = options;
    copy.error_if_exists = false;
    copy.create_if_missing = false;

    DB *db;
    const Tx *tx;

    // Determine the WAL filename, and make sure `filename` refers to a CalicoDB
    // database. The file identifier is not checked until a transaction is started.
    std::string wal_name;
    auto s = DB::open(copy, filename, db);
    if (s.is_ok()) {
        wal_name = db_impl(db)->m_wal_filename;
        s = db->new_tx(tx);
        if (s.is_ok()) {
            delete tx;
        }
        delete db;
    }

    // Remove the database files from the Env.
    Status t;
    if (s.is_ok()) {
        auto *env = options.env;
        if (env == nullptr) {
            env = Env::default_env();
        }
        s = env->remove_file(filename);
        if (env->file_exists(wal_name)) {
            // Delete the WAL file if it wasn't properly cleaned up when the database
            // was closed above. Under normal conditions, this branch is not hit.
            t = env->remove_file(wal_name);
        }
    }
    return s.is_ok() ? t : s;
}

auto DBImpl::get_property(const Slice &name, std::string *out) const -> bool
{
    if (out) {
        out->clear();
    }
    if (name.starts_with("calicodb.")) {
        const auto prop = name.range(std::strlen("calicodb."));
        std::string buffer;

        if (prop == "stats") {
            if (out != nullptr) {
                append_fmt_string(
                    buffer,
                    "Name            Value\n"
                    "------------------------\n"
                    "Pager read(MB)  %8.4f\n"
                    "Pager write(MB) %8.4f\n"
                    "WAL read(MB)    %8.4f\n"
                    "WAL write(MB)   %8.4f\n"
                    "Cache hits      %ld\n"
                    "Cache misses    %ld\n",
                    static_cast<double>(m_pager->statistics().bytes_read) / 1'048'576.0,
                    static_cast<double>(m_pager->statistics().bytes_written) / 1'048'576.0,
                    static_cast<double>(m_pager->wal_statistics().bytes_read) / 1'048'576.0,
                    static_cast<double>(m_pager->wal_statistics().bytes_written) / 1'048'576.0,
                    m_pager->hits(),
                    m_pager->misses());
                out->append(buffer);
            }
            return true;
        }
    }
    return false;
}

static auto already_running_error() -> Status
{
    return Status::not_supported("transaction is already running");
}

auto DBImpl::checkpoint(bool reset) -> Status
{
    if (m_tx) {
        return already_running_error();
    }
    log(m_log, "running%s checkpoint", reset ? " reset" : "");
    return m_pager->checkpoint(reset);
}

template <class TxType>
auto DBImpl::prepare_tx(bool write, TxType *&tx_out) const -> Status
{
    tx_out = nullptr;
    if (m_tx) {
        return already_running_error();
    }

    // Forward error statuses. If an error is set at this point, then something
    // has gone very wrong.
    auto s = m_status;
    if (!s.is_ok()) {
        return s;
    }
    s = m_pager->start_reader();
    if (s.is_ok() && write) {
        s = m_pager->start_writer();
    }
    if (s.is_ok()) {
        m_tx = new TxImpl(*m_pager, m_status);
        m_tx->m_backref = &m_tx;
        tx_out = m_tx;
    } else {
        m_pager->finish();
    }
    return s;
}

auto DBImpl::new_tx(WriteTag, Tx *&tx_out) -> Status
{
    return prepare_tx(true, tx_out);
}

auto DBImpl::new_tx(const Tx *&tx_out) const -> Status
{
    return prepare_tx(false, tx_out);
}

auto DBImpl::TEST_pager() const -> const Pager &
{
    return *m_pager;
}

} // namespace calicodb
