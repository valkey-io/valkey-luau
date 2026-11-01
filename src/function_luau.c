/*
 * valkey-luau: FUNCTION support -- library loading and server.register_function.
 */

#include <string.h>
#include <time.h>

#include "lua.h"
#include "lualib.h"
#include "luacode.h"

#include "engine_structs.h"
#include "function_luau.h"
#include "script_luau.h"

#define LIBRARY_LOAD_LIST "__luau_load_list"

typedef struct flagStr {
    ValkeyModuleScriptingEngineScriptFlag flag;
    const char *str;
} flagStr;

static flagStr luau_scripts_flags[] = {
    {.flag = VMSE_SCRIPT_FLAG_NO_WRITES, .str = "no-writes"},
    {.flag = VMSE_SCRIPT_FLAG_ALLOW_OOM, .str = "allow-oom"},
    {.flag = VMSE_SCRIPT_FLAG_ALLOW_STALE, .str = "allow-stale"},
    {.flag = VMSE_SCRIPT_FLAG_NO_CLUSTER, .str = "no-cluster"},
    {.flag = VMSE_SCRIPT_FLAG_ALLOW_CROSS_SLOT, .str = "allow-cross-slot-keys"},
    {.flag = 0, .str = NULL},
};

typedef struct luauPendingReg {
    ValkeyModuleString *name;
    ValkeyModuleString *desc;
    uint64_t flags;
    int closure_ref;
} luauPendingReg;

static int luauReadFlags(lua_State *lua, uint64_t *flags) {
    int j = 1;
    uint64_t f_flags = 0;

    while (1) {
        lua_pushnumber(lua, j++);
        lua_gettable(lua, -2);
        int t = lua_type(lua, -1);
        if (t == LUA_TNIL) {
            lua_pop(lua, 1);
            break;
        }
        if (!lua_isstring(lua, -1)) {
            lua_pop(lua, 1);
            return C_ERR;
        }

        const char *flag_str = lua_tostring(lua, -1);
        int found = 0;
        for (flagStr *flag = luau_scripts_flags; flag->str; ++flag) {
            if (strcasecmp(flag->str, flag_str) == 0) {
                f_flags |= flag->flag;
                found = 1;
                break;
            }
        }
        lua_pop(lua, 1);
        if (!found) return C_ERR;
    }

    *flags = f_flags;
    return C_OK;
}

static ValkeyModuleString *luauGetStringObject(lua_State *lua, int index) {
    if (!lua_isstring(lua, index)) return NULL;
    size_t len = 0;
    const char *str = lua_tolstring(lua, index, &len);
    return ValkeyModule_CreateString(NULL, str, len);
}

static void luauLoadListAppend(lua_State *lua, luauPendingReg *reg) {
    lua_getfield(lua, LUA_REGISTRYINDEX, LIBRARY_LOAD_LIST);
    int n = lua_objlen(lua, -1);
    lua_pushlightuserdata(lua, reg);
    lua_rawseti(lua, -2, n + 1);
    lua_pop(lua, 1);
}

static void luauPendingRegFree(lua_State *lua, luauPendingReg *reg) {
    if (reg->name) ValkeyModule_FreeString(NULL, reg->name);
    if (reg->desc) ValkeyModule_FreeString(NULL, reg->desc);
    if (reg->closure_ref) lua_unref(lua, reg->closure_ref);
    ValkeyModule_Free(reg);
}

