// Copyright (c) 2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MP_TEST_FOO_TYPES_H
#define MP_TEST_FOO_TYPES_H

#include <mp/proxy-types.h>

namespace mp {
namespace test {

template <typename Output>
void CustomBuildField(TypeList<FooCustom>, Priority<1>, InvokeContext& invoke_context, const FooCustom& value, Output&& output)
{
    BuildField(TypeList<std::string>(), invoke_context, output, value.v1);
    output.setV2(value.v2);
}

template <typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<FooCustom>, Priority<1>, InvokeContext& invoke_context, Input&& input, ReadDest&& read_dest)
{
    messages::FooCustom::Reader custom = input.get();
    return read_dest.update([&](FooCustom& value) {
        value.v1 = ReadField(TypeList<std::string>(), invoke_context, mp::Make<mp::ValueField>(custom.getV1()), ReadDestTemp<std::string>());
        value.v2 = custom.getV2();
    });
}

} // namespace test
} // namespace mp

#endif // MP_TEST_FOO_TYPES_H
