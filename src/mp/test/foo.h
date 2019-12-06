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

class FooImplementation
{
public:
    int add(int a, int b) { return a + b; }
    int mapSize(const std::map<std::string, std::string>& map) { return map.size(); }
    FooStruct pass(FooStruct foo) { return foo; }
};

} // namespace test
} // namespace mp

#endif // MP_TEST_FOO_H
