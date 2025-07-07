bext.registerHandler_tick(function()
    if not sm.isServerMode() then
        local localPlayer = sm.localPlayer.getPlayer() -- get local player
        local plrlist = sm.player.getAllPlayers() -- get all players
        if localPlayer ~= nil and plrlist ~= nil then -- fix game crsh when try change name but character is nil
            for i, player in pairs(plrlist) do -- execute for all player in list
                if player ~= nil then -- fix
                    local name = player:getName()
                    local id = tostring(player.id)
                    if player.character:isDowned() then
                        player.character:setNameTag("[" .. id .. "]" .. name .. "\n#ff0000Dead")
                    else
                        player.character:setNameTag("[" .. id .. "]" .. name)
                    end
                end
            end
        end
    end
end)
