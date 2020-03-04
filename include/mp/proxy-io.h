// Copyright (c) 2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MP_PROXY_IO_H
#define MP_PROXY_IO_H

#include <mp/proxy.h>
#include <mp/util.h>

#include <mp/proxy.capnp.h>

#include <boost/exception/diagnostic_information.hpp>
#include <boost/optional/optional.hpp>
#include <capnp/rpc-twoparty.h>

#include <assert.h>
#include <functional>
#include <memory>
#include <string>

namespace mp {
struct ThreadContext;

struct InvokeContext
{
    Connection& connection;
};

struct ClientInvokeContext : InvokeContext
{
    ThreadContext& thread_context;
    ClientInvokeContext(Connection& connection, ThreadContext& thread_context)
        : InvokeContext{connection}, thread_context{thread_context}
    {
    }
};

template <typename ProxyServer, typename CallContext_>
struct ServerInvokeContext : InvokeContext
{
    using CallContext = CallContext_;

    ProxyServer& proxy_server;
    CallContext& call_context;
    int req;

    ServerInvokeContext(ProxyServer& proxy_server, CallContext& call_context, int req)
        : InvokeContext{proxy_server.m_connection}, proxy_server{proxy_server}, call_context{call_context}, req{req}
    {
    }
};

template <typename Interface, typename Params, typename Results>
using ServerContext = ServerInvokeContext<ProxyServer<Interface>, ::capnp::CallContext<Params, Results>>;

template <>
struct ProxyClient<Thread> : public ProxyClientBase<Thread, ::capnp::Void>
{
    using ProxyClientBase::ProxyClientBase;
    // https://stackoverflow.com/questions/22357887/comparing-two-mapiterators-why-does-it-need-the-copy-constructor-of-stdpair
    ProxyClient(const ProxyClient&) = delete;
};

template <>
struct ProxyServer<Thread> final : public Thread::Server
{
public:
    ProxyServer(ThreadContext& thread_context, std::thread&& thread);
    ~ProxyServer();
    kj::Promise<void> getName(GetNameContext context) override;
    ThreadContext& m_thread_context;
    std::thread m_thread;
};

//! Handler for kj::TaskSet failed task events.
class LoggingErrorHandler : public kj::TaskSet::ErrorHandler
{
public:
    LoggingErrorHandler(EventLoop& loop) : m_loop(loop) {}
    void taskFailed(kj::Exception&& exception) override;
    EventLoop& m_loop;
};

using LogFn = std::function<void(bool raise, std::string message)>;

class Logger
{
public:
    Logger(bool raise, LogFn& fn) : m_raise(raise), m_fn(fn) {}
    Logger(Logger&& logger) : m_raise(logger.m_raise), m_fn(logger.m_fn), m_buffer(std::move(logger.m_buffer)) {}
    ~Logger() noexcept(false)
    {
        if (m_fn) m_fn(m_raise, m_buffer.str());
    }

    template <typename T>
    friend Logger& operator<<(Logger& logger, T&& value)
    {
        if (logger.m_fn) logger.m_buffer << std::forward<T>(value);
        return logger;
    }

    template <typename T>
    friend Logger& operator<<(Logger&& logger, T&& value)
    {
        return logger << std::forward<T>(value);
    }

    bool m_raise;
    LogFn& m_fn;
    std::ostringstream m_buffer;
};

std::string LongThreadName(const char* exe_name);

//! Event loop implementation.
//!
//! Based on https://groups.google.com/d/msg/capnproto/TuQFF1eH2-M/g81sHaTAAQAJ
class EventLoop
{
public:
    //! Construct event loop object.
    EventLoop(const char* exe_name, LogFn log_fn, void* context = nullptr);
    ~EventLoop();

    //! Run event loop. Does not return until shutdown. This should only be
    //! called once from the m_thread_id thread. This will block until
    //! the m_num_clients reference count is 0.
    void loop();

    //! Run function on event loop thread. Does not return until function completes.
    //! Must be called while the loop() function is active.
    void post(const std::function<void()>& fn);

