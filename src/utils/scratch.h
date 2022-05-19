
#ifndef CUB_UTILS_SCRATCH_H
#define CUB_UTILS_SCRATCH_H

#include <list>
#include <string>
#include <unordered_map>
#include "common.h"
#include "slice.h"
#include "types.h"

namespace cub {

class ScratchManager;

class Scratch final {
public:
    Scratch(Index, MutBytes, ScratchManager*);
    ~Scratch();
    [[nodiscard]] auto id() const -> Index;
    [[nodiscard]] auto size() const -> Size;
    auto data() -> MutBytes;

    Scratch(Scratch&&) = default;
    auto operator=(Scratch&&) -> Scratch& = default;

private:
    struct Internal {
        MutBytes data;
        Index id{};
        ScratchManager *parent{};
    };
    Unique<Internal> m_internal;
};

class ScratchManager final {
public:
    explicit ScratchManager(Size);
    auto get() -> Scratch;

private:
    friend class Scratch;
    auto on_scratch_release(Scratch&) -> void;

    static constexpr auto MIN_SCRATCH_ID = 1UL;
    std::unordered_map<Index, std::string> m_pinned;
    std::list<std::string> m_available;
    Size m_scratch_size;
    Size m_id_counter{MIN_SCRATCH_ID};
};

} // cub

#endif // CUB_UTILS_SCRATCH_H
