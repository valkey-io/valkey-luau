/*
 * valkey-luau: sandbox construction, the guest `server` API, RESP<->Luau value
 * conversion, error objects, and the invocation driver.
 */

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lua.h"
#include "lualib.h"
#include "luacode.h"

#include "engine_structs.h"
#include "fpconv_dtoa.h"

int luaopen_cjson(lua_State *l);
int luaopen_cjson_safe(lua_State *l);
int luaopen_struct(lua_State *L);
int luaopen_cmsgpack(lua_State *L);
int luaopen_cmsgpack_safe(lua_State *L);
int luaopen_bit(lua_State *L);
#include "rand.h"
#include "script_luau.h"
#include "sha1.h"

char *lm_strncpy(const char *str, size_t len) {
    char *out = ValkeyModule_Alloc(len + 1);
    if (str && len) memcpy(out, str, len);
    out[len] = '\0';
    return out;
}

char *lm_strcpy(const char *str) {
    return lm_strncpy(str, str ? strlen(str) : 0);
}

char *lm_asprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return lm_strcpy("");
    }
    char *out = ValkeyModule_Alloc((size_t)n + 1);
    vsnprintf(out, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return out;
}

static char *lm_strtrim(char *s, const char *cset) {
    char *start = s;
    char *end = s + strlen(s) - 1;
    while (*start && strchr(cset, *start)) start++;
    while (end >= start && strchr(cset, *end)) end--;
    size_t len = (size_t)(end - start + 1);
    if (start != s) memmove(s, start, len);
    s[len] = '\0';
    return s;
}

static int ll2string(char *dst, size_t dstlen, long long value) {
    int n = snprintf(dst, dstlen, "%lld", value);
    return (n < 0 || (size_t)n >= dstlen) ? 0 : n;
}

static int double2ll(double d, long long *out) {
#if (DBL_MANT_DIG >= 52) && (DBL_MANT_DIG <= 63) && (LLONG_MAX == 0x7fffffffffffffffLL)
    if (d < -(double)(1LL << 52) || d > (double)(1LL << 52)) return 0;
    if (d != (double)((long long)d)) return 0;
    *out = (long long)d;
    return 1;
#else
    (void)d;
    (void)out;
    return 0;
#endif
}

static void *luauAlloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    luauEngineCtx *ctx = ud;

    if (nsize == 0) {
        ctx->mem_used -= osize;
        ValkeyModule_Free(ptr);
        return NULL;
    }

    if (ctx->mem_limit && ctx->mem_used - osize + nsize > ctx->mem_limit) {
        return NULL;
    }

    void *np = ValkeyModule_Realloc(ptr, nsize);
    if (!np) return NULL;

    ctx->mem_used = ctx->mem_used - osize + nsize;
    return np;
}

static luauCallCtx *luauGetCallCtx(lua_State *lua) {
    return lua_getthreaddata(lua_mainthread(lua));
}

void luauSetCallCtx(lua_State *lua, luauCallCtx *ctx) {
    lua_setthreaddata(lua_mainthread(lua), ctx);
}

static uint64_t luauMonotonicUs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

#define LUAU_WATCHDOG_TICK_MS 5

static void luauInterrupt(lua_State *lua, int gc);

static pthread_t luau_wd_thread;
static int luau_wd_started = 0;
static _Atomic(lua_State *) luau_wd_vm;
static atomic_llong luau_wd_arm_at_ms;
static atomic_int luau_wd_stop;
static atomic_llong luau_busy_threshold_ms = 5000;

static long long luauNowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void *luauWatchdogMain(void *arg) {
    (void)arg;
    while (!atomic_load(&luau_wd_stop)) {
        struct timespec ts = {0, LUAU_WATCHDOG_TICK_MS * 1000 * 1000};
        nanosleep(&ts, NULL);

        lua_State *GL = atomic_load(&luau_wd_vm);
        if (GL == NULL) continue;
        if (luauNowMs() < atomic_load(&luau_wd_arm_at_ms)) continue;

        if (atomic_compare_exchange_strong(&luau_wd_vm, &GL, NULL)) {
            lua_callbacks(GL)->interrupt = luauInterrupt;
        }
    }
    return NULL;
}

void luauWatchdogStart(void) {
    if (luau_wd_started) return;
    atomic_store(&luau_wd_stop, 0);
    atomic_store(&luau_wd_vm, NULL);
    if (pthread_create(&luau_wd_thread, NULL, luauWatchdogMain, NULL) == 0) {
        luau_wd_started = 1;
    } else {
        ValkeyModule_Log(NULL, "warning",
                         "valkey-luau: could not start the script-kill watchdog; "
                         "the interrupt callback will stay installed for every script");
    }
}

void luauWatchdogStop(void) {
    if (!luau_wd_started) return;
    atomic_store(&luau_wd_stop, 1);
    pthread_join(luau_wd_thread, NULL);
    luau_wd_started = 0;
}

void luauWatchdogSetBusyThreshold(long long ms) {
    atomic_store(&luau_busy_threshold_ms, ms < 0 ? 0 : ms);
}

void luauInterruptInstall(lua_State *GL, int on) {
    lua_callbacks(GL)->interrupt = on ? luauInterrupt : NULL;
}

static void luauInterrupt(lua_State *lua, int gc) {
    if (gc >= 0) return;

    luauCallCtx *ctx = luauGetCallCtx(lua);
    if (!ctx) return;

    ctx->interrupts++;

    if (!ctx->killed && (ctx->interrupts & LUAU_INTERRUPT_POLL_MASK) == 0) {
        if (ctx->mem_budget &&
            luauMemoryForCategory(lua, ctx->memcat) > ctx->mem_budget) {
            ctx->mem_exceeded = 1;
            ctx->killed = 1;
        } else if (ctx->deadline_us && luauMonotonicUs() > ctx->deadline_us) {
            ctx->killed = 1;
        } else if (ctx->run_ctx &&
                   ValkeyModule_GetFunctionExecutionState(ctx->run_ctx) ==
                       VMSE_STATE_KILLED) {
            ctx->killed = 1;
        }
    }

    if (!ctx->killed) return;

    if (lua_isyieldable(lua)) {
        lua_yield(lua, 0);
        return;
    }

    lua_rawcheckstack(lua, 1);
    luaL_error(lua, "Script killed by user");
}

static int luauMathRandom(lua_State *lua) {
    luauCallCtx *ctx = luauGetCallCtx(lua);
    if (!ctx) {
        luauPushError(lua, "math.random is only available inside a script");
        return luauError(lua);
    }

    serverSetRandState(ctx->engine_ctx->rand_state);
    lua_Number r = (lua_Number)(serverLrand48() % SERVER_LRAND48_MAX) /
                   (lua_Number)SERVER_LRAND48_MAX;
    serverGetRandState(ctx->engine_ctx->rand_state);

    lua_Number m = 1, mm = 0;
    switch (lua_gettop(lua)) {
    case 0: lua_pushnumber(lua, r); return 1;
    case 1: m = luaL_checknumber(lua, 1); break;
    default:
        mm = luaL_checknumber(lua, 1);
        m = luaL_checknumber(lua, 2);
        break;
    }
    if (lua_gettop(lua) == 1) {
        luaL_argcheck(lua, 1 <= m, 1, "interval is empty");
        lua_pushnumber(lua, floor(r * m) + 1);
    } else {
        luaL_argcheck(lua, mm <= m, 2, "interval is empty");
        lua_pushnumber(lua, floor(r * (m - mm + 1)) + mm);
    }
    return 1;
}

static int luauMathRandomSeed(lua_State *lua) {
    luauCallCtx *ctx = luauGetCallCtx(lua);
    if (!ctx) return 0;
    serverSetRandState(ctx->engine_ctx->rand_state);
    serverSrand48((int32_t)luaL_checknumber(lua, 1));
    serverGetRandState(ctx->engine_ctx->rand_state);
    return 0;
}

static void luauFreezeRecursive(lua_State *lua) {
    if (lua_getreadonly(lua, -1)) return;

    lua_setreadonly(lua, -1, 1);

    lua_checkstack(lua, 3);
    lua_pushnil(lua);
    while (lua_next(lua, -2)) {
        if (lua_istable(lua, -1)) luauFreezeRecursive(lua);
        lua_pop(lua, 1);
    }

    if (lua_getmetatable(lua, -1)) {
        luauFreezeRecursive(lua);
        lua_pop(lua, 1);
    }
}

static void luauFreezeBasicTypeMetatables(lua_State *lua) {
    lua_pushliteral(lua, "");
    if (lua_getmetatable(lua, -1)) {
        luauFreezeRecursive(lua);
        lua_pop(lua, 1);
    }
    lua_pop(lua, 1);
}

static int luauReadGuardBody(lua_State *lua, int fall_through) {
    const char *name = lua_tostring(lua, 2);

    if (name && strcmp(name, "_G") == 0) {
        lua_pushvalue(lua, LUA_GLOBALSINDEX);
        return 1;
    }

    if (lua_getmetatable(lua, 1)) {
        lua_rawgetfield(lua, -1, "__overrides");
        if (lua_istable(lua, -1)) {
            lua_pushvalue(lua, 2);
            lua_rawget(lua, -2);
            if (!lua_isnil(lua, -1)) return 1;
            lua_pop(lua, 1);
        }
        lua_pop(lua, 2);
    }

    if (fall_through) {
        lua_pushvalue(lua, 2);
        lua_rawget(lua, lua_upvalueindex(1));
        if (!lua_isnil(lua, -1)) return 1;
        lua_pop(lua, 1);
    }

    luaL_error(lua, "Script attempted to access nonexistent global variable '%s'",
               name ? name : "?");
    return 0;
}

