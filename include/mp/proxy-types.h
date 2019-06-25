// Copyright (c) 2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MP_PROXY_TYPES_H
#define MP_PROXY_TYPES_H

#include <mp/proxy-io.h>
#include <set>
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
    template<typename A = Accessor> auto get() const -> AUTO_RETURN(A::get(this->m_struct))
    template<typename A = Accessor> auto has() const -> typename std::enable_if<A::optional, bool>::type { return A::getHas(m_struct); }
    template<typename A = Accessor> auto has() const -> typename std::enable_if<!A::optional && A::boxed, bool>::type { return A::has(m_struct); }
    template<typename A = Accessor> auto has() const -> typename std::enable_if<!A::optional && !A::boxed, bool>::type { return true; }
    template<typename A = Accessor> auto want() const -> typename std::enable_if<A::requested, bool>::type { return A::getWant(m_struct); }
    template<typename A = Accessor> auto want() const -> typename std::enable_if<!A::requested, bool>::type { return true; }

    template<typename A = Accessor, typename... Args> auto set(Args&&... args) const -> AUTO_RETURN(A::set(this->m_struct, std::forward<Args>(args)...))
    template<typename A = Accessor, typename... Args> auto init(Args&&... args) const -> AUTO_RETURN(A::init(this->m_struct, std::forward<Args>(args)...))
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
    typename std::enable_if<std::is_same<decltype(output.init()), Context::Builder>::value>::type* enable = nullptr)
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
                        connection))
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
        request_thread = request_threads
                             .emplace(std::piecewise_construct, std::forward_as_tuple(&connection),
                                 std::forward_as_tuple(request.send().getResult(), connection))
                             .first; // Nonblocking due to capnp request pipelining.
    }

    auto context = output.init();
    context.setThread(request_thread->second.m_client);
    context.setCallbackThread(callback_thread->second.m_client);
}

// Invoke promise1, then promise2, and return result of promise2.
template <typename T, typename U>
kj::Promise<U> JoinPromises(kj::Promise<T>&& prom1, kj::Promise<U>&& prom2)
{
    return prom1.then(kj::mvCapture(prom2, [](kj::Promise<U> prom2) { return prom2; }));
}

template <typename Accessor, typename ServerContext, typename Fn, typename... Args>
auto PassField(TypeList<>, ServerContext& server_context, const Fn& fn, const Args&... args) ->
    typename std::enable_if<
        std::is_same<decltype(Accessor::get(server_context.call_context.getParams())), Context::Reader>::value,
        kj::Promise<typename ServerContext::CallContext>>::type
{
    const auto& params = server_context.call_context.getParams();
    Context::Reader context_arg = Accessor::get(params);
    auto future = kj::newPromiseAndFulfiller<typename ServerContext::CallContext>();
    auto& server = server_context.proxy_server;
    int req = server_context.req;
    auto invoke = MakeAsyncCallable(kj::mvCapture(future.fulfiller,
        kj::mvCapture(server_context.call_context,
            [&server, req, fn, args...](typename ServerContext::CallContext call_context,
                kj::Own<kj::PromiseFulfiller<typename ServerContext::CallContext>> fulfiller) {
                const auto& params = call_context.getParams();
                Context::Reader context_arg = Accessor::get(params);
                ServerContext server_context{server, call_context, req};
                {
                    auto& request_threads = g_thread_context.request_threads;
                    auto request_thread = request_threads.find(server.m_connection);
                    if (request_thread == request_threads.end()) {
                        request_thread =
                            g_thread_context.request_threads
                                .emplace(std::piecewise_construct, std::forward_as_tuple(server.m_connection),
                                    std::forward_as_tuple(context_arg.getCallbackThread(), *server.m_connection))
                                .first;
                    } else {
                        // If recursive call, avoid remove request_threads map
                        // entry in KJ_DEFER below.
                        request_thread = request_threads.end();
                    }
                    KJ_DEFER(if (request_thread != request_threads.end()) request_threads.erase(request_thread));
                    fn.invoke(server_context, args...);
                }
                KJ_IF_MAYBE(exception, kj::runCatchingExceptions([&]() {
                    server.m_connection->m_loop.sync([&] {
                        auto fulfiller_dispose = kj::mv(fulfiller);
                        fulfiller_dispose->fulfill(kj::mv(call_context));
                    });
                }))
                {
                    server.m_connection->m_loop.sync([&]() {
                        auto fulfiller_dispose = kj::mv(fulfiller);
                        fulfiller_dispose->reject(kj::mv(*exception));
                    });
                }
            })));

    auto thread_client = context_arg.getThread();
    return JoinPromises(server.m_connection->m_threads.getLocalServer(thread_client)
                            .then([&server, invoke, req](kj::Maybe<Thread::Server&> perhaps) {
                                KJ_IF_MAYBE(thread_server, perhaps)
                                {
                                    const auto& thread = static_cast<ProxyServer<Thread>&>(*thread_server);
                                    server.m_connection->m_loop.log() << "IPC server post request  #" << req << " {"
                                                                      << thread.m_thread_context.thread_name << "}";
                                    thread.m_thread_context.waiter->post(std::move(invoke));
                                }
                                else
                                {
                                    server.m_connection->m_loop.log() << "IPC server error request #" << req
                                                                      << ", missing thread to execute request";
                                    throw std::runtime_error("invalid thread handle");
                                }
                            }),
        kj::mv(future.promise));
}


