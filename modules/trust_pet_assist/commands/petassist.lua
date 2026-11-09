-----------------------------------
-- func: petassist
-- desc: Toggle whether your Trusts follow your BST/SMN pet into combat.
--       Usage: !petassist [on|off]   (no argument shows current setting)
-----------------------------------
require('modules/module_utils')
-----------------------------------
---@type TCommand
local commandObj = {}

commandObj.cmdprops =
{
    permission = 0,
    parameters = 's',
}

local optOutVar = 'TrustPetAssistOff'

local function report(player, msg)
    player:printToPlayer(msg, xi.msg.channel.SYSTEM_3, '')
end

commandObj.onTrigger = function(player, arg)
    arg = arg and string.lower(arg) or ''

    if arg == 'on' then
        player:setCharVar(optOutVar, 0)
    elseif arg == 'off' then
        player:setCharVar(optOutVar, 1)
    elseif arg ~= '' then
        report(player, '!petassist [on|off]')
        return
    end

    local enabled = player:getCharVar(optOutVar) == 0
    report(player, string.format('Trust pet assist is %s.', enabled and 'ON' or 'OFF'))
end

xi.module.registerCommand('petassist', commandObj)
