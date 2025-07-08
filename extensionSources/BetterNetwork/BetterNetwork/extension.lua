local better, directory = ...

local BetterNetwork = require("BetterNetwork")
BetterNetwork.init()

bext.registerHandler_exit(function ()
    BetterNetwork.deinit()
end)

local function networkObject(object, objectType)
    return setmetatable({objectType = objectType}, {__index = function(_, key)
        if better.network[key] then
            return function (_, ...)
                return better.network[key](object, ...)
            end
        end
    end})
end

local function checkObjectType(object, objectType)
    if object.objectType ~= objectType then error("object is not \"" .. objectType .. "\"", 3) end
end

better.network = {
    newConnection = function(url, port)
        bext.checkArg(1, url, "string")
        bext.checkArg(2, port, "number", "nil")
        return networkObject(BetterNetwork.newConnection(url, port or 80), "connection")
    end,
    closeConnection = function(connection)
        BetterNetwork.closeConnection(connection)
    end,

    newRequest = function(connection, headers)
        checkObjectType(connection, "connection")
        bext.checkArg(2, headers, "number", "nil")
        return networkObject(BetterNetwork.newRequest(connection, headers), "request")
    end,
    sendRequest = function(request, headers)
        checkObjectType(request, "request")
        bext.checkArg(2, headers, "string", "nil")
        return networkObject(BetterNetwork.sendRequest(request, headers or ""))
    end,
    getResult = function(request, headers)
        checkObjectType(request, "request")
        return networkObject(BetterNetwork.getResult(request))
    end,
    closeRequest = function(request)
        checkObjectType(request, "request")
        return networkObject(BetterNetwork.closeRequest(request))
    end
}