template <typename Interface, typename Impl>
ProxyClientBase<Interface, Impl>::ProxyClientBase(typename Interface::Client client, Connection& connection)
    : m_client(std::move(client)), m_connection(&connection)
{
    {
        std::unique_lock<std::mutex> lock(m_connection->m_loop.m_mutex);
        m_connection->m_loop.addClient(lock);
    }
    m_cleanup = m_connection->addSyncCleanup([this]() {
        // Release client capability by move-assigning to temporary.
        {
            typename Interface::Client(std::move(self().m_client));
        }
        {
            std::unique_lock<std::mutex> lock(m_connection->m_loop.m_mutex);
            m_connection->m_loop.removeClient(lock);
        }
        m_connection = nullptr;
    });
    self().construct();
}

template <typename Value>
class Emplace
{
    Value& m_value;

    template <typename T, typename... Params>
    static T& call(boost::optional<T>& value, Params&&... params)
    {
        value.emplace(std::forward<Params>(params)...);
        return *value;
    }

    template <typename T, typename... Params>
    static T& call(std::vector<T>& value, Params&&... params)
    {
        value.emplace_back(std::forward<Params>(params)...);
        return value.back();
    }

    template <typename T, typename... Params>
    static const T& call(std::set<T>& value, Params&&... params)
    {
        return *value.emplace(std::forward<Params>(params)...).first;
    }

    template <typename K, typename V, typename... Params>
    static std::pair<const K, V>& call(std::map<K, V>& value, Params&&... params)
    {
        return *value.emplace(std::forward<Params>(params)...).first;
    }

    template <typename T, typename... Params>
    static T& call(std::shared_ptr<T>& value, Params&&... params)
    {
        value = std::make_shared<T>(std::forward<Params>(params)...);
        return *value;
    }

    template <typename T, typename... Params>
    static T& call(std::reference_wrapper<T>& value, Params&&... params)
    {
        value.get().~T();
        new (&value.get()) T(std::forward<Params>(params)...);
        return value.get();
    }

public:
    Emplace(Value& value) : m_value(value) {}

    // Needs to be declared after m_value for compiler to understand declaration.
    template <typename... Params>
    auto operator()(Params&&... params) -> AUTO_RETURN(Emplace::call(this->m_value, std::forward<Params>(params)...))
};

template <typename LocalType, typename Input, typename DestValue>
void ReadFieldUpdate(TypeList<boost::optional<LocalType>>,
    InvokeContext& invoke_context,
    Input&& input,
    DestValue&& value)
{
    if (!input.has()) {
        value.reset();
        return;
    }
    if (value) {
        ReadFieldUpdate(TypeList<LocalType>(), invoke_context, input, *value);
    } else {
        ReadField(TypeList<LocalType>(), invoke_context, input, Emplace<DestValue>(value));
    }
}

template <typename LocalType, typename Input, typename DestValue>
void ReadFieldUpdate(TypeList<std::shared_ptr<LocalType>>,
    InvokeContext& invoke_context,
    Input&& input,
    DestValue&& value)
{
    if (!input.has()) {
        value.reset();
        return;
    }
    if (value) {
        ReadFieldUpdate(TypeList<LocalType>(), invoke_context, input, *value);
    } else {
        ReadField(TypeList<LocalType>(), invoke_context, input, Emplace<DestValue>(value));
    }
}