static int luauGlobalReadGuard(lua_State *lua) {
    return luauReadGuardBody(lua, 1);
}

static int luauLibLoadReadGuard(lua_State *lua) {
    return luauReadGuardBody(lua, 0);
}

static int luauMissingFieldGuard(lua_State *lua) {
    const char *name = lua_tostring(lua, 2);
    luaL_error(lua, "Script attempted to access nonexistent global variable '%s'",
               name ? name : "?");
    return 0;
}

static int luauGlobalWriteGuard(lua_State *lua) {
    luaL_error(lua, "Attempt to modify a readonly table");
    return 0;
}

static void luauAttachSourceLine(lua_State *lua) {
    lua_Debug ar;
    for (int level = 1; level <= 8; level++) {
        if (!lua_getinfo(lua, level, "sl", &ar)) break;
        if (ar.what && strcmp(ar.what, "C") == 0) continue;

        lua_pushstring(lua, "source");
        lua_pushstring(lua, ar.source ? ar.source : ar.short_src);
        lua_settable(lua, -3);

        lua_pushstring(lua, "line");
        lua_pushinteger(lua, ar.currentline);
        lua_settable(lua, -3);
        return;
    }
}

static void luauPushErrorBuff(lua_State *lua, char *err_buffer) {
    char *msg;
    char *final_msg = NULL;

    if (err_buffer[0] == '-') {
        char *err_msg = strstr(err_buffer, " ");
        if (!err_msg) {
            msg = lm_strcpy(err_buffer + 1);
            final_msg = lm_asprintf("ERR %s", msg);
        } else {
            *err_msg = '\0';
            msg = lm_strcpy(err_msg + 1);
            msg = lm_strtrim(msg, "\r\n");
            final_msg = lm_asprintf("%s %s", err_buffer + 1, msg);
        }
    } else {
        msg = lm_strcpy(err_buffer);
        msg = lm_strtrim(msg, "\r\n");
        final_msg = lm_asprintf("ERR %s", msg);
    }

    lua_newtable(lua);
    lua_pushstring(lua, "err");
    lua_pushstring(lua, final_msg);
    lua_settable(lua, -3);
    luauAttachSourceLine(lua);

    ValkeyModule_Free(msg);
    ValkeyModule_Free(final_msg);
}

void luauPushError(lua_State *lua, const char *error) {
    char *copy = lm_strcpy(error);
    luauPushErrorBuff(lua, copy);
    ValkeyModule_Free(copy);
}

int luauError(lua_State *lua) {
    lua_error(lua);
    return 0;
}

void luauErrorInformationDiscard(errorInfo *err_info) {
    if (err_info->msg) ValkeyModule_Free(err_info->msg);
    if (err_info->source) ValkeyModule_Free(err_info->source);
    if (err_info->line) ValkeyModule_Free(err_info->line);
}

static void luauSplitPositionPrefix(const char *s, char **source, char **line, char **rest) {
    *source = *line = *rest = NULL;

    const char *colon1 = strchr(s, ':');
    if (!colon1) return;
    const char *colon2 = strchr(colon1 + 1, ':');
    if (!colon2 || colon2[1] != ' ') return;

    for (const char *p = colon1 + 1; p < colon2; p++) {
        if (!isdigit((unsigned char)*p)) return;
    }
    if (colon2 == colon1 + 1) return;

    *source = lm_strncpy(s, (size_t)(colon1 - s));
    *line = lm_strncpy(colon1 + 1, (size_t)(colon2 - colon1 - 1));
    *rest = lm_strcpy(colon2 + 2);
}

void luauExtractErrorInformation(lua_State *lua, errorInfo *err_info) {
    if (!lua_istable(lua, -1)) {
        const char *s = lua_isstring(lua, -1) ? lua_tostring(lua, -1)
                                              : "unknown error";
        char *source = NULL, *line = NULL, *rest = NULL;
        luauSplitPositionPrefix(s, &source, &line, &rest);
        err_info->msg = lm_asprintf("ERR %s", s);
        if (rest) {
            err_info->source = lm_asprintf("@%s", source);
            err_info->line = line;
            ValkeyModule_Free(source);
            ValkeyModule_Free(rest);
        }
        err_info->ignore_err_stats_update = 0;
        return;
    }

    lua_rawgetfield(lua, -1, "err");
    if (lua_isstring(lua, -1)) err_info->msg = lm_strcpy(lua_tostring(lua, -1));
    lua_pop(lua, 1);

    lua_rawgetfield(lua, -1, "source");
    if (lua_isstring(lua, -1)) err_info->source = lm_strcpy(lua_tostring(lua, -1));
    lua_pop(lua, 1);

    lua_rawgetfield(lua, -1, "line");
    if (lua_isstring(lua, -1)) err_info->line = lm_strcpy(lua_tostring(lua, -1));
    lua_pop(lua, 1);

    lua_rawgetfield(lua, -1, "ignore_error_stats_update");
    if (lua_isboolean(lua, -1))
        err_info->ignore_err_stats_update = lua_toboolean(lua, -1);
    lua_pop(lua, 1);

    if (err_info->msg == NULL) err_info->msg = lm_strcpy("ERR unknown error");
}

#define LUAU_REPLY_OK 0
#define LUAU_REPLY_TOO_DEEP 1
#define LUAU_REPLY_TOO_BIG 2
#define LUAU_REPLY_BAD_PROTO 3

#define LUAU_MAX_TABLE_SIZE ((size_t)1 << 26)
#define LUAU_MAX_STRING_SIZE ((size_t)1 << 30)

static void luauProcessReplyError(const char *err, int err_no, lua_State *lua);

static int luauReplyFailed(lua_State *lua, int base_top, int failed) {
    lua_settop(lua, base_top);
    switch (failed) {
    case LUAU_REPLY_BAD_PROTO:
        lua_pushnil(lua);
        return 0;
    case LUAU_REPLY_TOO_BIG:
        luauPushError(lua, "reply too large to convert to a Lua value");
        return 1;
    default:
        luauPushError(lua, "reached lua stack limit");
        return 1;
    }
}

static int callReplyIsStatus(ValkeyModuleCallReply *reply) {
    size_t len = 0, attr_len = 0;
    const char *proto = ValkeyModule_CallReplyProto(reply, &len);
    ValkeyModuleCallReply *attr = ValkeyModule_CallReplyAttribute(reply);
    if (attr) ValkeyModule_CallReplyProto(attr, &attr_len);
    return proto && attr_len < len && proto[attr_len] == '+';
}

