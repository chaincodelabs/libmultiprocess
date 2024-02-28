// Copyright (c) 2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MP_PROXY_TYPES_H
#define MP_PROXY_TYPES_H

#include <mp/proxy-io.h>

#include <exception>
#include <optional>
#include <set>
#include <typeindex>
#include <vector>

namespace mp {

template <typename Value>
class ValueField
{
public:
    ValueField(Value& value) : m_value(value) {}
    ValueField(Value&& value) : m_value(value) {}
    Value& m_value;

    Value& get() { return m_value; }
    Value& init() { return m_value; }
    bool has() { return true; }
};

template <typename Accessor, typename Struct>
struct StructField
{
    template <typename S>
    StructField(S& struct_) : m_struct(struct_)
    {
    }
    Struct& m_struct;

    // clang-format off
    template<typename A = Accessor> auto get() const -> decltype(A::get(this->m_struct)) { return A::get(this->m_struct); }
    template<typename A = Accessor> auto has() const -> typename std::enable_if<A::optional, bool>::type { return A::getHas(m_struct); }
    template<typename A = Accessor> auto has() const -> typename std::enable_if<!A::optional && A::boxed, bool>::type { return A::has(m_struct); }
    template<typename A = Accessor> auto has() const -> typename std::enable_if<!A::optional && !A::boxed, bool>::type { return true; }
    template<typename A = Accessor> auto want() const -> typename std::enable_if<A::requested, bool>::type { return A::getWant(m_struct); }
    template<typename A = Accessor> auto want() const -> typename std::enable_if<!A::requested, bool>::type { return true; }
    template<typename A = Accessor, typename... Args> decltype(auto) set(Args&&... args) const { return A::set(this->m_struct, std::forward<Args>(args)...); }
    template<typename A = Accessor, typename... Args> decltype(auto) init(Args&&... args) const { return A::init(this->m_struct, std::forward<Args>(args)...); }
    template<typename A = Accessor> auto setHas() const -> typename std::enable_if<A::optional>::type { return A::setHas(m_struct); }
    template<typename A = Accessor> auto setHas() const -> typename std::enable_if<!A::optional>::type { }
    template<typename A = Accessor> auto setWant() const -> typename std::enable_if<A::requested>::type { return A::setWant(m_struct); }
    template<typename A = Accessor> auto setWant() const -> typename std::enable_if<!A::requested>::type { }
    // clang-format on
};

template <typename Output>
void CustomBuildField(TypeList<>,
    Priority<1>,
    ClientInvokeContext& invoke_context,
    Output&& output,
    typename std::enable_if<std::is_same<decltype(output.get()), Context::Builder>::value>::type* enable = nullptr)
{
    auto& connection = invoke_context.connection;
    auto& thread_context = invoke_context.thread_context;
    auto& request_threads = thread_context.request_threads;
    auto& callback_threads = thread_context.callback_threads;

    auto callback_thread = callback_threads.find(&connection);
    if (callback_thread == callback_threads.end()) {
        callback_thread =
            callback_threads
                .emplace(std::piecewise_construct, std::forward_as_tuple(&connection),
                    std::forward_as_tuple(
                        connection.m_threads.add(kj::heap<ProxyServer<Thread>>(thread_context, std::thread{})),
                        &connection, /* destroy_connection= */ false))
                .first;
    }

    auto request_thread = request_threads.find(&connection);
    if (request_thread == request_threads.end()) {
        // This code will only run if IPC client call is being made for the
        // first time on a new thread. After the first call, subsequent calls
        // will use the existing request thread. This code will also never run at
        // all if the current thread is a request thread created for a different
        // IPC client, because in that case PassField code (below) will have set
        // request_thread to point to the calling thread.
        auto request = connection.m_thread_map.makeThreadRequest();
        request.setName(thread_context.thread_name);
        request_thread =
            request_threads
                .emplace(std::piecewise_construct, std::forward_as_tuple(&connection),
                    std::forward_as_tuple(request.send().getResult(), &connection, /* destroy_connection= */ false))
                .first; // Nonblocking due to capnp request pipelining.
    }

    auto context = output.init();
    context.setThread(request_thread->second.m_client);
    context.setCallbackThread(callback_thread->second.m_client);
}

//! PassField override for mp.Context arguments. Return asynchronously and call
//! function on other thread found in context.
template <typename Accessor, typename ServerContext, typename Fn, typename... Args>
auto PassField(Priority<1>, TypeList<>, ServerContext& server_context, const Fn& fn, Args&&... args) ->
    typename std::enable_if<
        std::is_same<decltype(Accessor::get(server_context.call_context.getParams())), Context::Reader>::value,
        kj::Promise<typename ServerContext::CallContext>>::type
{
    const auto& params = server_context.call_context.getParams();
    Context::Reader context_arg = Accessor::get(params);
    auto future = kj::newPromiseAndFulfiller<typename ServerContext::CallContext>();
    auto& server = server_context.proxy_server;
    int req = server_context.req;
    auto invoke = MakeAsyncCallable(
        [&server, req, fn, args...,
         fulfiller = kj::mv(future.fulfiller),
         call_context = kj::mv(server_context.call_context)]() mutable {
                const auto& params = call_context.getParams();
                Context::Reader context_arg = Accessor::get(params);
                ServerContext server_context{server, call_context, req};
                {
                    // Before invoking the function, store a reference to the
                    // callbackThread provided by the client in the
                    // thread_local.request_threads map. This way, if this
                    // server thread needs to execute any RPCs that call back to
                    // the client, they will happen on the same client thread
                    // that is waiting for this function, just like what would
                    // happen if a local function made a nested call.
                    //
                    // If the request_threads map already has an entry for this
                    // connections, it will be left unchanged, and it indicates
                    // that the current thread is an RPC client which is in the
                    // middle of another call, and this call is a nested call
                    // running on the same thread, which should execute any
                    // further RPC requests on this connection using the
                    // existing map value.
                    auto& request_threads = g_thread_context.request_threads;
                    auto request_thread = request_threads.find(server.m_context.connection);
                    if (request_thread == request_threads.end()) {
                        request_thread =
                            g_thread_context.request_threads
                                .emplace(std::piecewise_construct, std::forward_as_tuple(server.m_context.connection),
                                    std::forward_as_tuple(context_arg.getCallbackThread(), server.m_context.connection,
                                        /* destroy_connection= */ false))
                                .first;
                    }
                    KJ_DEFER(if (request_thread != request_threads.end()) request_threads.erase(request_thread));
                    fn.invoke(server_context, args...);
                }
                KJ_IF_MAYBE(exception, kj::runCatchingExceptions([&]() {
                    server.m_context.connection->m_loop.sync([&] {
                        auto fulfiller_dispose = kj::mv(fulfiller);
                        fulfiller_dispose->fulfill(kj::mv(call_context));
                    });
                }))
                {
                    server.m_context.connection->m_loop.sync([&]() {
                        auto fulfiller_dispose = kj::mv(fulfiller);
                        fulfiller_dispose->reject(kj::mv(*exception));
                    });
                }
            });

    // Lookup Thread object specified by the client. The specified thread should
    // be a local Thread::Server object, but it needs to be looked up
    // asynchronousely with getLocalServer().
    auto thread_client = context_arg.getThread();
    return server.m_context.connection->m_threads.getLocalServer(thread_client)
        .then([&server, invoke, req](const kj::Maybe<Thread::Server&>& perhaps) {
            // Assuming the thread object is found, pass it a pointer to the
            // `invoke` lambda above which will invoke the function on that
            // thread.
            KJ_IF_MAYBE (thread_server, perhaps) {
                const auto& thread = static_cast<ProxyServer<Thread>&>(*thread_server);
                server.m_context.connection->m_loop.log()
                    << "IPC server post request  #" << req << " {" << thread.m_thread_context.thread_name << "}";
                thread.m_thread_context.waiter->post(std::move(invoke));
            } else {
                server.m_context.connection->m_loop.log()
                    << "IPC server error request #" << req << ", missing thread to execute request";
                throw std::runtime_error("invalid thread handle");
            }
        })
        // Wait for the invocation to finish before returning to the caller.
        .then([invoke_wait = kj::mv(future.promise)]() mutable { return kj::mv(invoke_wait); });
}

// Destination parameter type that can be passed to ReadField function as an
// alternative to ReadDestValue. It allows the ReadField implementation to call
// the provided emplace_fn function with constructor arguments, so it only needs
// to determine the arguments, and can let the emplace function decide how to
// actually construct the read destination object. For example, if a std::string
// is being read, the ReadField call will call the custom emplace_fn with char*
// and size_t arguments, and the emplace function can decide whether to call the
// constructor via the operator or make_shared or emplace or just return a
// temporary string that is moved from.
template <typename LocalType, typename EmplaceFn>
struct ReadDestEmplace
{
    ReadDestEmplace(TypeList<LocalType>, EmplaceFn&& emplace_fn) : m_emplace_fn(emplace_fn) {}

