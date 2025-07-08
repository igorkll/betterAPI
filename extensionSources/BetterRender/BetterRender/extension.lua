local better, directory = ...

local ffi = require("ffi")
local BetterRender = ffi.load(directory .. "libBetterRender.dll")

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
]]

-- Example usage
local width, height = 800, 600
local render = BetterRender.create(width, height)

-- Begin drawing
BetterRender.begin_draw(render)

BetterRender.draw_clear(render, 0x0000ff)
BetterRender.draw_pixel(render, 100, 100, 0xFF0000)

-- End drawing
BetterRender.end_draw(render)

-- Begin reading
BetterRender.begin_read(render)

-- Read a pixel color
local pixelColor = BetterRender.read_pixel(render, 100, 100)

print("------------ TEST ------------", pixelColor)

-- End reading
--BetterRender.end_read(render)

-- Clean up
--BetterRender.destroy(render)
