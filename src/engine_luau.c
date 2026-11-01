/*
 * valkey-luau: module entry point and the scripting-engine vtable.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lualib.h"
#include "luacode.h"

#include "engine_structs.h"
#include "function_luau.h"
#include "script_luau.h"

#define DEFAULT_ENGINE_NAME "LUA"
#define MAX_ENGINE_NAMES 4

#define VKM_PUBLIC __attribute__((visibility("default")))

static luauEngineCtx *engine_ctx = NULL;

static ValkeyModuleString *engine_names[MAX_ENGINE_NAMES];
static const char *engine_names_cstr[MAX_ENGINE_NAMES];
static int engine_names_count = 0;

static uint32_t parse_semver(const char *version) {
    unsigned int major = 0, minor = 0, patch = 0;
    sscanf(version, "%u.%u.%u", &major, &minor, &patch);
    return ((major & 0xFF) << 16) | ((minor & 0xFF) << 8) | (patch & 0xFF);
}

static void luauGetVersionInfo(ValkeyModuleCtx *ctx, luauEngineCtx *lctx) {
    ValkeyModuleServerInfoData *info = ValkeyModule_GetServerInfo(ctx, "server");
    if (!info) return;

    const char *rv = ValkeyModule_ServerInfoGetFieldC(info, "redis_version");
    lctx->redis_version = lm_strcpy(rv ? rv : "0.0.0");
    lctx->redis_version_num = parse_semver(lctx->redis_version);

    const char *sn = ValkeyModule_ServerInfoGetFieldC(info, "server_name");
    lctx->server_name = lm_strcpy(sn ? sn : "valkey");

    const char *vv = ValkeyModule_ServerInfoGetFieldC(info, "valkey_version");
    lctx->valkey_version = lm_strcpy(vv ? vv : "0.0.0");
    lctx->valkey_version_num = parse_semver(lctx->valkey_version);

    ValkeyModule_FreeServerInfo(ctx, info);
}

static void refreshBusyReplyThreshold(ValkeyModuleCtx *module_ctx) {
    ValkeyModuleCallReply *reply =
        ValkeyModule_Call(module_ctx, "CONFIG", "ccE", "GET", "busy-reply-threshold");
    if (reply == NULL) return;
    if (ValkeyModule_CallReplyType(reply) == VALKEYMODULE_REPLY_ARRAY &&
        ValkeyModule_CallReplyLength(reply) == 2) {
        ValkeyModuleCallReply *val = ValkeyModule_CallReplyArrayElement(reply, 1);
        if (ValkeyModule_CallReplyType(val) == VALKEYMODULE_REPLY_STRING) {
            const char *v = ValkeyModule_CallReplyStringPtr(val, NULL);
            luauWatchdogSetBusyThreshold(strtoll(v, NULL, 10));
        }
    }
    ValkeyModule_FreeCallReply(reply);
}

static luauEngineCtx *createEngineContext(ValkeyModuleCtx *ctx) {
    luauEngineCtx *lctx = ValkeyModule_Calloc(1, sizeof(*lctx));
    lctx->libraries = ValkeyModule_CreateDict(NULL);
    luauGetVersionInfo(ctx, lctx);

    refreshBusyReplyThreshold(ctx);

    if (luauCreateVM(lctx, ctx) == NULL) {
        ValkeyModule_Log(ctx, "warning", "Failed to create the Luau VM");
        ValkeyModule_FreeDict(NULL, lctx->libraries);
        ValkeyModule_Free(lctx);
        return NULL;
    }

    lctx->core_commands = luauCoreCommandsCreate(ctx);
    if (lctx->core_commands == NULL)
        ValkeyModule_Log(ctx, "warning",
                         "Could not list the built-in commands; server.call will not reuse arguments");
    return lctx;
}

static void destroyEngineContext(luauEngineCtx *lctx) {
    if (!lctx) return;
    luauDestroyVM(lctx);
    luauArgvCacheFree(lctx);
    if (lctx->libraries) ValkeyModule_FreeDict(NULL, lctx->libraries);
    if (lctx->redis_version) ValkeyModule_Free(lctx->redis_version);
    if (lctx->server_name) ValkeyModule_Free(lctx->server_name);
    if (lctx->valkey_version) ValkeyModule_Free(lctx->valkey_version);
    ValkeyModule_Free(lctx);
}

static ValkeyModuleString *luauDecodeCompileError(luauEngineCtx *lctx,
                                                  const char *bytecode,
                                                  size_t bc_len) {
    lua_State *T = lua_newthread(lctx->GL);
    ValkeyModuleString *err = NULL;
    if (luau_load(T, "@user_script", bytecode, bc_len, 0) != 0) {
        err = ValkeyModule_CreateStringPrintf(
            NULL, "Error compiling script (new function): %s", lua_tostring(T, -1));
    }
    lua_pop(lctx->GL, 1);
    return err;
}

static ValkeyModuleScriptingEngineCompiledFunction **
luauEngineCompileCode(ValkeyModuleCtx *module_ctx,
                      ValkeyModuleScriptingEngineCtx *engine_ctx_opaque,
                      ValkeyModuleScriptingEngineSubsystemType type,
                      const char *code,
                      size_t code_len,
                      size_t timeout,
                      size_t *out_num_compiled_functions,
                      ValkeyModuleString **err) {
    luauEngineCtx *lctx = (luauEngineCtx *)engine_ctx_opaque;

    *out_num_compiled_functions = 0;

    if (type == VMSE_FUNCTION) {
        return luauFunctionLibraryCreate(lctx, module_ctx, code, code_len, timeout,
                                         out_num_compiled_functions, err);
    }

    size_t bc_len = 0;
    lua_CompileOptions opts = {0};
    opts.optimizationLevel = 1;
    opts.debugLevel = 1;
    char *bytecode = luau_compile(code, code_len, &opts, &bc_len);
    if (!bytecode) {
        *err = ValkeyModule_CreateStringPrintf(module_ctx,
                                               "Error compiling script: out of memory");
        return NULL;
    }

    ValkeyModuleString *compile_err = luauDecodeCompileError(lctx, bytecode, bc_len);
    if (compile_err) {
        free(bytecode);
        *err = compile_err;
        return NULL;
    }

    luauFunction *fn = ValkeyModule_Calloc(1, sizeof(*fn));
    fn->is_eval = 1;
    fn->eval.bytecode = ValkeyModule_Alloc(bc_len);
    memcpy(fn->eval.bytecode, bytecode, bc_len);
    fn->eval.bc_len = bc_len;
    free(bytecode);

    ValkeyModuleScriptingEngineCompiledFunction *func = ValkeyModule_Calloc(1, sizeof(*func));
    func->version = VALKEYMODULE_SCRIPTING_ENGINE_ABI_COMPILED_FUNCTION_VERSION;
    func->name = NULL;
    func->desc = NULL;
    func->function = fn;
    func->f_flags = 0;

    ValkeyModuleScriptingEngineCompiledFunction **functions =
        ValkeyModule_Calloc(1, sizeof(*functions));
    functions[0] = func;
    *out_num_compiled_functions = 1;
    return functions;
}

static void luauEngineFunctionCall(ValkeyModuleCtx *module_ctx,
                                   ValkeyModuleScriptingEngineCtx *engine_ctx_opaque,
                                   ValkeyModuleScriptingEngineServerRuntimeCtx *server_ctx,
                                   ValkeyModuleScriptingEngineCompiledFunction *compiled_function,
                                   ValkeyModuleScriptingEngineSubsystemType type,
                                   ValkeyModuleString **keys,
                                   size_t nkeys,
                                   ValkeyModuleString **args,
                                   size_t nargs) {
    luauEngineCtx *lctx = (luauEngineCtx *)engine_ctx_opaque;
    luauFunction *fn = (luauFunction *)compiled_function->function;

    luauCallCtx call_ctx = {
        .engine_ctx = lctx,
        .module_ctx = module_ctx,
        .run_ctx = server_ctx,
        .type = type,
        .replication_flags = PROPAGATE_AOF | PROPAGATE_REPL,
        .resp = 2,
        .memcat = (type == VMSE_EVAL) ? LUAU_MEMCAT_EVAL : LUAU_MEMCAT_FUNCTION,
        .mem_budget = 0,
        .deadline_us = 0,
    };
    if (type == VMSE_EVAL) {
        lua_State *T = luauNewScriptThread(lctx, keys, nkeys, args, nargs, 1,
                                           LUAU_MEMCAT_EVAL);
        if (luau_load(T, "@user_script", fn->eval.bytecode, fn->eval.bc_len, 0) != 0) {
            ValkeyModule_ReplyWithErrorFormat(module_ctx, "ERR Error loading script: %s",
                                              lua_tostring(T, -1));
            lua_pop(lctx->GL, 1);
            return;
        }
        luauCallFunction(&call_ctx, T, 0);
        lua_pop(lctx->GL, 1);
        return;
    }

    int nokey = 0;
    luauLibrary *lib = ValkeyModule_DictGetC(lctx->libraries, &fn->func.lib_id,
                                             sizeof(fn->func.lib_id), &nokey);
    if (lib == NULL || nokey) {
        ValkeyModule_ReplyWithError(module_ctx, "ERR Library for function not found");
        return;
    }

    lua_State *T = luauNewScriptThread(lctx, keys, nkeys, args, nargs, 0,
                                       LUAU_MEMCAT_FUNCTION);
    lua_getref(T, fn->func.closure_ref);
    if (!lua_isfunction(T, -1)) {
        ValkeyModule_ReplyWithError(module_ctx, "ERR Function not found");
        lua_pop(lctx->GL, 1);
        return;
    }
    lua_createtable(T, (int)nkeys, 0);
    for (size_t i = 0; i < nkeys; i++) {
        size_t len = 0;
        const char *s = ValkeyModule_StringPtrLen(keys[i], &len);
        lua_pushlstring(T, s, len);
        lua_rawseti(T, -2, (int)i + 1);
    }
    lua_createtable(T, (int)nargs, 0);
    for (size_t i = 0; i < nargs; i++) {
        size_t len = 0;
        const char *s = ValkeyModule_StringPtrLen(args[i], &len);
        lua_pushlstring(T, s, len);
        lua_rawseti(T, -2, (int)i + 1);
    }

    luauCallFunction(&call_ctx, T, 2);
    lua_pop(lctx->GL, 1);
}

static void luauEngineFreeFunction(ValkeyModuleCtx *module_ctx,
                                   ValkeyModuleScriptingEngineCtx *engine_ctx_opaque,
                                   ValkeyModuleScriptingEngineSubsystemType type,
                                   ValkeyModuleScriptingEngineCompiledFunction *compiled_function) {
    VALKEYMODULE_NOT_USED(module_ctx);
    VALKEYMODULE_NOT_USED(type);
    luauEngineCtx *lctx = (luauEngineCtx *)engine_ctx_opaque;
    luauFunction *fn = (luauFunction *)compiled_function->function;

    if (fn) {
        if (fn->is_eval) {
            if (fn->eval.bytecode) ValkeyModule_Free(fn->eval.bytecode);
        } else {
            int nokey = 0;
            luauLibrary *lib = ValkeyModule_DictGetC(lctx->libraries, &fn->func.lib_id,
                                                     sizeof(fn->func.lib_id), &nokey);
            if (lib && !nokey) {
                if (lib->env) lua_unref(lib->env, fn->func.closure_ref);
                luauLibraryRelease(lctx, lib);
            }
        }
        ValkeyModule_Free(fn);
    }

    if (compiled_function->name) ValkeyModule_FreeString(NULL, compiled_function->name);
    if (compiled_function->desc) ValkeyModule_FreeString(NULL, compiled_function->desc);
    ValkeyModule_Free(compiled_function);
}

static size_t luauEngineFunctionMemoryOverhead(
    ValkeyModuleCtx *module_ctx,
    ValkeyModuleScriptingEngineCompiledFunction *compiled_function) {
    VALKEYMODULE_NOT_USED(module_ctx);
    luauFunction *fn = (luauFunction *)compiled_function->function;
    size_t total = ValkeyModule_MallocSize(compiled_function);
    if (fn) {
        total += ValkeyModule_MallocSize(fn);
        if (fn->is_eval && fn->eval.bytecode)
            total += ValkeyModule_MallocSize(fn->eval.bytecode);
    }
    if (compiled_function->name) total += ValkeyModule_MallocSize(compiled_function->name);
    if (compiled_function->desc) total += ValkeyModule_MallocSize(compiled_function->desc);
    return total;
}

static ValkeyModuleScriptingEngineMemoryInfo luauEngineGetMemoryInfo(
    ValkeyModuleCtx *module_ctx,
    ValkeyModuleScriptingEngineCtx *engine_ctx_opaque,
    ValkeyModuleScriptingEngineSubsystemType type) {
    VALKEYMODULE_NOT_USED(module_ctx);
    luauEngineCtx *lctx = (luauEngineCtx *)engine_ctx_opaque;
    ValkeyModuleScriptingEngineMemoryInfo mem_info = {0};
    mem_info.version = VALKEYMODULE_SCRIPTING_ENGINE_ABI_MEMORY_INFO_VERSION;

    if (type == VMSE_ALL) {
        mem_info.used_memory = luauMemoryForCategory(lctx->GL, -1);
    } else if (type == VMSE_EVAL) {
        mem_info.used_memory = luauMemoryForCategory(lctx->GL, LUAU_MEMCAT_EVAL);
    } else {
        mem_info.used_memory = luauMemoryForCategory(lctx->GL, LUAU_MEMCAT_FUNCTION);
    }

    mem_info.engine_memory_overhead = ValkeyModule_MallocSize(lctx);
    return mem_info;
}

typedef struct luauLazyResetCtx {
    char **buffers;
    size_t count;
} luauLazyResetCtx;

static void luauLazyResetCallback(void *context) {
    luauLazyResetCtx *rc = context;
    for (size_t i = 0; i < rc->count; i++) ValkeyModule_Free(rc->buffers[i]);
    ValkeyModule_Free(rc->buffers);
    ValkeyModule_Free(rc);
}

static ValkeyModuleScriptingEngineCallableLazyEnvReset *luauEngineResetEnv(
    ValkeyModuleCtx *module_ctx,
    ValkeyModuleScriptingEngineCtx *engine_ctx_opaque,
    ValkeyModuleScriptingEngineSubsystemType type,
    int async) {
    luauEngineCtx *lctx = (luauEngineCtx *)engine_ctx_opaque;
    refreshBusyReplyThreshold(module_ctx);

    if (type == VMSE_FUNCTION || type == VMSE_ALL) {
        size_t n = 0;
        ValkeyModuleDictIter *it =
            ValkeyModule_DictIteratorStartC(lctx->libraries, "^", NULL, 0);
        while (ValkeyModule_DictNextC(it, NULL, NULL) != NULL) n++;
        ValkeyModule_DictIteratorStop(it);

        char **buffers = n ? ValkeyModule_Calloc(n, sizeof(char *)) : NULL;
        size_t idx = 0;

        it = ValkeyModule_DictIteratorStartC(lctx->libraries, "^", NULL, 0);
        void *data = NULL;
        while (ValkeyModule_DictNextC(it, NULL, &data) != NULL) {
            luauLibrary *lib = data;
            if (!lib) continue;
            if (lib->env_ref) lua_unref(lctx->GL, lib->env_ref);
            if (lib->name) ValkeyModule_Free(lib->name);
            if (buffers && idx < n)
                buffers[idx++] = lib->code;
            else if (lib->code)
                ValkeyModule_Free(lib->code);
            ValkeyModule_Free(lib);
        }
        ValkeyModule_DictIteratorStop(it);

        ValkeyModule_FreeDict(NULL, lctx->libraries);
        lctx->libraries = ValkeyModule_CreateDict(NULL);

        if (async && buffers) {
            luauLazyResetCtx *rc = ValkeyModule_Calloc(1, sizeof(*rc));
            rc->buffers = buffers;
            rc->count = idx;
            ValkeyModuleScriptingEngineCallableLazyEnvReset *cb =
                ValkeyModule_Calloc(1, sizeof(*cb));
            cb->context = rc;
            cb->engineLazyEnvResetCallback = luauLazyResetCallback;
            lua_gc(lctx->GL, LUA_GCCOLLECT, 0);
            return cb;
        }

        for (size_t i = 0; i < idx; i++) ValkeyModule_Free(buffers[i]);
        if (buffers) ValkeyModule_Free(buffers);
    }

    lua_gc(lctx->GL, LUA_GCCOLLECT, 0);
    return NULL;
}

static ValkeyModuleScriptingEngineDebuggerEnableRet luauEngineDebuggerEnable(
    ValkeyModuleCtx *module_ctx,
    ValkeyModuleScriptingEngineCtx *engine_ctx_opaque,
    ValkeyModuleScriptingEngineSubsystemType type,
    const ValkeyModuleScriptingEngineDebuggerCommand **commands,
    size_t *commands_len) {
    VALKEYMODULE_NOT_USED(module_ctx);
    VALKEYMODULE_NOT_USED(engine_ctx_opaque);
    VALKEYMODULE_NOT_USED(type);
    VALKEYMODULE_NOT_USED(commands);
    VALKEYMODULE_NOT_USED(commands_len);
    return VMSE_DEBUG_NOT_SUPPORTED;
}

static void luauEngineDebuggerDisable(ValkeyModuleCtx *module_ctx,
                                      ValkeyModuleScriptingEngineCtx *engine_ctx_opaque,
                                      ValkeyModuleScriptingEngineSubsystemType type) {
    VALKEYMODULE_NOT_USED(module_ctx);
    VALKEYMODULE_NOT_USED(engine_ctx_opaque);
    VALKEYMODULE_NOT_USED(type);
}

static void luauEngineDebuggerStart(ValkeyModuleCtx *module_ctx,
                                    ValkeyModuleScriptingEngineCtx *engine_ctx_opaque,
                                    ValkeyModuleScriptingEngineSubsystemType type,
                                    ValkeyModuleString *source) {
    VALKEYMODULE_NOT_USED(module_ctx);
    VALKEYMODULE_NOT_USED(engine_ctx_opaque);
    VALKEYMODULE_NOT_USED(type);
    VALKEYMODULE_NOT_USED(source);
}

static void luauEngineDebuggerEnd(ValkeyModuleCtx *module_ctx,
                                  ValkeyModuleScriptingEngineCtx *engine_ctx_opaque,
                                  ValkeyModuleScriptingEngineSubsystemType type) {
    VALKEYMODULE_NOT_USED(module_ctx);
    VALKEYMODULE_NOT_USED(engine_ctx_opaque);
    VALKEYMODULE_NOT_USED(type);
}

static ValkeyModuleString *engineNameGet(const char *name, void *privdata) {
    VALKEYMODULE_NOT_USED(name);
    VALKEYMODULE_NOT_USED(privdata);
    return engine_names_count > 0 ? engine_names[0] : NULL;
}

static int engineNameSet(const char *name, ValkeyModuleString *value, void *privdata, ValkeyModuleString **err) {
    VALKEYMODULE_NOT_USED(name);
    VALKEYMODULE_NOT_USED(privdata);
    VALKEYMODULE_NOT_USED(err);

    size_t len = 0;
    const char *val = ValkeyModule_StringPtrLen(value, &len);
    char *copy = lm_strncpy(val, len);

    for (int i = 0; i < engine_names_count; i++) {
        ValkeyModule_FreeString(NULL, engine_names[i]);
        engine_names[i] = NULL;
        engine_names_cstr[i] = NULL;
    }
    engine_names_count = 0;

    char *saveptr = NULL;
    for (char *tok = strtok_r(copy, ",", &saveptr);
         tok != NULL && engine_names_count < MAX_ENGINE_NAMES;
         tok = strtok_r(NULL, ",", &saveptr)) {
        while (*tok == ' ' || *tok == '\t') tok++;
        char *end = tok + strlen(tok);
        while (end > tok && (end[-1] == ' ' || end[-1] == '\t')) end--;
        *end = '\0';
        if (*tok == '\0') continue;
        engine_names[engine_names_count] = ValkeyModule_CreateString(NULL, tok, strlen(tok));
        engine_names_cstr[engine_names_count] =
            ValkeyModule_StringPtrLen(engine_names[engine_names_count], NULL);
        engine_names_count++;
    }

    ValkeyModule_Free(copy);
    return VALKEYMODULE_OK;
}

VKM_PUBLIC int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx, "luau", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    ValkeyModule_Log(ctx, "notice", "%s",
                     ValkeyModule_CallArgv
                         ? "server.call uses ValkeyModule_CallArgv"
                         : "server.call uses ValkeyModule_Call (ValkeyModule_CallArgv unavailable)");

    ValkeyModule_SetModuleOptions(ctx, VALKEYMODULE_OPTIONS_HANDLE_REPL_ASYNC_LOAD |
                                           VALKEYMODULE_OPTIONS_HANDLE_ATOMIC_SLOT_MIGRATION |
                                           VALKEYMODULE_OPTIONS_HANDLE_FORKLESS);

    if (ValkeyModule_RegisterStringConfig(ctx, "engine-name", DEFAULT_ENGINE_NAME,
                                          VALKEYMODULE_CONFIG_IMMUTABLE, engineNameGet,
                                          engineNameSet, NULL, NULL) == VALKEYMODULE_ERR) {
        ValkeyModule_Log(ctx, "warning", "Failed to register engine-name config");
        return VALKEYMODULE_ERR;
    }

    engine_ctx = createEngineContext(ctx);
    if (engine_ctx == NULL) return VALKEYMODULE_ERR;

    luauWatchdogStart();

    if (ValkeyModule_LoadConfigs(ctx) == VALKEYMODULE_ERR) {
        ValkeyModule_Log(ctx, "warning", "Failed to load valkey-luau module configs");
        luauWatchdogStop();
        destroyEngineContext(engine_ctx);
        engine_ctx = NULL;
        return VALKEYMODULE_ERR;
    }

    if (engine_names_count == 0) {
        engine_names[0] = ValkeyModule_CreateString(NULL, DEFAULT_ENGINE_NAME,
                                                    strlen(DEFAULT_ENGINE_NAME));
        engine_names_cstr[0] = ValkeyModule_StringPtrLen(engine_names[0], NULL);
        engine_names_count = 1;
    }

    ValkeyModuleScriptingEngineMethods methods = {
        .version = VALKEYMODULE_SCRIPTING_ENGINE_ABI_VERSION,
        .compile_code = luauEngineCompileCode,
        .free_function = luauEngineFreeFunction,
        .call_function = luauEngineFunctionCall,
        .get_function_memory_overhead = luauEngineFunctionMemoryOverhead,
        .reset_env = luauEngineResetEnv,
        .get_memory_info = luauEngineGetMemoryInfo,
        .debugger_enable = luauEngineDebuggerEnable,
        .debugger_disable = luauEngineDebuggerDisable,
        .debugger_start = luauEngineDebuggerStart,
        .debugger_end = luauEngineDebuggerEnd,
    };

    for (int i = 0; i < engine_names_count; i++) {
        if (ValkeyModule_RegisterScriptingEngine(ctx, engine_names_cstr[i], engine_ctx,
                                                 &methods) == VALKEYMODULE_ERR) {
            ValkeyModule_Log(ctx, "warning", "Failed to register scripting engine '%s'",
                             engine_names_cstr[i]);
            for (int j = 0; j < i; j++)
                ValkeyModule_UnregisterScriptingEngine(ctx, engine_names_cstr[j]);
            luauWatchdogStop();
            destroyEngineContext(engine_ctx);
            engine_ctx = NULL;
            return VALKEYMODULE_ERR;
        }
        ValkeyModule_Log(ctx, "notice", "Registered Luau scripting engine as '%s'",
                         engine_names_cstr[i]);
    }

    return VALKEYMODULE_OK;
}

VKM_PUBLIC int ValkeyModule_OnUnload(ValkeyModuleCtx *ctx) {
    luauWatchdogStop();
    for (int i = 0; i < engine_names_count; i++) {
        ValkeyModule_UnregisterScriptingEngine(ctx, engine_names_cstr[i]);
        ValkeyModule_FreeString(NULL, engine_names[i]);
        engine_names[i] = NULL;
        engine_names_cstr[i] = NULL;
    }
    engine_names_count = 0;

    destroyEngineContext(engine_ctx);
    engine_ctx = NULL;
    return VALKEYMODULE_OK;
}
