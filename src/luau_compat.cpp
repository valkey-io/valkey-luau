#include "luau_compat.h"

#include "lstate.h"

void luauDisableBuiltinPcall(lua_State *L) {
    L->global->builtinPcall = nullptr;
}