    //! Wrapper around EventLoop::post that takes advantage of the
    //! fact that callable will not go out of scope to avoid requirement that it
    //! be copyable.
    template <typename Callable>
    void sync(Callable&& callable)
    {
        return post(std::ref(callable));
    }

    //! Start asynchronous worker thread. This is only used when
    //! there is a broken connection, leaving behind ProxyServerBase objects
    //! that need to be destroyed, in which case server ProxyServer::m_impl
    //! destructors don't have a dedicated thread to work and on shouldn't tie
    //! up the eventloop thread because it may need to do I/O on their behalf.
    void startAsyncThread(std::unique_lock<std::mutex>& lock);

    //! Add/remove remote client reference counts.
    void addClient(std::unique_lock<std::mutex>& lock);
    void removeClient(std::unique_lock<std::mutex>& lock);

    Logger log()
    {
        Logger logger(false, m_log_fn);
        logger << "{" << LongThreadName(m_exe_name) << "} ";
        return std::move(logger);
    }
    Logger logPlain() { return Logger(false, m_log_fn); }
    Logger raise() { return Logger(true, m_log_fn); }

    //! Process name included in thread names so combined debug output from
    //! multiple processes is easier to understand.
    const char* m_exe_name;

    //! ID of the event loop thread
    std::thread::id m_thread_id = std::this_thread::get_id();

    //! Handle of an async worker thread. Joined on destruction. Unset if async
    //! method has not been called.
    std::thread m_async_thread;

    //! Callback function to run on event loop thread during post() or sync() call.
    const std::function<void()>* m_post_fn = nullptr;

    //! Callback functions to run on async thread.
    CleanupList m_async_fns;

    //! Pipe read handle used to wake up the event loop thread.
    int m_wait_fd = -1;

    //! Pipe write handle used to wake up the event loop thread.
    int m_post_fd = -1;

    //! Number of clients holding references to ProxyServerBase objects that
    //! reference this event loop.
    int m_num_clients = 0;

    //! Mutex and condition variable used to post tasks to event loop and async
    //! thread.
    std::mutex m_mutex;
    std::condition_variable m_cv;

    //! Capnp IO context.
    kj::AsyncIoContext m_io_context;

    //! Capnp error handler. Needs to outlive m_task_set.
    LoggingErrorHandler m_error_handler{*this};

    //! Capnp list of pending promises.
    std::unique_ptr<kj::TaskSet> m_task_set;

    //! List of connections.
    std::list<Connection> m_incoming_connections;

    //! External logging callback.
    LogFn m_log_fn;

    //! External context pointer.
    void* m_context;
};

//! Single element task queue used to handle recursive capnp calls. (If server
//! makes an callback into the client in the middle of a request, while client
//! thread is blocked waiting for server response, this is what allows the
//! client to run the request in the same thread, the same way code would run in
//! single process, with the callback sharing same thread stack as the original
//! call.
struct Waiter
{
    Waiter() = default;

    template <typename Fn>
    void post(Fn&& fn)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        assert(!m_fn);
        m_fn = std::move(fn);
        m_cv.notify_all();
    }

    template <class Predicate>
    void wait(std::unique_lock<std::mutex>& lock, Predicate pred)
    {
        m_cv.wait(lock, [&] {
            // Important for this to be "while (m_fn)", not "if (m_fn)" to avoid
            // a lost-wakeup bug. A new m_fn and m_cv notification might be set
            // after then fn() call and before the lock.lock() call in this loop
            // in the case where a capnp response is sent and a brand new
            // request is immediately received.
            while (m_fn) {
                auto fn = std::move(m_fn);
                m_fn = nullptr;
                lock.unlock();
                fn();
                lock.lock();
            }
            bool done = pred();
            return done;
        });
    }

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::function<void()> m_fn;
};