    //! Simple case. If ReadField impementation calls this construct() method
    //! with constructor arguments, just pass them on to the emplace function.
    template <typename... Args>
    decltype(auto) construct(Args&&... args)
    {
        return m_emplace_fn(std::forward<Args>(args)...);
    }

    //! More complicated case. If ReadField implementation works by calling this
    //! update() method, adapt it call construct() instead. This requires
    //! LocalType to have a default constructor to create new object that can be
    //! passed to update()
    template <typename UpdateFn>
    decltype(auto) update(UpdateFn&& update_fn)
    {
        if constexpr (std::is_const_v<std::remove_reference_t<std::invoke_result_t<EmplaceFn>>>) {
            // If destination type is const, default construct temporary
            // to pass to update, then call move constructor via construct() to
            // move from that temporary.
            std::remove_cv_t<LocalType> temp;
            update_fn(temp);
            return construct(std::move(temp));
        } else {
            // Default construct object and pass it to update_fn.
            decltype(auto) temp = construct();
            update_fn(temp);
            return temp;
        }
    }
    EmplaceFn& m_emplace_fn;
};

//! Helper function to create a ReadDestEmplace object that constructs a
//! temporary, ReadField can return.
template <typename LocalType>
auto ReadDestTemp()
{
    return ReadDestEmplace{TypeList<LocalType>(), [&](auto&&... args) -> decltype(auto) {
        return LocalType{std::forward<decltype(args)>(args)...};
    }};
}

//! Destination parameter type that can be passed to ReadField function as an
//! alternative to ReadDestEmplace. Instead of requiring an emplace callback to
//! construct a new value, it just takes a reference to an existing value and
//! assigns a new value to it.
template <typename Value>
struct ReadDestValue
{
    ReadDestValue(Value& value) : m_value(value) {}

    //! Simple case. If ReadField works by calling update() just forward arguments to update_fn.
    template <typename UpdateFn>
    Value& update(UpdateFn&& update_fn)
    {
        update_fn(m_value);
        return m_value;
    }

    //! More complicated case. If ReadField works by calling construct(), need
    //! to reconstruct m_value in place.
    template <typename... Args>
    Value& construct(Args&&... args)
    {
        m_value.~Value();
        new (&m_value) Value(std::forward<Args>(args)...);
        return m_value;
    }

    Value& m_value;
};

template <typename LocalType, typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<std::optional<LocalType>>,
    Priority<1>,
    InvokeContext& invoke_context,
    Input&& input,
    ReadDest&& read_dest)
{
    return read_dest.update([&](auto& value) {
        if (!input.has()) {
            value.reset();
        } else if (value) {
            ReadField(TypeList<LocalType>(), invoke_context, input, ReadDestValue(*value));
        } else {
            ReadField(TypeList<LocalType>(), invoke_context, input,
                ReadDestEmplace(TypeList<LocalType>(), [&](auto&&... args) -> auto& {
                    value.emplace(std::forward<decltype(args)>(args)...);
                    return *value;
                }));
        }
    });
}

template <typename LocalType, typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<std::shared_ptr<LocalType>>,
    Priority<0>,
    InvokeContext& invoke_context,
    Input&& input,
    ReadDest&& read_dest)
{
    return read_dest.update([&](auto& value) {
        if (!input.has()) {
            value.reset();
        } else if (value) {
            ReadField(TypeList<LocalType>(), invoke_context, input, ReadDestValue(*value));
        } else {
            ReadField(TypeList<LocalType>(), invoke_context, input,
                ReadDestEmplace(TypeList<LocalType>(), [&](auto&&... args) -> auto& {
                    value = std::make_shared<LocalType>(std::forward<decltype(args)>(args)...);
                    return *value;
                }));
        }
    });
}

template <typename LocalType, typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<LocalType*>,
    Priority<1>,
    InvokeContext& invoke_context,
    Input&& input,
    ReadDest&& read_dest)
{
    return read_dest.update([&](auto& value) {
        if (value) {
            ReadField(TypeList<LocalType>(), invoke_context, std::forward<Input>(input), ReadDestValue(*value));
        }
    });
}

template <typename LocalType, typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<std::shared_ptr<const LocalType>>,
    Priority<1>,
    InvokeContext& invoke_context,
    Input&& input,
    ReadDest&& read_dest)
{
    return read_dest.update([&](auto& value) {
        if (!input.has()) {
            value.reset();
            return;
        }
        ReadField(TypeList<LocalType>(), invoke_context, std::forward<Input>(input),
            ReadDestEmplace(TypeList<LocalType>(), [&](auto&&... args) -> auto& {
                value = std::make_shared<LocalType>(std::forward<decltype(args)>(args)...);
                return *value;
            }));
    });
}

template <typename LocalType, typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<std::vector<LocalType>>,
    Priority<1>,
    InvokeContext& invoke_context,
    Input&& input,
    ReadDest&& read_dest)
{
    return read_dest.update([&](auto& value) {
        auto data = input.get();
        value.clear();
        value.reserve(data.size());
        for (auto item : data) {
            ReadField(TypeList<LocalType>(), invoke_context, Make<ValueField>(item),
                ReadDestEmplace(TypeList<LocalType>(), [&](auto&&... args) -> auto& {
                    value.emplace_back(std::forward<decltype(args)>(args)...);
                    return value.back();
                }));
        }
    });
}

template <typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<std::vector<bool>>,
                               Priority<1>,
                               InvokeContext& invoke_context,
                               Input&& input,
                               ReadDest&& read_dest)
{
    return read_dest.update([&](auto& value) {
        auto data = input.get();
        value.clear();
        value.reserve(data.size());
        for (auto item : data) {
            value.push_back(ReadField(TypeList<bool>(), invoke_context, Make<ValueField>(item), ReadDestTemp<bool>()));
        }
    });
}

template <typename LocalType, typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<std::set<LocalType>>,
    Priority<1>,
    InvokeContext& invoke_context,
    Input&& input,
    ReadDest&& read_dest)
{
    return read_dest.update([&](auto& value) {
        auto data = input.get();
        value.clear();
        for (auto item : data) {
            ReadField(TypeList<LocalType>(), invoke_context, Make<ValueField>(item),
                ReadDestEmplace(TypeList<const LocalType>(), [&](auto&&... args) -> auto& {
                    return *value.emplace(std::forward<decltype(args)>(args)...).first;
                }));
        }
    });
}