static int callReplyToLuaType(lua_State *lua, ValkeyModuleCallReply *reply, int resp, int depth) {
    if (!lua_checkstack(lua, 6)) return LUAU_REPLY_TOO_DEEP;

    int type = ValkeyModule_CallReplyType(reply);
    if (type == VALKEYMODULE_REPLY_STRING && callReplyIsStatus(reply))
        type = VALKEYMODULE_REPLY_SIMPLE_STRING;
    if ((type == VALKEYMODULE_REPLY_ARRAY || type == VALKEYMODULE_REPLY_MAP ||
         type == VALKEYMODULE_REPLY_SET) &&
        depth >= LUAU_MAX_REPLY_DEPTH)
        return LUAU_REPLY_TOO_DEEP;

    switch (type) {
    case VALKEYMODULE_REPLY_STRING: {
        size_t len = 0;
        const char *str = ValkeyModule_CallReplyStringPtr(reply, &len);
        if (len > LUAU_MAX_STRING_SIZE) return LUAU_REPLY_TOO_BIG;
        lua_pushlstring(lua, str, len);
        break;
    }
    case VALKEYMODULE_REPLY_SIMPLE_STRING: {
        size_t len = 0;
        const char *str = ValkeyModule_CallReplyStringPtr(reply, &len);
        if (len > LUAU_MAX_STRING_SIZE) return LUAU_REPLY_TOO_BIG;
        lua_newtable(lua);
        lua_pushstring(lua, "ok");
        lua_pushlstring(lua, str, len);
        lua_rawset(lua, -3);
        break;
    }
    case VALKEYMODULE_REPLY_INTEGER: {
        lua_pushnumber(lua, (lua_Number)ValkeyModule_CallReplyInteger(reply));
        break;
    }
    case VALKEYMODULE_REPLY_ARRAY: {
        size_t items = ValkeyModule_CallReplyLength(reply);
        if (items > LUAU_MAX_TABLE_SIZE) return LUAU_REPLY_TOO_BIG;
        lua_createtable(lua, (int)items, 0);
        for (size_t i = 0; i < items; i++) {
            ValkeyModuleCallReply *val = ValkeyModule_CallReplyArrayElement(reply, i);
            int rc = callReplyToLuaType(lua, val, resp, depth + 1);
            if (rc != LUAU_REPLY_OK) return rc;
            lua_rawseti(lua, -2, (int)(i + 1));
        }
        break;
    }
    case VALKEYMODULE_REPLY_NULL:
    case VALKEYMODULE_REPLY_ARRAY_NULL:
        if (resp == 2)
            lua_pushboolean(lua, 0);
        else
            lua_pushnil(lua);
        break;
    case VALKEYMODULE_REPLY_MAP: {
        size_t items = ValkeyModule_CallReplyLength(reply);
        if (items > LUAU_MAX_TABLE_SIZE) return LUAU_REPLY_TOO_BIG;
        lua_newtable(lua);
        lua_pushstring(lua, "map");
        lua_createtable(lua, 0, (int)items);
        for (size_t i = 0; i < items; i++) {
            ValkeyModuleCallReply *key = NULL, *val = NULL;
            ValkeyModule_CallReplyMapElement(reply, i, &key, &val);
            int rc = callReplyToLuaType(lua, key, resp, depth + 1);
            if (rc == LUAU_REPLY_OK) rc = callReplyToLuaType(lua, val, resp, depth + 1);
            if (rc != LUAU_REPLY_OK) return rc;
            if (lua_isnil(lua, -2))
                lua_pop(lua, 2);
            else
                lua_rawset(lua, -3);
        }
        lua_rawset(lua, -3);
        break;
    }
    case VALKEYMODULE_REPLY_SET: {
        size_t items = ValkeyModule_CallReplyLength(reply);
        if (items > LUAU_MAX_TABLE_SIZE) return LUAU_REPLY_TOO_BIG;
        lua_newtable(lua);
        lua_pushstring(lua, "set");
        lua_createtable(lua, 0, (int)items);
        for (size_t i = 0; i < items; i++) {
            ValkeyModuleCallReply *val = ValkeyModule_CallReplySetElement(reply, i);
            int rc = callReplyToLuaType(lua, val, resp, depth + 1);
            if (rc != LUAU_REPLY_OK) return rc;
            if (lua_isnil(lua, -1)) {
                lua_pop(lua, 1);
                continue;
            }
            lua_pushboolean(lua, 1);
            lua_rawset(lua, -3);
        }
        lua_rawset(lua, -3);
        break;
    }
    case VALKEYMODULE_REPLY_BOOL:
        lua_pushboolean(lua, ValkeyModule_CallReplyBool(reply));
        break;
    case VALKEYMODULE_REPLY_DOUBLE: {
        lua_newtable(lua);
        lua_pushstring(lua, "double");
        lua_pushnumber(lua, ValkeyModule_CallReplyDouble(reply));
        lua_rawset(lua, -3);
        break;
    }
    case VALKEYMODULE_REPLY_BIG_NUMBER: {
        size_t len = 0;
        const char *str = ValkeyModule_CallReplyBigNumber(reply, &len);
        if (len > LUAU_MAX_STRING_SIZE) return LUAU_REPLY_TOO_BIG;
        lua_newtable(lua);
        lua_pushstring(lua, "big_number");
        lua_pushlstring(lua, str, len);
        lua_rawset(lua, -3);
        break;
    }
    case VALKEYMODULE_REPLY_VERBATIM_STRING: {
        size_t len = 0;
        const char *format = NULL;
        const char *str = ValkeyModule_CallReplyVerbatim(reply, &len, &format);
        if (len > LUAU_MAX_STRING_SIZE) return LUAU_REPLY_TOO_BIG;
        lua_newtable(lua);
        lua_pushstring(lua, "verbatim_string");
        lua_newtable(lua);
        lua_pushstring(lua, "string");
        lua_pushlstring(lua, str, len);
        lua_rawset(lua, -3);
        lua_pushstring(lua, "format");
        lua_pushlstring(lua, format, 3);
        lua_rawset(lua, -3);
        lua_rawset(lua, -3);
        break;
    }
    case VALKEYMODULE_REPLY_ERROR: {
        size_t len = 0;
        const char *str = ValkeyModule_CallReplyStringPtr(reply, &len);
        if (len > LUAU_MAX_STRING_SIZE) return LUAU_REPLY_TOO_BIG;
        char *err = lm_strncpy(str, len);
        luauProcessReplyError(err, 0, lua);
        ValkeyModule_Free(err);
        break;
    }
    default:
        lua_pushnil(lua);
        break;
    }
    return LUAU_REPLY_OK;
}

static void strmapchars(char *s, const char *from, const char *to, size_t setlen) {
    for (size_t j = 0; s[j]; j++) {
        for (size_t i = 0; i < setlen; i++) {
            if (s[j] == from[i]) {
                s[j] = to[i];
                break;
            }
        }
    }
}

static char *copyStringFromLuaStack(lua_State *lua, int index) {
    size_t len = 0;
    const char *s = lua_tolstring(lua, index, &len);
    if (!s) return lm_strcpy("");
    return lm_strncpy(s, len);
}

static void luauReplyToServerReplyDepth(ValkeyModuleCtx *ctx, int resp_version, lua_State *lua, int depth) {
    int t = lua_type(lua, -1);

    if (depth >= LUAU_MAX_REPLY_DEPTH || !lua_checkstack(lua, 4)) {
        ValkeyModule_ReplyWithError(ctx, "ERR reached lua stack limit");
        lua_pop(lua, 1);
        return;
    }

    switch (t) {
    case LUA_TSTRING: {
        size_t len = 0;
        const char *s = lua_tolstring(lua, -1, &len);
        ValkeyModule_ReplyWithStringBuffer(ctx, s, len);
        break;
    }
    case LUA_TBOOLEAN:
        if (resp_version == 2) {
            if (lua_toboolean(lua, -1))
                ValkeyModule_ReplyWithLongLong(ctx, 1);
            else
                ValkeyModule_ReplyWithNull(ctx);
        } else {
            ValkeyModule_ReplyWithBool(ctx, lua_toboolean(lua, -1));
        }
        break;
    case LUA_TNUMBER:
        ValkeyModule_ReplyWithLongLong(ctx, (long long)lua_tonumber(lua, -1));
        break;
    case LUA_TTABLE: {
        lua_pushstring(lua, "err");
        lua_rawget(lua, -2);
        if (lua_type(lua, -1) == LUA_TSTRING) {
            lua_pop(lua, 1);
            errorInfo err_info = {0};
            luauExtractErrorInformation(lua, &err_info);
            ValkeyModule_ReplyWithCustomErrorFormat(
                ctx, !err_info.ignore_err_stats_update, "%s", err_info.msg);
            luauErrorInformationDiscard(&err_info);
            break;
        }
        lua_pop(lua, 1);

        lua_pushstring(lua, "ok");
        lua_rawget(lua, -2);
        if (lua_type(lua, -1) == LUA_TSTRING) {
            char *ok = copyStringFromLuaStack(lua, -1);
            strmapchars(ok, "\r\n", "  ", 2);
            ValkeyModule_ReplyWithSimpleString(ctx, ok);
            ValkeyModule_Free(ok);
            lua_pop(lua, 1);
            break;
        }
        lua_pop(lua, 1);

        lua_pushstring(lua, "double");
        lua_rawget(lua, -2);
        if (lua_type(lua, -1) == LUA_TNUMBER) {
            ValkeyModule_ReplyWithDouble(ctx, lua_tonumber(lua, -1));
            lua_pop(lua, 1);
            break;
        }
        lua_pop(lua, 1);

        lua_pushstring(lua, "big_number");
        lua_rawget(lua, -2);
        if (lua_type(lua, -1) == LUA_TSTRING) {
            char *bn = copyStringFromLuaStack(lua, -1);
            strmapchars(bn, "\r\n", "  ", 2);
            ValkeyModule_ReplyWithBigNumber(ctx, bn, strlen(bn));
            ValkeyModule_Free(bn);
            lua_pop(lua, 1);
            break;
        }
        lua_pop(lua, 1);

        lua_pushstring(lua, "verbatim_string");
        lua_rawget(lua, -2);
        if (lua_type(lua, -1) == LUA_TTABLE) {
            lua_pushstring(lua, "format");
            lua_rawget(lua, -2);
            if (lua_type(lua, -1) == LUA_TSTRING) {
                char *format = copyStringFromLuaStack(lua, -1);
                lua_pop(lua, 1);
                lua_pushstring(lua, "string");
                lua_rawget(lua, -2);
                if (lua_type(lua, -1) == LUA_TSTRING) {
                    size_t len = 0;
                    const char *s = lua_tolstring(lua, -1, &len);
                    ValkeyModule_ReplyWithVerbatimStringType(ctx, s, len, format);
                    ValkeyModule_Free(format);
                    lua_pop(lua, 2);
                    break;
                }
                ValkeyModule_Free(format);
                lua_pop(lua, 1);
            } else {
                lua_pop(lua, 1);
            }
        }
        lua_pop(lua, 1);

        lua_pushstring(lua, "map");
        lua_rawget(lua, -2);
        if (lua_type(lua, -1) == LUA_TTABLE) {
            long len = 0;
            ValkeyModule_ReplyWithMap(ctx, VALKEYMODULE_POSTPONED_LEN);
            lua_pushnil(lua);
            while (lua_next(lua, -2)) {
                lua_pushvalue(lua, -2);
                luauReplyToServerReplyDepth(ctx, resp_version, lua, depth + 1);
                luauReplyToServerReplyDepth(ctx, resp_version, lua, depth + 1);
                len++;
            }
            ValkeyModule_ReplySetMapLength(ctx, len);
            lua_pop(lua, 1);
            break;
        }
        lua_pop(lua, 1);

        lua_pushstring(lua, "set");
        lua_rawget(lua, -2);
        if (lua_type(lua, -1) == LUA_TTABLE) {
            long len = 0;
            ValkeyModule_ReplyWithSet(ctx, VALKEYMODULE_POSTPONED_LEN);
            lua_pushnil(lua);
            while (lua_next(lua, -2)) {
                lua_pop(lua, 1);
                lua_pushvalue(lua, -1);
                luauReplyToServerReplyDepth(ctx, resp_version, lua, depth + 1);
                len++;
            }
            ValkeyModule_ReplySetSetLength(ctx, len);
            lua_pop(lua, 1);
            break;
        }
        lua_pop(lua, 1);

        long mbulklen = 0;
        int j = 1;
        ValkeyModule_ReplyWithArray(ctx, VALKEYMODULE_POSTPONED_LEN);
        while (1) {
            lua_pushnumber(lua, j++);
            lua_rawget(lua, -2);
            if (lua_type(lua, -1) == LUA_TNIL) {
                lua_pop(lua, 1);
                break;
            }
            luauReplyToServerReplyDepth(ctx, resp_version, lua, depth + 1);
            mbulklen++;
        }
        ValkeyModule_ReplySetArrayLength(ctx, mbulklen);
        lua_pop(lua, 1);
        return;
    }
    default:
        ValkeyModule_ReplyWithNull(ctx);
        break;
    }
    lua_pop(lua, 1);
}

void luauReplyToServerReply(ValkeyModuleCtx *ctx, int resp_version, lua_State *lua) {
    luauReplyToServerReplyDepth(ctx, resp_version, lua, 0);
}

static ValkeyModuleString *luauArgvCacheGet(luauEngineCtx *ectx,
                                            int j,
                                            const char *s,
                                            size_t len) {
    ValkeyModuleString *cached = ectx->argv_cache[j];
    if (cached) {
        size_t cached_len = 0;
        const char *cached_s = ValkeyModule_StringPtrLen(cached, &cached_len);
        if (cached_len != len || memcmp(cached_s, s, len) != 0) {
            ValkeyModule_FreeString(NULL, cached);
            cached = NULL;
        }
    }
    if (!cached) {
        cached = ValkeyModule_CreateString(NULL, s, len);
        ectx->argv_cache[j] = cached;
    }
    ValkeyModule_RetainString(NULL, cached);
    return cached;
}

static int luauArgvCacheAllowed(luauEngineCtx *ectx, lua_State *lua) {
    if (!ectx->core_commands || lua_type(lua, 1) != LUA_TSTRING) return 0;
    size_t len = 0;
    const char *name = lua_tolstring(lua, 1, &len);
    if (len == 0 || len > LUAU_ARGV_CACHE_MAX_LEN) return 0;

    if (ectx->argv_cache[0]) {
        size_t cached_len = 0;
        const char *cached = ValkeyModule_StringPtrLen(ectx->argv_cache[0], &cached_len);
        if (cached_len == len && memcmp(cached, name, len) == 0) return 1;
    }

    char lower[LUAU_ARGV_CACHE_MAX_LEN];
    for (size_t i = 0; i < len; i++) lower[i] = (char)tolower((unsigned char)name[i]);
    int nokey = 1;
    ValkeyModule_DictGetC(ectx->core_commands, lower, len, &nokey);
    return !nokey;
}

static int luauCommandListApply(ValkeyModuleDict *names, ValkeyModuleCallReply *reply, int add) {
    if (!reply || ValkeyModule_CallReplyType(reply) != VALKEYMODULE_REPLY_ARRAY) return 0;
    size_t n = ValkeyModule_CallReplyLength(reply);
    for (size_t i = 0; i < n; i++) {
        size_t len = 0;
        const char *name =
            ValkeyModule_CallReplyStringPtr(ValkeyModule_CallReplyArrayElement(reply, i), &len);
        if (!name || len == 0 || len > LUAU_ARGV_CACHE_MAX_LEN || memchr(name, '|', len)) continue;
        char lower[LUAU_ARGV_CACHE_MAX_LEN];
        for (size_t k = 0; k < len; k++) lower[k] = (char)tolower((unsigned char)name[k]);
        if (add)
            ValkeyModule_DictSetC(names, lower, len, (void *)1);
        else
            ValkeyModule_DictDelC(names, lower, len, NULL);
    }
    return 1;
}

ValkeyModuleDict *luauCoreCommandsCreate(ValkeyModuleCtx *ctx) {
    ValkeyModuleDict *core = ValkeyModule_CreateDict(NULL);
    ValkeyModuleCallReply *all = ValkeyModule_Call(ctx, "COMMAND", "c", "LIST");
    int ok = luauCommandListApply(core, all, 1);
    if (all) ValkeyModule_FreeCallReply(all);

    ValkeyModuleCallReply *mods = ok ? ValkeyModule_Call(ctx, "MODULE", "c", "LIST") : NULL;
    ok = mods && ValkeyModule_CallReplyType(mods) == VALKEYMODULE_REPLY_ARRAY;
    size_t nmods = ok ? ValkeyModule_CallReplyLength(mods) : 0;
    for (size_t i = 0; ok && i < nmods; i++) {
        ValkeyModuleCallReply *info = ValkeyModule_CallReplyArrayElement(mods, i);
        size_t nfields = ValkeyModule_CallReplyLength(info);
        const char *name = NULL;
        size_t name_len = 0;
        for (size_t j = 0; j + 1 < nfields && !name; j += 2) {
            size_t key_len = 0;
            const char *key =
                ValkeyModule_CallReplyStringPtr(ValkeyModule_CallReplyArrayElement(info, j), &key_len);
            if (key && key_len == 4 && memcmp(key, "name", 4) == 0)
                name = ValkeyModule_CallReplyStringPtr(ValkeyModule_CallReplyArrayElement(info, j + 1),
                                                       &name_len);
        }
        if (!name) {
            ok = 0;
            break;
        }
        ValkeyModuleCallReply *cmds =
            ValkeyModule_Call(ctx, "COMMAND", "cccb", "LIST", "FILTERBY", "MODULE", name, name_len);
        ok = luauCommandListApply(core, cmds, 0);
        if (cmds) ValkeyModule_FreeCallReply(cmds);
    }
    if (mods) ValkeyModule_FreeCallReply(mods);

    if (!ok) {
        ValkeyModule_FreeDict(NULL, core);
        return NULL;
    }
    return core;
}

void luauArgvCacheFree(luauEngineCtx *ectx) {
    for (int j = 0; j < LUAU_ARGV_CACHE_SIZE; j++) {
        if (ectx->argv_cache[j]) ValkeyModule_FreeString(NULL, ectx->argv_cache[j]);
        ectx->argv_cache[j] = NULL;
    }
    if (ectx->core_commands) ValkeyModule_FreeDict(NULL, ectx->core_commands);
    ectx->core_commands = NULL;
}

static void freeServerArgv(luauEngineCtx *ectx,
                           ValkeyModuleCtx *ctx,
                           ValkeyModuleString **argv,
                           int argc) {
    for (int i = 0; i < argc; i++) ValkeyModule_FreeString(ctx, argv[i]);
    if (argv == ectx->argv_buf) {
        ectx->argv_cache_busy = 0;
    } else {
        ValkeyModule_Free(argv);
    }
}

static ValkeyModuleString **luauArgsToServerArgv(luauEngineCtx *ectx,
                                                 ValkeyModuleCtx *ctx,
                                                 lua_State *lua,
                                                 int *out_argc) {
    int argc = lua_gettop(lua);
    if (argc == 0) {
        luauPushError(lua, "Please specify at least one argument for this call");
        return NULL;
    }

    int use_cache = !ectx->argv_cache_busy && argc <= LUAU_ARGV_CACHE_SIZE &&
                    luauArgvCacheAllowed(ectx, lua);
    ValkeyModuleString **argv;
    if (use_cache) {
        argv = ectx->argv_buf;
        ectx->argv_cache_busy = 1;
    } else {
        argv = ValkeyModule_Calloc(argc, sizeof(*argv));
    }
    int built = 0;

    for (int j = 0; j < argc; j++) {
        int idx = j + 1;
        char dbuf[64];
        const char *s;
        size_t len = 0;
        if (lua_type(lua, idx) == LUA_TNUMBER) {
            lua_Number num = lua_tonumber(lua, idx);
            long long ll;
            if (double2ll((double)num, &ll)) {
                len = (size_t)ll2string(dbuf, sizeof(dbuf), ll);
            } else {
                len = (size_t)fpconv_dtoa((double)num, dbuf);
                dbuf[len] = '\0';
            }
            s = dbuf;
        } else if (lua_type(lua, idx) == LUA_TSTRING) {
            s = lua_tolstring(lua, idx, &len);
        } else {
            break;
        }
        if (use_cache && len <= LUAU_ARGV_CACHE_MAX_LEN) {
            argv[j] = luauArgvCacheGet(ectx, j, s, len);
        } else {
            argv[j] = ValkeyModule_CreateString(ctx, s, len);
        }
        built++;
    }

    lua_pop(lua, argc);

    if (built != argc) {
        freeServerArgv(ectx, ctx, argv, built);
        luauPushError(lua, "Command arguments must be strings or integers");
        return NULL;
    }

    *out_argc = argc;
    return argv;
}

static void luauProcessReplyError(const char *err, int err_no, lua_State *lua) {
    int push_error = 1;

    if (err_no == ESPIPE) {
        if (strncmp(err, "ERR command ", strlen("ERR command ")) == 0) {
            luauPushError(lua, "This Valkey command is not allowed from script");
            push_error = 0;
        }
    } else if (err_no == EINVAL) {
        if (strncmp(err, "ERR wrong number of arguments for ",
                    strlen("ERR wrong number of arguments for ")) == 0) {
            luauPushError(lua, "Wrong number of args calling command from script");
            push_error = 0;
        }
    } else if (err_no == ENOENT) {
        if (strncmp(err, "ERR unknown command '", strlen("ERR unknown command '")) == 0) {
            luauPushError(lua, "Unknown command called from script");
            push_error = 0;
        }
    } else if (err_no == EACCES) {
        if (strncmp(err, "NOPERM ", strlen("NOPERM ")) == 0) {
            char *msg = lm_asprintf("ACL failure in script: %s",
                                    err + strlen("NOPERM "));
            luauPushError(lua, msg);
            ValkeyModule_Free(msg);
            push_error = 0;
        }
    }

    if (push_error) {
        char *err_with_dash = lm_asprintf("-%s", err);
        luauPushErrorBuff(lua, err_with_dash);
        ValkeyModule_Free(err_with_dash);
    }

    lua_pushstring(lua, "ignore_error_stats_update");
    lua_pushboolean(lua, 1);
    lua_settable(lua, -3);
}