//! Object holding network & rpc state associated with either an incoming server
//! connection, or an outgoing client connection. It must be created and destroyed
//! on the event loop thread.
//! In addition to Cap'n Proto state, it also holds lists of callbacks to run
//! when the connection is closed.
class Connection
{
public:
    Connection(EventLoop& loop, kj::Own<kj::AsyncIoStream>&& stream_)
        : m_loop(loop), m_stream(kj::mv(stream_)),
          m_network(*m_stream, ::capnp::rpc::twoparty::Side::CLIENT, ::capnp::ReaderOptions()),
          m_rpc_system(::capnp::makeRpcClient(m_network))
    {
        std::unique_lock<std::mutex> lock(m_loop.m_mutex);
        m_loop.addClient(lock);
    }
    Connection(EventLoop& loop,
        kj::Own<kj::AsyncIoStream>&& stream_,
        std::function<::capnp::Capability::Client(Connection&)> make_client)
        : m_loop(loop), m_stream(kj::mv(stream_)),
          m_network(*m_stream, ::capnp::rpc::twoparty::Side::SERVER, ::capnp::ReaderOptions()),
          m_rpc_system(::capnp::makeRpcServer(m_network, make_client(*this)))
    {
        std::unique_lock<std::mutex> lock(m_loop.m_mutex);
        m_loop.addClient(lock);
    }

    //! Run cleanup functions. Must be called from the event loop thread. First
    //! calls synchronous cleanup functions while blocked (to free capnp
    //! Capability::Client handles owned by ProxyClient objects), then schedules
    //! asynchronous cleanup functions to run in a worker thread (to run
    //! destructors of m_impl instances owned by ProxyServer objects).
    ~Connection();

    //! Register synchronous cleanup function to run on event loop thread (with
    //! access to capnp thread local variables) when disconnect() is called.
    //! any new i/o.
    CleanupIt addSyncCleanup(std::function<void()> fn);
    void removeSyncCleanup(CleanupIt it);

    //! Register asynchronous cleanup function to run on worker thread when
    //! disconnect() is called.
    void addAsyncCleanup(std::function<void()> fn);

    //! Add disconnect handler.
    template <typename F>
    void onDisconnect(F&& f)
    {
        // Add disconnect handler to local TaskSet to ensure it is cancelled and
        // will never after connection object is destroyed. But when disconnect
        // handler fires, do not call the function f right away, instead add it
        // to the EventLoop TaskSet to avoid "Promise callback destroyed itself"
        // error in cases where f deletes this Connection object.
        m_on_disconnect.add(m_network.onDisconnect().then(
            kj::mvCapture(f, [this](F&& f) { m_loop.m_task_set->add(kj::evalLater(kj::mv(f))); })));
    }

    EventLoop& m_loop;
    kj::Own<kj::AsyncIoStream> m_stream;
    LoggingErrorHandler m_error_handler{m_loop};
    kj::TaskSet m_on_disconnect{m_error_handler};
    ::capnp::TwoPartyVatNetwork m_network;
    ::capnp::RpcSystem<::capnp::rpc::twoparty::VatId> m_rpc_system;

    // ThreadMap interface client, used to create a remote server thread when an
    // client IPC call is being made for the first time from a new thread.
    ThreadMap::Client m_thread_map{nullptr};

    //! Collection of server-side IPC worker threads (ProxyServer<Thread> objects previously returned by
    //! ThreadMap.makeThread) used to service requests to clients.
    ::capnp::CapabilityServerSet<Thread> m_threads;

    //! Cleanup functions to run if connection is broken unexpectedly.
    //! Lists will be empty if all ProxyClient and ProxyServer objects are
    //! destroyed cleanly before the connection is destroyed.
    CleanupList m_sync_cleanup_fns;
    CleanupList m_async_cleanup_fns;
};

//! Vat id for server side of connection. Required argument to RpcSystem::bootStrap()
struct ServerVatId
{
    ::capnp::word scratch[4]{};
    ::capnp::MallocMessageBuilder message{scratch};
    ::capnp::rpc::twoparty::VatId::Builder vat_id{message.getRoot<::capnp::rpc::twoparty::VatId>()};
    ServerVatId() { vat_id.setSide(::capnp::rpc::twoparty::Side::SERVER); }
};

