#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <sys/stat.h>
#include <luajit/lua.h>
#include <luajit/lauxlib.h>
#include <luajit/lualib.h>

#include <windows.h>
#include <winhttp.h>

#define PACKED __attribute__((packed))
#define EXPORT __declspec(dllexport)

#define dmax(a, b) ((a) > (b) ? (a) : (b))
#define dmin(a, b) ((a) < (b) ? (a) : (b))

static HINTERNET hSession;

typedef struct {
    HINTERNET hConnect;
} Connection;

// -----------------------------------

static wchar_t* convertString(const char* serverName) {
    int wchars_num = MultiByteToWideChar(CP_UTF8, 0, serverName, -1, NULL, 0);
    if (wchars_num <= 0) {
        return NULL;
    }

    size_t len = wchars_num * sizeof(wchar_t);
    wchar_t* wideServerName = (wchar_t*)malloc(len + 1);
    if (!wideServerName) {
        return NULL;
    }

    MultiByteToWideChar(CP_UTF8, 0, serverName, -1, wideServerName, wchars_num);
    wideServerName[len] = '\0';
    return wideServerName;
}

// -----------------------------------

static int _init(lua_State* L) {
    hSession = WinHttpOpen(L"BetterAPI - BetterNetwork",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    return 0;
}

static int _deinit(lua_State* L) {
    WinHttpCloseHandle(hSession);
    return 0;
}

static int _newConnection(lua_State* L) {
    const char* url = luaL_checkstring(L, 1);
    INTERNET_PORT port = luaL_checkinteger(L, 2);


    Connection* connection = malloc(sizeof(Connection));
    
    wchar_t* wUrl = convertString(url);
    connection->hConnect = WinHttpConnect(hSession, wUrl, port, 0);
    free(wUrl);

    if (!connection->hConnect) {
        lua_pushinteger(L, GetLastError());
        return 1;
    }

    lua_pushlightuserdata(L, (void*)connection);
    return 1;
}

static int _closeConnection(lua_State* L) {
    Connection* connection = (Connection*)lua_touserdata(L, 1);
    WinHttpCloseHandle(connection->hConnect);
    free(connection);
    return 0;
}

// -----------------------------------

static const struct luaL_Reg g_functions[] = {
    {"init", _init},
    {"deinit", _deinit},

    {"newConnection", _newConnection},
    {"closeConnection", _closeConnection},

    {NULL, NULL}
};

EXPORT int luaopen_BetterNetwork(lua_State * L) {
    luaL_register(L, "BetterNetwork", g_functions);

    return 1;
}