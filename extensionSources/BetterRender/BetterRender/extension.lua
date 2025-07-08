local better, directory = ...

local ffi = require("ffi")
local BetterRender = ffi.load(directory .. "BetterRender.dll")

ffi.cdef[[
typedef struct BetterRender BetterRender;

BetterRender* create(int width, int height);
void destroy(BetterRender* betterRender);

void begin_draw(BetterRender* betterRender);
void draw_clear(BetterRender* betterRender, uint32_t color);
void draw_pixel(BetterRender* betterRender, int posX, int posY, uint32_t color);
void end_draw(BetterRender* betterRender);

void begin_read(BetterRender* betterRender);
uint32_t read_pixel(BetterRender* betterRender, int width, int height);
void end_read(BetterRender* betterRender);

int test(int test);
]]

better.render = {
    create = BetterRender.create,
    destroy = BetterRender.destroy,

    begin_draw = BetterRender.begin_draw,
    draw_clear = BetterRender.draw_clear,
    draw_pixel = BetterRender.draw_pixel,
    end_draw = BetterRender.end_draw,

    begin_read = BetterRender.begin_read,
    read_pixel = BetterRender.read_pixel,
    end_read = BetterRender.end_read,

    test = BetterRender.test
}