static int luauRegisterFunctionNamed(lua_State *lua, luauPendingReg *reg) {
    const char *err = NULL;

    if (!lua_istable(lua, 1)) {
        err = "calling server.register_function with a single argument is only "
              "applicable to Lua table (representing named arguments).";
        goto error;
    }

    lua_pushnil(lua);
    while (lua_next(lua, 1)) {
        if (!lua_isstring(lua, -2)) {
            err = "named argument key given to server.register_function is not a string";
            goto error;
        }
        const char *key = lua_tostring(lua, -2);

        if (strcasecmp(key, "function_name") == 0) {
            if (!(reg->name = luauGetStringObject(lua, -1))) {
                err = "function_name argument given to server.register_function "
                      "must be a string";
                goto error;
            }
        } else if (strcasecmp(key, "description") == 0) {
            if (!(reg->desc = luauGetStringObject(lua, -1))) {
                err = "description argument given to server.register_function "
                      "must be a string";
                goto error;
            }
        } else if (strcasecmp(key, "callback") == 0) {
            if (!lua_isfunction(lua, -1)) {
                err = "callback argument given to server.register_function must "
                      "be a function";
                goto error;
            }
            reg->closure_ref = lua_ref(lua, -1);
        } else if (strcasecmp(key, "flags") == 0) {
            if (!lua_istable(lua, -1)) {
                err = "flags argument to server.register_function must be a table "
                      "representing function flags";
                goto error;
            }
            if (luauReadFlags(lua, &reg->flags) != C_OK) {
                err = "unknown flag given";
                goto error;
            }
        } else {
            err = "unknown argument given to server.register_function";
            goto error;
        }
        lua_pop(lua, 1);
    }

    if (!reg->name) {
        err = "server.register_function must get a function name argument";
        goto error;
    }
    if (!reg->closure_ref) {
        err = "server.register_function must get a callback argument";
        goto error;
    }
    return C_OK;

error:
    luauPushError(lua, err);
    return C_ERR;
}

static int luauRegisterFunctionPositional(lua_State *lua, luauPendingReg *reg) {
    const char *err = NULL;

    if (!(reg->name = luauGetStringObject(lua, 1))) {
        err = "first argument to server.register_function must be a string";
        goto error;
    }
    if (!lua_isfunction(lua, 2)) {
        err = "second argument to server.register_function must be a function";
        goto error;
    }
    reg->closure_ref = lua_ref(lua, 2);
    return C_OK;

error:
    luauPushError(lua, err);
    return C_ERR;
}

static int luauRegisterFunction(lua_State *lua) {
    lua_getfield(lua, LUA_REGISTRYINDEX, LIBRARY_LOAD_LIST);
    int gated = lua_istable(lua, -1);
    lua_pop(lua, 1);
    if (!gated) {
        luauPushError(lua, "server.register_function can only be called on "
                           "FUNCTION LOAD command");
        return luauError(lua);
    }

    int argc = lua_gettop(lua);
    if (argc < 1 || argc > 2) {
        luauPushError(lua, "wrong number of arguments to server.register_function");
        return luauError(lua);
    }

    luauPendingReg *reg = ValkeyModule_Calloc(1, sizeof(*reg));
    int rc = (argc == 1) ? luauRegisterFunctionNamed(lua, reg)
                         : luauRegisterFunctionPositional(lua, reg);
    if (rc != C_OK) {
        luauPendingRegFree(lua, reg);
        return luauError(lua);
    }

    luauLoadListAppend(lua, reg);
    return 0;
}

void luauLibraryRelease(luauEngineCtx *engine_ctx, luauLibrary *lib) {
    if (!lib) return;
    if (--lib->ref_count > 0) return;

    ValkeyModule_DictDelC(engine_ctx->libraries, &lib->lib_id, sizeof(lib->lib_id), NULL);

    if (lib->env_ref) lua_unref(engine_ctx->GL, lib->env_ref);
    if (lib->name) ValkeyModule_Free(lib->name);
    if (lib->code) ValkeyModule_Free(lib->code);
    ValkeyModule_Free(lib);
}

