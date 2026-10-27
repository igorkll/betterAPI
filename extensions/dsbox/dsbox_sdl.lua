local dsbox = require "better_dsbox"
local sdl = dsbox.sdl


local window_metatable = {
    __metatable = false,
    __index = {}
}

local window_instances = setmetatable({}, {
    __mode = "k"
})

local function get_window_id(window)
    return tonumber(window_instances[window])
end

local function is_window_valid(window)
    local window_id = get_window_id(window)

    if window_id then
        return not not sdl.is_valid(window_id)
    end

    return false
end

local function get_valid_window_id(window)
    if not is_window_valid(window) then
        error("invalid window handle", 3)
    end

    return get_window_id(window)
end

function window_metatable.__index.isVisible(self)
    return not not sdl.is_visible(get_valid_window_id(self))
end

window_metatable.__index.getId = get_window_id
window_metatable.__index.isValid = is_window_valid

function window_metatable.__index.close(self)
    sdl.close_window(get_valid_window_id(self))
    window_instances[self] = nil
end

function window_metatable.__index.hide(self)
    return not not sdl.hide(get_valid_window_id(self))
end

function window_metatable.__index.show(self)
    return not not sdl.show(get_valid_window_id(self))
end

function window_metatable.__index.setTitle(self, title)
    if type(title) ~= "string" then
        error("title must be a string", 2)
    end

    sdl.set_title(get_valid_window_id(self), title)
end

function window_metatable.__index.setSize(self, width, height)
    if type(width) ~= "number"
    or type(height) ~= "number" then
        error("window dimensions must be numbers", 2)
    end

    return not not sdl.set_size(
        get_valid_window_id(self),
        width,
        height
    )
end

function window_metatable.__index.setPosition(self, x, y)
    if type(x) ~= "number"
    or type(y) ~= "number" then
        error("window position must be numbers", 2)
    end

    return not not sdl.set_position(
        get_valid_window_id(self),
        x,
        y
    )
end

function window_metatable.__index.getTitle(self)
    return tostring(sdl.get_title(get_valid_window_id(self)))
end

function window_metatable.__index.getSize(self)
    local width, height = sdl.get_size(get_valid_window_id(self))

    return tonumber(width), tonumber(height)
end

function window_metatable.__index.getPosition(self)
    local x, y = sdl.get_position(get_valid_window_id(self))

    return tonumber(x), tonumber(y)
end

function window_metatable.__gc(self)
    local window_id = window_instances[self]

    if window_id then
        sdl.close_window(window_id)
    end
end

function window_metatable.__index.render(self, commands)
    return not not sdl.render(get_valid_window_id(self), commands)
end

local dsbox_window = {
    isValid = is_window_valid,
    getId = get_window_id
}

function dsbox_window.new(title, width, height)
    local window_id = sdl.create_window(title, width, height)

    local window = newproxy(true)
    debug.setmetatable(window, window_metatable)

    window_instances[window] = window_id

    return window
end

return dsbox_window