template <typename KeyLocalType, typename ValueLocalType, typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<std::map<KeyLocalType, ValueLocalType>>,
    Priority<1>,
    InvokeContext& invoke_context,
    Input&& input,
    ReadDest&& read_dest)
{
    return read_dest.update([&](auto& value) {
        auto data = input.get();
        value.clear();
        for (auto item : data) {
            ReadField(TypeList<std::pair<const KeyLocalType, ValueLocalType>>(), invoke_context,
                Make<ValueField>(item),
                ReadDestEmplace(
                    TypeList<std::pair<const KeyLocalType, ValueLocalType>>(), [&](auto&&... args) -> auto& {
                        return *value.emplace(std::forward<decltype(args)>(args)...).first;
                    }));
        }
    });
}

template <typename KeyLocalType, typename ValueLocalType, typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<std::pair<KeyLocalType, ValueLocalType>>,
    Priority<1>,
    InvokeContext& invoke_context,
    Input&& input,
    ReadDest&& read_dest)
{
    const auto& pair = input.get();
    using Accessors = typename ProxyStruct<typename Decay<decltype(pair)>::Reads>::Accessors;

    ReadField(TypeList<KeyLocalType>(), invoke_context, Make<StructField, std::tuple_element_t<0, Accessors>>(pair),
        ReadDestEmplace(TypeList<KeyLocalType>(), [&](auto&&... key_args) -> auto& {
            KeyLocalType* key = nullptr;
            ReadField(TypeList<ValueLocalType>(), invoke_context, Make<StructField, std::tuple_element_t<1, Accessors>>(pair),
                ReadDestEmplace(TypeList<ValueLocalType>(), [&](auto&&... value_args) -> auto& {
                    auto& ret = read_dest.construct(std::piecewise_construct, std::forward_as_tuple(key_args...),
                        std::forward_as_tuple(value_args...));
                    key = &ret.first;
                    return ret.second;
                }));
            return *key;
        }));
}

// TODO: Should generalize this to work with arbitrary length tuples, not just length 2-tuples.
template <typename KeyLocalType, typename ValueLocalType, typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<std::tuple<KeyLocalType, ValueLocalType>>,
    Priority<1>,
    InvokeContext& invoke_context,
    Input&& input,
    ReadDest&& read_dest)
{
    return read_dest.update([&](auto& value) {
        const auto& pair = input.get();
        using Struct = ProxyStruct<typename Decay<decltype(pair)>::Reads>;
        using Accessors = typename Struct::Accessors;
        ReadField(TypeList<KeyLocalType>(), invoke_context, Make<StructField, std::tuple_element_t<0, Accessors>>(pair),
            ReadDestValue(std::get<0>(value)));
        ReadField(TypeList<ValueLocalType>(), invoke_context, Make<StructField, std::tuple_element_t<1, Accessors>>(pair),
            ReadDestValue(std::get<1>(value)));
    });
}

template <typename LocalType, typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<LocalType>,
    Priority<1>,
    InvokeContext& invoke_context,
    Input&& input,
    ReadDest&& read_dest,
    typename std::enable_if<std::is_enum<LocalType>::value>::type* enable = 0)
{
    return read_dest.construct(static_cast<LocalType>(input.get()));
}

template <typename LocalType, typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<LocalType>,
    Priority<1>,
    InvokeContext& invoke_context,
    Input&& input,
    ReadDest&& read_dest,
    typename std::enable_if<std::is_integral<LocalType>::value>::type* enable = nullptr)
{
    auto value = input.get();
    if (value < std::numeric_limits<LocalType>::min() || value > std::numeric_limits<LocalType>::max()) {
        throw std::range_error("out of bound int received");
    }
    return read_dest.construct(static_cast<LocalType>(value));
}

template <typename LocalType, typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<LocalType>,
    Priority<1>,
    InvokeContext& invoke_context,
    Input&& input,
    ReadDest&& read_dest,
    typename std::enable_if<std::is_floating_point<LocalType>::value>::type* enable = 0)
{
    auto value = input.get();
    static_assert(std::is_same<LocalType, decltype(value)>::value, "floating point type mismatch");
    return read_dest.construct(value);
}

template <typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<std::string>,
    Priority<1>,
    InvokeContext& invoke_context,
    Input&& input,
    ReadDest&& read_dest)
{
    auto data = input.get();
    return read_dest.construct(CharCast(data.begin()), data.size());
}

template <size_t size, typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<unsigned char[size]>,
    Priority<1>,
    InvokeContext& invoke_context,
    Input&& input,
    ReadDest&& read_dest)
{
    return read_dest.update([&](auto& value) {
        auto data = input.get();
        memcpy(value, data.begin(), size);
    });
}

template <typename Interface, typename Impl>
std::unique_ptr<Impl> MakeProxyClient(InvokeContext& context, typename Interface::Client&& client)
{
    return std::make_unique<ProxyClient<Interface>>(
        std::move(client), &context.connection, /* destroy_connection= */ false);
}

template <typename Interface, typename Impl>
std::unique_ptr<Impl> CustomMakeProxyClient(InvokeContext& context, typename Interface::Client&& client)
{
    return MakeProxyClient<Interface, Impl>(context, kj::mv(client));
}

template <typename LocalType, typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<std::unique_ptr<LocalType>>,
    Priority<1>,
    InvokeContext& invoke_context,
    Input&& input,
    ReadDest&& read_dest,
    typename Decay<decltype(input.get())>::Calls* enable = nullptr)
{
    using Interface = typename Decay<decltype(input.get())>::Calls;
    if (input.has()) {
        return read_dest.construct(
                                   CustomMakeProxyClient<Interface, LocalType>(invoke_context, std::move(input.get())));
    }
    return read_dest.construct();
}

template <typename LocalType, typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<std::shared_ptr<LocalType>>,
    Priority<1>,
    InvokeContext& invoke_context,
    Input&& input,
    ReadDest&& read_dest,
    typename Decay<decltype(input.get())>::Calls* enable = nullptr)
{
    using Interface = typename Decay<decltype(input.get())>::Calls;
    if (input.has()) {
        return read_dest.construct(
            CustomMakeProxyClient<Interface, LocalType>(invoke_context, std::move(input.get())));
    }
    return read_dest.construct();
}

// ProxyCallFn class is needed because c++11 doesn't support auto lambda parameters.
// It's equivalent c++14: [invoke_context](auto&& params) {
// invoke_context->call(std::forward<decltype(params)>(params)...)
template <typename InvokeContext>
struct ProxyCallFn
{
    InvokeContext m_proxy;

    template <typename... CallParams>
    decltype(auto) operator()(CallParams&&... params) { return this->m_proxy->call(std::forward<CallParams>(params)...); }
};

template <typename FnR, typename... FnParams, typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<std::function<FnR(FnParams...)>>,
    Priority<1>,
    InvokeContext& invoke_context,
    Input&& input,
    ReadDest&& read_dest)
{
    if (input.has()) {
        using Interface = typename Decay<decltype(input.get())>::Calls;
        auto client = std::make_shared<ProxyClient<Interface>>(
            input.get(), &invoke_context.connection, /* destroy_connection= */ false);
        return read_dest.construct(ProxyCallFn<decltype(client)>{std::move(client)});
    }
    return read_dest.construct();
};

template <size_t index, typename LocalType, typename Input, typename Value>
void ReadOne(TypeList<LocalType> param,
    InvokeContext& invoke_context,
    Input&& input,
    Value&& value,
    typename std::enable_if<index != ProxyType<LocalType>::fields>::type* enable = nullptr)
{
    using Index = std::integral_constant<size_t, index>;
    using Struct = typename ProxyType<LocalType>::Struct;
    using Accessor = typename std::tuple_element<index, typename ProxyStruct<Struct>::Accessors>::type;
    const auto& struc = input.get();
    auto&& field_value = value.*ProxyType<LocalType>::get(Index());
    ReadField(TypeList<RemoveCvRef<decltype(field_value)>>(), invoke_context, Make<StructField, Accessor>(struc),
        ReadDestValue(field_value));
    ReadOne<index + 1>(param, invoke_context, input, value);
}

