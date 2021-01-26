/*
 * Boost Software License - Version 1.0 - August 17th, 2003
 *
 * Permission is hereby granted, free of charge, to any person or organization
 * obtaining a copy of the software and accompanying documentation covered by
 * this license (the "Software") to use, reproduce, display, distribute,
 * execute, and transmit the Software, and to prepare derivative works of the
 * Software, and to permit third-parties to whom the Software is furnished to
 * do so, all subject to the following:
 *
 * The copyright notices in the Software and this entire statement, including
 * the above license grant, this restriction and the following disclaimer,
 * must be included in all copies of the Software, in whole or in part, and
 * all derivative works of the Software, unless such copies or derivative
 * works are solely in the form of machine-executable object code generated by
 * a source language processor.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
 * SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
 * FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include <type_traits>
#include <utility>
#include <functional>

namespace stdext {

namespace inplace_function_detail {

static constexpr size_t InplaceFunctionDefaultCapacity = 32;

#if defined(__GLIBCXX__)  // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=61458
template<size_t Cap>
union aligned_storage_helper {
    struct double1 { double a; };
    struct double4 { double a[4]; };
    template<class T> using maybe = std::conditional_t<(Cap >= sizeof(T)), T, char>;
    char real_data[Cap];
    maybe<int> a;
    maybe<long> b;
    maybe<long long> c;
    maybe<void*> d;
    maybe<void(*)()> e;
    maybe<double1> f;
    maybe<double4> g;
    maybe<long double> h;
};

template<size_t Cap, size_t Align = std::alignment_of<aligned_storage_helper<Cap>>::value>
struct aligned_storage {
    using type = std::aligned_storage_t<Cap, Align>;
};

template<size_t Cap, size_t Align = std::alignment_of<aligned_storage_helper<Cap>>::value>
using aligned_storage_t = typename aligned_storage<Cap, Align>::type;
#else
using std::aligned_storage;
using std::aligned_storage_t;
#endif

template<typename T> struct wrapper
{
    using type = T;
};

template<typename R, typename... Args> struct vtable
{
    using storage_ptr_t = void*;

    using invoke_ptr_t = R(*)(storage_ptr_t, Args&&...);
    using process_ptr_t = void(*)(storage_ptr_t, storage_ptr_t);
    using destructor_ptr_t = void(*)(storage_ptr_t);

    const invoke_ptr_t invoke_ptr;
    const process_ptr_t copy_ptr;
    const process_ptr_t move_ptr;
    const destructor_ptr_t destructor_ptr;

    explicit constexpr vtable() noexcept :
        invoke_ptr{ [](storage_ptr_t, Args&&...) -> R
            { throw std::bad_function_call(); }
        },
        copy_ptr{ [](storage_ptr_t, storage_ptr_t) noexcept -> void {} },
        move_ptr{ [](storage_ptr_t, storage_ptr_t) noexcept -> void {} },
        destructor_ptr{ [](storage_ptr_t) noexcept -> void {} }
    {}

    template<typename C> explicit constexpr vtable(wrapper<C>) noexcept :
        invoke_ptr{ [](storage_ptr_t storage_ptr, Args&&... args)
            noexcept(noexcept(std::declval<C>()(args...))) -> R
            { return (*static_cast<C*>(storage_ptr))(
                std::forward<Args>(args)...
            ); }
        },
        copy_ptr{ [](storage_ptr_t dst_ptr, storage_ptr_t src_ptr)
            noexcept(std::is_nothrow_copy_constructible<C>::value) -> void
            { new (dst_ptr) C{ (*static_cast<C*>(src_ptr)) }; }
        },
        move_ptr{ [](storage_ptr_t dst_ptr, storage_ptr_t src_ptr)
            noexcept(std::is_nothrow_move_constructible<C>::value) -> void
            { new (dst_ptr) C{ std::move(*static_cast<C*>(src_ptr)) }; }
        },
        destructor_ptr{ [](storage_ptr_t storage_ptr)
            noexcept -> void
            { static_cast<C*>(storage_ptr)->~C(); }
        }
    {}

    vtable(const vtable&) = delete;
    vtable(vtable&&) = delete;

    vtable& operator= (const vtable&) = delete;
    vtable& operator= (vtable&&) = delete;

    ~vtable() = default;
};

template<typename R, typename... Args>
#if __cplusplus >= 201703L
inline constexpr
#endif
vtable<R, Args...> empty_vtable{};

template<size_t DstCap, size_t DstAlign, size_t SrcCap, size_t SrcAlign>
struct is_valid_inplace_dst : std::true_type
{
    static_assert(DstCap >= SrcCap,
        "Can't squeeze larger inplace_function into a smaller one"
    );

    static_assert(DstAlign % SrcAlign == 0,
        "Incompatible inplace_function alignments"
    );
};

} // namespace inplace_function_detail

template<
    typename Signature,
    size_t Capacity = inplace_function_detail::InplaceFunctionDefaultCapacity,
    size_t Alignment = std::alignment_of<inplace_function_detail::aligned_storage_t<Capacity>>::value
>
class inplace_function; // unspecified

template<
    typename R,
    typename... Args,
    size_t Capacity,
    size_t Alignment
>
class inplace_function<R(Args...), Capacity, Alignment>
{
public:
    using capacity = std::integral_constant<size_t, Capacity>;
    using alignment = std::integral_constant<size_t, Alignment>;

    using storage_t = inplace_function_detail::aligned_storage_t<Capacity, Alignment>;
    using vtable_t = inplace_function_detail::vtable<R, Args...>;
    using vtable_ptr_t = const vtable_t*;

    template <typename, size_t, size_t>	friend class inplace_function;

    inplace_function() noexcept :
        vtable_ptr_{std::addressof(inplace_function_detail::empty_vtable<R, Args...>)}
    {}

    template<
        typename T,
        typename C = std::decay_t<T>,
        typename = std::enable_if_t<
            !(std::is_same<C, inplace_function>::value
            || std::is_convertible<C, inplace_function>::value)
        >
    >
    inplace_function(T&& closure)
    {
#if __cplusplus >= 201703L
        static_assert(std::is_invocable_r<R, C, Args...>::value,
            "inplace_function cannot be constructed from non-callable type"
        );
#endif
        static_assert(std::is_copy_constructible<C>::value,
            "inplace_function cannot be constructed from non-copyable type"
        );

        static_assert(sizeof(C) <= Capacity,
            "inplace_function cannot be constructed from object with this (large) size"
        );

        static_assert(Alignment % std::alignment_of<C>::value == 0,
            "inplace_function cannot be constructed from object with this (large) alignment"
        );

        static const vtable_t vt{inplace_function_detail::wrapper<C>{}};
        vtable_ptr_ = std::addressof(vt);

        new (std::addressof(storage_)) C{std::forward<T>(closure)};
    }

    inplace_function(std::nullptr_t) noexcept :
        vtable_ptr_{std::addressof(inplace_function_detail::empty_vtable<R, Args...>)}
    {}

    inplace_function(const inplace_function& other) :
        vtable_ptr_{other.vtable_ptr_}
    {
        vtable_ptr_->copy_ptr(
            std::addressof(storage_),
            std::addressof(other.storage_)
        );
    }

    inplace_function(inplace_function&& other) :
        vtable_ptr_{other.vtable_ptr_}
    {
        vtable_ptr_->move_ptr(
            std::addressof(storage_),
            std::addressof(other.storage_)
        );
    }

    inplace_function& operator= (std::nullptr_t) noexcept
    {
        vtable_ptr_->destructor_ptr(std::addressof(storage_));
        vtable_ptr_ = std::addressof(inplace_function_detail::empty_vtable<R, Args...>);
        return *this;
    }

    inplace_function& operator= (const inplace_function& other)
    {
        if(this != std::addressof(other))
        {
            vtable_ptr_->destructor_ptr(std::addressof(storage_));

            vtable_ptr_ = other.vtable_ptr_;
            vtable_ptr_->copy_ptr(
                std::addressof(storage_),
                std::addressof(other.storage_)
            );
        }
        return *this;
    }

    inplace_function& operator= (inplace_function&& other)
    {
        if(this != std::addressof(other))
        {
            vtable_ptr_->destructor_ptr(std::addressof(storage_));

            vtable_ptr_ = other.vtable_ptr_;
            vtable_ptr_->move_ptr(
                std::addressof(storage_),
                std::addressof(other.storage_)
            );
        }
        return *this;
    }

    ~inplace_function()
    {
        vtable_ptr_->destructor_ptr(std::addressof(storage_));
    }

    R operator() (Args... args) const
    {
        return vtable_ptr_->invoke_ptr(
            std::addressof(storage_),
            std::forward<Args>(args)...
        );
    }

    constexpr bool operator== (std::nullptr_t) const noexcept
    {
        return !operator bool();
    }

    constexpr bool operator!= (std::nullptr_t) const noexcept
    {
        return operator bool();
    }

    explicit constexpr operator bool() const noexcept
    {
        return vtable_ptr_ != std::addressof(inplace_function_detail::empty_vtable<R, Args...>);
    }

    template<size_t Cap, size_t Align>
    operator inplace_function<R(Args...), Cap, Align>() const&
    {
        static_assert(inplace_function_detail::is_valid_inplace_dst<
            Cap, Align, Capacity, Alignment
        >::value, "conversion not allowed");

        return {vtable_ptr_, vtable_ptr_->copy_ptr, std::addressof(storage_)};
    }

    template<size_t Cap, size_t Align>
    operator inplace_function<R(Args...), Cap, Align>() &&
    {
        static_assert(inplace_function_detail::is_valid_inplace_dst<
            Cap, Align, Capacity, Alignment
        >::value, "conversion not allowed");

        return {vtable_ptr_, vtable_ptr_->move_ptr, std::addressof(storage_)};
    }

    void swap(inplace_function& other)
    {
        if (this == std::addressof(other)) return;

        storage_t tmp;
        vtable_ptr_->move_ptr(
            std::addressof(tmp),
            std::addressof(storage_)
        );
        vtable_ptr_->destructor_ptr(std::addressof(storage_));

        other.vtable_ptr_->move_ptr(
            std::addressof(storage_),
            std::addressof(other.storage_)
        );
        other.vtable_ptr_->destructor_ptr(std::addressof(other.storage_));

        vtable_ptr_->move_ptr(
            std::addressof(other.storage_),
            std::addressof(tmp)
        );
        vtable_ptr_->destructor_ptr(std::addressof(tmp));

        std::swap(vtable_ptr_, other.vtable_ptr_);
    }

private:
    vtable_ptr_t vtable_ptr_;
    mutable storage_t storage_;

    inplace_function(
        vtable_ptr_t vtable_ptr,
        typename vtable_t::process_ptr_t process_ptr,
        typename vtable_t::storage_ptr_t storage_ptr
    ) : vtable_ptr_{vtable_ptr}
    {
        process_ptr(std::addressof(storage_), storage_ptr);
    }
};

} // namespace stdext
