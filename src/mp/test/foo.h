// Copyright (c) 2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MP_TEST_FOO_H
#define MP_TEST_FOO_H

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mp {
namespace test {

struct FooStruct
{
    std::string name;
    std::vector<int> num_set;
};

class FooCallback
{
public:
    virtual ~FooCallback() = default;
    virtual int call(int arg) = 0;
};

class FooImplementation
{
public:
    int add(int a, int b) { return a + b; }
    int mapSize(const std::map<std::string, std::string>& map) { return map.size(); }
    FooStruct pass(FooStruct foo) { return foo; }
    void raise(FooStruct foo) { throw foo; }
    void initThreadMap() {}
    int callback(FooCallback& callback, int arg) { return callback.call(arg); }
    int callbackUnique(std::unique_ptr<FooCallback> callback, int arg) { return callback->call(arg); }
    int callbackShared(std::shared_ptr<FooCallback> callback, int arg) { return callback->call(arg); }
    void saveCallback(std::shared_ptr<FooCallback> callback) { m_callback = std::move(callback); }
    int callbackSaved(int arg) { return m_callback->call(arg); }
    std::shared_ptr<FooCallback> m_callback;
};

} // namespace test
} // namespace mp

#endif // MP_TEST_FOO_H