static void luauInstallLibraryLoadEnv(luauEngineCtx *engine_ctx, lua_State *L) {
    lua_newtable(L);

    lua_pushstring(L, "register_function");
    lua_pushcfunction(L, luauRegisterFunction, "register_function");
    lua_rawset(L, -3);

    static const char *shared[] = {"log", "LOG_DEBUG", "LOG_VERBOSE",
                                   "LOG_NOTICE", "LOG_WARNING", "REDIS_VERSION",
                                   "REDIS_VERSION_NUM", "VALKEY_VERSION",
                                   "VALKEY_VERSION_NUM", "SERVER_NAME", NULL};
    lua_getfield(L, LUA_GLOBALSINDEX, SERVER_API_NAME);
    for (const char **k = shared; *k; ++k) {
        lua_getfield(L, -1, *k);
        lua_setfield(L, -3, *k);
    }
    lua_pop(L, 1);

    lua_newtable(L);
    luauPushFieldGuard(engine_ctx, L);
    lua_setfield(L, -2, "__index");
    lua_setreadonly(L, -1, 1);
    lua_setmetatable(L, -2);
    lua_setreadonly(L, -1, 1);

    lua_newtable(L);
    lua_pushvalue(L, -2);
    lua_setfield(L, -2, SERVER_API_NAME);
    lua_pushvalue(L, -2);
    lua_setfield(L, -2, REDIS_API_NAME);
    lua_remove(L, -2);

    luauFreezeThreadEnv(engine_ctx, L, 1, 1);
}

static void luauKeepErrorObject(lua_State *T) {
    if (lua_gettop(T) <= 1) return;
    lua_replace(T, 1);
    lua_settop(T, 1);
}

static void luauFinishLibraryEnv(luauEngineCtx *engine_ctx, lua_State *L) {
    lua_setreadonly(L, LUA_GLOBALSINDEX, 0);
    luauFreezeThreadEnv(engine_ctx, L, 0, 0);
}

