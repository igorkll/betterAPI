local better, directory = ...

local BetterNetwork = require("BetterNetwork")
BetterNetwork.init()

bext.registerHandler_exit(function ()
    BetterNetwork.deinit()
end)

local function networkObject(object, objectType)
    if type(object) == "number" then
        return nil, "error: " .. object
    end
    return setmetatable({objectType = objectType, object = object}, {__index = better.network})
end

local function formatResult(ret)
    if type(ret) == "number" then
        return nil, "error: " .. ret
    end
    return ret
end

local function checkObject(object, objectType)
    if object.destroyed then error("object destroyed", 3) end
    if object.objectType ~= objectType then error("object is not \"" .. objectType .. "\"", 3) end
end

local function urlEncode(str)
    if str then
        return str:gsub("([^%w])", function(c)
            return string.format("%%%02X", string.byte(c))
        end)
    else
        return ""
    end
end

local function tableToQueryString(params)
    local queryString = {}
    
    for key, value in pairs(params) do
        local encodedKey = urlEncode(tostring(key))
        local encodedValue = urlEncode(tostring(value))
        
        table.insert(queryString, encodedKey .. "=" .. encodedValue)
    end
    
    return table.concat(queryString, "&")
end

better.network = {
    newConnection = function(url, port)
        bext.checkArg(1, url, "string")
        bext.checkArg(2, port, "number", "nil")
        return networkObject(BetterNetwork.newConnection(url, port or 80), "connection")
    end,
    closeConnection = function(connection)
        checkObject(connection, "connection")
        BetterNetwork.closeConnection(connection.object)
        connection.destroyed = true
    end,

    newRequest = function(connection, requestType, requestPath)
        checkObject(connection, "connection")
        bext.checkArg(2, requestType, "string")
        bext.checkArg(3, requestPath, "string", "nil")
        return networkObject(BetterNetwork.newRequest(connection.object, requestType, requestPath or "/"), "request")
    end,
    sendRequest = function(request, headers, args)
        checkObject(request, "request")
        bext.checkArg(2, headers, "string", "nil")
        bext.checkArg(3, args, "string", "table", "nil")
        if type(args) == "table" then
            args = tableToQueryString(args)
        end
        return BetterNetwork.sendRequest(request.object, headers or "", args or "")
    end,
    getResult = function(request, headers)
        checkObject(request, "request")
        return formatResult(BetterNetwork.getResult(request.object))
    end,
    closeRequest = function(request)
        checkObject(request, "request")
        BetterNetwork.closeRequest(request.object)
        request.destroyed = true
    end
}