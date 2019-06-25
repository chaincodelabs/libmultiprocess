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
        EventLoop loop("mptest", [](bool raise, const std::string& log) {});
        auto pipe = loop.m_io_context.provider->newTwoWayPipe();

        auto connection_client = std::make_unique<Connection>(loop, kj::mv(pipe.ends[0]), true);
        auto foo_client = std::make_unique<ProxyClient<messages::FooInterface>>(
            connection_client->m_rpc_system.bootstrap(ServerVatId().vat_id).castAs<messages::FooInterface>(),
            *connection_client);
        foo_promise.set_value(std::move(foo_client));
        disconnect_client = [&] { loop.sync([&] { connection_client.reset(); }); };

        auto connection_server = std::make_unique<Connection>(loop, kj::mv(pipe.ends[1]), [&](Connection& connection) {
            auto foo_server = kj::heap<ProxyServer<messages::FooInterface>>(new FooImplementation, true, connection);
            return capnp::Capability::Client(kj::mv(foo_server));
        });
        loop.m_task_set->add(connection_server->m_network.onDisconnect().then([&] { connection_server.reset(); }));
        loop.loop();
    });

    auto foo = foo_promise.get_future().get();
    KJ_EXPECT(foo->add(1, 2) == 3);

    FooStruct in;
    in.name = "name";
    FooStruct out = foo->pass(in);
    KJ_EXPECT(in.name == out.name);

    disconnect_client();
    thread.join();
}

} // namespace test
} // namespace mp
