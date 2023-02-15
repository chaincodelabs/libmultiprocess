// Copyright (c) 2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MP_UTIL_H
#define MP_UTIL_H

#include <capnp/schema.h>
#include <cstddef>
#include <future>
#include <kj/common.h>
#include <kj/exception.h>
#include <kj/string-tree.h>
#include <memory>
#include <string.h>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace mp {

//! Generic utility functions used by capnp code. This mostly consists of
//! helpers that work around lack of C++14 functionality in C++11. This file
//! puts all C++14 workarounds in one place, so if/when bitcoin decides to
//! decides to upgrade to C++14, code can be accordingly simplified.
//!
//! C++14 has two features that really simplify generic programming. One is
//! auto-returning functions
//! (http://en.cppreference.com/w/cpp/language/template_argument_deduction#auto-returning_functions):
//!
//!    auto DoSomething(Arg arg) {
//!        return expression(arg);
//!    }
//!
//! which in c++11 has to be written:
//!
//!    auto DoSomething(Arg arg) -> decltype(expression(arg)) {
//!        return expression(arg);
//!    }
//!
//! Another is generic lambdas (http://en.cppreference.com/w/cpp/language/lambda):
//!
//!    [capture](auto arg) { do_something(arg); }
//!
//! which in c++11 has to be written like
//!
//!    struct DoSomething {
//!        Capture m_capture;
//!
//!        template<typename Arg>
//!        void operator()(Arg arg) {
//!            return do_something(arg);
//!        }
//!     };

//! Type holding a list of types.
//!
//! Example:
//!   TypeList<int, bool, void>
template <typename... Types>
struct TypeList
{
    static constexpr size_t size = sizeof...(Types);
};

//! Construct a template class value by deducing template arguments from the
//! types of constructor arguments, so they don't need to be specified manually.
//!
//! Uses of this can go away with class template deduction in C++17
//! (https://en.cppreference.com/w/cpp/language/class_template_argument_deduction)
//!
//! Example:
//!   Make<std::pair>(5, true) // Constructs std::pair<int, bool>(5, true);
template <template <typename...> class Class, typename... Types, typename... Args>
Class<Types..., typename std::remove_reference<Args>::type...> Make(Args&&... args)
{
    return Class<Types..., typename std::remove_reference<Args>::type...>{std::forward<Args>(args)...};
}

//! Function object composing two other function objects. Can be replaced with
//! auto lambdas when we update to C++14.
//!
//! Example:
//!   Make<ComposeFn>(sin, atan2)(3, 4) == sin(atan2(3, 4))
template <typename Fn1, typename Fn2>
struct ComposeFn
{
    Fn1&& fn1;
    Fn2&& fn2;

    template <typename... Args>
    decltype(auto) operator()(Args&&... args) { return this->fn1(this->fn2(std::forward<Args>(args)...)); }
};

//! Bound function. See Bind() below.
template <typename Fn, typename BindArgs, typename BoundArgs = TypeList<>>
struct BoundFn;

//! Specialization of above for base case.
template <typename Fn, typename... BoundArgs>
struct BoundFn<Fn, TypeList<>, TypeList<BoundArgs...>>
{
    Fn&& m_fn;

    template <typename... FreeArgs>
    decltype(auto) operator()(BoundArgs&... bound_args, FreeArgs&&... free_args)
    {
        return this->m_fn(bound_args..., std::forward<FreeArgs>(free_args)...);
    }
};

//! Specialization of above for recursive case.
template <typename Fn, typename BindArg, typename... BindArgs, typename... BoundArgs>
struct BoundFn<Fn, TypeList<BindArg, BindArgs...>, TypeList<BoundArgs...>>
    : BoundFn<Fn, TypeList<BindArgs...>, TypeList<BoundArgs..., BindArg>>
{
    using Base = BoundFn<Fn, TypeList<BindArgs...>, TypeList<BoundArgs..., BindArg>>;
    BindArg& m_bind_arg;

    BoundFn(Fn& fn, BindArg& bind_arg, BindArgs&... bind_args) : Base{fn, bind_args...}, m_bind_arg(bind_arg) {}

    // Use std::result_of instead of decltype return to work around gcc bug
    // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=83249
    template <typename... FreeArgs>
    auto operator()(BoundArgs&... bound_args, FreeArgs&&... free_args) ->
        typename std::result_of<Base(BoundArgs&..., BindArg&, FreeArgs...)>::type
    {
        return Base::operator()(bound_args..., m_bind_arg, std::forward<FreeArgs>(free_args)...);
    }
};