#define LUAU_REPLY_ARRAY 1
#define LUAU_REPLY_MAP 2
#define LUAU_REPLY_SET 3

typedef struct luauReplyFrame {
    int type;
    size_t count;
} luauReplyFrame;

typedef struct luauReplyCtx {
    lua_State *lua;
    int resp;
    int base_top;
    int depth;
    int attr_skip;
    int top_error;
    int failed;
    luauReplyFrame frames[LUAU_MAX_REPLY_DEPTH];
} luauReplyCtx;

static int luauReplyBegin(luauReplyCtx *rc, int slots, size_t size, size_t max_size) {
    if (rc->failed || rc->attr_skip) return 0;
    if (size > max_size) {
        rc->failed = LUAU_REPLY_TOO_BIG;
        return 0;
    }
    if (!lua_checkstack(rc->lua, slots + 1)) {
        rc->failed = LUAU_REPLY_TOO_DEEP;
        return 0;
    }
    return 1;
}

static int luauReplyBeginString(luauReplyCtx *rc, int slots, size_t len) {
    return luauReplyBegin(rc, slots, len, LUAU_MAX_STRING_SIZE);
}

static int luauReplyBeginScalar(luauReplyCtx *rc, int slots) {
    return luauReplyBegin(rc, slots, 0, 0);
}

static void luauReplyEmit(luauReplyCtx *rc) {
    if (rc->depth == 0) return;
    lua_State *lua = rc->lua;
    luauReplyFrame *f = &rc->frames[rc->depth - 1];
    f->count++;

    switch (f->type) {
    case LUAU_REPLY_ARRAY:
        lua_rawseti(lua, -2, (int)f->count);
        break;
    case LUAU_REPLY_MAP:
        if (f->count % 2) break;
        if (lua_isnil(lua, -2))
            lua_pop(lua, 2);
        else
            lua_rawset(lua, -3);
        break;
    case LUAU_REPLY_SET:
        if (lua_isnil(lua, -1)) {
            lua_pop(lua, 1);
            break;
        }
        lua_pushboolean(lua, 1);
        lua_rawset(lua, -3);
        break;
    }
}

static int luauReplyOpen(luauReplyCtx *rc, int type, size_t len) {
    if (!luauReplyBegin(rc, 3, len, LUAU_MAX_TABLE_SIZE)) return 0;
    if (rc->depth >= LUAU_MAX_REPLY_DEPTH) {
        rc->failed = LUAU_REPLY_TOO_DEEP;
        return 0;
    }
    rc->frames[rc->depth].type = type;
    rc->frames[rc->depth].count = 0;
    rc->depth++;
    return 1;
}

static void luauReplyNull(void *ctx) {
    luauReplyCtx *rc = ctx;
    if (!luauReplyBeginScalar(rc, 1)) return;
    if (rc->resp == 2)
        lua_pushboolean(rc->lua, 0);
    else
        lua_pushnil(rc->lua);
    luauReplyEmit(rc);
}

static void luauReplyBulkString(void *ctx, const char *str, size_t len) {
    luauReplyCtx *rc = ctx;
    if (!luauReplyBeginString(rc, 1, len)) return;
    lua_pushlstring(rc->lua, str, len);
    luauReplyEmit(rc);
}

static void luauReplySimpleString(void *ctx, const char *str, size_t len) {
    luauReplyCtx *rc = ctx;
    if (!luauReplyBeginString(rc, 3, len)) return;
    lua_newtable(rc->lua);
    lua_pushstring(rc->lua, "ok");
    lua_pushlstring(rc->lua, str, len);
    lua_rawset(rc->lua, -3);
    luauReplyEmit(rc);
}

static void luauReplyVerbatimString(void *ctx, const char *str, size_t len, const char *fmt) {
    luauReplyCtx *rc = ctx;
    lua_State *lua = rc->lua;
    if (!luauReplyBeginString(rc, 5, len)) return;
    lua_newtable(lua);
    lua_pushstring(lua, "verbatim_string");
    lua_newtable(lua);
    lua_pushstring(lua, "string");
    lua_pushlstring(lua, str, len);
    lua_rawset(lua, -3);
    lua_pushstring(lua, "format");
    lua_pushlstring(lua, fmt, 3);
    lua_rawset(lua, -3);
    lua_rawset(lua, -3);
    luauReplyEmit(rc);
}

static void luauReplyError(void *ctx, const char *msg, size_t len) {
    luauReplyCtx *rc = ctx;
    int err_no = errno;
    if (!luauReplyBeginString(rc, 5, len)) return;

    char *err = lm_strncpy(msg, len);
    luauProcessReplyError(err, rc->depth == 0 ? err_no : 0, rc->lua);
    ValkeyModule_Free(err);

    if (rc->depth == 0) rc->top_error = 1;
    luauReplyEmit(rc);
}

static void luauReplyInteger(void *ctx, long long val) {
    luauReplyCtx *rc = ctx;
    if (!luauReplyBeginScalar(rc, 1)) return;
    lua_pushnumber(rc->lua, (lua_Number)val);
    luauReplyEmit(rc);
}

static void luauReplyDouble(void *ctx, double val) {
    luauReplyCtx *rc = ctx;
    if (!luauReplyBeginScalar(rc, 3)) return;
    lua_newtable(rc->lua);
    lua_pushstring(rc->lua, "double");
    lua_pushnumber(rc->lua, val);
    lua_rawset(rc->lua, -3);
    luauReplyEmit(rc);
}

static void luauReplyBigNumber(void *ctx, const char *str, size_t len) {
    luauReplyCtx *rc = ctx;
    if (!luauReplyBeginString(rc, 3, len)) return;
    lua_newtable(rc->lua);
    lua_pushstring(rc->lua, "big_number");
    lua_pushlstring(rc->lua, str, len);
    lua_rawset(rc->lua, -3);
    luauReplyEmit(rc);
}

static void luauReplyBool(void *ctx, int val) {
    luauReplyCtx *rc = ctx;
    if (!luauReplyBeginScalar(rc, 1)) return;
    lua_pushboolean(rc->lua, val);
    luauReplyEmit(rc);
}

static void luauReplyAttributeStart(void *ctx, size_t len) {
    VALKEYMODULE_NOT_USED(len);
    ((luauReplyCtx *)ctx)->attr_skip++;
}

static void luauReplyAttributeEnd(void *ctx) {
    ((luauReplyCtx *)ctx)->attr_skip--;
}

static void luauReplyArrayStart(void *ctx, size_t len) {
    luauReplyCtx *rc = ctx;
    if (!luauReplyOpen(rc, LUAU_REPLY_ARRAY, len)) return;
    lua_createtable(rc->lua, (int)len, 0);
}

static void luauReplyArrayEnd(void *ctx) {
    luauReplyCtx *rc = ctx;
    if (rc->failed || rc->attr_skip) return;
    rc->depth--;
    luauReplyEmit(rc);
}

static void luauReplyKeyedStart(luauReplyCtx *rc, int type, const char *field, size_t len) {
    if (!luauReplyOpen(rc, type, len)) return;
    lua_newtable(rc->lua);
    lua_pushstring(rc->lua, field);
    lua_createtable(rc->lua, 0, (int)len);
}

static void luauReplyMapStart(void *ctx, size_t len) {
    luauReplyKeyedStart(ctx, LUAU_REPLY_MAP, "map", len);
}

static void luauReplySetStart(void *ctx, size_t len) {
    luauReplyKeyedStart(ctx, LUAU_REPLY_SET, "set", len);
}

static void luauReplyKeyedEnd(void *ctx) {
    luauReplyCtx *rc = ctx;
    if (rc->failed || rc->attr_skip) return;
    lua_rawset(rc->lua, -3);
    rc->depth--;
    luauReplyEmit(rc);
}

static void luauReplyParsingError(void *ctx) {
    luauReplyCtx *rc = ctx;
    if (!rc->failed) rc->failed = LUAU_REPLY_BAD_PROTO;
}

static const ValkeyModuleReplyHandlers luauReplyHandlers = {
    .version = 1,
    .null = luauReplyNull,
    .nullBulkString = luauReplyNull,
    .nullArray = luauReplyNull,
    .bulkString = luauReplyBulkString,
    .simpleString = luauReplySimpleString,
    .verbatimString = luauReplyVerbatimString,
    .error = luauReplyError,
    .integer = luauReplyInteger,
    .doubleVal = luauReplyDouble,
    .bigNumber = luauReplyBigNumber,
    .boolVal = luauReplyBool,
    .attributeStart = luauReplyAttributeStart,
    .attributeEnd = luauReplyAttributeEnd,
    .arrayStart = luauReplyArrayStart,
    .arrayEnd = luauReplyArrayEnd,
    .mapStart = luauReplyMapStart,
    .mapEnd = luauReplyKeyedEnd,
    .setStart = luauReplySetStart,
    .setEnd = luauReplyKeyedEnd,
    .replyParsingError = luauReplyParsingError,
};

static int luau_callargv_rejected = 0;

