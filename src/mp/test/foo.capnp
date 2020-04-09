# Copyright (c) 2019 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

@0xe102a54b33a43a20;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("mp::test::messages");

using Proxy = import "/mp/proxy.capnp";

interface FooInterface $Proxy.wrap("mp::test::FooImplementation") {
    add @0 (a :Int32, b :Int32) -> (result :Int32);
    mapSize @1 (map :List(Pair(Text, Text))) -> (result :Int32);
    pass @2 (arg :FooStruct) -> (result :FooStruct);
    raise @3 (arg :FooStruct) -> (error :FooStruct $Proxy.exception("mp::test::FooStruct"));
    initThreadMap @4 (threadMap: Proxy.ThreadMap) -> (threadMap :Proxy.ThreadMap);
    callback @5 (context :Proxy.Context, callback :FooCallback, arg: Int32) -> (result :Int32);
    callbackUnique @6 (context :Proxy.Context, callback :FooCallback, arg: Int32) -> (result :Int32);
    callbackShared @7 (context :Proxy.Context, callback :FooCallback, arg: Int32) -> (result :Int32);
    saveCallback @8 (context :Proxy.Context, callback :FooCallback) -> ();
    callbackSaved @9 (context :Proxy.Context, arg: Int32) -> (result :Int32);
}

interface FooCallback $Proxy.wrap("mp::test::FooCallback") {
    destroy @0 (context :Proxy.Context) -> ();
    call @1 (context :Proxy.Context, arg :Int32) -> (result :Int32);
}

struct FooStruct $Proxy.wrap("mp::test::FooStruct") {
    name @0 :Text;
}

struct Pair(Key, Value) {
    key @0 :Key;
    value @1 :Value;
}
