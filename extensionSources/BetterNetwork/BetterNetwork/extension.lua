local better, directory = ...

local BetterNetwork = require("BetterNetwork")
BetterNetwork.init()

bext.registerHandler_exit(function ()
    BetterNetwork.deinit()
end)

better.network = {

}