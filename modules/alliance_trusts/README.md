# Alliance Trusts

Lets players use Trusts while in an alliance. Retail (and LandSandBoat's
default behaviour) blocks this in three places; this module lifts all three
without touching core files.

| Retail restriction | Where it lives in core | Handled by |
| --- | --- | --- |
| Can't cast Trust magic while in an alliance | `xi.trust.canCast` in `scripts/globals/trust.lua` | `lua/alliance_trusts.lua` |
| Can't form or join an alliance while Trusts are out | The **FFXI client** (refuses `/acmd` with alter egos in the party) and `0x074_group_solicit_res.cpp` | `!ally` command (`commands/ally.lua` + `cpp/alliance_trusts.cpp`) |
| Trusts never appear in the alliance party window | `CParty::ReloadParty()` (alliance branch skips Trusts) | `cpp/alliance_trusts.cpp` |

All the other Trust rules still apply: only a party leader can call Trusts,
each party is still capped at 6 (players + Trusts), the Rhapsody key item caps
on Trust count still apply, plus zone, battlefield, enmity, seeking-party and
120-second new-member checks. Players still can't join a *party* that has
Trusts out (that's core `CParty::AddMember`, unchanged), so every party in the
alliance can be a leader with up to 5 Trusts, or a group of players, or a mix
where the leader filled the open slots with Trusts.

## Forming an alliance with Trusts out: `!ally`

The retail client won't send `/acmd add` while your party has alter egos.
It shows "You are not authorized to perform that action" and the server
never sees the request. Use the server command instead:

1. Leader A types `!ally <leader B's name>`. B sees a message asking them to accept.
2. Leader B types `!ally <leader A's name>` within 60 seconds.
3. B's party joins A's alliance, and A leads it.

Both players must be party leaders. If you're solo, call one Trust first to
start a party. Any player can use the command (permission 0). `/acmd` still
works as normal for parties without Trusts.

## Install

1. Copy the `alliance_trusts` folder into your server's `modules/` folder.
2. Add this line to `modules/init.txt`:

   ```
   alliance_trusts
   ```

3. Re-run CMake and rebuild (the C++ half is compiled into the map server).
4. Restart the map server(s).

### Lua-only install

If you don't want to rebuild, list just the Lua file in `init.txt`:

```
alliance_trusts/lua/alliance_trusts.lua
```

Party leaders can then call Trusts after their alliance is formed. `!ally`
needs the C++ half (it tells you so if it's missing), and the Trusts won't show
in the alliance party window (they still fight, heal and follow normally).

## How it works

**Casting (Lua).** The module overrides `xi.trust.canCast`. When the caster is
in an alliance, it calls the original function with a thin proxy around the
caster whose `checkSoloPartyAlliance()` reports "party" instead of "alliance".
Every other method call is forwarded to the real entity, so the module keeps
working if upstream changes the other checks.

**Alliance forming (C++ + Lua).** The C++ half registers
`xi.allianceTrusts.formAlliance(inviter, invitee)`, which runs core's
alliance create/add logic without the Trust refusal. It keeps the other checks:
both must lead a party, the invitee's party can't already be in an alliance, the
alliance can't be full, and neither leader can be dead, jailed, in Monstrosity
or under Level Sync. `!ally` handles the request/accept flow. As a backstop, an
invite response packet (`0x074`) that does arrive with Trusts present goes
through the same function instead of being refused.

**Duplicate Trusts (Lua).** Core only stops you calling a Trust that's already
in *your* party. In an alliance, the override also checks every party in the
alliance. That covers the same Trust, plus excluded variants such as Shantotto
and Shantotto II. It runs after core's checks, so core's messages still take
priority.

**Party window (C++).** The client treats the party table packet (`0x0C8`) as
the full member list, and drops anyone not in it. Core leaves Trusts out of that
table in alliance mode. The module adds each party's Trusts to the table, with
the right party number, before it's sent. On the next zone tick it sends the
`0x0DD` member details (name, HP, job) for those Trusts.

## Limitations

- **Multi-process servers:** `!ally` only finds leaders on the same map
  process. Leaders in zones served by different processes need to meet in a
  zone on the same process.
- **Trust-only parties disband when their Trusts are gone.** If a leader whose
  party was started by calling a Trust zones, or all their Trusts die or are
  released, core disbands that party and it leaves the alliance. Re-call a
  Trust and `!ally` again.
- Trusts still despawn on zoning, as in core.
- Server-side gameplay rules that say "party" (e.g. most Trust AI targeting and
  buffs) still mean the Trust's own party, not the whole alliance.
