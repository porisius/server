-----------------------------------
-- func: ally <player>
-- desc: Form an alliance with another party leader, even with Trusts out.
--
-- The FFXI client refuses /acmd while alter egos are in your party
-- ("You are not authorized to perform that action"), so the request never
-- reaches the server. This command does the same thing server-side.
--
--   Leader A:  !ally B    -> B is told A wants to form an alliance
--   Leader B:  !ally A    -> B's party joins A's alliance (A leads it)
--
-- Requests expire after 60 seconds. Needs the alliance_trusts C++ module.
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

local REQUEST_TIMEOUT = 60 -- seconds

-- pending[requesterId] = { targetId = id, time = os.time() }
local pending = {}

local function say(player, msg)
    player:printToPlayer(msg, xi.msg.channel.SYSTEM_3, '')
end

local function isPartyLeader(player)
    local leader = player:getPartyLeader()
    return leader ~= nil and leader:getID() == player:getID()
end

local function hasLiveRequest(fromId, toId)
    local request = pending[fromId]
    return request ~= nil and
        request.targetId == toId and
        os.time() - request.time <= REQUEST_TIMEOUT
end

commandObj.onTrigger = function(player, targetName)
    if not xi.allianceTrusts or not xi.allianceTrusts.formAlliance then
        say(player, 'Alliance Trusts: the C++ part of this module is not built into the server.')
        return
    end

    if not targetName or targetName == '' then
        say(player, 'Usage: !ally <party leader name>')
        return
    end

    local target = GetPlayerByName(targetName)
    if not target or target:getID() == player:getID() then
        say(player, string.format('Player "%s" was not found.', targetName))
        return
    end

    if not isPartyLeader(player) then
        say(player, 'You must be leading a party. (Call a Trust to start one.)')
        return
    end

    if not isPartyLeader(target) then
        say(player, string.format('%s is not leading a party.', target:getName()))
        return
    end

    local playerId = player:getID()
    local targetId = target:getID()

    -- The other leader already asked us: accept, joining THEIR alliance.
    if hasLiveRequest(targetId, playerId) then
        pending[targetId] = nil

        local err = xi.allianceTrusts.formAlliance(target, player)
        if err ~= '' then
            say(player, err)
            say(target, err)
            return
        end

        say(player, string.format('You joined %s\'s alliance.', target:getName()))
        say(target, string.format('%s\'s party joined your alliance.', player:getName()))
        return
    end

    -- Otherwise this is a new request from us.
    pending[playerId] = { targetId = targetId, time = os.time() }

    say(player, string.format('Alliance request sent to %s.', target:getName()))
    say(target, string.format('%s wants to form an alliance. Type "!ally %s" within %d seconds to accept.',
        player:getName(), player:getName(), REQUEST_TIMEOUT))
end

xi.module.registerCommand('ally', commandObj)