template <typename Interface, typename Impl>
ProxyClientBase<Interface, Impl>::ProxyClientBase(typename Interface::Client client,
    Connection* connection,
    bool destroy_connection)
    : m_client(std::move(client)), m_connection(connection), m_destroy_connection(destroy_connection)
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

template <typename Interface, typename Impl>
ProxyClientBase<Interface, Impl>::~ProxyClientBase() noexcept
{
    // Two shutdown sequences are supported:
    //
    // - A normal sequence where client proxy objects are deleted by external
    //   code that no longer needs them
    //
    // - A garbage collection sequence where the connection or event loop shuts
    //   down while external code is still holding client references.
    //
    // The first case is handled here in destructor when m_loop is not null. The
    // second case is handled by the m_cleanup function, which sets m_connection to
    // null so nothing happens here.
    if (m_connection) {
        // Remove m_cleanup callback so it doesn't run and try to access
        // this object after it's already destroyed.
        m_connection->removeSyncCleanup(m_cleanup);

        // Destroy remote object, waiting for it to deleted server side.
        self().destroy();

        // FIXME: Could just invoke removed addCleanup fn here instead of duplicating code
        m_connection->m_loop.sync([&]() {
            // Release client capability by move-assigning to temporary.
            {
                typename Interface::Client(std::move(self().m_client));
            }
            {
                std::unique_lock<std::mutex> lock(m_connection->m_loop.m_mutex);
                m_connection->m_loop.removeClient(lock);
            }

            if (m_destroy_connection) {
                delete m_connection;
                m_connection = nullptr;
            }
        });
    }
}

template <typename Interface, typename Impl>
ProxyServerBase<Interface, Impl>::ProxyServerBase(Impl* impl, bool owned, Connection& connection)
    : m_impl(impl), m_owned(owned), m_connection(connection)
{
    assert(impl != nullptr);
    std::unique_lock<std::mutex> lock(m_connection.m_loop.m_mutex);
    m_connection.m_loop.addClient(lock);
}

template <typename Interface, typename Impl>
ProxyServerBase<Interface, Impl>::~ProxyServerBase()
{
    if (Impl* impl = m_impl) {
        // If impl is non-null, it means client was not destroyed cleanly (was
        // killed or disconnected). Since client isn't providing thread to run
        // destructor on, run asynchronously. Do not run destructor on current
        // (event loop) thread since destructors could be making IPC calls or
        // doing expensive cleanup.
        if (m_owned) {
            m_connection.addAsyncCleanup([impl] { delete impl; });
        }
        m_impl = nullptr;
        m_owned = false;
    }
    std::unique_lock<std::mutex> lock(m_connection.m_loop.m_mutex);
    m_connection.m_loop.removeClient(lock);
}

template <typename Interface, typename Impl>
void ProxyServerBase<Interface, Impl>::invokeDestroy()
{
    if (m_owned) delete m_impl;
    m_impl = nullptr;
    m_owned = false;
}

struct ThreadContext
{
    //! Identifying string for debug.
    std::string thread_name;

    //! Waiter object used allow client threads blocked waiting for a server
    //! response to execute callbacks made from the client's corresponding
    //! server thread.
    std::unique_ptr<Waiter> waiter = nullptr;

    //! When client is making a request a to server, this is the
    //! `callbackThread` argument it passes in the request, used by the server
    //! in case it needs to make callbacks into the client that need to execute
    //! while the client is waiting. This will be set to a local thread object.
    std::map<Connection*, ProxyClient<Thread>> callback_threads;

    //! When client is making a request to a server, this is the `thread`
    //! argument it passes in the request, used to control which thread on
    //! server will be responsible for executing it. If client call is being
    //! made from a local thread, this will be a remote thread object returned
    //! by makeThread. If a client call is being made from a thread currently
    //! handling a server request, this will be set to the `callbackThread`
    //! request thread argument passed in that request.
    std::map<Connection*, ProxyClient<Thread>> request_threads;

