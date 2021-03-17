# Copyright (c) 2021 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

add_custom_command(
  OUTPUT
    init.capnp.h
    init.capnp.c++
    init.capnp.proxy.h
    init.capnp.proxy-server.c++
    init.capnp.proxy-client.c++
    init.capnp.proxy-types.c++
    init.capnp.proxy-types.h
  COMMAND mpgen "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/init.capnp" "${CMAKE_SOURCE_DIR}/include" "${capnp_PREFIX}/include"
  DEPENDS init.capnp mpgen
)

add_custom_command(
  OUTPUT
    calculator.capnp.h
    calculator.capnp.c++
    calculator.capnp.proxy.h
    calculator.capnp.proxy-server.c++
    calculator.capnp.proxy-client.c++
    calculator.capnp.proxy-types.c++
    calculator.capnp.proxy-types.h
  COMMAND mpgen "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/calculator.capnp" "${CMAKE_SOURCE_DIR}/include" "${capnp_PREFIX}/include"
  DEPENDS calculator.capnp mpgen
)

add_executable(mpcalculator
  calculator.capnp.h
  calculator.capnp.c++
  calculator.capnp.proxy.h
  calculator.capnp.proxy-server.c++
  calculator.capnp.proxy-client.c++
  calculator.capnp.proxy-types.c++
  calculator.capnp.proxy-types.h
  calculator.cpp
  init.capnp.h
  init.capnp.c++
  init.capnp.proxy.h
  init.capnp.proxy-server.c++
  init.capnp.proxy-client.c++
  init.capnp.proxy-types.c++
  init.capnp.proxy-types.h
  printer.capnp.h
  printer.capnp.c++
  printer.capnp.proxy.h
  printer.capnp.proxy-server.c++
  printer.capnp.proxy-client.c++
  printer.capnp.proxy-types.c++
  printer.capnp.proxy-types.h
)
target_include_directories(mpcalculator PUBLIC
  ${CAPNP_INCLUDE_DIRECTORY}
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(mpcalculator PRIVATE CapnProto::capnp)
target_link_libraries(mpcalculator PRIVATE CapnProto::capnp-rpc)
target_link_libraries(mpcalculator PRIVATE CapnProto::kj)
target_link_libraries(mpcalculator PRIVATE CapnProto::kj-async)
target_link_libraries(mpcalculator PRIVATE Threads::Threads)
target_link_libraries(mpcalculator PRIVATE multiprocess)
set_target_properties(mpcalculator PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED YES)

add_custom_command(
  OUTPUT
    printer.capnp.h
    printer.capnp.c++
    printer.capnp.proxy.h
    printer.capnp.proxy-server.c++
    printer.capnp.proxy-client.c++
    printer.capnp.proxy-types.c++
    printer.capnp.proxy-types.h
  COMMAND mpgen "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/printer.capnp" "${CMAKE_SOURCE_DIR}/include" "${capnp_PREFIX}/include"
  DEPENDS printer.capnp mpgen
)

add_executable(mpprinter
  calculator.capnp.c++
  calculator.capnp.h
  calculator.capnp.proxy-client.c++
  calculator.capnp.proxy-server.c++
  calculator.capnp.proxy-types.c++
  calculator.capnp.proxy-types.h
  calculator.capnp.proxy.h
  init.capnp.h
  init.capnp.c++
  init.capnp.proxy.h
  init.capnp.proxy-server.c++
  init.capnp.proxy-client.c++
  init.capnp.proxy-types.c++
  init.capnp.proxy-types.h
  printer.capnp.h
  printer.capnp.c++
  printer.capnp.proxy.h
  printer.capnp.proxy-server.c++
  printer.capnp.proxy-client.c++
  printer.capnp.proxy-types.c++
  printer.capnp.proxy-types.h
  printer.cpp
)
target_include_directories(mpprinter PUBLIC
  ${CAPNP_INCLUDE_DIRECTORY}
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(mpprinter PRIVATE CapnProto::capnp)
target_link_libraries(mpprinter PRIVATE CapnProto::capnp-rpc)
target_link_libraries(mpprinter PRIVATE CapnProto::kj)
target_link_libraries(mpprinter PRIVATE CapnProto::kj-async)
target_link_libraries(mpprinter PRIVATE Threads::Threads)
target_link_libraries(mpprinter PRIVATE multiprocess)
set_target_properties(mpprinter PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED YES)

add_executable(mpexample
  calculator.capnp.c++
  calculator.capnp.h
  calculator.capnp.proxy-client.c++
  calculator.capnp.proxy-server.c++
  calculator.capnp.proxy-types.c++
  calculator.capnp.proxy-types.h
  calculator.capnp.proxy.h
  init.capnp.c++
  init.capnp.h
  init.capnp.proxy-client.c++
  init.capnp.proxy-server.c++
  init.capnp.proxy-types.c++
  init.capnp.proxy-types.h
  init.capnp.proxy.h
  printer.capnp.h
  printer.capnp.c++
  printer.capnp.proxy.h
  printer.capnp.proxy-server.c++
  printer.capnp.proxy-client.c++
  printer.capnp.proxy-types.c++
  printer.capnp.proxy-types.h
  printer.h
  calculator.h
  example.cpp
)
target_include_directories(mpexample PUBLIC
  ${CAPNP_INCLUDE_DIRECTORY}
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(mpexample PRIVATE CapnProto::capnp)
target_link_libraries(mpexample PRIVATE CapnProto::capnp-rpc)
target_link_libraries(mpexample PRIVATE CapnProto::kj)
target_link_libraries(mpexample PRIVATE CapnProto::kj-async)
target_link_libraries(mpexample PRIVATE Threads::Threads)
target_link_libraries(mpexample PRIVATE multiprocess)
target_link_libraries(mpexample PRIVATE stdc++fs)
set_target_properties(mpexample PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED YES)

add_custom_target(example DEPENDS mpexample mpcalculator mpprinter)
