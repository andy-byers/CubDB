// Copyright (c) 2022, The CalicoDB Authors. All rights reserved.
// This source code is licensed under the MIT License, which can be found in
// LICENSE.md. See AUTHORS.md for a list of contributor names.

#ifndef CALICODB_TOOLS_TEST_ENV_H
#define CALICODB_TOOLS_TEST_ENV_H

#include "tools.h"

namespace calicodb::tools
{

class FakeEnv : public Env
{
public:
    [[nodiscard]] virtual auto clone() const -> Env *;
    [[nodiscard]] virtual auto get_file_contents(const std::string &filename) const -> std::string;
    virtual auto put_file_contents(const std::string &filename, std::string contents) -> void;

    ~FakeEnv() override = default;
    [[nodiscard]] auto new_file(const std::string &filename, File *&out) -> Status override;
    [[nodiscard]] auto new_log_file(const std::string &filename, LogFile *&out) -> Status override;
    [[nodiscard]] auto file_exists(const std::string &filename) const -> bool override;
    [[nodiscard]] auto resize_file(const std::string &filename, std::size_t size) -> Status override;
    [[nodiscard]] auto file_size(const std::string &filename, std::size_t &out) const -> Status override;
    [[nodiscard]] auto remove_file(const std::string &filename) -> Status override;

    auto srand(unsigned seed) -> void override;
    [[nodiscard]] auto rand() -> unsigned override;

    [[nodiscard]] auto lock(File &, LockMode) -> Status override {return Status::ok();}
    [[nodiscard]] auto unlock(File &) -> Status override {return Status::ok();}

protected:
    friend class FakeFile;
    friend class FakeLogFile;
    friend class TestEnv;

    struct FileState {
        std::string buffer;
        bool created = false;
    };

    [[nodiscard]] auto open_or_create_file(const std::string &filename) const -> FileState &;
    [[nodiscard]] auto read_file_at(const FileState &mem, std::size_t offset, std::size_t size, char *scratch, Slice *out) -> Status;
    [[nodiscard]] auto write_file_at(FileState &mem, std::size_t offset, const Slice &in) -> Status;

    mutable std::unordered_map<std::string, FileState> m_state;
};

class FakeFile : public File
{
public:
    FakeFile(std::string filename, FakeEnv &env, FakeEnv::FileState &mem)
        : m_state(&mem),
          m_env(&env),
          m_filename(std::move(filename))
    {
    }

    ~FakeFile() override = default;
    [[nodiscard]] auto read(std::size_t offset, std::size_t size, char *scratch, Slice *out) -> Status override;
    [[nodiscard]] auto write(std::size_t offset, const Slice &in) -> Status override;
    [[nodiscard]] auto sync() -> Status override;

    [[nodiscard]] auto env() -> FakeEnv &
    {
        return *m_env;
    }

    [[nodiscard]] auto env() const -> const FakeEnv &
    {
        return *m_env;
    }

    [[nodiscard]] auto filename() -> const std::string &
    {
        return m_filename;
    }

protected:
    FakeEnv::FileState *m_state = nullptr;
    FakeEnv *m_env = nullptr;
    std::string m_filename;
};

class FakeLogFile : public LogFile
{
public:
    ~FakeLogFile() override = default;
    auto write(const Slice &) -> void override {}
};

struct Interceptor {
    enum Type {
        kRead,
        kWrite,
        kOpen,
        kSync,
        kUnlink,
        kResize,
        kTypeCount
    };

    using Callback = std::function<Status()>;

    explicit Interceptor(Type t, Callback c)
        : callback(std::move(c)),
          type(t)
    {
    }

    [[nodiscard]] auto operator()() const -> Status
    {
        return callback();
    }

    Callback callback;
    Type type;
};

class TestEnv : public EnvWrapper
{
public:
    explicit TestEnv();
    explicit TestEnv(Env &env);
    ~TestEnv() override;

    // NOTE: clone() always clones files into a FakeEnv, and only works properly if
    //       the wrapped Env was empty when passed to the constructor.
    [[nodiscard]] virtual auto clone() -> Env *;

    // The TestFile wrapper reads the whole file and saves it in memory after a
    // successful call to sync().
    virtual auto drop_after_last_sync(const std::string &filename) -> void;
    virtual auto drop_after_last_sync() -> void;

    virtual auto add_interceptor(const std::string &filename, Interceptor interceptor) -> void;
    virtual auto clear_interceptors() -> void;
    virtual auto clear_interceptors(const std::string &filename) -> void;

    [[nodiscard]] auto new_file(const std::string &filename, File *&out) -> Status override;
    [[nodiscard]] auto resize_file(const std::string &filename, std::size_t file_size) -> Status override;
    [[nodiscard]] auto remove_file(const std::string &filename) -> Status override;

private:
    friend class TestFile;
    friend class CrashFile;
    friend class CrashEnv;

    struct FileState {
        std::vector<Interceptor> interceptors;
        std::string saved_state;
        bool unlinked = false;
    };

    mutable std::unordered_map<std::string, FileState> m_state;

    [[nodiscard]] auto try_intercept_syscall(Interceptor::Type type, const std::string &filename) -> Status;
    auto save_file_contents(const std::string &filename) -> void;
    auto overwrite_file(const std::string &filename, const std::string &contents) -> void;
};

class TestFile : public File
{
    friend class TestEnv;

    std::string m_filename;
    TestEnv *m_env = nullptr;
    File *m_file = nullptr;

    explicit TestFile(std::string filename, File &file, TestEnv &env);

public:
    ~TestFile() override;
    [[nodiscard]] auto read(std::size_t offset, std::size_t size, char *scratch, Slice *out) -> Status override;
    [[nodiscard]] auto write(std::size_t offset, const Slice &in) -> Status override;
    [[nodiscard]] auto sync() -> Status override;
};

class TestLogFile : public LogFile
{
public:
    ~TestLogFile() override = default;
    auto write(const Slice &) -> void override {}
};

class StderrLog : public LogFile
{
public:
    ~StderrLog() override = default;

    auto write(const Slice &in) -> void override
    {
        std::fputs(in.to_string().c_str(), stderr);
        if (in.data()[in.size() - 1] != '\n') {
            std::fputc('\n', stderr);
        }
        std::fflush(stderr);
    }
};

} // namespace calicodb::tools

#endif // CALICODB_TOOLS_TEST_ENV_H