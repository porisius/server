# trust_pet_assist

Lets Beastmasters and Summoners stay in the back line: when your pet is sent in
(BST **Fight**, SMN **Assault**, or anything else that engages the pet), your
Trusts engage the pet's target and fight alongside it — even if you never
draw your weapon.

## Behaviour

| Situation | What Trusts do |
|---|---|
| Pet engaged, you are **not** engaged | Engage the pet's target, position by their normal role (melee / mid / long range / no-move), run their usual gambits (heals, buffs, WS, spells). |
| Pet engaged, you are engaged on the same mob but **not swinging** | Engage the pet's target, then normal Trust combat logic takes over. |
| You are meleeing (swung in the last 3s) | Stock LandSandBoat behaviour, unchanged. |
| Pet disengages, dies, or is released | Trusts disengage and return to you as normal. |
| Non-combat Trusts and Cornelia | Never pulled in (same as stock). |

Only main-job BST and SMN qualify. The pet's target must be within 30 yalms of
you, so Trusts won't sprint across a zone.

Players can opt out per character:

```
!petassist off
!petassist on
!petassist        (shows current setting)
```

## Install

1. Copy the `trust_pet_assist` folder into `modules/`.
2. Add a line to `modules/init.txt`:
   ```
   trust_pet_assist
   ```
3. Re-run CMake and rebuild the server (the C++ part is compiled in).

## Configuration

Constants at the top of `cpp/trust_pet_assist.cpp`:

- `PetAssistJobs` — add `xi::Job::DRG`, `xi::Job::PUP`, `xi::Job::GEO` etc. to extend it to other pet jobs.
- `MaxPetTargetDistanceFromMaster` — default 30 yalms.
- `MasterMeleeWindow` — how recently you must have swung to count as "meleeing" (default 3s).

## How it works (why it's C++)

Stock `CTrustController` only fights while the master is engaged: its combat
tick disengages the Trust every tick that the master isn't. That logic can't be
changed from Lua, so this module subclasses `CTrustController` as
`CPetAssistTrustController`, which handles the pet-assist case and defers to
the stock controller for everything else.

Each zone tick (after all entities have ticked), any idle Trust still on the
stock controller is swapped to the subclass. Its gambits/skills container is
moved across intact, and the PathFind/status the stock destructor clears are
restored, so Trust Lua scripts and `addGambit` setups keep working.

## Notes

- Built and compile-checked against LandSandBoat `base` (October 2026) with the
  project's `-Werror` warning set. Not yet play-tested in-game — try it with a
  couple of Trust types (melee, healer, ranged) on a test character first.
- If upstream changes `CTrustController`'s destructor or constructor, re-check
  `WrapController()` in the `.cpp`.
