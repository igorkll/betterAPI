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

EXPORT BetterRender* create(int width, int height) {
    init();

    BetterRender* betterRender = malloc(sizeof(BetterRender));
    betterRender->renderTarget = LoadRenderTexture(width, height);

    return betterRender;
}

EXPORT void destroy(BetterRender* betterRender) {
    UnloadRenderTexture(betterRender->renderTarget);
    free(betterRender);
}

// -----------------------------------

EXPORT void begin_draw(BetterRender* betterRender) {
    BeginTextureMode(betterRender->renderTarget);
}

EXPORT void draw_clear(BetterRender* betterRender, uint32_t color) {
    ClearBackground(unpackColor(color));
}

EXPORT void draw_pixel(BetterRender* betterRender, int x, int y, uint32_t color) {
    DrawPixel(x, y, unpackColor(color));
}

EXPORT void end_draw(BetterRender* betterRender) {
    EndTextureMode();
}

// -----------------------------------

EXPORT void begin_read(BetterRender* betterRender) {
    betterRender->image = LoadImageFromTexture(betterRender->renderTarget.texture);
}

EXPORT uint32_t read_pixel(BetterRender* betterRender, int x, int y) {
    Color color = GetImageColor(betterRender->image, x, y);
    return packColor(color);
}

EXPORT void end_read(BetterRender* betterRender) {
    UnloadImage(betterRender->image);
}

EXPORT int test(int test) {
    return test * 2;
}
