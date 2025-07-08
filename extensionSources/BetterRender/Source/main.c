#define RLIGHTS_IMPLEMENTATION
#define GLSL_VERSION 330

#include <raylib.h>
#include <raymath.h>
#include <rcamera.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <sys/stat.h>
#include <rlights.h>
#include <luajit/lua.h>
#include <luajit/lauxlib.h>
#include <luajit/lualib.h>

#define PACKED __attribute__((packed))
#define EXPORT __declspec(dllexport)

#define dmax(a, b) ((a) > (b) ? (a) : (b))
#define dmin(a, b) ((a) < (b) ? (a) : (b))

typedef struct {
    RenderTexture2D renderTarget;
    Image image;
} BetterRender;

// -----------------------------------

static bool inited = false;

static void init() {
    if (!inited) {
        InitWindow(100, 100, "empty window");
        SetWindowState(FLAG_WINDOW_HIDDEN);
        SetTargetFPS(0);
        inited = true;
    }
}

static uint32_t packColor(Color color) {
    return (color.r * 256 * 256) + (color.g * 256) + color.b;
}

static Color unpackColor(uint32_t color) {
    return GetColor((color * 256) + 255);
}

// -----------------------------------

static int create(lua_State* L) {
    init();

    int width = luaL_checkinteger(L, 1);
    int height = luaL_checkinteger(L, 2);

    BetterRender* betterRender = malloc(sizeof(BetterRender));
    betterRender->renderTarget = LoadRenderTexture(width, height);

    lua_pushlightuserdata(L, (void*)betterRender);
    return 1;
}

static int destroy(lua_State* L) {
    BetterRender* betterRender = (BetterRender*)lua_touserdata(L, 1);

    UnloadRenderTexture(betterRender->renderTarget);
    free(betterRender);

    return 0;
}

// -----------------------------------

static int begin_draw(lua_State* L) {
    BetterRender* betterRender = (BetterRender*)lua_touserdata(L, 1);
    BeginTextureMode(betterRender->renderTarget);
    return 0;
}

static int draw_clear(lua_State* L) {
    BetterRender* betterRender = (BetterRender*)lua_touserdata(L, 1);
    int64_t color = luaL_checkinteger(L, 2);

    ClearBackground(unpackColor(color));
    return 0;
}

static int draw_pixel(lua_State* L) {
    BetterRender* betterRender = (BetterRender*)lua_touserdata(L, 1);
    int x = luaL_checkinteger(L, 2);
    int y = luaL_checkinteger(L, 3);
    int64_t color = luaL_checkinteger(L, 4);

    DrawPixel(x, y, unpackColor(color));
    return 0;
}

static int end_draw(lua_State* L) {
    EndTextureMode();
    return 0;
}

// -----------------------------------

static int begin_read(lua_State* L) {
    BetterRender* betterRender = (BetterRender*)lua_touserdata(L, 1);
    betterRender->image = LoadImageFromTexture(betterRender->renderTarget.texture);
    return 0;
}

static int read_pixel(lua_State* L) {
    BetterRender* betterRender = (BetterRender*)lua_touserdata(L, 1);
    int x = luaL_checkinteger(L, 2);
    int y = luaL_checkinteger(L, 3);
    Color color = GetImageColor(betterRender->image, x, y);
    lua_pushinteger(L, packColor(color));
    return 1;
}

static int end_read(lua_State* L) {
    BetterRender* betterRender = (BetterRender*)lua_touserdata(L, 1);
    UnloadImage(betterRender->image);
    return 0;
}

// -----------------------------------

static const struct luaL_Reg g_functions[] = {
    {"create", create},
    {"destroy", destroy},

    {"begin_draw", begin_draw},
    {"draw_clear", draw_clear},
    {"draw_pixel", draw_pixel},
    {"end_draw", end_draw},

    {"begin_read", begin_read},
    {"read_pixel", read_pixel},
    {"end_read", end_read},

    {NULL, NULL}
};

EXPORT int luaopen_BetterRender(lua_State * L) {
    luaL_register(L, "BetterRender", g_functions);

    return 1;
}