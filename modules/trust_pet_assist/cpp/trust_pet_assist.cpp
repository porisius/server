//
// Trust Pet Assist
//
//   When a Beastmaster or Summoner sends their pet in (Fight / Assault /
//   Blood Pact that engages the pet), their Trusts follow the pet into
//   combat and attack the pet's target -- even if the master is NOT
//   engaged. This lets BST/SMN stay in the back line while their Trusts
//   fight alongside the pet.
//
//   How it works:
//     Stock CTrustController only fights while the master is engaged
//     (DoCombatTick disengages the Trust every tick otherwise). This module
//     wraps each Trust's controller in CPetAssistTrustController, which:
//       - engages the Trust on the pet's target when the pet is engaged and
//         the master is not meleeing that target,
//       - runs positioning + gambits itself while the master is NOT engaged,
//       - defers to the stock controller for everything else (so normal
//         Trust behaviour is unchanged when the master is fighting).
//
//   Players can opt out per character with:  !petassist off
//   (sets char var "TrustPetAssistOff" = 1; "!petassist on" clears it)
//

#include "common/logging.h"
#include "common/utils.h"
#include "common/xirand.h"

#include "map/ai/ai_container.h"
#include "map/ai/controllers/player_controller.h"
#include "map/ai/controllers/trust_controller.h"
#include "map/ai/helpers/gambits_container.h"
#include "map/ai/helpers/pathfind.h"
#include "map/ai/states/magic_state.h"
#include "map/ai/states/range_state.h"
#include "data/enums/mob_mod.h"
#include "map/entities/char_entity.h"
#include "map/entities/mob_entity.h"
#include "map/entities/trust_entity.h"
#include "map/utils/charutils.h"
#include "map/utils/moduleutils.h"
#include "map/zone.h"

#include <chrono>
#include <cmath>
#include <memory>

namespace
{

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

// Main jobs whose pets Trusts will follow. Add xi::Job::DRG / PUP / GEO here
// if you want the same behaviour for other pet jobs.
constexpr xi::Job PetAssistJobs[] = { xi::Job::BST, xi::Job::SMN };

// Ignore pet targets further than this from the master (yalms). Stops Trusts
// sprinting across the zone after a pet that was sent at something far away.
constexpr float MaxPetTargetDistanceFromMaster = 30.0f;

// If the master swung at the pet's target within this window, treat them as
// "meleeing" and let stock Trust logic handle engagement.
constexpr auto MasterMeleeWindow = std::chrono::seconds(3);

// Char var that disables the feature for a character when set to non-zero.
constexpr const char* OptOutCharVar = "TrustPetAssistOff";

// Mirrors the (file-local) TRUST_MOVEMENT_TYPE values in trust_controller.cpp
constexpr int16 MoveNoMove    = -1;
constexpr int16 MoveNonCombat = -2;
constexpr int16 MoveMelee     = 0;

// Cornelia has no attack schedule; stock code never engages her either.
constexpr uint16 ModelIdCornelia = 3119;

// ---------------------------------------------------------------------------

auto IsPetAssistJob(const CCharEntity* PChar) -> bool
{
    for (const auto job : PetAssistJobs)
    {
        if (PChar->GetMJob() == job)
        {
            return true;
        }
    }
    return false;
}

// Returns the mob the master's pet is currently fighting, or nullptr when
// pet-assist should not apply.
auto GetPetAssistTarget(CCharEntity* PMaster) -> CBattleEntity*
{
    if (!PMaster || PMaster->isDead() || PMaster->isCharmed || !IsPetAssistJob(PMaster))
    {
        return nullptr;
    }

    if (charutils::GetCharVar(PMaster, OptOutCharVar) != 0)
    {
        return nullptr;
    }

    auto* PPet = PMaster->PPet;
    if (!PPet || PPet->isDead() || !PPet->PAI || !PPet->PAI->IsEngaged())
    {
        return nullptr;
    }

    auto* PTarget = dynamic_cast<CMobEntity*>(PPet->GetBattleTarget());
    if (!PTarget || PTarget->isDead() || PTarget->loc.zone != PMaster->loc.zone)
    {
        return nullptr;
    }

    if (distance(PMaster->loc.p, PTarget->loc.p) > MaxPetTargetDistanceFromMaster)
    {
        return nullptr;
    }

    return PTarget;
}

auto IsMasterMeleeing(CCharEntity* PMaster, const CBattleEntity* PTarget) -> bool
{
    if (!PMaster->PAI->IsEngaged() || PMaster->GetBattleTarget() != PTarget)
    {
        return false;
    }

    auto* PController = dynamic_cast<CPlayerController*>(PMaster->PAI->GetController());
    return PController && PController->getLastAttackTime() > timer::now() - MasterMeleeWindow;
}

} // namespace