    //! Whether this thread is a capnp event loop thread. Not really used except
    //! to assert false if there's an attempt to execute a blocking operation
    //! which could deadlock the thread.
    bool loop_thread = false;
};

//! Given stream file descriptor, make a new ProxyClient object to send requests
//! over the stream. Also create a new Connection object embedded in the
//! client that is freed when the client is closed.
template <typename InitInterface>
std::unique_ptr<ProxyClient<InitInterface>> ConnectStream(EventLoop& loop, int fd)
{
    typename InitInterface::Client init_client(nullptr);
    std::unique_ptr<Connection> connection;
    loop.sync([&] {
        auto stream =
            loop.m_io_context.lowLevelProvider->wrapSocketFd(fd, kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP);
        connection = std::make_unique<Connection>(loop, kj::mv(stream));
        init_client = connection->m_rpc_system.bootstrap(ServerVatId().vat_id).castAs<InitInterface>();
        Connection* connection_ptr = connection.get();
        connection->onDisconnect([&loop, connection_ptr] {
            loop.log() << "IPC client: unexpected network disconnect.";
            delete connection_ptr;
        });
    });
    return std::make_unique<ProxyClient<InitInterface>>(
        kj::mv(init_client), connection.release(), /* destroy_connection= */ true);
}

//! Given stream and init objects, construct a new ProxyServer object that
//! handles requests from the stream by calling the init object. Embed the
//! ProxyServer in a Connection object that is stored and erased if
//! disconnected. This should be called from the event loop thread.
template <typename InitInterface, typename InitImpl>
void _Serve(EventLoop& loop, kj::Own<kj::AsyncIoStream>&& stream, InitImpl& init)
{
    loop.m_incoming_connections.emplace_front(loop, kj::mv(stream), [&](Connection& connection) {
        // Set owned to false so proxy object doesn't attempt to delete init
        // object on disconnect/close.
        return kj::heap<mp::ProxyServer<InitInterface>>(&init, false, connection);
    });
    auto it = loop.m_incoming_connections.begin();
    it->onDisconnect([&loop, it] {
        loop.log() << "IPC server: socket disconnected.";
        loop.m_incoming_connections.erase(it);
    });
}

//! Given connection receiver and an init object, handle incoming connections by
//! calling _Serve, to create ProxyServer objects and forward requests to the
//! init object.
template <typename InitInterface, typename InitImpl>
void _Listen(EventLoop& loop, kj::Own<kj::ConnectionReceiver>&& listener, InitImpl& init)
{
    auto* ptr = listener.get();
    loop.m_task_set->add(ptr->accept().then(kj::mvCapture(kj::mv(listener),
        [&loop, &init](kj::Own<kj::ConnectionReceiver>&& listener, kj::Own<kj::AsyncIoStream>&& stream) {
            _Serve<InitInterface>(loop, kj::mv(stream), init);
            _Listen<InitInterface>(loop, kj::mv(listener), init);
        })));
}

//! Given stream file descriptor and an init object, handle requests on the
//! stream by calling methods on the Init object.
template <typename InitInterface, typename InitImpl>
void ServeStream(EventLoop& loop, int fd, InitImpl& init)
{
    _Serve<InitInterface>(
        loop, loop.m_io_context.lowLevelProvider->wrapSocketFd(fd, kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP), init);
}

//! Given listening socket file descriptor and an init object, handle incoming
//! connections and requests by calling methods on the Init object.
template <typename InitInterface, typename InitImpl>
void ListenConnections(EventLoop& loop, int fd, InitImpl& init)
{
    loop.sync([&]() {
        _Listen<InitInterface>(loop,
            loop.m_io_context.lowLevelProvider->wrapListenSocketFd(fd, kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP),
            init);
    });
}

extern thread_local ThreadContext g_thread_context;

} // namespace mp

#endif // MP_PROXY_IO_H