template <typename LocalType, typename Input, typename DestValue>
void ReadFieldUpdate(TypeList<LocalType*>, InvokeContext& invoke_context, Input&& input, DestValue&& value)
{
    if (value) {
        ReadFieldUpdate(TypeList<LocalType>(), invoke_context, std::forward<Input>(input), *value);
    }
}

template <typename LocalType, typename Input, typename DestValue>
void ReadFieldUpdate(TypeList<std::shared_ptr<const LocalType>>,
    InvokeContext& invoke_context,
    Input&& input,
    DestValue&& value)
{
    if (!input.has()) {
        value.reset();
        return;
    }
    ReadField(TypeList<LocalType>(), invoke_context, std::forward<Input>(input), Emplace<DestValue>(value));
}

template <typename LocalType, typename Input, typename DestValue>
void ReadFieldUpdate(TypeList<std::vector<LocalType>>, InvokeContext& invoke_context, Input&& input, DestValue&& value)
{
    auto data = input.get();
    value.clear();
    value.reserve(data.size());
    for (auto item : data) {
        ReadField(TypeList<LocalType>(), invoke_context, Make<ValueField>(item), Emplace<DestValue>(value));
    }
}

template <typename LocalType, typename Input, typename DestValue>
void ReadFieldUpdate(TypeList<std::set<LocalType>>, InvokeContext& invoke_context, Input&& input, DestValue&& value)
{
    auto data = input.get();
    value.clear();
    for (auto item : data) {
        ReadField(TypeList<LocalType>(), invoke_context, Make<ValueField>(item), Emplace<DestValue>(value));
    }
}

template <typename KeyLocalType, typename ValueLocalType, typename Input, typename DestValue>
void ReadFieldUpdate(TypeList<std::map<KeyLocalType, ValueLocalType>>,
    InvokeContext& invoke_context,
    Input&& input,
    DestValue&& value)
{
    auto data = input.get();
    value.clear();
    for (auto item : data) {
        ReadField(TypeList<std::pair<KeyLocalType, ValueLocalType>>(), invoke_context, Make<ValueField>(item),
            Emplace<DestValue>(value));
    }
}

// Emplace function that when called with tuple of key constructor arguments
// reads value from pair and calls piecewise construct.
template <typename ValueLocalType, typename Input, typename Emplace>
struct PairValueEmplace
{
    InvokeContext& m_context;
    Input& m_input;
    Emplace& m_emplace;
    template <typename KeyTuple>

    // FIXME Should really return reference to emplaced key object.
    void operator()(KeyTuple&& key_tuple)
    {
        const auto& pair = m_input.get();
        using ValueAccessor = typename ProxyStruct<typename Decay<decltype(pair)>::Reads>::ValueAccessor;
        ReadField(TypeList<ValueLocalType>(), m_context, Make<StructField, ValueAccessor>(pair),
            BindTuple(Make<ComposeFn>(GetFn<1>(), Bind(m_emplace, std::piecewise_construct, key_tuple))));
    }
};

template <typename KeyLocalType, typename ValueLocalType, typename Input, typename Emplace>
void ReadFieldNew(TypeList<std::pair<KeyLocalType, ValueLocalType>>,
    InvokeContext& invoke_context,
    Input&& input,
    Emplace&& emplace)
{
    /* This could be simplified a lot with c++14 generic lambdas. All it is doing is:
    ReadField(TypeList<KeyLocalType>(), invoke_context, Make<ValueField>(input.get().getKey()), [&](auto&&... key_args)
    { ReadField(TypeList<ValueLocalType>(), invoke_context, Make<ValueField>(input.get().getValue()), [&](auto&&...
    value_args)
    {
            emplace(std::piecewise_construct, std::forward_as_tuple(key_args...),
    std::forward_as_tuple(value_args...));
        })
    });
    */
    const auto& pair = input.get();
    using KeyAccessor = typename ProxyStruct<typename Decay<decltype(pair)>::Reads>::KeyAccessor;
    ReadField(TypeList<KeyLocalType>(), invoke_context, Make<StructField, KeyAccessor>(pair),
        BindTuple(PairValueEmplace<ValueLocalType, Input, Emplace>{invoke_context, input, emplace}));
}

