
#include "registry.h"

namespace calico {

auto PageRegistry::put(PageId pid, FrameNumber fid) -> void
{
    CALICO_EXPECT_FALSE(m_warm.contains(pid));
    CALICO_EXPECT_FALSE(m_hot.contains(pid));
    m_warm.put(pid, Entry {fid});
}

auto PageRegistry::get(PageId id) -> Iterator
{
    if (auto itr = m_hot.get(id); itr != m_hot.end()) {
        m_hits++;
        return itr;
    }
    if (auto itr = m_warm.extract(id)) {
        m_hits++;
        m_hot.put(id, std::move(*itr));
        return m_hot.get(id);
    }
    m_misses++;
    return end();
}

auto PageRegistry::erase(PageId id) -> void
{
    if (m_hot.extract(id))
        return;
    if (m_warm.extract(id))
        return;
    CALICO_EXPECT_TRUE(false && "erase(): cannot find entry");
}

} // namespace calico