//! std::bind replacement. Unlike std::bind it doesn't copy the function object
//! or argument but instead takes rvalue references. This allows it to work with
//! uncopyable objects, but also limits its use to situations where objects
//! don't go out of scope. Uses of this can be replaced with auto lambdas when
//! we update to C++14.
//!
//! Example:
//!    Bind(atan2, 3)(4) == atan2(3, 4)
//!
//! Possible TODO: It might be nice to make binding more consistent with composing
//! and switch to calling syntax Make<Bind>(...) instead of Bind(...).
template <typename Fn, typename... BindArgs>
BoundFn<Fn, TypeList<BindArgs...>> Bind(Fn&& fn, BindArgs&... bind_args)
{
    return {fn, bind_args...};
}

//! Bound tuple function. See BindTuple() below.
//!
//! C++14 equivalent:
//!
//!   BoundTupleFn = [&fn](auto&&... params) { fn(std::forward_as_tuple(params...)); };
template <typename Fn>
struct BoundTupleFn
{
    Fn& m_fn;
    template <typename... Params>
    decltype(auto) operator()(Params&&... params) { return this->m_fn(std::forward_as_tuple(params...)); }
};

//! Bind tuple argument to function. Arguments passed to the returned function
//! object are grouped together and passed to the wrapped function object as a
//! single forward_as_tuple argument.
template <typename Fn>
BoundTupleFn<Fn> BindTuple(Fn&& fn)
{
    return {fn};
}

//! Function object wrapping std::get. Can be replaced with auto lambdas when we
//! update to C++14.
//!
//! Example:
//!   GetFn<3>()(a) // Equivalent to std::get<3>(a)
template <std::size_t I>
struct GetFn
{
    template <typename Tuple>
    auto operator()(Tuple&& tuple) -> decltype(std::get<I>(tuple))&
    {
        return std::get<I>(tuple);
    }
};

//! Function object that throws an exception. Can be replaced with auto lambdas
//! when we update to C++14.
//!
//! Example:
//!   ThrowFn<E>()(args) // Equivalent to: throw E(args)
template <typename Exception>
struct ThrowFn
{
    template <typename... Params>
    const Exception& operator()(Params&&... params)
    {
        throw Exception(std::forward<Params>(params)...);
    }
};

//! Type helper splitting a TypeList into two halves at position index.
//!
//! Example:
//!   is_same<TypeList<int, double>, Split<2, TypeList<int, double, float, bool>>::First>
//!   is_same<TypeList<float, bool>, Split<2, TypeList<int, double, float, bool>>::Second>
template <std::size_t index, typename List, typename _First = TypeList<>, bool done = index == 0>
struct Split;

//! Specialization of above (base case)
template <typename _Second, typename _First>
struct Split<0, _Second, _First, true>
{
    using First = _First;
    using Second = _Second;
};

//! Specialization of above (recursive case)
template <std::size_t index, typename Type, typename... _Second, typename... _First>
struct Split<index, TypeList<Type, _Second...>, TypeList<_First...>, false>
{
    using _Next = Split<index - 1, TypeList<_Second...>, TypeList<_First..., Type>>;
    using First = typename _Next::First;
    using Second = typename _Next::Second;
};

//! Type helper giving return type of a callable type.
template <typename Callable>
using ResultOf = decltype(std::declval<Callable>()());

//! Substitutue for std::remove_cvref_t
template <typename T>
using RemoveCvRef = std::remove_cv_t<std::remove_reference_t<T>>;

//! Type helper abbreviating std::decay.
template <typename T>
using Decay = typename std::decay<T>::type;

//! SFINAE helper, see using Require below.
template <typename SfinaeExpr, typename Result_>
struct _Require
{
    using Result = Result_;
};

//! SFINAE helper, basically the same as to C++17's void_t, but allowing types other than void to be returned.
template <typename SfinaeExpr, typename Result = void>
using Require = typename _Require<SfinaeExpr, Result>::Result;