static int luauServerCallLegacy(luauCallCtx *rctx,
                                lua_State *lua,
                                ValkeyModuleString **argv,
                                int argc) {
    char fmt[12] = "v!EMSX";
    int fmt_idx = 6;

    if (!(rctx->replication_flags & PROPAGATE_AOF)) fmt[fmt_idx++] = 'A';
    if (!(rctx->replication_flags & PROPAGATE_REPL)) fmt[fmt_idx++] = 'R';
    if (!rctx->replication_flags) {
        fmt[fmt_idx++] = 'A';
        fmt[fmt_idx++] = 'R';
    }
    if (rctx->resp == 3) fmt[fmt_idx++] = '3';
    fmt[fmt_idx] = '\0';

    size_t cmdlen = 0;
    const char *cmdname = ValkeyModule_StringPtrLen(argv[0], &cmdlen);
    if (memchr(cmdname, '\0', cmdlen)) {
        freeServerArgv(rctx->engine_ctx, rctx->module_ctx, argv, argc);
        luauPushError(lua, "Unknown command called from script");
        return 1;
    }

    errno = 0;
    ValkeyModuleCallReply *reply =
        ValkeyModule_Call(rctx->module_ctx, cmdname, fmt, argv + 1, argc - 1);
    int call_errno = errno;
    freeServerArgv(rctx->engine_ctx, rctx->module_ctx, argv, argc);

    if (reply == NULL) {
        luauPushError(lua, "Unknown command called from script");
        return 1;
    }

    int is_error = 1;
    if (call_errno != 0) {
        size_t len = 0;
        const char *str = ValkeyModule_CallReplyStringPtr(reply, &len);
        char *err = lm_strncpy(str ? str : "", str ? len : 0);
        luauProcessReplyError(err, call_errno, lua);
        ValkeyModule_Free(err);
    } else {
        int base_top = lua_gettop(lua);
        is_error = ValkeyModule_CallReplyType(reply) == VALKEYMODULE_REPLY_ERROR;
        int failed = callReplyToLuaType(lua, reply, rctx->resp, 0);
        if (failed != LUAU_REPLY_OK) is_error = luauReplyFailed(lua, base_top, failed);
    }

    ValkeyModule_FreeCallReply(reply);
    return is_error;
}

static int luauServerCallArgv(luauCallCtx *rctx,
                              lua_State *lua,
                              ValkeyModuleString **argv,
                              int argc) {
    int flags = VALKEYMODULE_CALL_ARGV_SCRIPT_MODE | VALKEYMODULE_CALL_ARGV_REPLICATE |
                VALKEYMODULE_CALL_ARGV_ERRORS_AS_REPLIES |
                VALKEYMODULE_CALL_ARGV_RESPECT_DENY_OOM | VALKEYMODULE_CALL_ARGV_REPLY_EXACT;

    if (!(rctx->replication_flags & PROPAGATE_AOF)) flags |= VALKEYMODULE_CALL_ARGV_NO_AOF;
    if (!(rctx->replication_flags & PROPAGATE_REPL)) flags |= VALKEYMODULE_CALL_ARGV_NO_REPLICAS;
    if (rctx->resp == 3) flags |= VALKEYMODULE_CALL_ARGV_RESP_3;

    luauReplyCtx rc;
    rc.lua = lua;
    rc.resp = rctx->resp;
    rc.base_top = lua_gettop(lua);
    rc.depth = 0;
    rc.attr_skip = 0;
    rc.top_error = 0;
    rc.failed = LUAU_REPLY_OK;

    int res = ValkeyModule_CallArgv(rctx->module_ctx, argv, argc, flags, &luauReplyHandlers, &rc);
    if (res != VALKEYMODULE_OK) {
        int err_no = errno;
        lua_settop(lua, rc.base_top);
        luau_callargv_rejected = 1;
        ValkeyModule_Log(rctx->module_ctx, "warning",
                         "ValkeyModule_CallArgv rejected server.call (%s); "
                         "server.call uses ValkeyModule_Call from now on",
                         strerror(err_no));
        return luauServerCallLegacy(rctx, lua, argv, argc);
    }
    freeServerArgv(rctx->engine_ctx, rctx->module_ctx, argv, argc);

    if (rc.failed != LUAU_REPLY_OK) return luauReplyFailed(lua, rc.base_top, rc.failed);
    if (lua_gettop(lua) == rc.base_top) {
        luauPushError(lua, "Unknown command called from script");
        return 1;
    }
    return rc.top_error;
}

static int luauServerGenericCommand(lua_State *lua, int raise_error) {
    luauCallCtx *rctx = luauGetCallCtx(lua);
    if (!rctx) {
        luauPushError(lua, "server.call is only available inside a script");
        return luauError(lua);
    }

    int argc = 0;
    ValkeyModuleString **argv =
        luauArgsToServerArgv(rctx->engine_ctx, rctx->module_ctx, lua, &argc);
    if (argv == NULL) return raise_error ? luauError(lua) : 1;

    int is_error = ValkeyModule_CallArgv && !luau_callargv_rejected
                       ? luauServerCallArgv(rctx, lua, argv, argc)
                       : luauServerCallLegacy(rctx, lua, argv, argc);

    if (raise_error && is_error) return luauError(lua);
    return 1;
}

static int luauRedisCallCommand(lua_State *lua) {
    return luauServerGenericCommand(lua, 1);
}

static int luauRedisPCallCommand(lua_State *lua) {
    return luauServerGenericCommand(lua, 0);
}

static int luauRedisSha1hexCommand(lua_State *lua) {
    if (lua_gettop(lua) != 1) {
        luauPushError(lua, "wrong number of arguments");
        return luauError(lua);
    }
    size_t len = 0;
    const char *s = lua_tolstring(lua, 1, &len);

    SHA1_CTX sha;
    unsigned char digest[20];
    char digest_hex[41];
    const char *cset = "0123456789abcdef";

    SHA1Init(&sha);
    SHA1Update(&sha, (const unsigned char *)(s ? s : ""), s ? len : 0);
    SHA1Final(digest, &sha);
    for (int j = 0; j < 20; j++) {
        digest_hex[j * 2] = cset[((digest[j] & 0xF0) >> 4)];
        digest_hex[j * 2 + 1] = cset[(digest[j] & 0xF)];
    }
    digest_hex[40] = '\0';

    lua_pushlstring(lua, digest_hex, 40);
    return 1;
}

static int luauReturnSingleFieldTable(lua_State *lua, const char *field) {
    if (lua_gettop(lua) != 1 || lua_type(lua, -1) != LUA_TSTRING) {
        luauPushError(lua, "wrong number or type of arguments");
        return 1;
    }
    lua_newtable(lua);
    lua_pushstring(lua, field);
    lua_pushvalue(lua, -3);
    lua_settable(lua, -3);
    return 1;
}

static int luauRedisStatusReplyCommand(lua_State *lua) {
    return luauReturnSingleFieldTable(lua, "ok");
}

static int luauRedisErrorReplyCommand(lua_State *lua) {
    if (lua_gettop(lua) != 1 || lua_type(lua, -1) != LUA_TSTRING) {
        luauPushError(lua, "wrong number or type of arguments");
        return 1;
    }
    const char *err_info = lua_tostring(lua, -1);
    char *err_buff = (err_info[0] == '-') ? lm_strcpy(err_info)
                                          : lm_asprintf("-%s", err_info);
    luauPushErrorBuff(lua, err_buff);
    ValkeyModule_Free(err_buff);
    return 1;
}

static int luauLogCommand(lua_State *lua) {
    luauCallCtx *rctx = luauGetCallCtx(lua);
    int argc = lua_gettop(lua);

    if (argc < 2) {
        luauPushError(lua, "server.log() requires two arguments or more.");
        return luauError(lua);
    }
    if (!lua_isnumber(lua, -argc)) {
        luauPushError(lua, "First argument must be a number (log level).");
        return luauError(lua);
    }
    int level = (int)lua_tonumber(lua, -argc);
    if (level < LL_DEBUG || level > LL_WARNING) {
        luauPushError(lua, "Invalid log level.");
        return luauError(lua);
    }

    char *log = lm_strcpy("");
    for (int j = 1; j < argc; j++) {
        size_t len = 0;
        const char *s = lua_tolstring(lua, (-argc) + j, &len);
        if (!s) continue;
        char *joined = lm_asprintf("%s%s%.*s", log, log[0] ? " " : "", (int)len, s);
        ValkeyModule_Free(log);
        log = joined;
    }

    static const char *levels[] = {"debug", "verbose", "notice", "warning"};
    ValkeyModule_Log(rctx ? rctx->module_ctx : NULL, levels[level], "%s", log);
    ValkeyModule_Free(log);
    return 0;
}

static int luauSetResp(lua_State *lua) {
    luauCallCtx *rctx = luauGetCallCtx(lua);
    if (!rctx) {
        luauPushError(lua, "server.setresp is only available inside a script");
        return luauError(lua);
    }
    if (lua_gettop(lua) != 1) {
        luauPushError(lua, "server.setresp() requires one argument.");
        return luauError(lua);
    }
    int resp = (int)lua_tonumber(lua, -1);
    if (resp != 2 && resp != 3) {
        luauPushError(lua, "RESP version must be 2 or 3.");
        return luauError(lua);
    }
    rctx->resp = resp;
    return 0;
}

static int luauRedisSetReplCommand(lua_State *lua) {
    luauCallCtx *rctx = luauGetCallCtx(lua);
    if (!rctx) {
        luauPushError(lua, "server.set_repl is only available inside a script");
        return luauError(lua);
    }
    if (lua_gettop(lua) != 1) {
        luauPushError(lua, "server.set_repl() requires one argument.");
        return luauError(lua);
    }
    int flags = (int)lua_tonumber(lua, -1);
    if ((flags & ~(PROPAGATE_AOF | PROPAGATE_REPL)) != 0) {
        luauPushError(lua, "Invalid replication flags. Use REPL_AOF, REPL_REPLICA, "
                           "REPL_ALL or REPL_NONE.");
        return luauError(lua);
    }
    rctx->replication_flags = flags;
    return 0;
}

