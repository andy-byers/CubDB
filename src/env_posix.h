#ifndef CALICODB_ENV_POSIX_H
#define CALICODB_ENV_POSIX_H

#include "calicodb/env.h"
#include "calicodb/slice.h"
#include "calicodb/status.h"
#include "utils.h"

namespace calicodb
{

class PosixReader : public Reader
{
public:
    explicit PosixReader(std::string path, int file)
        : m_path {std::move(path)},
          m_file {file}
    {
        CDB_EXPECT_GE(file, 0);
    }

    ~PosixReader() override;
    [[nodiscard]] auto read(char *out, std::size_t &size, std::size_t offset) -> Status override;

private:
    std::string m_path;
    int m_file {};
};

class PosixEditor : public Editor
{
public:
    explicit PosixEditor(std::string path, int file)
        : m_path {std::move(path)},
          m_file {file}
    {
        CDB_EXPECT_GE(file, 0);
    }

    ~PosixEditor() override;
    [[nodiscard]] auto read(char *out, std::size_t &size, std::size_t offset) -> Status override;
    [[nodiscard]] auto write(Slice in, std::size_t size) -> Status override;
    [[nodiscard]] auto sync() -> Status override;

private:
    std::string m_path;
    int m_file {};
};

class PosixLogger : public Logger
{
public:
    explicit PosixLogger(std::string path, int file)
        : m_path {std::move(path)},
          m_file {file}
    {
        CDB_EXPECT_GE(file, 0);
    }

    ~PosixLogger() override;
    [[nodiscard]] auto write(Slice in) -> Status override;
    [[nodiscard]] auto sync() -> Status override;

private:
    std::string m_path;
    int m_file {};
};

class PosixInfoLogger : public InfoLogger
{
public:
    explicit PosixInfoLogger(std::string path, int file)
        : m_buffer(BUFFER_SIZE, '\0'),
          m_path {std::move(path)},
          m_file {file}
    {
        CDB_EXPECT_GE(file, 0);
    }

    ~PosixInfoLogger() override;
    auto logv(const char *fmt, ...) -> void override;

private:
    static constexpr std::size_t BUFFER_SIZE {512};
    std::string m_buffer;
    std::string m_path;
    int m_file {};
};

class EnvPosix : public Env
{
public:
    EnvPosix() = default;
    ~EnvPosix() override = default;
    [[nodiscard]] auto create_directory(const std::string &path) -> Status override;
    [[nodiscard]] auto remove_directory(const std::string &path) -> Status override;
    [[nodiscard]] auto get_children(const std::string &path, std::vector<std::string> &out) const -> Status override;
    [[nodiscard]] auto new_reader(const std::string &path, Reader **out) -> Status override;
    [[nodiscard]] auto new_editor(const std::string &path, Editor **out) -> Status override;
    [[nodiscard]] auto new_logger(const std::string &path, Logger **out) -> Status override;
    [[nodiscard]] auto new_info_logger(const std::string &path, InfoLogger **out) -> Status override;
    [[nodiscard]] auto rename_file(const std::string &old_path, const std::string &new_path) -> Status override;
    [[nodiscard]] auto remove_file(const std::string &path) -> Status override;
    [[nodiscard]] auto resize_file(const std::string &path, std::size_t size) -> Status override;
    [[nodiscard]] auto file_exists(const std::string &path) const -> Status override;
    [[nodiscard]] auto file_size(const std::string &path, std::size_t &out) const -> Status override;
};

} // namespace calicodb

#endif // CALICODB_ENV_POSIX_H