// ---------------------------------------------------------------------------
// Controller
// ---------------------------------------------------------------------------

class CPetAssistTrustController : public CTrustController
{
public:
    CPetAssistTrustController(CCharEntity* PChar, CTrustEntity* PTrust)
    : CTrustController(PChar, PTrust)
    {
    }

    auto Tick(timer::time_point tick) -> Task<void> override
    {
        m_Tick = tick;

        auto* PTrust  = static_cast<CTrustEntity*>(POwner);
        auto* PMaster = dynamic_cast<CCharEntity*>(PTrust->PMaster);

        auto* PPetTarget = (PMaster && !PTrust->isDead()) ? GetPetAssistTarget(PMaster) : nullptr;

        const int16 moveType = PTrust->getMobMod(xi::MobMod::TrustDistance);

        if (!PPetTarget ||
            moveType == MoveNonCombat ||
            PTrust->GetModelId() == ModelIdCornelia ||
            IsMasterMeleeing(PMaster, PPetTarget))
        {
            // Stock behaviour. If the Trust was pet-assisting and the pet has
            // since disengaged, stock DoCombatTick will disengage it.
            co_await CTrustController::Tick(tick);
            co_return;
        }

        // Get the Trust onto the pet's target.
        if (!PTrust->PAI->IsEngaged())
        {
            PTrust->PAI->Internal_Engage(PPetTarget->entityId());
        }
        else if (PTrust->GetBattleTarget() != PPetTarget)
        {
            PTrust->PAI->Internal_ChangeTarget(PPetTarget->entityId());
        }

        // Master engaged (on this target, but not swinging): stock combat
        // logic works fine now that the Trust is engaged.
        if (PMaster->PAI->IsEngaged())
        {
            co_await CTrustController::Tick(tick);
            co_return;
        }

        // Master is hanging back un-engaged: drive combat ourselves, since
        // stock DoCombatTick would disengage the Trust immediately.
        co_await DoPetAssistCombatTick(tick, PMaster, PPetTarget);
        co_return;
    }

private:
    timer::time_point m_lastReposition{};

    auto DoPetAssistCombatTick(timer::time_point tick, CCharEntity* PMaster, CBattleEntity* PTarget) -> Task<void>
    {
        auto* PTrust = static_cast<CTrustEntity*>(POwner);

        if (!PTrust->PAI->IsEngaged())
        {
            co_return; // Engage queued; next tick.
        }

        setTarget(PTarget);

        // Don't run around mid-cast / mid-shot.
        if (PTrust->PAI->IsCurrentState<CMagicState>() || PTrust->PAI->IsCurrentState<CRangeState>())
        {
            co_return;
        }

        if (PTrust->PAI->CanFollowPath() && PTrust->GetSpeed() > 0)
        {
            const float distToTarget = distance(PTrust->loc.p, PTarget->loc.p);

            if (distToTarget > WarpDistance)
            {
                PTrust->PAI->PathFind->WarpTo(PTarget->loc.p);
            }

            PTrust->PAI->PathFind->LookAt(PTarget->loc.p);

            const int16 moveType = PTrust->getMobMod(xi::MobMod::TrustDistance);
            if (moveType == MoveMelee)
            {
                std::unique_ptr<CBasicPacket> err;
                if (!PTrust->CanAttack(PTarget, err) && distToTarget > RoamDistance)
                {
                    if (distToTarget < RoamDistance * 3.0f &&
                        PTrust->PAI->PathFind->PathAround(PTarget->loc.p, RoamDistance, PATHFLAG_RUN))
                    {
                        PTrust->PAI->PathFind->FollowPath(m_Tick);
                    }
                    else
                    {
                        PTrust->PAI->PathFind->StepTo(PTarget->loc.p, true);
                    }
                }
            }
            else
            {
                // NO_MOVE trusts only close in if out of casting range;
                // ranged trusts hold their preferred distance.
                const float wanted = (moveType == MoveNoMove) ? 9.0f : static_cast<float>(moveType);
                if (moveType != MoveNoMove || distToTarget > CastingDistance)
                {
                    HoldDistance(PTarget, wanted);
                }
            }

            if (!PTrust->PAI->PathFind->IsFollowingPath())
            {
                Declump(PMaster, PTarget);
            }

            PTrust->PAI->PathFind->FollowPath(m_Tick);
        }

        co_await m_GambitsContainer->Tick(tick);

        PTrust->PAI->EventHandler.triggerListener("COMBAT_TICK", PTrust, PMaster, PTarget);
        co_return;
    }