static int luauRedisAclCheckCmdPermissionsCommand(lua_State *lua) {
    luauCallCtx *rctx = luauGetCallCtx(lua);
    if (!rctx) {
        luauPushError(lua, "server.acl_check_cmd is only available inside a script");
        return luauError(lua);
    }

    int argc = 0;
    ValkeyModuleString **argv =
        luauArgsToServerArgv(rctx->engine_ctx, rctx->module_ctx, lua, &argc);
    if (argv == NULL) return luauError(lua);

    ValkeyModuleString *username = ValkeyModule_GetCurrentUserName(rctx->module_ctx);
    ValkeyModuleUser *user =
        username ? ValkeyModule_GetModuleUserFromUserName(username) : NULL;
    int dbid = ValkeyModule_GetSelectedDb(rctx->module_ctx);
    if (username) ValkeyModule_FreeString(rctx->module_ctx, username);

    if (user == NULL) {
        freeServerArgv(rctx->engine_ctx, rctx->module_ctx, argv, argc);
        luauPushError(lua, "Invalid command passed to server.acl_check_cmd()");
        return luauError(lua);
    }

    int raise_error = 0;
    errno = 0;
    if (ValkeyModule_ACLCheckPermissions(user, argv, argc, dbid, NULL) != VALKEYMODULE_OK) {
        if (errno == EINVAL) {
            luauPushError(lua, "Invalid command passed to server.acl_check_cmd()");
            raise_error = 1;
        } else {
            lua_pushboolean(lua, 0);
        }
    } else {
        lua_pushboolean(lua, 1);
    }

    ValkeyModule_FreeModuleUser(user);
    freeServerArgv(rctx->engine_ctx, rctx->module_ctx, argv, argc);

    if (raise_error) return luauError(lua);
    return 1;
}

static int luauReplicateCommandsCommand(lua_State *lua) {
    lua_pushboolean(lua, 1);
    return 1;
}

static int luauRedisPcall(lua_State *lua) {
    int argc = lua_gettop(lua);
    if (argc < 1) {
        luauPushError(lua, "Please specify at least one argument for this call");
        return luauError(lua);
    }
    int status = lua_pcall(lua, argc - 1, LUA_MULTRET, 0);
    lua_pushboolean(lua, status == 0);
    lua_insert(lua, 1);
    if (status != 0 && lua_istable(lua, 2)) {
        lua_getfield(lua, 2, "err");
        if (lua_isstring(lua, -1)) {
            lua_replace(lua, 2);
            lua_settop(lua, 2);
        } else {
            lua_pop(lua, 1);
        }
    }
    return lua_gettop(lua);
}

static int luauErrorFunction(lua_State *lua) {
    int argc = lua_gettop(lua);

    if (argc >= 1 && lua_istable(lua, 1)) {
        lua_settop(lua, 1);
        lua_rawgetfield(lua, 1, "source");
        int has_source = !lua_isnil(lua, -1);
        lua_pop(lua, 1);
        if (!has_source && !lua_getreadonly(lua, 1)) {
            lua_pushvalue(lua, 1);
            luauAttachSourceLine(lua);
            lua_pop(lua, 1);
        }
        lua_error(lua);
        return 0;
    }

    int level = (int)luaL_optinteger(lua, 2, 1);
    lua_settop(lua, 1);
    lua_pushinteger(lua, level > 0 ? level + 1 : level);
    lua_pushvalue(lua, lua_upvalueindex(1));
    lua_insert(lua, 1);
    lua_call(lua, 2, 0);
    (void)argc;
    return 0;
}

static void luauRegisterLogFunction(lua_State *lua) {
    lua_pushstring(lua, "LOG_DEBUG");
    lua_pushnumber(lua, LL_DEBUG);
    lua_settable(lua, -3);
    lua_pushstring(lua, "LOG_VERBOSE");
    lua_pushnumber(lua, LL_VERBOSE);
    lua_settable(lua, -3);
    lua_pushstring(lua, "LOG_NOTICE");
    lua_pushnumber(lua, LL_NOTICE);
    lua_settable(lua, -3);
    lua_pushstring(lua, "LOG_WARNING");
    lua_pushnumber(lua, LL_WARNING);
    lua_settable(lua, -3);

    lua_pushstring(lua, "log");
    lua_pushcfunction(lua, luauLogCommand, "log");
    lua_settable(lua, -3);
}

static void luauRegisterVersion(luauEngineCtx *ctx, lua_State *lua) {
    lua_pushstring(lua, "REDIS_VERSION_NUM");
    lua_pushnumber(lua, ctx->redis_version_num);
    lua_settable(lua, -3);
    lua_pushstring(lua, "REDIS_VERSION");
    lua_pushstring(lua, ctx->redis_version ? ctx->redis_version : "");
    lua_settable(lua, -3);
    lua_pushstring(lua, "VALKEY_VERSION_NUM");
    lua_pushnumber(lua, ctx->valkey_version_num);
    lua_settable(lua, -3);
    lua_pushstring(lua, "VALKEY_VERSION");
    lua_pushstring(lua, ctx->valkey_version ? ctx->valkey_version : "");
    lua_settable(lua, -3);
    lua_pushstring(lua, "SERVER_NAME");
    lua_pushstring(lua, ctx->server_name ? ctx->server_name : "");
    lua_settable(lua, -3);
}

static void luauRegisterServerAPI(luauEngineCtx *ctx, lua_State *lua) {
    lua_newtable(lua);

    lua_pushstring(lua, "call");
    lua_pushcfunction(lua, luauRedisCallCommand, "call");
    lua_settable(lua, -3);

    lua_pushstring(lua, "pcall");
    lua_pushcfunction(lua, luauRedisPCallCommand, "pcall");
    lua_settable(lua, -3);

    luauRegisterLogFunction(lua);
    luauRegisterVersion(ctx, lua);

    lua_pushstring(lua, "setresp");
    lua_pushcfunction(lua, luauSetResp, "setresp");
    lua_settable(lua, -3);

    lua_pushstring(lua, "sha1hex");
    lua_pushcfunction(lua, luauRedisSha1hexCommand, "sha1hex");
    lua_settable(lua, -3);

    lua_pushstring(lua, "error_reply");
    lua_pushcfunction(lua, luauRedisErrorReplyCommand, "error_reply");
    lua_settable(lua, -3);

    lua_pushstring(lua, "status_reply");
    lua_pushcfunction(lua, luauRedisStatusReplyCommand, "status_reply");
    lua_settable(lua, -3);

    lua_pushstring(lua, "set_repl");
    lua_pushcfunction(lua, luauRedisSetReplCommand, "set_repl");
    lua_settable(lua, -3);

    lua_pushstring(lua, "REPL_NONE");
    lua_pushnumber(lua, PROPAGATE_NONE);
    lua_settable(lua, -3);
    lua_pushstring(lua, "REPL_AOF");
    lua_pushnumber(lua, PROPAGATE_AOF);
    lua_settable(lua, -3);
    lua_pushstring(lua, "REPL_SLAVE");
    lua_pushnumber(lua, PROPAGATE_REPL);
    lua_settable(lua, -3);
    lua_pushstring(lua, "REPL_REPLICA");
    lua_pushnumber(lua, PROPAGATE_REPL);
    lua_settable(lua, -3);
    lua_pushstring(lua, "REPL_ALL");
    lua_pushnumber(lua, PROPAGATE_AOF | PROPAGATE_REPL);
    lua_settable(lua, -3);

    lua_pushstring(lua, "acl_check_cmd");
    lua_pushcfunction(lua, luauRedisAclCheckCmdPermissionsCommand, "acl_check_cmd");
    lua_settable(lua, -3);

    lua_pushstring(lua, "replicate_commands");
    lua_pushcfunction(lua, luauReplicateCommandsCommand, "replicate_commands");
    lua_settable(lua, -3);

    lua_pushvalue(lua, -1);
    lua_setfield(lua, LUA_GLOBALSINDEX, SERVER_API_NAME);
    lua_setfield(lua, LUA_GLOBALSINDEX, REDIS_API_NAME);
}

