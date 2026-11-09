-----------------------------------
-- Alliance Trusts
--
-- Allows party leaders to call Trusts while their party is in an alliance.
--
-- Retail blocks Trust magic for anyone in an alliance. This override removes
-- only that one restriction: every other rule in xi.trust.canCast still
-- applies (leader-only casting, 6 members per party, the RoV key item caps,
-- zone and battlefield restrictions, enmity, the 120s new-member delay, etc).
--
-- Rather than copying canCast (and drifting from upstream), the caster is
-- handed to the original function through a thin proxy that reports
-- "party" instead of "alliance" for checkSoloPartyAlliance(). Every other
-- method call is forwarded to the real entity unchanged.
--
-- Pair with the alliance_trusts.cpp module so parties that already have
-- Trusts out can form or join an alliance, and so the Trusts show up in the
-- alliance party window.
-----------------------------------
require('modules/module_utils')
require('scripts/globals/trust')
-----------------------------------
local m = Module:new('alliance_trusts')

local IN_ALLIANCE = 2
local IN_PARTY    = 1

-- Wraps an entity so checkSoloPartyAlliance() never reports an alliance.
local function withoutAllianceFlag(entity)
    local proxy = {}

    return setmetatable(proxy, {
        __index = function(_, key)
            if key == 'checkSoloPartyAlliance' then
                return function()
                    return IN_PARTY
                end
            end

            local value = entity[key]
            if type(value) ~= 'function' then
                return value
            end

            -- Swap the proxy back for the real entity on method calls,
            -- since the C++ bindings only accept the real userdata.
            return function(self, ...)
                if self == proxy then
                    return value(entity, ...)
                end

                return value(self, ...)
            end
        end,
    })
end

-- Trust IDs already called anywhere in the caster's alliance.
-- Core's duplicate check only looks at the caster's own party.
local function allianceTrustIds(caster)
    local ids = {}

    for _, member in ipairs(caster:getAlliance()) do
        if member:isPC() then
            for _, partyMember in ipairs(member:getPartyWithTrusts()) do
                if partyMember:getObjType() == xi.objType.TRUST then
                    ids[partyMember:getTrustID()] = true
                end
            end
        end
    end

    return ids
end

-- Same rule as core: the spell itself, plus any variants it excludes
-- (e.g. Shantotto vs Shantotto II).
local function alreadyCalledInAlliance(caster, spell, notAllowedTrustIds)
    local called = allianceTrustIds(caster)

    if called[spell:getID()] then
        return true
    end

    if type(notAllowedTrustIds) == 'number' then
        return called[notAllowedTrustIds] == true
    elseif type(notAllowedTrustIds) == 'table' then
        for _, id in pairs(notAllowedTrustIds) do
            if called[id] then
                return true
            end
        end
    end

    return false
end

m:addOverride('xi.trust.canCast', function(caster, spell, notAllowedTrustIds)
    if caster:checkSoloPartyAlliance() ~= IN_ALLIANCE then
        return super(caster, spell, notAllowedTrustIds)
    end

    -- Run core's checks first so its messages (disabled, not leader, zone,
    -- etc.) still win; only add the alliance-wide duplicate check on top.
    local result = super(withoutAllianceFlag(caster), spell, notAllowedTrustIds)
    if result ~= 0 then
        return result
    end

    if alreadyCalledInAlliance(caster, spell, notAllowedTrustIds) then
        caster:messageSystem(xi.msg.system.TRUST_ALREADY_CALLED)
        return -1
    end

    return 0
end)
