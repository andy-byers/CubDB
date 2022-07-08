
#include "database_impl.h"
#include "calico/cursor.h"
#include "storage/directory.h"

namespace calico {

namespace fs = std::filesystem;

auto Database::open(const std::string &path, Options options) -> Database
{
    Database db;
    db.m_impl = std::make_unique<Impl>(Impl::Parameters {
        std::make_unique<Directory>(path),
        options,
    });
    return db;
}

auto Database::temp(Options options) -> Database
{
    Impl::Parameters param;
    param.options = options;
    Database db;
    db.m_impl = std::make_unique<Impl>(std::move(param), Impl::InMemoryTag {});
    return db;
}

auto Database::destroy(Database db) -> void
{
    if (db.m_impl->is_temp())
        return;

    if (const auto &path = db.m_impl->path(); !path.empty())
        fs::remove_all(path);
}

Database::Database() = default;

Database::~Database() = default;

Database::Database(Database&&) noexcept = default;

auto Database::operator=(Database&&) noexcept -> Database& = default;

auto Database::contains(BytesView key) const -> bool
{
    return find_exact(key).is_valid();
}

auto Database::contains(const std::string &key) const -> bool
{
    return contains(stob(key));
}

auto Database::find_exact(BytesView key) const -> Cursor
{
    return m_impl->find_exact(key);
}

auto Database::find_exact(const std::string &key) const -> Cursor
{
    return find_exact(stob(key));
}

auto Database::find(BytesView key) const -> Cursor
{
    return m_impl->find(key);
}

auto Database::find(const std::string &key) const -> Cursor
{
    return m_impl->find(stob(key));
}

auto Database::find_minimum() const -> Cursor
{
    return m_impl->find_minimum();
}

auto Database::find_maximum() const -> Cursor
{
    return m_impl->find_maximum();
}

auto Database::insert(BytesView key, BytesView value) -> bool
{
    return m_impl->insert(key, value);
}

auto Database::insert(const std::string &key, const std::string &value) -> bool
{
    return insert(stob(key), stob(value));
}

auto Database::insert(const Record &record) -> bool
{
    const auto &[key, value] = record;
    return insert(key, value);
}

auto Database::erase(BytesView key) -> bool
{
    return m_impl->erase(key);
}

auto Database::erase(const std::string &key) -> bool
{
    return erase(stob(key));
}

auto Database::erase(Cursor cursor) -> bool
{
    return m_impl->erase(cursor);
}

auto Database::commit() -> bool
{
    return m_impl->commit();
}

auto Database::abort() -> bool
{
    return m_impl->abort();
}

auto Database::info() const -> Info
{
    return m_impl->info();
}

} // calico