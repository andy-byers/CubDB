// Copyright (c) 2022, The CalicoDB Authors. All rights reserved.
// This source code is licensed under the MIT License, which can be found in
// LICENSE.md. See AUTHORS.md for a list of contributor names.

#ifndef CALICODB_UNIQUE_PTR_H
#define CALICODB_UNIQUE_PTR_H

#include "alloc.h"
#include "calicodb/db.h"
#include "calicodb/string.h"
#include "utils.h"

namespace calicodb
{

struct ObjectDestructor {
    template <class Object>
    auto operator()(Object *ptr) const -> void
    {
        Alloc::delete_object(ptr);
    }
};

struct UserObjectDestructor {
    template <class Object>
    auto operator()(Object *ptr) const -> void
    {
        delete ptr;
    }
};

struct DefaultDestructor {
    auto operator()(void *ptr) const -> void
    {
        Alloc::deallocate(ptr);
    }
};

template <class Object, class Destructor = DefaultDestructor>
class UniquePtr : private Destructor
{
    Object *m_ptr;

    auto destroy() const
    {
        Destructor::operator()(m_ptr);
    }

public:
    explicit UniquePtr(Object *ptr = nullptr)
        : m_ptr(ptr)
    {
    }

    template <class Dx>
    explicit UniquePtr(Object *ptr, Dx &&destructor)
        : Destructor(forward<Dx>(destructor)),
          m_ptr(ptr)
    {
    }

    ~UniquePtr()
    {
        destroy();
    }

    UniquePtr(const UniquePtr &rhs) noexcept = delete;
    auto operator=(const UniquePtr &rhs) noexcept -> UniquePtr & = delete;

    UniquePtr(UniquePtr &&rhs) noexcept
        : Destructor(rhs),
          m_ptr(exchange(rhs.m_ptr, nullptr))
    {
    }

    auto operator=(UniquePtr &&rhs) noexcept -> UniquePtr &
    {
        if (this != &rhs) {
            reset(rhs.release());
        }
        return *this;
    }

    auto operator*() -> Object &
    {
        return *m_ptr;
    }

    auto operator*() const -> const Object &
    {
        return *m_ptr;
    }

    auto operator->() -> Object *
    {
        return m_ptr;
    }

    auto operator->() const -> const Object *
    {
        return m_ptr;
    }

    operator bool() const
    {
        return m_ptr != nullptr;
    }

    [[nodiscard]] auto get() -> Object *
    {
        return m_ptr;
    }

    [[nodiscard]] auto get() const -> const Object *
    {
        return m_ptr;
    }

    [[nodiscard]] auto ref() -> Object *&
    {
        return m_ptr;
    }

    auto reset(Object *ptr = nullptr) -> void
    {
        destroy();
        m_ptr = ptr;
    }

    auto release() -> Object *
    {
        return exchange(m_ptr, nullptr);
    }
};

template <class Object>
using ObjectPtr = UniquePtr<Object, ObjectDestructor>;
template <class Object>
using UserPtr = UniquePtr<Object, UserObjectDestructor>;

} // namespace calicodb

#endif // CALICODB_UNIQUE_PTR_H