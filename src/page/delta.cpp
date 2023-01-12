#include "delta.h"
#include <algorithm>

namespace Calico {

static auto can_merge_ordered_deltas(const PageDelta &lhs, const PageDelta &rhs) -> bool
{
    CALICO_EXPECT_LE(lhs.offset, rhs.offset);
    return rhs.offset <= lhs.offset + lhs.size;
}

static auto merge_deltas(const PageDelta &lhs, const PageDelta &rhs) -> PageDelta
{
    const auto rhs_end = rhs.offset + rhs.size;
    const auto new_dx = std::max(lhs.size, rhs_end - lhs.offset);
    return PageDelta {lhs.offset, new_dx};
}

auto compress_deltas(std::vector<PageDelta> &deltas) -> Size
{
    if (deltas.empty()) {
        return 0;
    } else if (deltas.size() == 1) {
        return deltas.front().size;
    }

    auto lhs = begin(deltas);
    for (auto rhs = next(lhs); rhs != end(deltas); ++rhs) {
        if (can_merge_ordered_deltas(*lhs, *rhs)) {
            *lhs = merge_deltas(*lhs, *rhs);
        } else {
            lhs++;
            *lhs = *rhs;
        }
    }
    deltas.erase(next(lhs), end(deltas));

    return std::accumulate(begin(deltas), end(deltas), Size {}, [](auto accum, auto delta) {
        return accum + delta.size + sizeof(delta);
    });
}

auto insert_delta(std::vector<PageDelta> &deltas, PageDelta delta) -> void
{
    CALICO_EXPECT_GT(delta.size, 0);
    if (deltas.empty()) {
        deltas.emplace_back(delta);
        return;
    }
    auto itr = std::upper_bound(begin(deltas), end(deltas), delta, [](const auto &lhs, const auto &rhs) {
        return lhs.offset <= rhs.offset;
    });
    const auto try_merge = [&itr](const auto &lhs, const auto &rhs) {
        if (can_merge_ordered_deltas(lhs, rhs)) {
            *itr = merge_deltas(lhs, rhs);
            return true;
        }
        return false;
    };
    if (itr != end(deltas)) {
        if (try_merge(delta, *itr))
            return;
    }
    if (itr != begin(deltas)) {
        itr--;
        if (try_merge(*itr, delta))
            return;
        itr++;
    }
    deltas.insert(itr, delta);
}

} // namespace Calico