template <typename KeyLocalType, typename ValueLocalType, typename Input, typename Tuple>
void ReadFieldUpdate(TypeList<std::tuple<KeyLocalType, ValueLocalType>>,
    InvokeContext& invoke_context,
    Input&& input,
    Tuple&& tuple)
{
    const auto& pair = input.get();
    using Struct = ProxyStruct<typename Decay<decltype(pair)>::Reads>;
    ReadFieldUpdate(TypeList<KeyLocalType>(), invoke_context, Make<StructField, typename Struct::KeyAccessor>(pair),
        std::get<0>(tuple));
    ReadFieldUpdate(TypeList<ValueLocalType>(), invoke_context,
        Make<StructField, typename Struct::ValueAccessor>(pair), std::get<1>(tuple));
}

template <typename LocalType, typename Input, typename Emplace>
void ReadFieldNew(TypeList<LocalType>,
    InvokeContext& invoke_context,
    Input&& input,
    Emplace&& emplace,
    typename std::enable_if<std::is_enum<LocalType>::value>::type* enable = 0)
{
    emplace(static_cast<LocalType>(input.get()));
}

template <typename LocalType, typename Input, typename Emplace>
void ReadFieldNew(TypeList<LocalType>,
    InvokeContext& invoke_context,
    Input&& input,
    Emplace&& emplace,
    typename std::enable_if<std::is_integral<LocalType>::value>::type* enable = 0)
{
    auto value = input.get();
    if (value < std::numeric_limits<LocalType>::min() || value > std::numeric_limits<LocalType>::max()) {
        throw std::range_error("out of bound int received");
    }
    emplace(static_cast<LocalType>(value));
}

template <typename LocalType, typename Input, typename Emplace>
void ReadFieldNew(TypeList<LocalType>,
    InvokeContext& invoke_context,
    Input&& input,
    Emplace&& emplace,
    typename std::enable_if<std::is_floating_point<LocalType>::value>::type* enable = 0)
{
    auto value = input.get();
    static_assert(std::is_same<LocalType, decltype(value)>::value, "floating point type mismatch");
    emplace(value);
}

template <typename Input, typename Emplace>
void ReadFieldNew(TypeList<std::string>, InvokeContext& invoke_context, Input&& input, Emplace&& emplace)
{
    auto data = input.get();
    emplace(CharCast(data.begin()), data.size());
}

template <typename Input, size_t size>
void ReadFieldUpdate(TypeList<unsigned char*>,
    InvokeContext& invoke_context,
    Input&& input,
    unsigned char (&value)[size])
{
    auto data = input.get();
    memcpy(value, data.begin(), size);
}

template <typename Interface, typename Impl>
std::unique_ptr<Impl> MakeProxyClient(InvokeContext& context, typename Interface::Client&& client)
{
    return std::make_unique<ProxyClient<Interface>>(std::move(client), context.connection);
}

template <typename Interface, typename Impl>
std::unique_ptr<Impl> CustomMakeProxyClient(InvokeContext& context, typename Interface::Client&& client)
{
    return MakeProxyClient<Interface, Impl>(context, kj::mv(client));
}

template <typename LocalType, typename Input, typename Emplace>
void ReadFieldNew(TypeList<std::unique_ptr<LocalType>>,
    InvokeContext& invoke_context,
    Input&& input,
    Emplace&& emplace,
    typename Decay<decltype(input.get())>::Calls* enable = nullptr)
{
    using Interface = typename Decay<decltype(input.get())>::Calls;
    if (input.has()) {
        emplace(CustomMakeProxyClient<Interface, LocalType>(invoke_context, std::move(input.get())));
    }
}

// ProxyCallFn class is needed because c++11 doesn't support auto lambda parameters.
// It's equivalent c++14: [invoke_context](auto&& params) {
// invoke_context->call(std::forward<decltype(params)>(params)...)
template <typename InvokeContext>
struct ProxyCallFn
{
    InvokeContext m_proxy;

    template <typename... CallParams>
    auto operator()(CallParams&&... params) -> AUTO_RETURN(this->m_proxy->call(std::forward<CallParams>(params)...))
};