template <size_t index, typename LocalType, typename Input, typename Value>
void ReadOne(TypeList<LocalType> param,
    InvokeContext& invoke_context,
    Input& input,
    Value& value,
    typename std::enable_if<index == ProxyType<LocalType>::fields>::type* enable = nullptr)
{
}

template <typename LocalType, typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<LocalType> param,
    Priority<1>,
    InvokeContext& invoke_context,
    Input&& input,
    ReadDest&& read_dest,
    typename ProxyType<LocalType>::Struct* enable = nullptr)
{
    return read_dest.update([&](auto& value) { ReadOne<0>(param, invoke_context, input, value); });
}

template <typename... LocalTypes, typename... Args>
decltype(auto) ReadField(TypeList<LocalTypes...>, Args&&... args)
{
    return CustomReadField(TypeList<RemoveCvRef<LocalTypes>...>(), Priority<2>(), std::forward<Args>(args)...);
}

template <typename LocalType, typename Input>
void ThrowField(TypeList<LocalType>, InvokeContext& invoke_context, Input&& input)
{
    ReadField(
        TypeList<LocalType>(), invoke_context, input, ReadDestEmplace(TypeList<LocalType>(),
            [](auto&& ...args) -> const LocalType& { throw LocalType{std::forward<decltype(args)>(args)...}; }));
}

//! Special case for generic std::exception. It's an abstract type so it can't
//! be created directly. Rethrow as std::runtime_error so callers expecting it
//! will still catch it.
template <typename Input>
void ThrowField(TypeList<std::exception>, InvokeContext& invoke_context, Input&& input)
{
    auto data = input.get();
    throw std::runtime_error(std::string(CharCast(data.begin()), data.size()));
}

template <typename LocalType, typename Output>
void CustomBuildField(TypeList<LocalType>, Priority<1>, InvokeContext& invoke_context, ::capnp::Void, Output&& output)
{
}

template <typename Value, typename Output>
void CustomBuildField(TypeList<std::string>,
    Priority<1>,
    InvokeContext& invoke_context,
    Value&& value,
    Output&& output)
{
    auto result = output.init(value.size());
    memcpy(result.begin(), value.data(), value.size());
}

template <typename Output, size_t size>
void CustomBuildField(TypeList<const unsigned char*>,
    Priority<3>,
    InvokeContext& invoke_context,
    const unsigned char (&value)[size],
    Output&& output)
{
    auto result = output.init(size);
    memcpy(result.begin(), value, size);
}

template <typename... Values>
bool CustomHasValue(InvokeContext& invoke_context, Values&&... value)
{
    return true;
}

template <typename... LocalTypes, typename Context, typename... Values, typename Output>
void BuildField(TypeList<LocalTypes...>, Context& context, Output&& output, Values&&... values)
{
    if (CustomHasValue(context, std::forward<Values>(values)...)) {
        CustomBuildField(TypeList<LocalTypes...>(), Priority<3>(), context, std::forward<Values>(values)...,
            std::forward<Output>(output));
    }
}

//! Adapter to convert ProxyCallback object call to function object call.
template <typename Result, typename... Args>
class ProxyCallbackImpl final : public ProxyCallback<std::function<Result(Args...)>>
{
    using Fn = std::function<Result(Args...)>;
    Fn m_fn;

public:
    ProxyCallbackImpl(Fn fn) : m_fn(std::move(fn)) {}
    Result call(Args&&... args) override { return m_fn(std::forward<Args>(args)...); }
};

template <typename Value, typename FnR, typename... FnParams, typename Output>
void CustomBuildField(TypeList<std::function<FnR(FnParams...)>>,
    Priority<1>,
    InvokeContext& invoke_context,
    Value& value,
    Output&& output)
{
    if (value) {
        using Interface = typename decltype(output.get())::Calls;
        using Callback = ProxyCallbackImpl<FnR, FnParams...>;
        output.set(kj::heap<ProxyServer<Interface>>(
            std::make_shared<Callback>(std::forward<Value>(value)), invoke_context.connection));
    }
}

template <typename Interface, typename Impl>
kj::Own<typename Interface::Server> MakeProxyServer(InvokeContext& context, std::shared_ptr<Impl> impl)
{
    return kj::heap<ProxyServer<Interface>>(std::move(impl), context.connection);
}

template <typename Interface, typename Impl>
kj::Own<typename Interface::Server> CustomMakeProxyServer(InvokeContext& context, std::shared_ptr<Impl>&& impl)
{
    return MakeProxyServer<Interface, Impl>(context, std::move(impl));
}

template <typename Impl, typename Value, typename Output>
void CustomBuildField(TypeList<std::unique_ptr<Impl>>,
    Priority<1>,
    InvokeContext& invoke_context,
    Value&& value,
    Output&& output,
    typename Decay<decltype(output.get())>::Calls* enable = nullptr)
{
    if (value) {
        using Interface = typename decltype(output.get())::Calls;
        output.set(CustomMakeProxyServer<Interface, Impl>(invoke_context, std::shared_ptr<Impl>(value.release())));
    }
}

template <typename Impl, typename Value, typename Output>
void CustomBuildField(TypeList<std::shared_ptr<Impl>>,
    Priority<2>,
    InvokeContext& invoke_context,
    Value&& value,
    Output&& output,
    typename Decay<decltype(output.get())>::Calls* enable = nullptr)
{
    if (value) {
        using Interface = typename decltype(output.get())::Calls;
        output.set(CustomMakeProxyServer<Interface, Impl>(invoke_context, std::move(value)));
    }
}

template <typename Impl, typename Output>
void CustomBuildField(TypeList<Impl&>,
    Priority<1>,
    InvokeContext& invoke_context,
    Impl& value,
    Output&& output,
    typename decltype(output.get())::Calls* enable = nullptr)
{
    // Disable deleter so proxy server object doesn't attempt to delete the
    // wrapped implementation when the proxy client is destroyed or
    // disconnected.
    using Interface = typename decltype(output.get())::Calls;
    output.set(CustomMakeProxyServer<Interface, Impl>(invoke_context, std::shared_ptr<Impl>(&value, [](Impl*){})));
}

template <typename LocalType, typename Value, typename Output>
void CustomBuildField(TypeList<LocalType*>, Priority<3>, InvokeContext& invoke_context, Value&& value, Output&& output)
{
    if (value) {
        BuildField(TypeList<LocalType>(), invoke_context, output, *value);
    }
}

template <typename LocalType, typename Value, typename Output>
void CustomBuildField(TypeList<std::shared_ptr<LocalType>>,
    Priority<1>,
    InvokeContext& invoke_context,
    Value&& value,
    Output&& output)
{
    if (value) {
        BuildField(TypeList<LocalType>(), invoke_context, output, *value);
    }
}

// Adapter to let BuildField overloads methods work set & init list elements as
// if they were fields of a struct. If BuildField is changed to use some kind of
// accessor class instead of calling method pointers, then then maybe this could
// go away or be simplified, because would no longer be a need to return
// ListOutput method pointers emulating capnp struct method pointers..
template <typename ListType>
struct ListOutput;

template <typename T, ::capnp::Kind kind>
struct ListOutput<::capnp::List<T, kind>>
{
    using Builder = typename ::capnp::List<T, kind>::Builder;

