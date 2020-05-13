# Copyright (c) 2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

include(CTest)

# Custom test targets for convenience, based on
# https://gitlab.kitware.com/cmake/community/-/wikis/doc/tutorials/EmulateMakeCheck.
#
# CTest already provides a "make test" target, but it just runs existing tests
# that were previously built, without building anything itself. Define "make
# tests" here as a custom target to build all available tests and "make check"
# as a custom target to build and run them.
add_custom_target(tests)
add_custom_target(check COMMAND ${CMAKE_CTEST_COMMAND} DEPENDS tests)

if(BUILD_TESTING AND TARGET CapnProto::kj-test)
  add_custom_command(
    OUTPUT
      src/mp/test/foo.capnp.h
      src/mp/test/foo.capnp.c++
      src/mp/test/foo.capnp.proxy.h
      src/mp/test/foo.capnp.proxy-server.c++
      src/mp/test/foo.capnp.proxy-client.c++
      src/mp/test/foo.capnp.proxy-types.c++
      src/mp/test/foo.capnp.proxy-types.h
    COMMAND mpgen "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/src/mp/test/foo.capnp" "${CMAKE_SOURCE_DIR}/include" "${capnp_PREFIX}/include"
    DEPENDS src/mp/test/foo.capnp mpgen
  )

  set_property(SOURCE ${MP_PROXY_HDRS} PROPERTY GENERATED 1)

  add_executable(mptest
    ${MP_PROXY_HDRS}
    src/mp/test/foo.capnp.h
    src/mp/test/foo.capnp.c++
    src/mp/test/foo.capnp.proxy.h
    src/mp/test/foo.capnp.proxy-server.c++
    src/mp/test/foo.capnp.proxy-client.c++
    src/mp/test/foo.capnp.proxy-types.c++
    src/mp/test/foo.capnp.proxy-types.h
    src/mp/test/foo-types.h
    src/mp/test/foo.h
    src/mp/test/test.cpp
  )
  target_include_directories(mptest PUBLIC
    ${CAPNP_INCLUDE_DIRECTORY}
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/src>
  )
  target_link_libraries(mptest PRIVATE CapnProto::capnp)
  target_link_libraries(mptest PRIVATE CapnProto::capnp-rpc)
  target_link_libraries(mptest PRIVATE CapnProto::kj)
  target_link_libraries(mptest PRIVATE CapnProto::kj-async)
  target_link_libraries(mptest PRIVATE CapnProto::kj-test)
  target_link_libraries(mptest PRIVATE Threads::Threads)
  target_link_libraries(mptest PRIVATE multiprocess)
  set_target_properties(mptest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES)

  add_dependencies(tests mptest)
  add_test(NAME mptest COMMAND mptest)
endif()