template <typename FnR, typename... FnParams, typename Input, typename Emplace>
void ReadFieldNew(TypeList<std::function<FnR(FnParams...)>>,
    InvokeContext& invoke_context,
    Input&& input,
    Emplace&& emplace)
{
    if (input.has()) {
        using Interface = typename Decay<decltype(input.get())>::Calls;
        auto client = std::make_shared<ProxyClient<Interface>>(input.get(), invoke_context.connection);
        emplace(ProxyCallFn<decltype(client)>{std::move(client)});
    }
};

template <typename LocalType, typename Input, typename Value>
void ReadFieldUpdate(TypeList<LocalType>,
    InvokeContext& invoke_context,
    Input&& input,
    Value&& value,
    decltype(ReadFieldNew(TypeList<Decay<LocalType>>(),
        invoke_context,
        std::forward<Input>(input),
        std::declval<Emplace<decltype(std::ref(value))>>()))* enable = nullptr)
{
    auto ref = std::ref(value);
    ReadFieldNew(TypeList<Decay<LocalType>>(), invoke_context, input, Emplace<decltype(ref)>(ref));
}

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
    ReadFieldUpdate(
        TypeList<Decay<decltype(field_value)>>(), invoke_context, Make<StructField, Accessor>(struc), field_value);
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

template <typename LocalType, typename Input, typename Value>
void ReadFieldUpdate(TypeList<LocalType> param,
    InvokeContext& invoke_context,
    Input&& input,
    Value&& value,
    typename ProxyType<LocalType>::Struct* enable = nullptr)
{
    ReadOne<0>(param, invoke_context, input, value);
}

// ReadField calling Emplace when ReadFieldNew is available.
template <typename LocalType, typename Input, typename Emplace>
void ReadFieldImpl(TypeList<LocalType>,
    Priority<2>,
    InvokeContext& invoke_context,
    Input&& input,
    Emplace&& emplace,
    decltype(
        ReadFieldNew(TypeList<Decay<LocalType>>(), invoke_context, input, std::forward<Emplace>(emplace)))* enable =
        nullptr)
{
    ReadFieldNew(TypeList<Decay<LocalType>>(), invoke_context, input, std::forward<Emplace>(emplace));
}

// ReadField calling Emplace when ReadFieldNew is not available, and emplace creates non-const object.
// Call emplace first to create empty value, then ReadFieldUpdate into the new object.
template <typename LocalType, typename Input, typename Emplace>
void ReadFieldImpl(TypeList<LocalType>,
    Priority<1>,
    InvokeContext& invoke_context,
    Input&& input,
    Emplace&& emplace,
    typename std::enable_if<!std::is_void<decltype(emplace())>::value &&
                            !std::is_const<typename std::remove_reference<decltype(emplace())>::type>::value>::type*
        enable = nullptr)
{
    auto&& ref = emplace();
    ReadFieldUpdate(TypeList<Decay<LocalType>>(), invoke_context, input, ref);
}

// ReadField calling Emplace when ReadFieldNew is not available, and emplace creates const object.
// Initialize temporary with ReadFieldUpdate then std::move into emplace.
template <typename LocalType, typename Input, typename Emplace>
void ReadFieldImpl(TypeList<LocalType>, Priority<0>, InvokeContext& invoke_context, Input&& input, Emplace&& emplace)
{
    Decay<LocalType> temp;
    ReadFieldUpdate(TypeList<Decay<LocalType>>(), invoke_context, input, temp);
    emplace(std::move(temp));
}

template <typename LocalTypes, typename Input, typename... Values>
void ReadField(LocalTypes, InvokeContext& invoke_context, Input&& input, Values&&... values)
{
    ReadFieldImpl(LocalTypes(), Priority<2>(), invoke_context, input, std::forward<Values>(values)...);
}

template <typename LocalType, typename Input>
void ThrowField(TypeList<LocalType>, InvokeContext& invoke_context, Input&& input)
{
    ReadField(TypeList<LocalType>(), invoke_context, input, ThrowFn<LocalType>());
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
class ProxyCallbackImpl : public ProxyCallback<std::function<Result(Args...)>>
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
            new Callback(std::forward<Value>(value)), true /* owned */, invoke_context.connection));
    }
}