    // Simplified CTrustController::PathOutToDistance (that one is private).
    void HoldDistance(const CBattleEntity* PTarget, float wanted)
    {
        const float current = distance(POwner->loc.p, PTarget->loc.p);
        if (current >= wanted - 2.5f && current <= wanted + 2.5f)
        {
            FaceTarget(PTarget->entityId());
            return;
        }

        if (POwner->PAI->PathFind->IsFollowingPath() || m_Tick - m_lastReposition < std::chrono::seconds(3))
        {
            return;
        }

        m_lastReposition = m_Tick;

        for (int attempt = 0; attempt < 5; ++attempt)
        {
            const float      angle = rotationToRadian(static_cast<uint8>(xirand::GetRandomNumber(256)));
            const position_t pos   = {
                PTarget->loc.p.x - cosf(angle) * wanted,
                PTarget->loc.p.y,
                PTarget->loc.p.z + sinf(angle) * wanted,
                0,
                0,
            };

            if (POwner->PAI->PathFind->ValidPosition(pos) && POwner->CanSeeTarget(pos))
            {
                POwner->PAI->PathFind->PathTo(pos, PATHFLAG_RUN);
                return;
            }
        }
    }

    // Copy of CTrustController::Declump (private there).
    void Declump(const CCharEntity* PMaster, const CBattleEntity* PTarget) const
    {
        const uint8 partyPos = GetPartyPosition();
        for (const auto* POther : PMaster->PTrusts)
        {
            if (POther == POwner || POther->PAI->PathFind->IsFollowingPath() || distance(POther->loc.p, POwner->loc.p) >= 1.5f)
            {
                continue;
            }

            const float offset = xirand::GetRandomNumber(0.0f, 1.5f) * ((partyPos % 2) ? 1.0f : -1.0f);
            const auto  newPos = sidestepPosition(POwner->loc.p, PTarget->loc.p, offset);
            if (POwner->PAI->PathFind->ValidPosition(newPos))
            {
                POwner->PAI->PathFind->PathTo(newPos, PATHFLAG_RUN);
            }
            break;
        }
    }
};

// ---------------------------------------------------------------------------
// Module
// ---------------------------------------------------------------------------

class TrustPetAssistModule : public CPPModule
{
    void OnInit() override
    {
        ShowInfo("[trust_pet_assist] Trusts will follow BST/SMN pets into combat.");
    }

    // Runs after every entity in the zone has ticked, so no controller
    // coroutine is in flight when we swap controllers here.
    void OnZoneTick(CZone* PZone) override
    {
        PZone->ForEachChar(
            [](CCharEntity* PChar)
            {
                for (auto* PTrust : PChar->PTrusts)
                {
                    WrapController(PChar, PTrust);
                }
            });
    }

    static void WrapController(CCharEntity* PChar, CTrustEntity* PTrust)
    {
        if (!PTrust || !PTrust->PAI || PTrust->isDead() || PTrust->status == xi::Status::Disappear)
        {
            return;
        }

        auto* PCurrent = PTrust->PAI->GetController();
        if (dynamic_cast<CPetAssistTrustController*>(PCurrent))
        {
            return; // Already wrapped.
        }

        auto* POld = dynamic_cast<CTrustController*>(PCurrent);
        if (!POld || PTrust->PAI->IsEngaged())
        {
            return; // Only swap a stock controller, and only while idle.
        }

        // Keep the gambits/skills the Trust's Lua onSpawn already set up.
        auto controller                = std::make_unique<CPetAssistTrustController>(PChar, PTrust);
        controller->m_GambitsContainer = std::move(POld->m_GambitsContainer);

        // ~CTrustController() resets PathFind and marks the Trust as
        // disappearing; snapshot what it clobbers and put it back.
        const auto status     = PTrust->status;
        const auto allegiance = PTrust->allegiance;

        PTrust->PAI->SetController(std::move(controller)); // destroys POld

        PTrust->PAI->PathFind = std::make_unique<CPathFind>(PTrust);
        PTrust->status        = status;
        PTrust->allegiance    = allegiance;
    }
};

REGISTER_CPP_MODULE(TrustPetAssistModule);
