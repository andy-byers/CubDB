/*
 * Slice objects based off of https://github.com/google/leveldb/blob/main/include/leveldb/slice.h.
 */

#ifndef CALICO_SLICE_H
#define CALICO_SLICE_H

#include <cassert>
#include <cstring>
#include <string>
#include "common.h"

namespace Calico {

enum class ThreeWayComparison {
    LT = -1,
    EQ = 0,
    GT = 1,
};

template<class T>
concept CanSlice = requires(T t)
{
    {t.data()} -> std::convertible_to<Byte const *>;
    {t.size()} -> std::convertible_to<Size>;
};

class Slice {
public:
    constexpr Slice() noexcept = default;

    constexpr Slice(const Byte *data, Size size) noexcept
        : m_data {data},
          m_size {size}
    {}

    constexpr Slice(const Byte *data) noexcept
        : m_data {data}
    {
        if (m_data != nullptr)
            m_size = std::char_traits<Byte>::length(m_data);
    }

    template<CanSlice T>
    constexpr Slice(const T &rhs) noexcept
        : Slice {rhs.data(), rhs.size()} {}

    [[nodiscard]]
    constexpr auto is_empty() const noexcept -> bool
    {
        return m_size == 0;
    }

    [[nodiscard]]
    constexpr auto data() const noexcept -> const Byte *
    {
        return m_data;
    }

    [[nodiscard]]
    constexpr auto size() const noexcept -> Size
    {
        return m_size;
    }

    constexpr auto operator[](Size index) const noexcept -> const Byte &
    {
        assert(index < m_size);
        return m_data[index];
    }

    [[nodiscard]]
    constexpr auto range(Size offset, Size size) const noexcept -> Slice
    {
        assert(size <= m_size);
        assert(offset <= m_size);
        assert(offset + size <= m_size);

        return Slice {m_data + offset, size};
    }

    [[nodiscard]]
    constexpr auto range(Size offset) const noexcept -> Slice
    {
        assert(offset <= m_size);
        return range(offset, m_size - offset);
    }

    [[nodiscard]]
    constexpr auto copy() const noexcept -> Slice
    {
        return *this;
    }

    constexpr auto clear() noexcept -> void
    {
        m_data = nullptr;
        m_size = 0;
    }

    constexpr auto advance(Size n = 1) noexcept -> Slice
    {
        assert(n <= m_size);
        m_data += n;
        m_size -= n;
        return *this;
    }

    constexpr auto truncate(Size size) noexcept -> Slice
    {
        assert(size <= m_size);
        m_size = size;
        return *this;
    }

    [[nodiscard]]
    constexpr auto starts_with(const Byte *rhs) const noexcept -> bool
    {
        // NOTE: rhs must be null-terminated.
        const auto size = std::char_traits<Byte>::length(rhs);
        if (size > m_size)
            return false;
        return std::memcmp(m_data, rhs, size) == 0;
    }

    [[nodiscard]]
    constexpr auto starts_with(Slice rhs) const noexcept -> bool
    {
        if (rhs.m_size > m_size)
            return false;
        return std::memcmp(m_data, rhs.data(), rhs.m_size) == 0;
    }

    [[nodiscard]]
    auto to_string() const noexcept -> std::string
    {
        return {m_data, m_size};
    }

private:
    Byte const *m_data {};
    Size m_size {};
};

/*
 * Three-way comparison based off the one in LevelDB's slice.h.
 */
inline auto compare_three_way(Slice lhs, Slice rhs) noexcept -> ThreeWayComparison
{
    const auto min_length = lhs.size() < rhs.size() ? lhs.size() : rhs.size();
    auto r = std::memcmp(lhs.data(), rhs.data(), min_length);
    if (r == 0) {
        if (lhs.size() < rhs.size()) {
            r = -1;
        } else if (lhs.size() > rhs.size()) {
            r = 1;
        } else {
            return ThreeWayComparison::EQ;
        }
    }
    return r < 0 ? ThreeWayComparison::LT : ThreeWayComparison::GT;
}

} // namespace Calico

inline auto operator<(Calico::Slice lhs, Calico::Slice rhs) noexcept -> bool
{
    return Calico::compare_three_way(lhs, rhs) == Calico::ThreeWayComparison::LT;
}

inline auto operator<=(Calico::Slice lhs, Calico::Slice rhs) noexcept -> bool
{
    return Calico::compare_three_way(lhs, rhs) != Calico::ThreeWayComparison::GT;
}

inline auto operator>(Calico::Slice lhs, Calico::Slice rhs) noexcept -> bool
{
    return Calico::compare_three_way(lhs, rhs) == Calico::ThreeWayComparison::GT;
}

inline auto operator>=(Calico::Slice lhs, Calico::Slice rhs) noexcept -> bool
{
    return Calico::compare_three_way(lhs, rhs) != Calico::ThreeWayComparison::LT;
}

inline auto operator==(Calico::Slice lhs, Calico::Slice rhs) noexcept -> bool
{
    return Calico::compare_three_way(lhs, rhs) == Calico::ThreeWayComparison::EQ;
}

inline auto operator!=(Calico::Slice lhs, Calico::Slice rhs) noexcept -> bool
{
    return !(lhs == rhs);
}

#endif // CALICO_SLICE_H