template <typename Interface, typename Impl>
kj::Own<typename Interface::Server> MakeProxyServer(InvokeContext& context, std::unique_ptr<Impl>&& impl)
{
    return kj::heap<ProxyServer<Interface>>(impl.release(), true /* owned */, context.connection);
}

template <typename Interface, typename Impl>
kj::Own<typename Interface::Server> CustomMakeProxyServer(InvokeContext& context, std::unique_ptr<Impl>&& impl)
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
        output.set(CustomMakeProxyServer<Interface, Impl>(invoke_context, std::move(value)));
    }
}

template <typename LocalType, typename Output>
void CustomBuildField(TypeList<LocalType&>,
    Priority<1>,
    InvokeContext& invoke_context,
    LocalType& value,
    Output&& output,
    typename decltype(output.get())::Calls* enable = nullptr)
{
    // Set owned to false so proxy object doesn't attempt to delete interface
    // reference when it is discarded remotely, or on disconnect.
    output.set(kj::heap<ProxyServer<typename decltype(output.get())::Calls>>(
        &value, false /* owned */, invoke_context.connection));
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
    auto get() const -> AUTO_RETURN(this->m_builder[this->m_index])
    auto init() const -> AUTO_RETURN(this->m_builder[this->m_index])
    template<typename B = Builder, typename Arg> auto set(Arg&& arg) const -> AUTO_RETURN(static_cast<B&>(this->m_builder).set(m_index, std::forward<Arg>(arg)))
    template<typename B = Builder, typename Arg> auto init(Arg&& arg) const -> AUTO_RETURN(static_cast<B&>(this->m_builder).init(m_index, std::forward<Arg>(arg)))
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
    for (auto& elem : value) {
        BuildField(TypeList<LocalType>(), invoke_context, ListOutput<typename decltype(list)::Builds>(list, i),
            std::move(elem));
        ++i;
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
void CustomBuildField(TypeList<boost::optional<LocalType>>,
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
    using KeyAccessor = typename ProxyStruct<typename decltype(pair)::Builds>::KeyAccessor;
    using ValueAccessor = typename ProxyStruct<typename decltype(pair)::Builds>::ValueAccessor;
    BuildField(TypeList<KeyLocalType>(), invoke_context, Make<StructField, KeyAccessor>(pair), value.first);
    BuildField(TypeList<ValueLocalType>(), invoke_context, Make<StructField, ValueAccessor>(pair), value.second);
}

template <typename KeyLocalType, typename ValueLocalType, typename Value, typename Output>
void CustomBuildField(TypeList<std::tuple<KeyLocalType, ValueLocalType>>,
    Priority<1>,
    InvokeContext& invoke_context,
    Value&& value,
    Output&& output)
{
    auto pair = output.init();
    using KeyAccessor = typename ProxyStruct<typename decltype(pair)::Builds>::KeyAccessor;
    using ValueAccessor = typename ProxyStruct<typename decltype(pair)::Builds>::ValueAccessor;
    BuildField(TypeList<KeyLocalType>(), invoke_context, Make<StructField, KeyAccessor>(pair), std::get<0>(value));
    BuildField(TypeList<ValueLocalType>(), invoke_context, Make<StructField, ValueAccessor>(pair), std::get<1>(value));
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

template <typename Accessor, typename LocalType, typename ServerContext, typename Fn, typename... Args>
void PassField(TypeList<LocalType*>, ServerContext& server_context, const Fn& fn, Args&&... args)
{
    InvokeContext& invoke_context = server_context;
    boost::optional<Decay<LocalType>> param;
    const auto& params = server_context.call_context.getParams();
    const auto& input = Make<StructField, Accessor>(params);
    bool want = input.want();
    if (want) {
        MaybeReadField(std::integral_constant<bool, Accessor::in>(), TypeList<LocalType>(), invoke_context, input,
            Emplace<decltype(param)>(param));
        if (!param) param.emplace();
    }
    fn.invoke(server_context, std::forward<Args>(args)..., param ? &*param : nullptr);
    auto&& results = server_context.call_context.getResults();
    if (want) {
        MaybeBuildField(std::integral_constant<bool, Accessor::out>(), TypeList<LocalType>(), invoke_context,
            Make<StructField, Accessor>(results), *param);
    }
}

template <typename Accessor, typename LocalType, typename ServerContext, typename Fn, typename... Args>
auto PassField(TypeList<LocalType&>, ServerContext& server_context, Fn&& fn, Args&&... args)
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
    auto param = std::make_unique<ProxyClient<Interface>>(input.get(), *server_context.proxy_server.m_connection);
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
template <typename... Args>
void MaybeReadFieldUpdate(std::true_type, Args&&... args)
{
    ReadFieldUpdate(std::forward<Args>(args)...);
}
template <typename... Args>
void MaybeReadFieldUpdate(std::false_type, Args&&...)
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

template <typename Accessor, typename LocalType, typename ServerContext, typename Fn, typename... Args>
void DefaultPassField(TypeList<LocalType>, ServerContext& server_context, Fn&& fn, Args&&... args)
{
    InvokeContext& invoke_context = server_context;
    boost::optional<Decay<LocalType>> param;
    const auto& params = server_context.call_context.getParams();
    MaybeReadField(std::integral_constant<bool, Accessor::in>(), TypeList<LocalType>(), invoke_context,
        Make<StructField, Accessor>(params), Emplace<decltype(param)>(param));
    if (!param) param.emplace();
    fn.invoke(server_context, std::forward<Args>(args)..., static_cast<LocalType&&>(*param));
    auto&& results = server_context.call_context.getResults();
    MaybeBuildField(std::integral_constant<bool, Accessor::out>(), TypeList<LocalType>(), invoke_context,
        Make<StructField, Accessor>(results), *param);
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
void ReadFieldUpdate(TypeList<>,
    InvokeContext& invoke_context,
    Input&& input,
    typename std::enable_if<std::is_same<decltype(input.get()), ThreadMap::Client>::value>::type* enable = nullptr)
{
    invoke_context.connection.m_thread_map = input.get();
}

template <typename Accessor, typename ServerContext, typename Fn, typename... Args>
auto PassField(TypeList<>, ServerContext& server_context, const Fn& fn, Args&&... args) -> typename std::enable_if<
    std::is_same<decltype(Accessor::get(server_context.call_context.getParams())), ThreadMap::Client>::value>::type
{
    const auto& params = server_context.call_context.getParams();
    const auto& input = Make<StructField, Accessor>(params);
    ReadFieldUpdate(TypeList<>(), server_context, input);
    fn.invoke(server_context, std::forward<Args>(args)...);
    auto&& results = server_context.call_context.getResults();
    BuildField(TypeList<>(), server_context, Make<StructField, Accessor>(results));
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
            MaybeReadFieldUpdate(std::integral_constant<bool, Accessor::out>(), TypeList<Decay<Params>...>(),
                invoke_context, Make<StructField, Accessor>(results), std::forward<Values>(values)...);
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
    auto invoke(ServerContext& server_context, TypeList<>, Args&&... args) const -> AUTO_RETURN(
        ProxyServerMethodTraits<typename decltype(server_context.call_context.getParams())::Reads>::invoke(
            server_context,
            std::forward<Args>(args)...))
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

// clang-format off
template <typename Accessor, typename... Args>
auto CallPassField(Priority<2>, Args&&... args) -> AUTO_RETURN(CustomPassField<Accessor>(std::forward<Args>(args)...));

template <typename Accessor, typename... Args>
auto CallPassField(Priority<1>, Args&&... args) -> AUTO_RETURN(PassField<Accessor>(std::forward<Args>(args)...));

template <typename Accessor, typename... Args>
auto CallPassField(Priority<0>, Args&&... args) -> AUTO_RETURN(DefaultPassField<Accessor>(std::forward<Args>(args)...));
// clang-format on

template <int argc, typename Accessor, typename Parent>
struct ServerField : Parent
{
    ServerField(Parent parent) : Parent(parent) {}

    const Parent& parent() const { return *this; }

    template <typename ServerContext, typename ArgTypes, typename... Args>
    auto invoke(ServerContext& server_context, ArgTypes, Args&&... args) const
        -> AUTO_RETURN(CallPassField<Accessor>(Priority<2>(),
            typename Split<argc, ArgTypes>::First(),
            server_context,
            this->parent(),
            typename Split<argc, ArgTypes>::Second(),
            std::forward<Args>(args)...))
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
    if (client.m_connection) {
        client.m_connection->m_loop.log() << "IPC client destroy " << typeid(client).name();
    } else {
        KJ_LOG(INFO, "IPC interrupted client destroy", typeid(client).name());
    }
}

template <typename Server>
void serverDestroy(Server& server)
{
    server.m_connection->m_loop.log() << "IPC server destroy" << typeid(server).name();
}

template <typename ProxyClient, typename GetRequest, typename... FieldObjs>
void clientInvoke(ProxyClient& proxy_client, const GetRequest& get_request, FieldObjs&&... fields)
{
    if (!proxy_client.m_connection) {
        throw std::logic_error("clientInvoke call made after disconnect");
    }
    if (!g_thread_context.waiter) {
        assert(g_thread_context.thread_name.empty());
        g_thread_context.thread_name = ThreadName(proxy_client.m_connection->m_loop.m_exe_name);
        assert(!g_thread_context.loop_thread);
        g_thread_context.waiter = std::make_unique<Waiter>();
        proxy_client.m_connection->m_loop.logPlain()
            << "{" << g_thread_context.thread_name
            << "} IPC client first request from current thread, constructing waiter";
    }
    ClientInvokeContext invoke_context{*proxy_client.m_connection, g_thread_context};
    std::exception_ptr exception;
    std::string kj_exception;
    bool done = false;
    proxy_client.m_connection->m_loop.sync([&]() {
        auto request = (proxy_client.m_client.*get_request)(nullptr);
        using Request = CapRequestTraits<decltype(request)>;
        using FieldList = typename ProxyClientMethodTraits<typename Request::Params>::Fields;
        IterateFields().handleChain(invoke_context, request, FieldList(), typename FieldObjs::BuildParams{&fields}...);
        proxy_client.m_connection->m_loop.logPlain()
            << "{" << invoke_context.thread_context.thread_name << "} IPC client send "
            << TypeName<typename Request::Params>() << " " << LogEscape(request.toString());

        proxy_client.m_connection->m_loop.m_task_set->add(request.send().then(
            [&](::capnp::Response<typename Request::Results>&& response) {
                proxy_client.m_connection->m_loop.logPlain()
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
                proxy_client.m_connection->m_loop.logPlain()
                    << "{" << invoke_context.thread_context.thread_name << "} IPC client exception " << kj_exception;
                std::unique_lock<std::mutex> lock(invoke_context.thread_context.waiter->m_mutex);
                done = true;
                invoke_context.thread_context.waiter->m_cv.notify_all();
            }));
    });

    std::unique_lock<std::mutex> lock(invoke_context.thread_context.waiter->m_mutex);
    invoke_context.thread_context.waiter->wait(lock, [&done]() { return done; });
    if (exception) std::rethrow_exception(exception);
    if (!kj_exception.empty()) proxy_client.m_connection->m_loop.raise() << kj_exception;
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

template <typename Server, typename CallContext, typename Fn>
kj::Promise<void> serverInvoke(Server& server, CallContext& call_context, Fn fn)
{
    auto params = call_context.getParams();
    using Params = decltype(params);
    using Results = typename decltype(call_context.getResults())::Builds;

    int req = ++server_reqs;
    server.m_connection->m_loop.log() << "IPC server recv request  #" << req << " "
                                      << TypeName<typename Params::Reads>() << " " << LogEscape(params.toString());

    try {
        using ServerContext = ServerInvokeContext<Server, CallContext>;
        using ArgList = typename ProxyClientMethodTraits<typename Params::Reads>::Params;
        ServerContext server_context{server, call_context, req};
        return ReplaceVoid([&]() { return fn.invoke(server_context, ArgList()); },
            [&]() { return kj::Promise<CallContext>(kj::mv(call_context)); })
            .then([&server, req](CallContext call_context) {
                server.m_connection->m_loop.log() << "IPC server send response #" << req << " " << TypeName<Results>()
                                                  << " " << LogEscape(call_context.getResults().toString());
            });
    } catch (...) {
        server.m_connection->m_loop.log()
            << "IPC server unhandled exception " << boost::current_exception_diagnostic_information();
        throw;
    }
}

} // namespace mp

#endif // MP_PROXY_TYPES_H