    ListOutput(Builder& builder, size_t index) : m_builder(builder), m_index(index) {}
    Builder& m_builder;
    size_t m_index;

    // clang-format off
    decltype(auto) get() const { return this->m_builder[this->m_index]; }
    decltype(auto) init() const { return this->m_builder[this->m_index]; }
    template<typename B = Builder, typename Arg> decltype(auto) set(Arg&& arg) const { return static_cast<B&>(this->m_builder).set(m_index, std::forward<Arg>(arg)); }
    template<typename B = Builder, typename Arg> decltype(auto) init(Arg&& arg) const { return static_cast<B&>(this->m_builder).init(m_index, std::forward<Arg>(arg)); }
    // clang-format on
};

template <typename LocalType, typename Value, typename Output>
void CustomBuildField(TypeList<std::vector<LocalType>>,
    Priority<1>,
    InvokeContext& invoke_context,
    Value&& value,
    Output&& output)
{
    // FIXME dedup with set handler below
    auto list = output.init(value.size());
    size_t i = 0;
    for (auto it = value.begin(); it != value.end(); ++it, ++i) {
        BuildField(TypeList<LocalType>(), invoke_context, ListOutput<typename decltype(list)::Builds>(list, i), *it);
    }
}

template <typename LocalType, typename Value, typename Output>
void CustomBuildField(TypeList<std::set<LocalType>>,
    Priority<1>,
    InvokeContext& invoke_context,
    Value&& value,
    Output&& output)
{
    // FIXME dededup with vector handler above
    auto list = output.init(value.size());
    size_t i = 0;
    for (const auto& elem : value) {
        BuildField(TypeList<LocalType>(), invoke_context, ListOutput<typename decltype(list)::Builds>(list, i), elem);
        ++i;
    }
}

template <typename KeyLocalType, typename ValueLocalType, typename Value, typename Output>
void CustomBuildField(TypeList<std::map<KeyLocalType, ValueLocalType>>,
    Priority<1>,
    InvokeContext& invoke_context,
    Value&& value,
    Output&& output)
{
    // FIXME dededup with vector handler above
    auto list = output.init(value.size());
    size_t i = 0;
    for (const auto& elem : value) {
        BuildField(TypeList<std::pair<KeyLocalType, ValueLocalType>>(), invoke_context,
            ListOutput<typename decltype(list)::Builds>(list, i), elem);
        ++i;
    }
}
template <typename Value>
::capnp::Void BuildPrimitive(InvokeContext& invoke_context, Value&&, TypeList<::capnp::Void>)
{
    return {};
}

inline static bool BuildPrimitive(InvokeContext& invoke_context, std::vector<bool>::const_reference value, TypeList<bool>)
{
    return value;
}

template <typename LocalType, typename Value>
LocalType BuildPrimitive(InvokeContext& invoke_context,
    const Value& value,
    TypeList<LocalType>,
    typename std::enable_if<std::is_enum<Value>::value>::type* enable = nullptr)
{
    return static_cast<LocalType>(value);
}

template <typename LocalType, typename Value>
LocalType BuildPrimitive(InvokeContext& invoke_context,
    const Value& value,
    TypeList<LocalType>,
    typename std::enable_if<std::is_integral<Value>::value, int>::type* enable = nullptr)
{
    static_assert(
        std::numeric_limits<LocalType>::lowest() <= std::numeric_limits<Value>::lowest(), "mismatched integral types");
    static_assert(
        std::numeric_limits<LocalType>::max() >= std::numeric_limits<Value>::max(), "mismatched integral types");
    return value;
}

template <typename LocalType, typename Value>
LocalType BuildPrimitive(InvokeContext& invoke_context,
    const Value& value,
    TypeList<LocalType>,
    typename std::enable_if<std::is_floating_point<Value>::value>::type* enable = nullptr)
{
    static_assert(std::is_same<Value, LocalType>::value,
        "mismatched floating point types. please fix message.capnp type declaration to match wrapped interface");
    return value;
}

template <typename LocalType, typename Value, typename Output>
void CustomBuildField(TypeList<std::optional<LocalType>>,
    Priority<1>,
    InvokeContext& invoke_context,
    Value&& value,
    Output&& output)
{
    if (value) {
        output.setHas();
        // FIXME: should std::move value if destvalue is rref?
        BuildField(TypeList<LocalType>(), invoke_context, output, *value);
    }
}

template <typename Output>
void CustomBuildField(TypeList<std::exception>,
    Priority<1>,
    InvokeContext& invoke_context,
    const std::exception& value,
    Output&& output)
{
    BuildField(TypeList<std::string>(), invoke_context, output, std::string(value.what()));
}

// FIXME: Overload on output type instead of value type and switch to std::get and merge with next overload
template <typename KeyLocalType, typename ValueLocalType, typename Value, typename Output>
void CustomBuildField(TypeList<std::pair<KeyLocalType, ValueLocalType>>,
    Priority<1>,
    InvokeContext& invoke_context,
    Value&& value,
    Output&& output)
{
    auto pair = output.init();
    using Accessors = typename ProxyStruct<typename decltype(pair)::Builds>::Accessors;
    BuildField(TypeList<KeyLocalType>(), invoke_context, Make<StructField, std::tuple_element_t<0, Accessors>>(pair), value.first);
    BuildField(TypeList<ValueLocalType>(), invoke_context, Make<StructField, std::tuple_element_t<1, Accessors>>(pair), value.second);
}

// TODO: Should generalize this to work with arbitrary length tuples, not just length 2-tuples.
template <typename KeyLocalType, typename ValueLocalType, typename Value, typename Output>
void CustomBuildField(TypeList<std::tuple<KeyLocalType, ValueLocalType>>,
    Priority<1>,
    InvokeContext& invoke_context,
    Value&& value,
    Output&& output)
{
    auto pair = output.init();
    using Accessors = typename ProxyStruct<typename decltype(pair)::Builds>::Accessors;
    BuildField(TypeList<KeyLocalType>(), invoke_context, Make<StructField, std::tuple_element_t<0, Accessors>>(pair), std::get<0>(value));
    BuildField(TypeList<ValueLocalType>(), invoke_context, Make<StructField, std::tuple_element_t<1, Accessors>>(pair), std::get<1>(value));
}

template <typename LocalType, typename Value, typename Output>
void CustomBuildField(TypeList<const LocalType>,
    Priority<0>,
    InvokeContext& invoke_context,
    Value&& value,
    Output&& output)
{
    BuildField(TypeList<LocalType>(), invoke_context, output, std::forward<Value>(value));
}

template <typename LocalType, typename Value, typename Output>
void CustomBuildField(TypeList<LocalType&>, Priority<0>, InvokeContext& invoke_context, Value&& value, Output&& output)
{
    BuildField(TypeList<LocalType>(), invoke_context, output, std::forward<Value>(value));
}

template <typename LocalType, typename Value, typename Output>
void CustomBuildField(TypeList<LocalType&&>,
    Priority<0>,
    InvokeContext& invoke_context,
    Value&& value,
    Output&& output)
{
    BuildField(TypeList<LocalType>(), invoke_context, output, std::forward<Value>(value));
}

template <typename LocalType, typename Value, typename Output>
void CustomBuildField(TypeList<LocalType>, Priority<0>, InvokeContext& invoke_context, Value&& value, Output&& output)
{
    output.set(BuildPrimitive(invoke_context, std::forward<Value>(value), TypeList<decltype(output.get())>()));
}

