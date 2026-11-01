#ifndef _ENGINE_STRUCTS_H_
#define _ENGINE_STRUCTS_H_

#include <stddef.h>
#include <stdint.h>

#include "lua.h"
#include "valkeymodule.h"

#define LUAU_MEMCAT_EVAL 1
#define LUAU_MEMCAT_FUNCTION 2

#define LUAU_ARGV_CACHE_SIZE 32
#define LUAU_ARGV_CACHE_MAX_LEN 64

typedef struct luauEngineCtx {
    lua_State *GL;

    ValkeyModuleDict *libraries;
    uint64_t next_lib_id;

    int read_guard_ref;
    int write_guard_ref;
    int libload_guard_ref;
    int env_meta_ref;
    int field_guard_ref;

    char *redis_version;
    uint32_t redis_version_num;
    char *server_name;
    char *valkey_version;
    uint32_t valkey_version_num;

    size_t mem_used;
    size_t mem_limit;

    long gc_count;
    long full_gc_count;

    uint32_t rand_state[3];

    ValkeyModuleDict *core_commands;
    ValkeyModuleString *argv_cache[LUAU_ARGV_CACHE_SIZE];
    ValkeyModuleString *argv_buf[LUAU_ARGV_CACHE_SIZE];
    int argv_cache_busy;
} luauEngineCtx;

typedef struct luauLibrary {
    uint64_t lib_id;
    char *name;
    char *code;
    size_t code_len;
    lua_State *env;
    int env_ref;
    int ref_count;
} luauLibrary;

typedef struct luauFunction {
    int is_eval;
    union {
        struct {
            char *bytecode;
            size_t bc_len;
        } eval;
        struct {
            uint64_t lib_id;
            int closure_ref;
        } func;
    };
} luauFunction;

#endif
