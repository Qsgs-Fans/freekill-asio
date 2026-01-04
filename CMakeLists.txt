# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.18)

project(freekill-asio VERSION 0.1.8)
add_definitions(-DFK_VERSION=\"${CMAKE_PROJECT_VERSION}\")

if (DEFINED RPC_DEBUG)
  add_definitions(-DRPC_DEBUG)
endif()
add_definitions(-DFK_SERVER_ONLY)

add_compile_options(-Wall)
if (${CMAKE_BUILD_TYPE}0 STREQUAL "Debug0")
  # 多么残酷的调试
  add_compile_options(-fsanitize=address -fsanitize=leak -fsanitize=undefined)
  add_link_options(-fsanitize=address -fsanitize=leak -fsanitize=undefined)
endif ()

set(CMAKE_COLOR_DIAGNOSTICS ON)

find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(ZLIB REQUIRED)
find_package(PkgConfig)
pkg_search_module(libgit2 REQUIRED libgit2)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

include_directories(src)

add_executable(freekill-asio)

add_subdirectory(src)

install(TARGETS freekill-asio DESTINATION bin)
install(FILES
  ${PROJECT_SOURCE_DIR}/packages/init.sql
  DESTINATION share/freekill-asio/packages
)
install(FILES
  ${PROJECT_SOURCE_DIR}/server/init.sql
  DESTINATION share/freekill-asio/server
)