template <size_t index, typename LocalType, typename Value, typename Output>
void BuildOne(TypeList<LocalType> param,
    InvokeContext& invoke_context,
    Output&& output,
    Value&& value,
    typename std::enable_if < index<ProxyType<LocalType>::fields>::type * enable = nullptr)
{
    using Index = std::integral_constant<size_t, index>;
    using Struct = typename ProxyType<LocalType>::Struct;
    using Accessor = typename std::tuple_element<index, typename ProxyStruct<Struct>::Accessors>::type;
    auto&& field_output = Make<StructField, Accessor>(output);
    auto&& field_value = value.*ProxyType<LocalType>::get(Index());
    BuildField(TypeList<Decay<decltype(field_value)>>(), invoke_context, field_output, field_value);
    BuildOne<index + 1>(param, invoke_context, output, value);
}

template <size_t index, typename LocalType, typename Value, typename Output>
void BuildOne(TypeList<LocalType> param,
    InvokeContext& invoke_context,
    Output& output,
    Value& value,
    typename std::enable_if<index == ProxyType<LocalType>::fields>::type* enable = nullptr)
{
}

template <typename LocalType, typename Value, typename Output>
void CustomBuildField(TypeList<LocalType> local_type,
    Priority<1>,
    InvokeContext& invoke_context,
    Value&& value,
    Output&& output,
    typename ProxyType<LocalType>::Struct* enable = nullptr)
{
    BuildOne<0>(local_type, invoke_context, output.init(), value);
}

//! PassField override for C++ pointer arguments.
template <typename Accessor, typename LocalType, typename ServerContext, typename Fn, typename... Args>
void PassField(Priority<1>, TypeList<LocalType*>, ServerContext& server_context, const Fn& fn, Args&&... args)
{
    const auto& params = server_context.call_context.getParams();
    const auto& input = Make<StructField, Accessor>(params);

    if (!input.want()) {
        fn.invoke(server_context, std::forward<Args>(args)..., nullptr);
        return;
    }

    InvokeContext& invoke_context = server_context;
    Decay<LocalType> param;

    MaybeReadField(std::integral_constant<bool, Accessor::in>(), TypeList<LocalType>(), invoke_context, input,
        ReadDestValue(param));

    fn.invoke(server_context, std::forward<Args>(args)..., &param);

    auto&& results = server_context.call_context.getResults();
    MaybeBuildField(std::integral_constant<bool, Accessor::out>(), TypeList<LocalType>(), invoke_context,
        Make<StructField, Accessor>(results), param);
}

//! PassField override for callable interface reference arguments.
template <typename Accessor, typename LocalType, typename ServerContext, typename Fn, typename... Args>
auto PassField(Priority<1>, TypeList<LocalType&>, ServerContext& server_context, Fn&& fn, Args&&... args)
    -> Require<typename decltype(Accessor::get(server_context.call_context.getParams()))::Calls>
{
    // Just create a temporary ProxyClient if argument is a reference to an
    // interface client. If argument needs to have a longer lifetime and not be
    // destroyed after this call, a CustomPassField overload can be implemented
    // to bypass this code, and a custom ProxyServerMethodTraits overload can be
    // implemented in order to read the capability pointer out of params and
    // construct a ProxyClient with a longer lifetime.
    const auto& params = server_context.call_context.getParams();
    const auto& input = Make<StructField, Accessor>(params);
    using Interface = typename Decay<decltype(input.get())>::Calls;
    auto param = std::make_unique<ProxyClient<Interface>>(input.get(), server_context.proxy_server.m_context.connection, false);
    fn.invoke(server_context, std::forward<Args>(args)..., *param);
}

template <typename... Args>
void MaybeBuildField(std::true_type, Args&&... args)
{
    BuildField(std::forward<Args>(args)...);
}
template <typename... Args>
void MaybeBuildField(std::false_type, Args&&...)
{
}
template <typename... Args>
void MaybeReadField(std::true_type, Args&&... args)
{
    ReadField(std::forward<Args>(args)...);
}
template <typename... Args>
void MaybeReadField(std::false_type, Args&&...)
{
}

template <typename LocalType, typename Value, typename Output>
void MaybeSetWant(TypeList<LocalType*>, Priority<1>, Value&& value, Output&& output)
{
    if (value) {
        output.setWant();
    }
}

template <typename LocalTypes, typename... Args>
void MaybeSetWant(LocalTypes, Priority<0>, Args&&...)
{
}

//! Default PassField implementation calling MaybeReadField/MaybeBuildField.
template <typename Accessor, typename LocalType, typename ServerContext, typename Fn, typename... Args>
void PassField(Priority<0>, TypeList<LocalType>, ServerContext& server_context, Fn&& fn, Args&&... args)
{
    InvokeContext& invoke_context = server_context;
    using ArgType = RemoveCvRef<LocalType>;
    std::optional<ArgType> param;
    const auto& params = server_context.call_context.getParams();
    MaybeReadField(std::integral_constant<bool, Accessor::in>(), TypeList<ArgType>(), invoke_context,
        Make<StructField, Accessor>(params), ReadDestEmplace(TypeList<ArgType>(), [&](auto&&... args) -> auto& {
            param.emplace(std::forward<decltype(args)>(args)...);
            return *param;
        }));
    if constexpr (Accessor::in) {
        assert(param);
    } else {
        if (!param) param.emplace();
    }
    fn.invoke(server_context, std::forward<Args>(args)..., static_cast<LocalType&&>(*param));
    auto&& results = server_context.call_context.getResults();
    MaybeBuildField(std::integral_constant<bool, Accessor::out>(), TypeList<LocalType>(), invoke_context,
        Make<StructField, Accessor>(results), *param);
}

//! Default PassField implementation for count(0) arguments, calling ReadField/BuildField
template <typename Accessor, typename ServerContext, typename Fn, typename... Args>
void PassField(Priority<0>, TypeList<>, ServerContext& server_context, const Fn& fn, Args&&... args)
{
    const auto& params = server_context.call_context.getParams();
    const auto& input = Make<StructField, Accessor>(params);
    ReadField(TypeList<>(), server_context, input);
    fn.invoke(server_context, std::forward<Args>(args)...);
    auto&& results = server_context.call_context.getResults();
    BuildField(TypeList<>(), server_context, Make<StructField, Accessor>(results));
}

template <>
struct ProxyServer<ThreadMap> final : public virtual ThreadMap::Server
{
public:
    ProxyServer(Connection& connection);
    kj::Promise<void> makeThread(MakeThreadContext context) override;
    Connection& m_connection;
};

template <typename Output>
void CustomBuildField(TypeList<>,
    Priority<1>,
    InvokeContext& invoke_context,
    Output&& output,
    typename std::enable_if<std::is_same<decltype(output.get()), ThreadMap::Client>::value>::type* enable = nullptr)
{
    output.set(kj::heap<ProxyServer<ThreadMap>>(invoke_context.connection));
}

template <typename Input>
decltype(auto) CustomReadField(TypeList<>,
    Priority<1>,
    InvokeContext& invoke_context,
    Input&& input,
    typename std::enable_if<std::is_same<decltype(input.get()), ThreadMap::Client>::value>::type* enable = nullptr)
{
    invoke_context.connection.m_thread_map = input.get();
}

template <typename Derived, size_t N = 0>
struct IterateFieldsHelper
{
    template <typename Arg1, typename Arg2, typename ParamList, typename NextFn, typename... NextFnArgs>
    void handleChain(Arg1&& arg1, Arg2&& arg2, ParamList, NextFn&& next_fn, NextFnArgs&&... next_fn_args)
    {
        using S = Split<N, ParamList>;
        handleChain(std::forward<Arg1>(arg1), std::forward<Arg2>(arg2), typename S::First());
        next_fn.handleChain(std::forward<Arg1>(arg1), std::forward<Arg2>(arg2), typename S::Second(),
            std::forward<NextFnArgs>(next_fn_args)...);
    }