ValkeyModuleScriptingEngineCompiledFunction **
luauFunctionLibraryCreate(luauEngineCtx *engine_ctx, ValkeyModuleCtx *module_ctx, const char *code, size_t code_len, size_t timeout, size_t *out_num_compiled_functions, ValkeyModuleString **err) {
    *out_num_compiled_functions = 0;

    size_t bc_len = 0;
    lua_CompileOptions opts = {0};
    opts.optimizationLevel = 1;
    opts.debugLevel = 1;
    char *bytecode = luau_compile(code, code_len, &opts, &bc_len);
    if (!bytecode) {
        *err = ValkeyModule_CreateStringPrintf(NULL, "Error compiling function: "
                                                     "out of memory");
        return NULL;
    }

    lua_State *env = lua_newthread(engine_ctx->GL);
    int env_ref = lua_ref(engine_ctx->GL, -1);
    lua_pop(engine_ctx->GL, 1);

    lua_setmemcat(env, LUAU_MEMCAT_FUNCTION);
    luaL_sandboxthread(env);
    luauInstallLibraryLoadEnv(engine_ctx, env);

    lua_newtable(env);
    lua_setfield(env, LUA_REGISTRYINDEX, LIBRARY_LOAD_LIST);

    ValkeyModuleScriptingEngineCompiledFunction **functions = NULL;

    if (luau_load(env, "@user_function", bytecode, bc_len, 0) != 0) {
        *err = ValkeyModule_CreateStringPrintf(NULL, "Error compiling function: %s",
                                               lua_tostring(env, -1));
        free(bytecode);
        goto cleanup;
    }

    luauCallCtx load_ctx = {
        .engine_ctx = engine_ctx,
        .module_ctx = module_ctx,
        .run_ctx = NULL,
        .type = VMSE_FUNCTION,
        .replication_flags = PROPAGATE_AOF | PROPAGATE_REPL,
        .resp = 2,
        .memcat = LUAU_MEMCAT_FUNCTION,
        .deadline_us = 0,
    };
    if (timeout > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        load_ctx.deadline_us = (uint64_t)ts.tv_sec * 1000000 +
                               (uint64_t)ts.tv_nsec / 1000 + timeout * 1000;
    }
    lua_setthreaddata(env, &load_ctx);
    lua_setmemcat(env, LUAU_MEMCAT_FUNCTION);

    luauInterruptInstall(engine_ctx->GL, 1);
    int status = luauResume(&load_ctx, env, 0);
    luauInterruptInstall(engine_ctx->GL, 0);
    lua_setthreaddata(env, NULL);
    if (status == LUA_OK || status == LUA_YIELD)
        lua_settop(env, 0);
    else
        luauKeepErrorObject(env);
    luauFinishLibraryEnv(engine_ctx, env);

    if (status != LUA_OK) {
        if (status == LUA_YIELD) {
            *err = ValkeyModule_CreateStringPrintf(NULL, "FUNCTION LOAD timeout");
        } else {
            errorInfo err_info = {0};
            luauExtractErrorInformation(env, &err_info);
            *err = ValkeyModule_CreateStringPrintf(
                NULL, "Error registering functions: %s", err_info.msg);
            luauErrorInformationDiscard(&err_info);
        }
        free(bytecode);
        goto cleanup;
    }

    lua_getfield(env, LUA_REGISTRYINDEX, LIBRARY_LOAD_LIST);
    int count = lua_objlen(env, -1);
    if (count == 0) {
        lua_pop(env, 1);
        *err = ValkeyModule_CreateStringPrintf(NULL, "No functions registered");
        free(bytecode);
        goto cleanup;
    }

    luauLibrary *lib = ValkeyModule_Calloc(1, sizeof(*lib));
    lib->lib_id = ++engine_ctx->next_lib_id;
    lib->code = lm_strncpy(code, code_len);
    lib->code_len = code_len;
    lib->env = env;
    lib->env_ref = env_ref;
    lib->ref_count = count;
    lib->name = lm_asprintf("lib_%llu", (unsigned long long)lib->lib_id);
    ValkeyModule_DictSetC(engine_ctx->libraries, &lib->lib_id, sizeof(lib->lib_id), lib);

    functions = ValkeyModule_Calloc(count, sizeof(*functions));
    for (int i = 0; i < count; i++) {
        lua_rawgeti(env, -1, i + 1);
        luauPendingReg *reg = (luauPendingReg *)lua_topointer(env, -1);
        lua_pop(env, 1);

        luauFunction *fn = ValkeyModule_Calloc(1, sizeof(*fn));
        fn->is_eval = 0;
        fn->func.lib_id = lib->lib_id;
        fn->func.closure_ref = reg->closure_ref;

        ValkeyModuleScriptingEngineCompiledFunction *cf =
            ValkeyModule_Calloc(1, sizeof(*cf));
        cf->version = VALKEYMODULE_SCRIPTING_ENGINE_ABI_COMPILED_FUNCTION_VERSION;
        cf->name = reg->name;
        cf->desc = reg->desc;
        cf->function = fn;
        cf->f_flags = reg->flags;
        functions[i] = cf;

        ValkeyModule_Free(reg);
    }
    lua_pop(env, 1);

    *out_num_compiled_functions = (size_t)count;
    free(bytecode);

    lua_pushnil(env);
    lua_setfield(env, LUA_REGISTRYINDEX, LIBRARY_LOAD_LIST);
    return functions;

cleanup:
    lua_getfield(env, LUA_REGISTRYINDEX, LIBRARY_LOAD_LIST);
    if (lua_istable(env, -1)) {
        int n = lua_objlen(env, -1);
        for (int i = 0; i < n; i++) {
            lua_rawgeti(env, -1, i + 1);
            luauPendingRegFree(env, (luauPendingReg *)lua_topointer(env, -1));
            lua_pop(env, 1);
        }
    }
    lua_pop(env, 1);
    lua_pushnil(env);
    lua_setfield(env, LUA_REGISTRYINDEX, LIBRARY_LOAD_LIST);
    lua_unref(engine_ctx->GL, env_ref);
    return NULL;
}
