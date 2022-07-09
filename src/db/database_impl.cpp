
#include "database_impl.h"
#include <filesystem>
#include "calico/cursor.h"
#include "calico/exception.h"
#include "page/file_header.h"
#include "pool/buffer_pool.h"
#include "pool/in_memory.h"
#include "storage/directory.h"
#include "storage/file.h"
#include "storage/io.h"
#include "tree/tree.h"
#include "utils/layout.h"
#include "utils/logging.h"
#include "wal/wal_reader.h"
#include "wal/wal_writer.h"

namespace calico {

namespace fs = std::filesystem;

auto initialize_log(spdlog::logger &logger, const std::string &base)
{
    logger.info("starting CalicoDB v{} at \"{}\"", VERSION_NAME, base);
    logger.info("tree is located at \"{}/{}\"", base, DATA_NAME);
    logger.info("WAL is located at \"{}/{}\"", base, WAL_NAME);
    logger.info("log is located at \"{}/{}\"", base, logging::LOG_NAME);
}

auto Record::operator<(const Record &rhs) const -> bool
{
    return stob(key) < stob(rhs.key);
}

auto Info::cache_hit_ratio() const -> double
{
    return m_db->cache_hit_ratio();
}

auto Info::record_count() const -> Size
{
    return m_db->record_count();
}

auto Info::page_count() const -> Size
{
    return m_db->page_count();
}

auto Info::page_size() const -> Size
{
    return m_db->page_size();
}

auto Info::maximum_key_size() const -> Size
{
    return get_max_local(page_size());
}

auto Info::uses_transactions() const -> bool
{
    return m_db->uses_transactions();
}

auto Info::is_temp() const -> bool
{
    return m_db->is_temp();
}

Database::Impl::~Impl()
{
    auto committed = true;

    try {
        commit();
    } catch (const std::exception &error) {
        logging::MessageGroup group;
        group.set_primary("cannot commit");
        group.set_detail("{}", error.what());
        group.error(*m_logger);
        committed = false;
    }

    try {
        if (uses_transactions() && committed && !m_is_temp) {
            const fs::path base {m_directory->path()};
            fs::resize_file(base / WAL_NAME, 0);
        }
    } catch (const std::exception &error) {
        logging::MessageGroup group;
        group.set_primary("cannot unlink WAL");
        group.set_detail("{}", error.what());
        group.error(*m_logger);
    }
}

Database::Impl::Impl(Parameters param)
    : m_sink {logging::create_sink(param.directory->path(), param.options.log_level)},
      m_logger {logging::create_logger(m_sink, "Database")},
      m_directory {std::move(param.directory)}
{
    initialize_log(*m_logger, m_directory->path());
    m_logger->trace("constructing Database object");
    auto [state, revised, is_new] = setup(m_directory->path(), param.options, *m_logger);
    std::unique_ptr<WALReader> wal_reader;
    std::unique_ptr<WALWriter> wal_writer;

    if (revised.use_transactions) {
        wal_reader = std::make_unique<WALReader>(WALReader::Parameters {
            *m_directory,
            m_sink,
            state.block_size(),
        });
        wal_writer = std::make_unique<WALWriter>(WALWriter::Parameters {
            *m_directory,
            m_sink,
            state.block_size(),
        });
    }

    m_pool = std::make_unique<BufferPool>(BufferPool::Parameters {
        *m_directory,
        std::move(wal_reader),
        std::move(wal_writer),
        m_sink,
        state.flushed_lsn(),
        revised.frame_count,
        state.page_count(),
        state.page_size(),
        revised.permissions,
        revised.use_transactions,
    });

    m_tree = std::make_unique<Tree>(Tree::Parameters {
        m_pool.get(),
        m_sink,
        state.free_start(),
        state.free_count(),
        state.record_count(),
        state.node_count(),
    });

    if (is_new) {
        {
            auto root = m_tree->root(true);
            FileHeader header {root};
            header.update_magic_code();
            header.set_page_size(state.page_size());
            header.set_block_size(state.block_size());
        }
        commit();
    } else if (revised.use_transactions) {
        // This will do nothing if the WAL is empty.
        m_pool->recover();
    }
}

Database::Impl::Impl(Parameters param, InMemoryTag)
    : m_sink {logging::create_sink("", spdlog::level::off)},
      m_logger {logging::create_logger(m_sink, "Database")},
      m_pool {std::make_unique<InMemory>(param.options.page_size, param.options.use_transactions, m_sink)},
      m_tree {std::make_unique<Tree>(Tree::Parameters {m_pool.get(), m_sink, PID::null(), 0, 0, 0})},
      m_is_temp {true}
{
    if (param.options.use_transactions)
        commit();
}

auto Database::Impl::path() const -> std::string
{
    return m_directory->path();
}

auto Database::Impl::recover() -> void
{
    if (m_pool->recover())
        load_header();
}

auto Database::Impl::info() -> Info
{
    Info info;
    info.m_db = this;
    return info;
}

inline auto check_key(BytesView key, spdlog::logger &logger)
{
    if (key.is_empty()) {
        logging::MessageGroup group;
        group.set_primary("cannot read record");
        group.set_detail("key cannot be empty");
        throw std::invalid_argument {group.error(logger)};
    }
}

auto Database::Impl::find_exact(BytesView key) -> Cursor
{
    check_key(key, *m_logger);
    return m_tree->find_exact(key);
}

auto Database::Impl::find(BytesView key) -> Cursor
{
    check_key(key, *m_logger);
    return m_tree->find(key);
}

auto Database::Impl::find_minimum() -> Cursor
{
    return m_tree->find_minimum();
}

auto Database::Impl::find_maximum() -> Cursor
{
    return m_tree->find_maximum();
}

auto Database::Impl::insert(BytesView key, BytesView value) -> bool
{
    return m_tree->insert(key, value);
}

auto Database::Impl::erase(BytesView key) -> bool
{
    return erase(m_tree->find_exact(key));
}

auto Database::Impl::erase(Cursor cursor) -> bool
{
    return m_tree->erase(cursor);
}

auto Database::Impl::commit() -> bool
{
    if (m_pool->can_commit()) {
        save_header();
        m_pool->commit();
        return true;
    }
    return false;
}

auto Database::Impl::abort() -> bool
{
    if (!m_pool->uses_transactions()) {
        logging::MessageGroup group;
        group.set_primary("cannot abort transaction");
        group.set_detail("transactions are disabled");
        throw std::logic_error {group.error(*m_logger)};
    }

    if (m_pool->can_commit()) {
        m_pool->abort();
        load_header();
        return true;
    }
    return false;
}

auto Database::Impl::save_header() -> void
{
    auto root = m_tree->root(true);
    FileHeader header {root};
    m_pool->save_header(header);
    m_tree->save_header(header);
    header.update_header_crc();
}

auto Database::Impl::load_header() -> void
{
    auto root = m_tree->root(true);
    FileHeader header {root};
    m_pool->load_header(header);
    m_tree->load_header(header);
}

auto Database::Impl::cache_hit_ratio() const -> double
{
    return m_pool->hit_ratio();
}

auto Database::Impl::record_count() const -> Size
{
    return m_tree->cell_count();
}

auto Database::Impl::page_count() const -> Size
{
    return m_pool->page_count();
}

auto Database::Impl::page_size() const -> Size
{
    return m_pool->page_size();
}

auto Database::Impl::uses_transactions() const -> Size
{
    return m_pool->uses_transactions();
}

auto Database::Impl::is_temp() const -> bool
{
    return m_is_temp;
}

auto setup(const std::string &path, const Options &options, spdlog::logger &logger) -> InitialState
{
    static constexpr auto ERROR_PRIMARY = "cannot open database";
    logging::MessageGroup group;
    group.set_primary(ERROR_PRIMARY);

    if (path.empty()) {
        group.set_detail("path argument cannot be empty");
        throw std::invalid_argument {group.error(logger)};
    }

    auto directory = std::make_unique<Directory>(path);
    const auto perm = options.permissions;
    auto revised = options;
    FileHeader header;
    bool is_new {};

    try {
        auto file = directory->open_file(DATA_NAME, Mode::READ_ONLY, perm);
        auto reader = file->open_reader();
        const auto file_size = file->size();

        if (file_size < FileLayout::HEADER_SIZE) {
            group.set_detail("database is too small to read the file header");
            group.set_hint("file header is {} B", FileLayout::HEADER_SIZE);
            throw CorruptionError {group.error(logger)};
        }
        if (!read_exact(*reader, header.data())) {
            group.set_detail("cannot read file header");
            throw std::runtime_error {group.error(logger)};
        }
        if (file_size % header.page_size()) {
            group.set_detail("database has an invalid size");
            group.set_hint("database must contain an integral number of pages");
            throw CorruptionError {group.error(logger)};
        }
        if (!header.is_magic_code_consistent()) {
            group.set_detail("path does not point to a Calico DB database");
            group.set_hint("magic code is {}, but should be {}", header.magic_code(), FileHeader::MAGIC_CODE);
            throw std::invalid_argument {group.error(logger)};
        }
        if (!header.is_header_crc_consistent()) {
            group.set_detail("header has an inconsistent CRC");
            group.set_hint("CRC is {}", header.header_crc());
            throw CorruptionError {group.error(logger)};
        }

        // If the database does not use transactions, this field will always be 0.
        revised.use_transactions = !header.flushed_lsn().is_null();

    } catch (const std::system_error &error) {
        if (error.code() != std::errc::no_such_file_or_directory)
            throw;

        header.update_magic_code();
        header.set_page_size(options.page_size);
        header.set_block_size(options.block_size);
        header.update_header_crc();
        is_new = true;
    }

    const auto choose_error = [is_new, &logger](logging::MessageGroup group) {
        if (is_new)
            throw std::invalid_argument {group.error(logger)};
        throw CorruptionError {group.error(logger)};
    };

    if (header.page_size() < MINIMUM_PAGE_SIZE) {
        group.set_detail("page size is too small");
        group.set_hint("must be greater than or equal to {}", MINIMUM_PAGE_SIZE);
        choose_error(group);
    }
    if (header.page_size() > MAXIMUM_PAGE_SIZE) {
        group.set_detail("page size is too large");
        group.set_hint("must be less than or equal to {}", MAXIMUM_PAGE_SIZE);
        choose_error(group);
    }
    if (!is_power_of_two(header.page_size())) {
        group.set_detail("page size is invalid");
        group.set_hint("must be a power of 2");
        choose_error(group);
    }

    return {
        std::move(header),
        revised,
        is_new,
    };
}

} // calico

auto operator<(const calico::Record &lhs, const calico::Record &rhs) -> bool
{
    return calico::stob(lhs.key) < calico::stob(rhs.key);
}

auto operator>(const calico::Record &lhs, const calico::Record &rhs) -> bool
{
    return calico::stob(lhs.key) > calico::stob(rhs.key);
}

auto operator<=(const calico::Record &lhs, const calico::Record &rhs) -> bool
{
    return calico::stob(lhs.key) <= calico::stob(rhs.key);
}

auto operator>=(const calico::Record &lhs, const calico::Record &rhs) -> bool
{
    return calico::stob(lhs.key) >= calico::stob(rhs.key);
}

auto operator==(const calico::Record &lhs, const calico::Record &rhs) -> bool
{
    return calico::stob(lhs.key) == calico::stob(rhs.key);
}

auto operator!=(const calico::Record &lhs, const calico::Record &rhs) -> bool
{
    return calico::stob(lhs.key) != calico::stob(rhs.key);
}