    template <typename Arg1, typename Arg2, typename ParamList>
    void handleChain(Arg1&& arg1, Arg2&& arg2, ParamList)
    {
        static_cast<Derived*>(this)->handleField(std::forward<Arg1>(arg1), std::forward<Arg2>(arg2), ParamList());
    }
};

struct IterateFields : IterateFieldsHelper<IterateFields, 0>
{
    template <typename Arg1, typename Arg2, typename ParamList>
    void handleField(Arg1&&, Arg2&&, ParamList)
    {
    }
};

template <typename Exception, typename Accessor>
struct ClientException
{
    struct BuildParams : IterateFieldsHelper<BuildParams, 0>
    {
        template <typename Params, typename ParamList>
        void handleField(InvokeContext& invoke_context, Params& params, ParamList)
        {
        }

        BuildParams(ClientException* client_exception) : m_client_exception(client_exception) {}
        ClientException* m_client_exception;
    };

    struct ReadResults : IterateFieldsHelper<ReadResults, 0>
    {
        template <typename Results, typename ParamList>
        void handleField(InvokeContext& invoke_context, Results& results, ParamList)
        {
            StructField<Accessor, Results> input(results);
            if (input.has()) {
                ThrowField(TypeList<Exception>(), invoke_context, input);
            }
        }

        ReadResults(ClientException* client_exception) : m_client_exception(client_exception) {}
        ClientException* m_client_exception;
    };
};

template <typename Accessor, typename... Types>
struct ClientParam
{
    ClientParam(Types&&... values) : m_values(values...) {}

    struct BuildParams : IterateFieldsHelper<BuildParams, sizeof...(Types)>
    {
        template <typename... Args>
        void handleField(Args&&... args)
        {
            callBuild<0>(std::forward<Args>(args)...);
        }

        // TODO Possible optimization to speed up compile time:
        // https://stackoverflow.com/a/7858971 Using enable_if below to check
        // position when unpacking tuple might be slower than pattern matching
        // approach in the stack overflow solution
        template <size_t I, typename... Args>
        auto callBuild(Args&&... args) -> typename std::enable_if<(I < sizeof...(Types))>::type
        {
            callBuild<I + 1>(std::forward<Args>(args)..., std::get<I>(m_client_param->m_values));
        }

        template <size_t I, typename Params, typename ParamList, typename... Values>
        auto callBuild(ClientInvokeContext& invoke_context, Params& params, ParamList, Values&&... values) ->
            typename std::enable_if<(I == sizeof...(Types))>::type
        {
            MaybeBuildField(std::integral_constant<bool, Accessor::in>(), ParamList(), invoke_context,
                Make<StructField, Accessor>(params), std::forward<Values>(values)...);
            MaybeSetWant(
                ParamList(), Priority<1>(), std::forward<Values>(values)..., Make<StructField, Accessor>(params));
        }

        BuildParams(ClientParam* client_param) : m_client_param(client_param) {}
        ClientParam* m_client_param;
    };

    struct ReadResults : IterateFieldsHelper<ReadResults, sizeof...(Types)>
    {
        template <typename... Args>
        void handleField(Args&&... args)
        {
            callRead<0>(std::forward<Args>(args)...);
        }

        template <int I, typename... Args>
        auto callRead(Args&&... args) -> typename std::enable_if<(I < sizeof...(Types))>::type
        {
            callRead<I + 1>(std::forward<Args>(args)..., std::get<I>(m_client_param->m_values));
        }

        template <int I, typename Results, typename... Params, typename... Values>
        auto callRead(ClientInvokeContext& invoke_context, Results& results, TypeList<Params...>, Values&&... values)
            -> typename std::enable_if<I == sizeof...(Types)>::type
        {
            MaybeReadField(std::integral_constant<bool, Accessor::out>(), TypeList<Decay<Params>...>(), invoke_context,
                Make<StructField, Accessor>(results), ReadDestValue(values)...);
        }

        ReadResults(ClientParam* client_param) : m_client_param(client_param) {}
        ClientParam* m_client_param;
    };

    std::tuple<Types&&...> m_values;
};

template <typename Accessor, typename... Types>
ClientParam<Accessor, Types...> MakeClientParam(Types&&... values)
{
    return {std::forward<Types>(values)...};
}

struct ServerCall
{
    // FIXME: maybe call call_context.releaseParams()
    template <typename ServerContext, typename... Args>
    decltype(auto) invoke(ServerContext& server_context, TypeList<>, Args&&... args) const
    {
        return ProxyServerMethodTraits<typename decltype(server_context.call_context.getParams())::Reads>::invoke(
            server_context,
            std::forward<Args>(args)...);
    }
};

struct ServerDestroy
{
    template <typename ServerContext, typename... Args>
    void invoke(ServerContext& server_context, TypeList<>, Args&&... args) const
    {
        server_context.proxy_server.invokeDestroy(std::forward<Args>(args)...);
    }
};

template <typename Accessor, typename Parent>
struct ServerRet : Parent
{
    ServerRet(Parent parent) : Parent(parent) {}

    template <typename ServerContext, typename... Args>
    void invoke(ServerContext& server_context, TypeList<>, Args&&... args) const
    {
        auto&& result = Parent::invoke(server_context, TypeList<>(), std::forward<Args>(args)...);
        auto&& results = server_context.call_context.getResults();
        InvokeContext& invoke_context = server_context;
        BuildField(TypeList<decltype(result)>(), invoke_context, Make<StructField, Accessor>(results),
            std::forward<decltype(result)>(result));
    }
};

template <typename Exception, typename Accessor, typename Parent>
struct ServerExcept : Parent
{
    ServerExcept(Parent parent) : Parent(parent) {}

    template <typename ServerContext, typename... Args>
    void invoke(ServerContext& server_context, TypeList<>, Args&&... args) const
    {
        try {
            return Parent::invoke(server_context, TypeList<>(), std::forward<Args>(args)...);
        } catch (const Exception& exception) {
            auto&& results = server_context.call_context.getResults();
            BuildField(TypeList<Exception>(), server_context, Make<StructField, Accessor>(results), exception);
        }
    }
};

template <class Accessor>
void CustomPassField();

//! PassField override calling CustomPassField function, if it exists.
template <typename Accessor, typename... Args>
auto PassField(Priority<2>, Args&&... args) -> decltype(CustomPassField<Accessor>(std::forward<Args>(args)...))
{
    return CustomPassField<Accessor>(std::forward<Args>(args)...);
};

template <int argc, typename Accessor, typename Parent>
struct ServerField : Parent
{
    ServerField(Parent parent) : Parent(parent) {}

    const Parent& parent() const { return *this; }

    template <typename ServerContext, typename ArgTypes, typename... Args>
    decltype(auto) invoke(ServerContext& server_context, ArgTypes, Args&&... args) const
    {
        return PassField<Accessor>(Priority<2>(),
            typename Split<argc, ArgTypes>::First(),
            server_context,
            this->parent(),
            typename Split<argc, ArgTypes>::Second(),
            std::forward<Args>(args)...);
    }
};

template <int argc, typename Accessor, typename Parent>
ServerField<argc, Accessor, Parent> MakeServerField(Parent parent)
{
    return {parent};
}

template <typename Request>
struct CapRequestTraits;

template <typename _Params, typename _Results>
struct CapRequestTraits<::capnp::Request<_Params, _Results>>
{
    using Params = _Params;
    using Results = _Results;
};

template <typename Client>
void clientDestroy(Client& client)
{
    if (client.m_context.connection) {
        client.m_context.connection->m_loop.log() << "IPC client destroy " << typeid(client).name();
    } else {
        KJ_LOG(INFO, "IPC interrupted client destroy", typeid(client).name());
    }
}

template <typename Server>
void serverDestroy(Server& server)
{
    server.m_context.connection->m_loop.log() << "IPC server destroy" << typeid(server).name();
}

