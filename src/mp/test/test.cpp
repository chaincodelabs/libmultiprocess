// Copyright (c) 2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <mp/proxy-types.h>
#include <mp/test/foo.capnp.h>
#include <mp/test/foo.capnp.proxy.h>
#include <mp/test/foo.h>

#include <future>
#include <kj/common.h>
#include <kj/memory.h>
#include <kj/test.h>

namespace mp {
namespace test {

KJ_TEST("Call FooInterface methods")
{
    std::promise<std::unique_ptr<ProxyClient<messages::FooInterface>>> foo_promise;
    std::function<void()> disconnect_client;
    std::thread thread([&]() {
        EventLoop loop("mptest", [](bool raise, const std::string& log) { printf("LOG%i: %s\n", raise, log.c_str()); });
        auto pipe = loop.m_io_context.provider->newTwoWayPipe();

        auto connection_client = std::make_unique<Connection>(loop, kj::mv(pipe.ends[0]));
        auto foo_client = std::make_unique<ProxyClient<messages::FooInterface>>(
            connection_client->m_rpc_system.bootstrap(ServerVatId().vat_id).castAs<messages::FooInterface>(),
            connection_client.get(), /* destroy_connection= */ false);
        foo_promise.set_value(std::move(foo_client));
        disconnect_client = [&] { loop.sync([&] { connection_client.reset(); }); };

        auto connection_server = std::make_unique<Connection>(loop, kj::mv(pipe.ends[1]), [&](Connection& connection) {
            auto foo_server = kj::heap<ProxyServer<messages::FooInterface>>(std::make_shared<FooImplementation>(), connection);
            return capnp::Capability::Client(kj::mv(foo_server));
        });
        connection_server->onDisconnect([&] { connection_server.reset(); });
        loop.loop();
    });

    auto foo = foo_promise.get_future().get();
    KJ_EXPECT(foo->add(1, 2) == 3);

    FooStruct in;
    in.name = "name";
    FooStruct out = foo->pass(in);
    KJ_EXPECT(in.name == out.name);

    FooStruct err;
    try {
        foo->raise(in);
    } catch (const FooStruct& e) {
        err = e;
    }
    KJ_EXPECT(in.name == err.name);

    class Callback : public FooCallback
    {
    public:
        Callback(int expect, int ret) : m_expect(expect), m_ret(ret) {}
        int call(int arg) override
        {
            KJ_EXPECT(arg == m_expect);
            return m_ret;
        }
        int m_expect, m_ret;
    };

    foo->initThreadMap();
    Callback callback(1, 2);
    KJ_EXPECT(foo->callback(callback, 1) == 2);
    KJ_EXPECT(foo->callbackUnique(std::make_unique<Callback>(3, 4), 3) == 4);
    KJ_EXPECT(foo->callbackShared(std::make_shared<Callback>(5, 6), 5) == 6);
    auto saved = std::make_shared<Callback>(7, 8);
    KJ_EXPECT(saved.use_count() == 1);
    foo->saveCallback(saved);
    KJ_EXPECT(saved.use_count() == 2);
    foo->callbackSaved(7);
    KJ_EXPECT(foo->callbackSaved(7) == 8);
    foo->saveCallback(nullptr);
    KJ_EXPECT(saved.use_count() == 1);

    disconnect_client();
    thread.join();
}

} // namespace test
} // namespace mp
