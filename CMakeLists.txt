# Copyright (c) 2019 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

cmake_minimum_required(VERSION 3.0)
project("Libmultiprocess" CXX)
include(CMakePushCheckState)
include(CheckCXXSourceCompiles)
find_package(CapnProto REQUIRED)
find_package(Threads REQUIRED)

include("cmake/capnp_compat.cmake")

cmake_push_check_state()
set(CMAKE_REQUIRED_LIBRARIES Threads::Threads)
check_cxx_source_compiles("
  #include <pthread.h>
  int main(int argc, char** argv)
  {
    char thread_name[16];
    return pthread_getname_np(pthread_self(), thread_name, sizeof(thread_name));
  }"
  HAVE_PTHREAD_GETNAME_NP)

check_cxx_source_compiles("
  #include <pthread.h>
  int main(int argc, char** argv)
  {
    uint64_t tid;
    pthread_threadid_np(NULL, &tid);
    return 0;
  }"
  HAVE_PTHREAD_THREADID_NP)

check_cxx_source_compiles("
  #include <pthread.h>
  #include <pthread_np.h>
  int main(int argc, char** argv)
  {
    return pthread_getthreadid_np();
  }"
  HAVE_PTHREAD_GETTHREADID_NP)
cmake_pop_check_state()

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
  ${CAPNP_INCLUDE_DIRECTORY})
set_target_properties(multiprocess PROPERTIES
    PUBLIC_HEADER "${MP_PUBLIC_HEADERS}"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES)
install(TARGETS multiprocess EXPORT Multiprocess ARCHIVE DESTINATION lib PUBLIC_HEADER DESTINATION include/mp)

add_executable(mpgen src/mp/gen.cpp)
target_include_directories(mpgen PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>)
target_include_directories(mpgen PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_link_libraries(mpgen PRIVATE CapnProto::capnp)
target_link_libraries(mpgen PRIVATE CapnProto::capnp-rpc)
target_link_libraries(mpgen PRIVATE CapnProto::capnpc)
target_link_libraries(mpgen PRIVATE CapnProto::kj)
target_link_libraries(mpgen PRIVATE Threads::Threads)
target_link_libraries(mpgen PRIVATE multiprocess)
set_target_properties(mpgen PROPERTIES
    INSTALL_RPATH_USE_LINK_PATH TRUE
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES)
install(TARGETS mpgen EXPORT Multiprocess RUNTIME DESTINATION bin)

configure_file(include/mp/config.h.in "${CMAKE_CURRENT_BINARY_DIR}/include/mp/config.h")
configure_file(pkgconfig/libmultiprocess.pc.in "${CMAKE_CURRENT_BINARY_DIR}/pkgconfig/libmultiprocess.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/pkgconfig/libmultiprocess.pc" DESTINATION "lib/pkgconfig")
install(FILES "include/mpgen.mk" DESTINATION "include")

install(EXPORT Multiprocess DESTINATION lib/cmake/Multiprocess)

add_subdirectory(test EXCLUDE_FROM_ALL)
