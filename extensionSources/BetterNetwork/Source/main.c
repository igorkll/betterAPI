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
    wchar_t* wUrl;
} Connection;

typedef struct {
    HINTERNET hRequest;
    wchar_t* wRequestType;
    wchar_t* wRequestPath;
} Request;

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
    
    connection->wUrl = convertString(url);
    connection->hConnect = WinHttpConnect(hSession, connection->wUrl, port, 0);

    if (!connection->hConnect) {
        lua_pushinteger(L, GetLastError());
        free(connection);
        return 1;
    }

    lua_pushlightuserdata(L, (void*)connection);
    return 1;
}

static int _closeConnection(lua_State* L) {
    Connection* connection = (Connection*)lua_touserdata(L, 1);
    WinHttpCloseHandle(connection->hConnect);
    free(connection->wUrl);
    free(connection);
    return 0;
}


static int _newRequest(lua_State* L) {
    Connection* connection = (Connection*)lua_touserdata(L, 1);
    const char* requestType = luaL_checkstring(L, 2);
    const char* requestPath = luaL_checkstring(L, 3);


    Request* request = malloc(sizeof(Request));
    
    request->wRequestType = convertString(requestType);
    request->wRequestPath = convertString(requestPath);
    request->hRequest = WinHttpOpenRequest(connection->hConnect, request->wRequestType, request->wRequestPath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);

    if (!request->hRequest) {
        lua_pushinteger(L, GetLastError());
        free(request);
        return 1;
    }

    lua_pushlightuserdata(L, (void*)request);
    return 1;
}

static int _sendRequest(lua_State* L) {
    Request* request = (Request*)lua_touserdata(L, 1);
    const char* headers = luaL_checkstring(L, 2);
    if (strlen(headers) > 0) {
        wchar_t* wHeaders = convertString(headers);
        WinHttpAddRequestHeaders(request->hRequest, wHeaders, (ULONG)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        free(wHeaders);
    }
    lua_pushboolean(L, WinHttpSendRequest(request->hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0));
    free(request);
    return 1;
}

static int _getResult(lua_State* L) {
    Request* request = (Request*)lua_touserdata(L, 1);
    
    if (!WinHttpReceiveResponse(request->hRequest, NULL)) {
        lua_pushinteger(L, GetLastError());
        return 1;
    }

    DWORD dwSize = 0;
    DWORD dwDownloaded = 0;
    LPSTR pszOutBuffer;

    WinHttpQueryDataAvailable(request->hRequest, &dwSize);
    
    if (dwSize > 0) {
        pszOutBuffer = (LPSTR)malloc(dwSize + 1);
        if (pszOutBuffer) {
            pszOutBuffer[dwSize] = '\0';
            WinHttpReadData(request->hRequest, (LPVOID)pszOutBuffer, dwSize, &dwDownloaded);
            lua_pushlstring(L, pszOutBuffer, dwSize);
            free(pszOutBuffer);
        }
    } else {
        lua_pushstring(L, "");
    }

    return 1;
}

static int _closeRequest(lua_State* L) {
    Request* request = (Request*)lua_touserdata(L, 1);
    WinHttpCloseHandle(request->hRequest);
    free(request->wRequestType);
    free(request->wRequestPath);
    free(request);
    return 0;
}

// -----------------------------------

static const struct luaL_Reg g_functions[] = {
    {"init", _init},
    {"deinit", _deinit},

    {"newConnection", _newConnection},
    {"closeConnection", _closeConnection},

    {"newRequest", _newRequest},
    {"sendRequest", _sendRequest},
    {"getResult", _getResult},
    {"closeRequest", _closeRequest},

    {NULL, NULL}
};

EXPORT int luaopen_BetterNetwork(lua_State * L) {
    luaL_register(L, "BetterNetwork", g_functions);

    return 1;
}