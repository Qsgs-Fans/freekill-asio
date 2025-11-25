# alpine_static.cmake

set(CMAKE_BUILD_TYPE MinSizeRel)

set(BUILD_SHARED_LIBS OFF)
set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_EXE_LINKER_FLAGS "-static -static-libgcc -static-libstdc++ -Wl,--gc-sections ${CMAKE_EXE_LINKER_FLAGS}")

add_definitions(-DFK_EMBEDDED)

add_library(OpenSSL::Crypto STATIC IMPORTED)
set_target_properties(OpenSSL::Crypto PROPERTIES
  IMPORTED_LOCATION /usr/lib/libcrypto.a
)

add_library(readline STATIC IMPORTED)
set_target_properties(readline PROPERTIES
  IMPORTED_LOCATION /usr/lib/libreadline.a
  INTERFACE_LINK_LIBRARIES "ncursesw"
)

# libgit2不仅要手动编译还要手动导入
add_library(git2 STATIC IMPORTED)
set_target_properties(git2 PROPERTIES
  IMPORTED_LOCATION /usr/local/lib/libgit2.a
  INTERFACE_LINK_LIBRARIES "z;ssl;crypto"
)

function(target_link_libraries target)
  set(new_args)
  foreach(arg IN LISTS ARGN)
    if(arg STREQUAL "spdlog::spdlog")
      list(APPEND new_args "fmt::fmt-header-only")
    else()
      list(APPEND new_args ${arg})
    endif()
  endforeach()

  _target_link_libraries(${target} ${new_args})
endfunction()
