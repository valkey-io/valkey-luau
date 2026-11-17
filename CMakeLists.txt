cmake_minimum_required(VERSION 3.13)
project(valkey-luau C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    set(IS_MACOS TRUE)
else()
    set(IS_MACOS FALSE)
endif()

if(NOT EXISTS ${CMAKE_SOURCE_DIR}/deps/luau/CMakeLists.txt)
    message(FATAL_ERROR "deps/luau is empty; run: git submodule update --init --recursive")
endif()

set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_WEB OFF CACHE BOOL "" FORCE)
set(LUAU_EXTERN_C OFF CACHE BOOL "" FORCE)

set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_subdirectory(deps/luau luau EXCLUDE_FROM_ALL)

target_compile_definitions(Luau.VM PRIVATE
    "LUA_API=extern \"C\"" LUA_USE_LONGJMP=1)
target_compile_definitions(Luau.Compiler PRIVATE
    "LUACODE_API=extern \"C\"")

set(MODULE_SRCS
    src/engine_luau.c
    src/script_luau.c
    src/function_luau.c
    src/luau_compat.cpp
    src/sha1.c
    src/rand.c
    src/lua_cjson.c
    src/lua_struct.c
    src/lua_cmsgpack.c
    src/lua_bit.c
    src/strbuf.c
    src/fpconv.c
    src/fpconv_dtoa.c)

add_library(valkeyluau SHARED ${MODULE_SRCS})

target_compile_options(valkeyluau PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
    -W -Wall -fno-common -fPIC -fvisibility=hidden -O2 -D_GNU_SOURCE)

target_compile_definitions(valkeyluau PRIVATE LUA_VERSION_NUM=501)

target_include_directories(valkeyluau PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/deps/luau/VM/include
    ${CMAKE_SOURCE_DIR}/deps/luau/Compiler/include
    ${CMAKE_SOURCE_DIR}/deps/luau/Common/include)

target_link_libraries(valkeyluau PRIVATE Luau.VM Luau.VM.Internals Luau.Compiler Luau.Ast Luau.Bytecode)

if(IS_MACOS)
    set_target_properties(valkeyluau PROPERTIES
        SUFFIX ".so" PREFIX "lib" LINK_FLAGS "-undefined dynamic_lookup")
else()
    set_target_properties(valkeyluau PROPERTIES SUFFIX ".so" PREFIX "lib")
    target_link_libraries(valkeyluau PRIVATE m dl)
    target_link_options(valkeyluau PRIVATE
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/valkeyluau.map"
        "-Wl,-Bsymbolic-functions")
endif()

install(TARGETS valkeyluau LIBRARY DESTINATION lib)