//! Entry point called by generated client code that looks like:
//!
//! ProxyClient<ClassName>::M0::Result ProxyClient<ClassName>::methodName(M0::Param<0> arg0, M0::Param<1> arg1) {
//!     typename M0::Result result;
//!     clientInvoke(*this, &InterfaceName::Client::methodNameRequest, MakeClientParam<...>(arg0), MakeClientParam<...>>(arg1), MakeClientParam<...>(result));
//!     return result;
//! }
//!
//! Ellipses above are where generated Accessor<> type declarations are inserted.
template <typename ProxyClient, typename GetRequest, typename... FieldObjs>
void clientInvoke(ProxyClient& proxy_client, const GetRequest& get_request, FieldObjs&&... fields)
{
    if (!proxy_client.m_context.connection) {
        throw std::logic_error("clientInvoke call made after disconnect");
    }
    if (!g_thread_context.waiter) {
        assert(g_thread_context.thread_name.empty());
        g_thread_context.thread_name = ThreadName(proxy_client.m_context.connection->m_loop.m_exe_name);
        // If next assert triggers, it means clientInvoke is being called from
        // the capnp event loop thread. This can happen when a ProxyServer
        // method implementation that runs synchronously on the event loop
        // thread tries to make a blocking callback to the client. Any server
        // method that makes a blocking callback or blocks in general needs to
        // run asynchronously off the event loop thread. This is easy to fix by
        // just adding a 'context :Proxy.Context' argument to the capnp method
        // declaration so the server method runs in a dedicated thread.
        assert(!g_thread_context.loop_thread);
        g_thread_context.waiter = std::make_unique<Waiter>();
        proxy_client.m_context.connection->m_loop.logPlain()
            << "{" << g_thread_context.thread_name
            << "} IPC client first request from current thread, constructing waiter";
    }
    ClientInvokeContext invoke_context{*proxy_client.m_context.connection, g_thread_context};
    std::exception_ptr exception;
    std::string kj_exception;
    bool done = false;
    proxy_client.m_context.connection->m_loop.sync([&]() {
        auto request = (proxy_client.m_client.*get_request)(nullptr);
        using Request = CapRequestTraits<decltype(request)>;
        using FieldList = typename ProxyClientMethodTraits<typename Request::Params>::Fields;
        IterateFields().handleChain(invoke_context, request, FieldList(), typename FieldObjs::BuildParams{&fields}...);
        proxy_client.m_context.connection->m_loop.logPlain()
            << "{" << invoke_context.thread_context.thread_name << "} IPC client send "
            << TypeName<typename Request::Params>() << " " << LogEscape(request.toString());

        proxy_client.m_context.connection->m_loop.m_task_set->add(request.send().then(
            [&](::capnp::Response<typename Request::Results>&& response) {
                proxy_client.m_context.connection->m_loop.logPlain()
                    << "{" << invoke_context.thread_context.thread_name << "} IPC client recv "
                    << TypeName<typename Request::Results>() << " " << LogEscape(response.toString());
                try {
                    IterateFields().handleChain(
                        invoke_context, response, FieldList(), typename FieldObjs::ReadResults{&fields}...);
                } catch (...) {
                    exception = std::current_exception();
                }
                std::unique_lock<std::mutex> lock(invoke_context.thread_context.waiter->m_mutex);
                done = true;
                invoke_context.thread_context.waiter->m_cv.notify_all();
            },
            [&](const ::kj::Exception& e) {
                kj_exception = kj::str("kj::Exception: ", e).cStr();
                proxy_client.m_context.connection->m_loop.logPlain()
                    << "{" << invoke_context.thread_context.thread_name << "} IPC client exception " << kj_exception;
                std::unique_lock<std::mutex> lock(invoke_context.thread_context.waiter->m_mutex);
                done = true;
                invoke_context.thread_context.waiter->m_cv.notify_all();
            }));
    });

    std::unique_lock<std::mutex> lock(invoke_context.thread_context.waiter->m_mutex);
    invoke_context.thread_context.waiter->wait(lock, [&done]() { return done; });
    if (exception) std::rethrow_exception(exception);
    if (!kj_exception.empty()) proxy_client.m_context.connection->m_loop.raise() << kj_exception;
}

//! Invoke callable `fn()` that may return void. If it does return void, replace
//! return value with value of `ret()`. This is useful for avoiding code
//! duplication and branching in generic code that forwards calls to functions.
template <typename Fn, typename Ret>
auto ReplaceVoid(Fn&& fn, Ret&& ret) ->
    typename std::enable_if<std::is_same<void, decltype(fn())>::value, decltype(ret())>::type
{
    fn();
    return ret();
}

//! Overload of above for non-void `fn()` case.
template <typename Fn, typename Ret>
auto ReplaceVoid(Fn&& fn, Ret&& ret) ->
    typename std::enable_if<!std::is_same<void, decltype(fn())>::value, decltype(fn())>::type
{
    return fn();
}

extern std::atomic<int> server_reqs;

//! Entry point called by generated server code that looks like:
//!
//! kj::Promise<void> ProxyServer<InterfaceName>::methodName(CallContext call_context) {
//!     return serverInvoke(*this, call_context, MakeServerField<0, ...>(MakeServerField<1, ...>(Make<ServerRet, ...>(ServerCall()))));
//! }
//!
//! Ellipses above are where generated Accessor<> type declarations are inserted.
template <typename Server, typename CallContext, typename Fn>
kj::Promise<void> serverInvoke(Server& server, CallContext& call_context, Fn fn)
{
    auto params = call_context.getParams();
    using Params = decltype(params);
    using Results = typename decltype(call_context.getResults())::Builds;

    int req = ++server_reqs;
    server.m_context.connection->m_loop.log() << "IPC server recv request  #" << req << " "
                                     << TypeName<typename Params::Reads>() << " " << LogEscape(params.toString());

    try {
        using ServerContext = ServerInvokeContext<Server, CallContext>;
        using ArgList = typename ProxyClientMethodTraits<typename Params::Reads>::Params;
        ServerContext server_context{server, call_context, req};
        // ReplaceVoid is used to support fn.invoke implementations that execute
        // asynchronously and return promises as well as implementations that
        // execute synchronously and returing void. The invoke function will by
        // synchronous by default, but asynchronous if a proxy.capnp Context
        // argument is passed, and the PassField overload returns a promise
        // posting the request to a thread and waiting for it to complete.
        return ReplaceVoid([&]() { return fn.invoke(server_context, ArgList()); },
            [&]() { return kj::Promise<CallContext>(kj::mv(call_context)); })
            .then([&server, req](CallContext call_context) {
                server.m_context.connection->m_loop.log() << "IPC server send response #" << req << " " << TypeName<Results>()
                                                 << " " << LogEscape(call_context.getResults().toString());
            });
    } catch (const std::exception& e) {
        server.m_context.connection->m_loop.log() << "IPC server unhandled exception: " << e.what();
        throw;
    } catch (...) {
        server.m_context.connection->m_loop.log() << "IPC server unhandled exception";
        throw;
    }
}

//! Map to convert client interface pointers to ProxyContext struct references
//! at runtime using typeids.
struct ProxyTypeRegister {
    template<typename Interface>
    ProxyTypeRegister(TypeList<Interface>) {
        types().emplace(typeid(Interface), [](void* iface) -> ProxyContext& { return static_cast<typename mp::ProxyType<Interface>::Client&>(*static_cast<Interface*>(iface)).m_context; });
    }
    using Types = std::map<std::type_index, ProxyContext&(*)(void*)>;
    static Types& types() { static Types types; return types; }
};

} // namespace mp

#endif // MP_PROXY_TYPES_H
