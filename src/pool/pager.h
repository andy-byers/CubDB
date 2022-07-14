#ifndef CCO_POOL_PAGER_H
#define CCO_POOL_PAGER_H

#include <list>
#include <memory>
#include <optional>
#include <spdlog/spdlog.h>
#include "calico/error.h"

namespace cco {

class Frame;
class IFileReader;
class IFileWriter;
struct PID;

class Pager final {
public:
    struct Parameters {
        std::unique_ptr<IFileReader> reader;
        std::unique_ptr<IFileWriter> writer;
        spdlog::sink_ptr log_sink;
        Size page_size {};
        Size frame_count {};
    };

    struct AlignedDeleter {

        explicit AlignedDeleter(std::align_val_t alignment)
            : align {alignment} {}

        auto operator()(Byte *ptr) const -> void
        {
            operator delete[](ptr, align);
        }

        std::align_val_t align;
    };

    using AlignedBuffer = std::unique_ptr<Byte[], AlignedDeleter>;

    ~Pager() = default;
    [[nodiscard]] static auto open(Parameters) -> Result<std::unique_ptr<Pager>>;
    [[nodiscard]] auto available() const -> Size;
    [[nodiscard]] auto page_size() const -> Size;
    [[nodiscard]] auto pin(PID) -> Result<Frame>;
    [[nodiscard]] auto unpin(Frame) -> Result<void>;
    [[nodiscard]] auto discard(Frame) -> Result<void>;
    [[nodiscard]] auto truncate(Size) -> Result<void>;
    [[nodiscard]] auto sync() -> Result<void>;

    auto operator=(Pager&&) -> Pager& = default;
    Pager(Pager&&) = default;

private:
    Pager(AlignedBuffer, Parameters);
    [[nodiscard]] auto read_page_from_file(PID, Bytes) const -> Result<bool>;
    [[nodiscard]] auto write_page_to_file(PID, BytesView) const -> Result<void>;

    AlignedBuffer m_buffer;
    std::list<Frame> m_available;
    std::unique_ptr<IFileReader> m_reader;
    std::unique_ptr<IFileWriter> m_writer;
    std::shared_ptr<spdlog::logger> m_logger;
    Size m_frame_count{};
    Size m_page_size{};
};

} // calico

#endif // CCO_POOL_PAGER_H
