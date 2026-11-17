#ifndef _LUAU_COMPAT_H_
#define _LUAU_COMPAT_H_

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

void luauDisableBuiltinPcall(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif
