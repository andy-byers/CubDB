#ifndef CCO_POOL_BUFFER_POOL_H
#define CCO_POOL_BUFFER_POOL_H

#include <list>
#include <mutex>
#include <unordered_map>
#include <spdlog/logger.h>
#include "page_cache.h"
#include "frame.h"
#include "interface.h"
#include "utils/scratch.h"

namespace cco {

class IDirectory;
class IFile;
class IWALManager;
class Pager;

class BufferPool: public IBufferPool {
public:
    struct Parameters {
        IDirectory &directory;
        spdlog::sink_ptr log_sink;
        LSN flushed_lsn;
        Size frame_count {};
        Size page_count {};
        Size page_size {};
        int permissions {};
        bool use_xact {};
    };

    ~BufferPool() override;

    [[nodiscard]] static auto open(const Parameters&) -> Result<std::unique_ptr<IBufferPool>>;

    [[nodiscard]] auto page_count() const -> Size override
    {
        return m_page_count;
    }

    [[nodiscard]] auto hit_ratio() const -> double override
    {
        return m_cache.hit_ratio();
    }

    [[nodiscard]] auto status() const -> Status override
    {
        return m_status;
    }

    auto clear_error() -> void override
    {
        m_status = Status::ok();
    }

    [[nodiscard]] auto page_size() const -> Size override;
    [[nodiscard]] auto allocate() -> Result<page::Page> override;
    [[nodiscard]] auto fetch(PID, bool) -> Result<page::Page> override;
    [[nodiscard]] auto acquire(PID, bool) -> Result<page::Page> override;
    [[nodiscard]] auto release(page::Page) -> Result<void> override;
    [[nodiscard]] auto recover() -> Result<void> override;
    [[nodiscard]] auto flush() -> Result<void> override;
    [[nodiscard]] auto close() -> Result<void> override;
    [[nodiscard]] auto commit() -> Result<void> override;
    [[nodiscard]] auto abort() -> Result<void> override;
    auto purge() -> void override;
    auto on_release(page::Page&) -> void override;
    auto save_header(page::FileHeader&) -> void override;
    auto load_header(const page::FileHeader&) -> void override;

private:
    explicit BufferPool(const Parameters&);
    [[nodiscard]] auto pin_frame(PID) -> Result<void>;
    [[nodiscard]] auto try_evict_frame() -> Result<bool>;
    [[nodiscard]] auto do_release(page::Page&) -> Result<void>;
    [[nodiscard]] auto has_updates() const -> bool;
    mutable std::mutex m_mutex;
    std::unique_ptr<Pager> m_pager;
    std::unique_ptr<IWALManager> m_wal;
    std::shared_ptr<spdlog::logger> m_logger;
    utils::ScratchManager m_scratch;
    PageCache m_cache;
    Status m_status {Status::ok()};
    Size m_page_count {};
    Size m_dirty_count {};
    Size m_ref_sum {};
    bool m_use_xact {};
};

} // calico

#endif // CCO_POOL_BUFFER_POOL_H
