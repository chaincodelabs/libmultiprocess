# Copyright (c) 2024-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

function(target_capnp_sources target include_prefix)
  cmake_parse_arguments(PARSE_ARGV 2
    "TCS"           # prefix
    ""              # options
    ""              # one_value_keywords
    "IMPORT_PATHS"  # multi_value_keywords
  )

  if(NOT TARGET Libmultiprocess::mpgen)
    message(FATAL_ERROR "Target 'Libmultiprocess::mpgen' does not exist.")
  endif()

  set(source_include_prefix ${CMAKE_SOURCE_DIR})
  set(build_include_prefix ${CMAKE_BINARY_DIR})
  file(RELATIVE_PATH relative_path ${CMAKE_SOURCE_DIR} ${include_prefix})
  if(relative_path)
    string(APPEND source_include_prefix "/" "${relative_path}")
    string(APPEND build_include_prefix "/" "${relative_path}")
  endif()

  foreach(capnp_file IN LISTS TCS_UNPARSED_ARGUMENTS)
    add_custom_command(
      OUTPUT ${capnp_file}.c++ ${capnp_file}.h ${capnp_file}.proxy-client.c++ ${capnp_file}.proxy-types.h ${capnp_file}.proxy-server.c++ ${capnp_file}.proxy-types.c++ ${capnp_file}.proxy.h
      COMMAND Libmultiprocess::mpgen ${CMAKE_CURRENT_SOURCE_DIR} ${source_include_prefix} ${CMAKE_CURRENT_SOURCE_DIR}/${capnp_file} ${TCS_IMPORT_PATHS}
      DEPENDS ${capnp_file}
      VERBATIM
    )
    target_sources(${target} PRIVATE
      ${CMAKE_CURRENT_BINARY_DIR}/${capnp_file}.c++
      ${CMAKE_CURRENT_BINARY_DIR}/${capnp_file}.proxy-client.c++
      ${CMAKE_CURRENT_BINARY_DIR}/${capnp_file}.proxy-server.c++
      ${CMAKE_CURRENT_BINARY_DIR}/${capnp_file}.proxy-types.c++
    )
  endforeach()
  target_include_directories(${target} PUBLIC $<BUILD_INTERFACE:${build_include_prefix}>)
  if(TARGET Libmultiprocess::multiprocess)
    target_link_libraries(${target} PRIVATE Libmultiprocess::multiprocess)
  endif()
endfunction()
