#ifndef CALICO_PAGER_H
#define CALICO_PAGER_H

#include <unordered_set>

#include "page_cache.h"
#include "pager.h"

#include "utils/scratch.h"
#include "wal/helpers.h"

namespace Calico {

class Storage;
class FrameBuffer;

class Pager {
public:
    using Ptr = std::unique_ptr<Pager>;

    friend class Recovery;

    struct Parameters {
        std::string prefix;
        Storage *storage {};
        std::string *scratch {};
        WriteAheadLog *wal {};
        Logger *info_log {};
        Status *status {};
        Lsn *commit_lsn {};
        bool *in_txn {};
        Size frame_count {};
        Size page_size {};
    };

    ~Pager() = default;

    [[nodiscard]] static auto open(const Parameters &param) -> tl::expected<Pager::Ptr, Status>;
    [[nodiscard]] auto page_count() const -> Size;
    [[nodiscard]] auto page_size() const -> Size;
    [[nodiscard]] auto hit_ratio() const -> double;
    [[nodiscard]] auto recovery_lsn() -> Id;
    [[nodiscard]] auto bytes_written() const -> Size;
    [[nodiscard]] auto flush(Lsn target_lsn) -> Status;
    [[nodiscard]] auto sync() -> Status;
    [[nodiscard]] auto allocate() -> tl::expected<Page, Status>;
    [[nodiscard]] auto acquire(Id pid) -> tl::expected<Page, Status>;
    auto upgrade(Page &page) -> void;
    auto release(Page page) -> void;
    auto save_state(FileHeader &header) -> void;

    auto load_state(const FileHeader &header) -> void;

private:
    explicit Pager(const Parameters &param, FrameBuffer framer);
    [[nodiscard]] auto pin_frame(Id) -> Status;
    [[nodiscard]] auto try_make_available() -> tl::expected<bool, Status>;
    auto watch_page(Page &page, PageCache::Entry &entry) -> void;
    auto clean_page(PageCache::Entry &entry) -> PageList::Iterator;
    auto set_recovery_lsn(Lsn lsn) -> void;

    FrameBuffer m_framer;
    PageList m_dirty;
    PageCache m_registry;
    Lsn m_recovery_lsn;
    Lsn *m_commit_lsn {};
    bool *m_in_txn {};
    Status *m_status {};
    std::string *m_scratch {};
    WriteAheadLog *m_wal {};
    Logger *m_info_log {};
};

} // namespace Calico

#endif // CALICO_PAGER_H