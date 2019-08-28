# Copyright (c) 2019 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

cmake_minimum_required(VERSION 3.0)
project("Libmultiprocess" CXX)
include(CTest)
find_package(Boost)
find_package(CapnProto)

capnp_generate_cpp(MP_PROXY_SRCS MP_PROXY_HDRS include/mp/proxy.capnp)

set(MP_PUBLIC_HEADERS
  ${MP_PROXY_HDRS}
  include/mp/proxy-io.h
  include/mp/proxy-types.h
  include/mp/proxy.capnp
  include/mp/proxy.h
  include/mp/util.h)

add_library(multiprocess STATIC
  ${MP_PROXY_SRCS}
  ${MP_PUBLIC_HEADERS}
  src/mp/proxy.cpp
  src/mp/util.cpp)
target_include_directories(multiprocess PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
  $<INSTALL_INTERFACE:include>
  ${CAPNP_INCLUDE_DIRECTORY}
  ${Boost_INCLUDE_DIR})
set_target_properties(multiprocess PROPERTIES
    PUBLIC_HEADER "${MP_PUBLIC_HEADERS}"
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED YES)
install(TARGETS multiprocess EXPORT Multiprocess ARCHIVE DESTINATION lib PUBLIC_HEADER DESTINATION include/mp)

add_executable(mpgen src/mp/gen.cpp)
target_include_directories(mpgen PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>)
target_include_directories(mpgen PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_link_libraries(mpgen PRIVATE CapnProto::capnp-rpc)
target_link_libraries(mpgen PRIVATE -L${capnp_LIBRARY_DIRS} capnpc)
target_link_libraries(mpgen PRIVATE multiprocess)
set_target_properties(mpgen PROPERTIES
    INSTALL_RPATH_USE_LINK_PATH TRUE
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED YES)
install(TARGETS mpgen EXPORT Multiprocess RUNTIME DESTINATION bin)

configure_file(include/mp/config.h.in "${CMAKE_CURRENT_BINARY_DIR}/include/mp/config.h")
configure_file(pkgconfig/libmultiprocess.pc.in "${CMAKE_CURRENT_BINARY_DIR}/pkgconfig/libmultiprocess.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/pkgconfig/libmultiprocess.pc" DESTINATION "lib/pkgconfig")
install(FILES "include/mpgen.mk" DESTINATION "include")

install(EXPORT Multiprocess DESTINATION lib/cmake/Multiprocess)

if(BUILD_TESTING)
  add_custom_command(
    OUTPUT
      src/mp/test/foo.capnp.h
      src/mp/test/foo.capnp.c++
      src/mp/test/foo.capnp.proxy.h
      src/mp/test/foo.capnp.proxy-server.c++
      src/mp/test/foo.capnp.proxy-client.c++
      src/mp/test/foo.capnp.proxy-types.c++
      src/mp/test/foo.capnp.proxy-types.h
    COMMAND mpgen "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/src/mp/test/foo.capnp" "${CMAKE_CURRENT_SOURCE_DIR}/include" "${capnp_PREFIX}/include"
    DEPENDS src/mp/test/foo.capnp mpgen
  )
  add_executable(mptest EXCLUDE_FROM_ALL
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
  target_link_libraries(mptest PRIVATE CapnProto::capnp-rpc)
  target_link_libraries(mptest PRIVATE CapnProto::kj-test)
  target_link_libraries(mptest PRIVATE multiprocess)
  set_target_properties(mptest PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED YES)
  add_test(NAME mptest COMMAND mptest)
endif()
