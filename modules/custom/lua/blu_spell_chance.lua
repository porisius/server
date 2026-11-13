-----------------------------------
-- Global Blue Magic Learn Bonus
-----------------------------------

require('modules/module_utils')
require('scripts/globals/player')

-----------------------------------

local m = Module:new('blue_learn_bonus')

local BASE_BLUE_LEARN_CHANCE = 17

m:addOverride('xi.player.onGameIn', function(player, firstLogin, zoning)
    super(player, firstLogin, zoning)

    player:addMod(xi.mod.BLUE_LEARN_CHANCE, BASE_BLUE_LEARN_CHANCE)
end)

return m