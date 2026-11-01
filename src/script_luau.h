#ifndef _SCRIPT_LUAU_H_
#define _SCRIPT_LUAU_H_

#include "engine_structs.h"

#define C_OK 0
#define C_ERR -1

#define LUAU_GC_CYCLE_PERIOD 50
#define LUAU_FULL_GC_CYCLE 500
#define LUAU_MAX_REPLY_DEPTH 256
#define LUAU_INTERRUPT_POLL_MASK 255

#define REDIS_API_NAME "redis"
#define SERVER_API_NAME "server"

#define PROPAGATE_NONE 0
#define PROPAGATE_AOF 1
#define PROPAGATE_REPL 2

#define LL_DEBUG 0
#define LL_VERBOSE 1
#define LL_NOTICE 2
#define LL_WARNING 3

typedef struct luauCallCtx {
    luauEngineCtx *engine_ctx;
    ValkeyModuleCtx *module_ctx;
    ValkeyModuleScriptingEngineServerRuntimeCtx *run_ctx;
    ValkeyModuleScriptingEngineSubsystemType type;
    int replication_flags;
    int resp;

    long long interrupts;
    int killed;
    int mem_exceeded;
    int memcat;
    size_t mem_budget;

    uint64_t deadline_us;
} luauCallCtx;

typedef struct errorInfo {
    char *msg;
    char *source;
    char *line;
    int ignore_err_stats_update;
} errorInfo;

char *lm_strcpy(const char *str);
char *lm_strncpy(const char *str, size_t len);
char *lm_asprintf(const char *fmt, ...);

lua_State *luauCreateVM(luauEngineCtx *engine_ctx, ValkeyModuleCtx *module_ctx);
void luauDestroyVM(luauEngineCtx *engine_ctx);

lua_State *luauNewScriptThread(luauEngineCtx *engine_ctx,
                               ValkeyModuleString **keys,
                               size_t nkeys,
                               ValkeyModuleString **args,
                               size_t nargs,
                               int set_keys_globals,
                               int memcat);
void luauFreezeThreadEnv(luauEngineCtx *engine_ctx, lua_State *T, int has_overrides, int restricted);
void luauPushFieldGuard(luauEngineCtx *engine_ctx, lua_State *T);

void luauPushError(lua_State *lua, const char *error);
int luauError(lua_State *lua);
void luauExtractErrorInformation(lua_State *lua, errorInfo *err_info);
void luauErrorInformationDiscard(errorInfo *err_info);

void luauReplyToServerReply(ValkeyModuleCtx *ctx, int resp_version, lua_State *lua);

int luauResume(luauCallCtx *call_ctx, lua_State *T, int nargs);
void luauCallFunction(luauCallCtx *call_ctx, lua_State *T, int nargs);

size_t luauMemoryForCategory(lua_State *lua, int category);

ValkeyModuleDict *luauCoreCommandsCreate(ValkeyModuleCtx *ctx);
void luauArgvCacheFree(luauEngineCtx *ectx);

void luauWatchdogStart(void);
void luauWatchdogStop(void);
void luauWatchdogSetBusyThreshold(long long ms);
void luauInterruptInstall(lua_State *GL, int on);

#endif
