# Copyright (c) 2019 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

cmake_minimum_required(VERSION 3.8)

project("Libmultiprocess" CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED YES)

include(CMakePushCheckState)
include(CheckCXXSourceCompiles)
include(GNUInstallDirs)
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
  #include <cstdint>
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

add_library(util OBJECT src/mp/util.cpp)
target_include_directories(util PRIVATE
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
  ${CAPNP_INCLUDE_DIRECTORY})

set(MP_PUBLIC_HEADERS
  ${MP_PROXY_HDRS}
  include/mp/proxy-io.h
  include/mp/proxy-types.h
  include/mp/proxy.h
  include/mp/util.h)

add_library(multiprocess STATIC
  ${MP_PROXY_SRCS}
  ${MP_PUBLIC_HEADERS}
  src/mp/proxy.cpp
  $<TARGET_OBJECTS:util>)
target_include_directories(multiprocess PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  ${CAPNP_INCLUDE_DIRECTORY})
target_link_libraries(multiprocess PRIVATE CapnProto::capnp)
target_link_libraries(multiprocess PRIVATE CapnProto::capnp-rpc)
target_link_libraries(multiprocess PRIVATE CapnProto::kj)
target_link_libraries(multiprocess PRIVATE CapnProto::kj-async)
set_target_properties(multiprocess PROPERTIES
    PUBLIC_HEADER "${MP_PUBLIC_HEADERS}")
install(TARGETS multiprocess EXPORT libmultiprocess-lib
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT lib
  PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mp COMPONENT lib)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/pkgconfig/libmultiprocess.pc"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig COMPONENT lib)
install(EXPORT libmultiprocess-lib
  NAMESPACE Libmultiprocess::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake COMPONENT lib)
add_custom_target(install-lib
  COMMAND ${CMAKE_COMMAND} -DCOMPONENT=lib -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_install.cmake
  VERBATIM)
add_dependencies(install-lib multiprocess)

add_executable(mpgen src/mp/gen.cpp $<TARGET_OBJECTS:util>)
target_include_directories(mpgen PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>)
target_include_directories(mpgen PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(mpgen PRIVATE CapnProto::capnp)
target_link_libraries(mpgen PRIVATE CapnProto::capnp-rpc)
target_link_libraries(mpgen PRIVATE CapnProto::capnpc)
target_link_libraries(mpgen PRIVATE CapnProto::kj)
target_link_libraries(mpgen PRIVATE Threads::Threads)
set_target_properties(mpgen PROPERTIES
    INSTALL_RPATH_USE_LINK_PATH TRUE)
set_target_properties(mpgen PROPERTIES
    PUBLIC_HEADER include/mp/proxy.capnp)
install(TARGETS mpgen EXPORT libmultiprocess-bin
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT bin
  PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mp COMPONENT bin)
install(FILES "include/mpgen.mk"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT bin)
install(EXPORT libmultiprocess-bin
  NAMESPACE Libmultiprocess::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake COMPONENT bin)
add_custom_target(install-bin
  COMMAND ${CMAKE_COMMAND} -DCOMPONENT=bin -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_install.cmake
  VERBATIM)
add_dependencies(install-bin mpgen)

configure_file(include/mp/config.h.in "${CMAKE_CURRENT_BINARY_DIR}/include/mp/config.h")
configure_file(pkgconfig/libmultiprocess.pc.in "${CMAKE_CURRENT_BINARY_DIR}/pkgconfig/libmultiprocess.pc" @ONLY)

add_subdirectory(example EXCLUDE_FROM_ALL)
add_subdirectory(test EXCLUDE_FROM_ALL)
