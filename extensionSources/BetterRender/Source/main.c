#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <windows.h>
#include <sys/stat.h>
#include <luajit/lua.h>
#include <luajit/lauxlib.h>
#include <luajit/lualib.h>
#include <d3d11.h>
#include <dxgi.h>

#define PACKED __attribute__((packed))
#define EXPORT __declspec(dllexport)


// -----------------------------------



// -----------------------------------

static const struct luaL_Reg g_functions[] = {

    {NULL, NULL}
};

EXPORT int luaopen_BetterRender(lua_State * L) {
    luaL_register(L, "BetterRender", g_functions);

    return 1;
}