lua_State *luauCreateVM(luauEngineCtx *ctx, ValkeyModuleCtx *module_ctx) {
    (void)module_ctx;

    lua_State *GL = lua_newstate(luauAlloc, ctx);
    if (!GL) return NULL;
    ctx->GL = GL;

    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        serverSrand48((int32_t)(ts.tv_nsec ^ ts.tv_sec));
        serverGetRandState(ctx->rand_state);
    }

    luaL_openlibs(GL);

    static const char *denied[] = {"print", "getfenv", "setfenv", "newproxy", NULL};
    for (const char **d = denied; *d; ++d) {
        lua_pushnil(GL);
        lua_setfield(GL, LUA_GLOBALSINDEX, *d);
    }

    lua_getfield(GL, LUA_GLOBALSINDEX, "math");
    lua_pushcfunction(GL, luauMathRandom, "random");
    lua_setfield(GL, -2, "random");
    lua_pushcfunction(GL, luauMathRandomSeed, "randomseed");
    lua_setfield(GL, -2, "randomseed");
    lua_pop(GL, 1);

    lua_newtable(GL);
    lua_getfield(GL, LUA_GLOBALSINDEX, "os");
    lua_getfield(GL, -1, "clock");
    lua_setfield(GL, -3, "clock");
    lua_pop(GL, 1);
    lua_setfield(GL, LUA_GLOBALSINDEX, "os");

    lua_pushcfunction(GL, luauRedisPcall, "pcall");
    lua_setfield(GL, LUA_GLOBALSINDEX, "pcall");

    lua_getfield(GL, LUA_GLOBALSINDEX, "error");
    lua_pushcclosure(GL, luauErrorFunction, "error", 1);
    lua_setfield(GL, LUA_GLOBALSINDEX, "error");

    lua_pushcfunction(GL, luaopen_cjson, "luaopen_cjson");
    lua_call(GL, 0, 1);
    lua_pushcfunction(GL, luaopen_cjson_safe, "luaopen_cjson_safe");
    lua_call(GL, 0, 1);
    lua_setfield(GL, -2, "safe");
    lua_setfield(GL, LUA_GLOBALSINDEX, "cjson");

    lua_pushcfunction(GL, luaopen_struct, "luaopen_struct");
    lua_call(GL, 0, 1);
    lua_pop(GL, 1);

    lua_pushcfunction(GL, luaopen_cmsgpack, "luaopen_cmsgpack");
    lua_call(GL, 0, 1);
    lua_pop(GL, 1);

    lua_pushcfunction(GL, luaopen_cmsgpack_safe, "luaopen_cmsgpack_safe");
    lua_call(GL, 0, 1);
    lua_pop(GL, 1);

    lua_pushcfunction(GL, luaopen_bit, "luaopen_bit");
    lua_call(GL, 0, 1);
    lua_setfield(GL, LUA_GLOBALSINDEX, "bit");

    luauRegisterServerAPI(ctx, GL);

    lua_pushvalue(GL, LUA_GLOBALSINDEX);
    lua_pushcclosure(GL, luauGlobalReadGuard, "__index", 1);
    ctx->read_guard_ref = lua_ref(GL, -1);
    lua_pop(GL, 1);

    lua_pushvalue(GL, LUA_GLOBALSINDEX);
    lua_pushcclosure(GL, luauGlobalWriteGuard, "__newindex", 1);
    ctx->write_guard_ref = lua_ref(GL, -1);
    lua_pop(GL, 1);

    lua_pushvalue(GL, LUA_GLOBALSINDEX);
    lua_pushcclosure(GL, luauLibLoadReadGuard, "__index", 1);
    ctx->libload_guard_ref = lua_ref(GL, -1);
    lua_pop(GL, 1);

    lua_pushcfunction(GL, luauMissingFieldGuard, "__index");
    ctx->field_guard_ref = lua_ref(GL, -1);
    lua_pop(GL, 1);

    lua_newtable(GL);
    lua_getref(GL, ctx->read_guard_ref);
    lua_setfield(GL, -2, "__index");
    lua_getref(GL, ctx->write_guard_ref);
    lua_setfield(GL, -2, "__newindex");
    lua_setreadonly(GL, -1, 1);
    ctx->env_meta_ref = lua_ref(GL, -1);
    lua_pop(GL, 1);

    lua_pushvalue(GL, LUA_GLOBALSINDEX);
    luauFreezeRecursive(GL);
    lua_pop(GL, 1);
    luauFreezeBasicTypeMetatables(GL);

    luaL_sandbox(GL);

    return GL;
}

void luauDestroyVM(luauEngineCtx *ctx) {
    if (ctx->GL) {
        lua_close(ctx->GL);
        ctx->GL = NULL;
    }
}

void luauPushFieldGuard(luauEngineCtx *ctx, lua_State *T) {
    lua_getref(T, ctx->field_guard_ref);
}

void luauFreezeThreadEnv(luauEngineCtx *ctx, lua_State *T, int has_overrides, int restricted) {
    lua_pushvalue(T, LUA_GLOBALSINDEX);
    lua_newtable(T);
    lua_getref(T, restricted ? ctx->libload_guard_ref : ctx->read_guard_ref);
    lua_setfield(T, -2, "__index");
    lua_getref(T, ctx->write_guard_ref);
    lua_setfield(T, -2, "__newindex");
    if (has_overrides) {
        lua_pushvalue(T, -3);
        lua_setreadonly(T, -1, 1);
        lua_setfield(T, -2, "__overrides");
    }
    lua_setreadonly(T, -1, 1);
    lua_setmetatable(T, -2);
    lua_pop(T, has_overrides ? 2 : 1);

    lua_setreadonly(T, LUA_GLOBALSINDEX, 1);

    lua_setsafeenv(T, LUA_GLOBALSINDEX, restricted ? 0 : 1);
}

lua_State *luauNewScriptThread(luauEngineCtx *ctx, ValkeyModuleString **keys, size_t nkeys, ValkeyModuleString **args, size_t nargs, int set_keys_globals, int memcat) {
    lua_State *T = lua_newthread(ctx->GL);
    lua_setmemcat(T, memcat);

    lua_newtable(T);

    if (set_keys_globals) {
        lua_createtable(T, (int)nkeys, 0);
        for (size_t i = 0; i < nkeys; i++) {
            size_t len = 0;
            const char *str = ValkeyModule_StringPtrLen(keys[i], &len);
            lua_pushlstring(T, str, len);
            lua_rawseti(T, -2, (int)i + 1);
        }
        lua_setfield(T, -2, "KEYS");

        lua_createtable(T, (int)nargs, 0);
        for (size_t i = 0; i < nargs; i++) {
            size_t len = 0;
            const char *str = ValkeyModule_StringPtrLen(args[i], &len);
            lua_pushlstring(T, str, len);
            lua_rawseti(T, -2, (int)i + 1);
        }
        lua_setfield(T, -2, "ARGV");
    }

    lua_getref(T, ctx->env_meta_ref);
    lua_setmetatable(T, -2);

    lua_replace(T, LUA_GLOBALSINDEX);
    lua_setreadonly(T, LUA_GLOBALSINDEX, 1);
    lua_setsafeenv(T, LUA_GLOBALSINDEX, 1);

    return T;
}

size_t luauMemoryForCategory(lua_State *lua, int category) {
    return lua_totalbytes(lua, category);
}

int luauResume(luauCallCtx *call_ctx, lua_State *T, int nargs) {
    int status = lua_resume(T, NULL, nargs);
    while (status == LUA_YIELD && !call_ctx->killed) {
        lua_settop(T, 0);
        luaL_where(T, 1);
        lua_pushstring(T, "attempt to yield across metamethod/C-call boundary");
        lua_concat(T, 2);
        status = lua_resumeerror(T, NULL);
    }
    return status;
}

void luauCallFunction(luauCallCtx *call_ctx, lua_State *T, int nargs) {
    ValkeyModuleCtx *ctx = call_ctx->module_ctx;
    luauEngineCtx *engine_ctx = call_ctx->engine_ctx;

    luauSetCallCtx(T, call_ctx);

    long long busy_ms = atomic_load(&luau_busy_threshold_ms);
    int watched = 0;
    if (call_ctx->mem_budget == 0 && busy_ms > 0 && luau_wd_started) {
        atomic_store(&luau_wd_arm_at_ms, luauNowMs() + busy_ms);
        atomic_store(&luau_wd_vm, engine_ctx->GL);
        watched = 1;
    } else {
        lua_callbacks(engine_ctx->GL)->interrupt = luauInterrupt;
    }

    int status = luauResume(call_ctx, T, nargs);

    if (watched) {
        lua_State *expected = engine_ctx->GL;
        atomic_compare_exchange_strong(&luau_wd_vm, &expected, NULL);
    }
    lua_callbacks(engine_ctx->GL)->interrupt = NULL;
    luauSetCallCtx(T, NULL);

    if (++engine_ctx->gc_count >= LUAU_GC_CYCLE_PERIOD) {
        lua_gc(engine_ctx->GL, LUA_GCSTEP, LUAU_GC_CYCLE_PERIOD);
        engine_ctx->gc_count = 0;
    }
    if (++engine_ctx->full_gc_count >= LUAU_FULL_GC_CYCLE) {
        lua_gc(engine_ctx->GL, LUA_GCCOLLECT, 0);
        engine_ctx->full_gc_count = 0;
    }

    if (status == LUA_YIELD) {
        if (call_ctx->mem_exceeded) {
            ValkeyModule_ReplyWithError(
                ctx, "OOM script exceeded its memory limit");
        } else if (call_ctx->type == VMSE_EVAL) {
            ValkeyModule_ReplyWithError(
                ctx, "ERR Script killed by user with SCRIPT KILL.");
        } else {
            ValkeyModule_ReplyWithError(
                ctx, "ERR Script killed by user with FUNCTION KILL.");
        }
        return;
    }

    if (status != LUA_OK) {
        lua_State *GL = engine_ctx->GL;
        errorInfo err_info = {0};
        lua_checkstack(GL, 2);
        lua_xmove(T, GL, 1);
        luauExtractErrorInformation(GL, &err_info);
        lua_pop(GL, 1);
        if (err_info.line && err_info.source) {
            ValkeyModule_ReplyWithCustomErrorFormat(
                ctx, !err_info.ignore_err_stats_update, "%s script: on %s:%s.",
                err_info.msg, err_info.source, err_info.line);
        } else {
            ValkeyModule_ReplyWithCustomErrorFormat(
                ctx, !err_info.ignore_err_stats_update, "%s", err_info.msg);
        }
        luauErrorInformationDiscard(&err_info);
        return;
    }

    if (lua_gettop(T) == 0) {
        ValkeyModule_ReplyWithNull(ctx);
        return;
    }

    lua_settop(T, 1);
    luauReplyToServerReply(ctx, call_ctx->resp, T);
}