//! Function parameter type for prioritizing overloaded function calls that
//! would otherwise be ambiguous.
//!
//! Example:
//!   auto foo(Priority<1>) -> std::enable_if<>;
//!   auto foo(Priority<0>) -> void;
//!
//!   foo(Priority<1>());   // Calls higher priority overload if enabled.
template <int priority>
struct Priority : Priority<priority - 1>
{
};

//! Specialization of above (base case)
template <>
struct Priority<0>
{
};

//! Function parameter type for discarded argument. Useful to be able to write a
//! function overload that discards arguments as a normal function instead of a
//! template function.
struct Discard
{
    template <typename... Args>
    Discard(Args&&...)
    {
    }
};

//! Return capnp type name with filename prefix removed.
template <typename T>
const char* TypeName()
{
    // DisplayName string looks like
    // "interfaces/capnp/common.capnp:ChainNotifications.resendWalletTransactions$Results"
    // This discards the part of the string before the first ':' character.
    // Another alternative would be to use the displayNamePrefixLength field,
    // but this discards everything before the last '.' character, throwing away
    // the object name, which is useful.
    const char* display_name = ::capnp::Schema::from<T>().getProto().getDisplayName().cStr();
    const char* short_name = strchr(display_name, ':');
    return short_name ? short_name + 1 : display_name;
}

//! Analog to std::lock_guard that unlocks instead of locks.
template <typename Lock>
struct UnlockGuard
{
    UnlockGuard(Lock& lock) : m_lock(lock) { m_lock.unlock(); }
    ~UnlockGuard() { m_lock.lock(); }
    Lock& m_lock;
};

template <typename Lock, typename Callback>
void Unlock(Lock& lock, Callback&& callback)
{
    UnlockGuard<Lock> unlock(lock);
    callback();
}

//! Needed for libc++/macOS compatibility. Lets code work with shared_ptr nothrow declaration
//! https://github.com/capnproto/capnproto/issues/553#issuecomment-328554603
template <typename T>
struct DestructorCatcher
{
    T value;
    template <typename... Params>
    DestructorCatcher(Params&&... params) : value(kj::fwd<Params>(params)...)
    {
    }
    ~DestructorCatcher() noexcept try {
    } catch (const kj::Exception& e) {
    }
};

//! Wrapper around callback function for compatibility with std::async.
//!
//! std::async requires callbacks to be copyable and requires noexcept
//! destructors, but this doesn't work well with kj types which are generally
//! move-only and not noexcept.
template <typename Callable>
struct AsyncCallable
{
    AsyncCallable(Callable&& callable) : m_callable(std::make_shared<DestructorCatcher<Callable>>(std::move(callable)))
    {
    }
    AsyncCallable(const AsyncCallable&) = default;
    AsyncCallable(AsyncCallable&&) = default;
    ~AsyncCallable() noexcept = default;
    ResultOf<Callable> operator()() const { return (m_callable->value)(); }
    mutable std::shared_ptr<DestructorCatcher<Callable>> m_callable;
};

//! Construct AsyncCallable object.
template <typename Callable>
AsyncCallable<typename std::remove_reference<Callable>::type> MakeAsyncCallable(Callable&& callable)
{
    return std::move(callable);
}

//! Format current thread name as "{exe_name}-{$pid}/{thread_name}-{$tid}".
std::string ThreadName(const char* exe_name);

//! Escape binary string for use in log so it doesn't trigger unicode decode
//! errors in python unit tests.
std::string LogEscape(const kj::StringTree& string);

//! Callback type used by SpawnProcess below.
using FdToArgsFn = std::function<std::vector<std::string>(int fd)>;

//! Spawn a new process that communicates with the current process over a socket
//! pair. Returns pid through an output argument, and file descriptor for the
//! local side of the socket. Invokes fd_to_args callback with the remote file
//! descriptor number which returns the command line arguments that should be
//! used to execute the process, and which should have the remote file
//! descriptor embedded in whatever format the child process expects.
int SpawnProcess(int& pid, FdToArgsFn&& fd_to_args);

//! Call execvp with vector args.
void ExecProcess(const std::vector<std::string>& args);

//! Wait for a process to exit and return its exit code.
int WaitProcess(int pid);

inline char* CharCast(char* c) { return c; }
inline char* CharCast(unsigned char* c) { return (char*)c; }
inline const char* CharCast(const char* c) { return c; }
inline const char* CharCast(const unsigned char* c) { return (const char*)c; }

} // namespace mp

#endif // MP_UTIL_H
