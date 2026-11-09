//
// Alliance Trusts
//
//   Companion to alliance_trusts.lua (which lets party leaders call Trusts
//   while in an alliance). This C++ half handles the two things Lua can't:
//
//   1. Forming / joining an alliance while Trusts are out.
//      The retail client refuses /acmd while alter egos are in the party
//      ("You are not authorized to perform that action"), so the request
//      never reaches the server. We expose xi.allianceTrusts.formAlliance()
//      to Lua for the !ally command (commands/ally.lua). As a backstop, if an
//      invite response (0x074) does arrive with Trusts present, we run the
//      same logic instead of core's "TrustCannotJoinAlliance" refusal.
//
//   2. Showing Trusts in the alliance party window.
//      The client treats the party table (0x0C8) as the roster, and core only
//      lists Trusts in it for plain parties. When an alliance 0x0C8 is pushed,
//      we append each party's Trusts to its rows in place. On the next zone
//      tick we send their 0x0DD member details (name, HP, job).
//
//   Limitation: both party leaders must be on the same map process for
//   !ally to find each other.
//

#include "map/utils/moduleutils.h"

#include "common/database.h"

#include "map/alliance.h"
#include "map/entities/char_entity.h"
#include "map/entities/trust_entity.h"
#include "map/enums/msg_std.h"
#include "map/enums/party_kind.h"
#include "map/map_session.h"
#include "map/monstrosity.h"
#include "map/packets/basic.h"
#include "map/packets/c2s/0x074_group_solicit_res.h"
#include "map/packets/s2c/0x009_message.h"
#include "map/packets/s2c/0x0c8_group_tbl.h"
#include "map/packets/s2c/0x0dd_group_list.h"
#include "map/party.h"
#include "map/status_effect_container.h"
#include "map/utils/jailutils.h"
#include "map/utils/zoneutils.h"
#include "map/zone.h"

#include "common/lua.h"
#include "map/lua/lua_base_entity.h"

#include <string>

#include <unordered_map>
#include <unordered_set>

namespace
{
    constexpr uint16 PACKET_C2S_GROUP_SOLICIT_RES = 0x074;
    constexpr uint16 PACKET_S2C_GROUP_TBL         = 0x0C8;

    // GP_SERV_COMMAND_GROUP_LIST: 4-byte header + UniqueNo/Hp/Mp/Tp, then GAttr.
    // Bits 0-1 of GAttr are the party number within the alliance.
    constexpr std::size_t GROUP_LIST_GATTR_OFFSET = 0x14;

    // GP_SERV_COMMAND_GROUP_TBL: 4-byte header, Kind at 0x04, then 20 rows of
    // 12 bytes from 0x08 (UniqueNo u32, ActIndex u16, flags u8, pad u8,
    // ZoneNo u16, pad u16).
    constexpr std::size_t GROUP_TBL_KIND_OFFSET = 0x04;
    constexpr std::size_t GROUP_TBL_ROWS_OFFSET = 0x08;
    constexpr std::size_t GROUP_TBL_ROW_SIZE    = 0x0C;
    constexpr std::size_t GROUP_TBL_MAX_ROWS    = 20;

    constexpr auto GroupTblRow(std::size_t row) -> std::size_t
    {
        return GROUP_TBL_ROWS_OFFSET + row * GROUP_TBL_ROW_SIZE;
    }

    // Fail the build (rather than corrupt packets) if upstream changes the layout.
    using GroupTblData = GP_SERV_COMMAND_GROUP_TBL::PacketData;
    static_assert(sizeof(GP_SERV_HEADER) + offsetof(GroupTblData, Kind) == GROUP_TBL_KIND_OFFSET);
    static_assert(sizeof(GP_SERV_HEADER) + offsetof(GroupTblData, GroupTbl) == GROUP_TBL_ROWS_OFFSET);
    static_assert(sizeof(GROUP_TBL) == GROUP_TBL_ROW_SIZE);
    static_assert(sizeof(GroupTblData::GroupTbl) / sizeof(GROUP_TBL) == GROUP_TBL_MAX_ROWS);
    static_assert(offsetof(GROUP_TBL, ActIndex) == 0x04 && offsetof(GROUP_TBL, ZoneNo) == 0x08);
    static_assert(sizeof(GP_SERV_HEADER) + offsetof(GP_SERV_COMMAND_GROUP_LIST::PacketData, GAttr) == GROUP_LIST_GATTR_OFFSET);

    auto AllianceHasTrusts(const CAlliance* PAlliance) -> bool
    {
        if (PAlliance == nullptr)
        {
            return false;
        }

        for (auto* PParty : PAlliance->partyList)
        {
            if (PParty != nullptr && PParty->HasTrusts())
            {
                return true;
            }
        }

        return false;
    }

    // Number of player slots this party occupies in the party window,
    // including members on other map processes.
    auto CountPartyPlayers(CParty* PParty) -> uint8
    {
        const auto rset = db::preparedStmt("SELECT COUNT(*) AS members FROM accounts_parties WHERE partyid = ?", PParty->GetPartyID());
        FOR_DB_SINGLE_RESULT(rset)
        {
            return static_cast<uint8>(rset->get<uint32>("members"));
        }

        return static_cast<uint8>(PParty->members.size());
    }

    // Puts PInvitee's party into PInviter's alliance (creating it if needed),
    // ignoring the "no Trusts" rule. Returns an empty string on success, or
    // the reason it couldn't be done.
    auto FormAllianceIgnoringTrusts(CCharEntity* PInviter, CCharEntity* PInvitee) -> std::string
    {
        if (PInviter == nullptr || PInvitee == nullptr || PInviter == PInvitee)
        {
            return "Invalid target.";
        }

        if (PInviter->PParty == nullptr || PInviter->PParty->GetLeader() != PInviter)
        {
            return fmt::format("{} is not leading a party.", PInviter->getName());
        }

        if (PInvitee->PParty == nullptr || PInvitee->PParty->GetLeader() != PInvitee)
        {
            return fmt::format("{} is not leading a party.", PInvitee->getName());
        }

        if (PInvitee->PParty->m_PAlliance != nullptr)
        {
            return fmt::format("{}'s party is already in an alliance.", PInvitee->getName());
        }

        if (PInviter->isDead() || PInvitee->isDead() || jailutils::InPrison(PInviter) || jailutils::InPrison(PInvitee) ||
            !monstrosity::CanParty(PInviter) || !monstrosity::CanParty(PInvitee))
        {
            return "An alliance cannot be formed right now.";
        }

        if (PInviter->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::LevelSync) ||
            PInvitee->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::LevelSync))
        {
            return "An alliance cannot be formed while Level Sync is active.";
        }

        if (CAlliance* PAlliance = PInviter->PParty->m_PAlliance)
        {
            if (PAlliance->getMainParty() != PInviter->PParty)
            {
                return fmt::format("{} is not the alliance leader.", PInviter->getName());
            }

            if (PAlliance->isFull())
            {
                return "The alliance is full.";
            }

            PAlliance->addParty(PInvitee->PParty);
        }
        else
        {
            PInviter->PParty->m_PAlliance = new CAlliance(PInviter);
            PInviter->PParty->m_PAlliance->addParty(PInvitee->PParty);
        }

        ShowDebug("[ALLIANCE TRUSTS] %s's party joined %s's alliance", PInvitee->getName(), PInviter->getName());
        return "";
    }
} // namespace

class AllianceTrustsModule : public CPPModule
{
    void OnInit() override
    {
        TracyZoneScoped;

        // The FFXI client itself refuses /acmd while alter egos are in the
        // party ("You are not authorized to perform that action"), so the
        // invite never reaches the server. Expose the alliance logic to Lua
        // for the !ally command in commands/ally.lua.
        sol::table api = lua.create_table();
        api.set_function("formAlliance", [](CLuaBaseEntity* inviter, CLuaBaseEntity* invitee) -> std::string
        {
            auto* PInviter = inviter ? dynamic_cast<CCharEntity*>(inviter->GetBaseEntity()) : nullptr;
            auto* PInvitee = invitee ? dynamic_cast<CCharEntity*>(invitee->GetBaseEntity()) : nullptr;

            return FormAllianceIgnoringTrusts(PInviter, PInvitee);
        });
        lua["xi"]["allianceTrusts"] = api;

        ShowInfo("[ALLIANCE TRUSTS] Trusts are allowed in alliances (use !ally to form one with Trusts out).");
    }

    // ---------------------------------------------------------------------
    // 1. Alliance invites while Trusts are out
    // ---------------------------------------------------------------------
    auto OnIncomingPacket(MapSession* session, CCharEntity* PChar, CBasicPacket& packet) -> bool override
    {
        if (packet.getType() != PACKET_C2S_GROUP_SOLICIT_RES)
        {
            return false;
        }

        const auto* typedPacket = packet.as<GP_CLI_COMMAND_GROUP_SOLICIT_RES>();
        if (static_cast<GP_CLI_COMMAND_GROUP_SOLICIT_RES_RES>(typedPacket->Res) != GP_CLI_COMMAND_GROUP_SOLICIT_RES_RES::Accept)
        {
            return false;
        }

        // Only the alliance case: invitee leads a party...
        if (PChar->PParty == nullptr || PChar->PParty->GetLeader() != PChar)
        {
            return false;
        }

        // ...and so does an inviter on this same process.
        CCharEntity* PInviter = zoneutils::GetCharFromWorld(PChar->InvitePending.entity.UniqueNo, PChar->InvitePending.entity.ActIndex);
        if (PInviter == nullptr || PInviter->PParty == nullptr || PInviter->PParty->GetLeader() != PInviter)
        {
            return false;
        }

        // Nothing to bypass: let core handle it.
        if (!PChar->PParty->HasTrusts() && !PInviter->PParty->HasTrusts())
        {
            return false;
        }

        // Keep core's Monstrosity rules (it responds with the right messages).
        if (!monstrosity::CanParty(PChar) || !monstrosity::CanParty(PInviter))
        {
            return false;
        }

        if (!FormAllianceIgnoringTrusts(PInviter, PChar).empty())
        {
            PChar->pushPacket<GP_SERV_COMMAND_MESSAGE>(PChar, 0, 0, MsgStd::CannotBeProcessed);
        }

        PChar->InvitePending.clean();
        return true;
    }

    // ---------------------------------------------------------------------
    // 2. Trusts in the alliance party window
    // ---------------------------------------------------------------------
    void OnPushPacket(CCharEntity* PChar, const std::unique_ptr<CBasicPacket>& packet) override
    {
        if (packet->getType() != PACKET_S2C_GROUP_TBL)
        {
            return;
        }

        if (PChar->PParty == nullptr || !AllianceHasTrusts(PChar->PParty->m_PAlliance))
        {
            return;
        }

        // The client treats 0x0C8 as the authoritative roster: anyone not in
        // it is dropped from the party window and later 0x0DD updates for
        // them are ignored. Core only adds Trusts to it for plain parties,
        // so add each alliance party's Trusts to the table in place.
        if (packet->ref<uint8>(GROUP_TBL_KIND_OFFSET) == static_cast<uint8>(PartyKind::Alliance))
        {
            AddAllianceTrustsToTable(*packet, PChar->PParty->m_PAlliance);
        }

        // Member details (name, HP, job) for those Trusts follow on the next tick.
        pendingRefresh_.insert(PChar->id);
    }

    static void AddAllianceTrustsToTable(CBasicPacket& packet, CAlliance* PAlliance)
    {
        // Find the first empty row.
        std::size_t row = 0;
        while (row < GROUP_TBL_MAX_ROWS && packet.ref<uint32>(GroupTblRow(row)) != 0)
        {
            row++;
        }

        for (auto* PParty : PAlliance->partyList)
        {
            auto* PLeader = PParty ? dynamic_cast<CCharEntity*>(PParty->GetLeader()) : nullptr;
            if (PLeader == nullptr)
            {
                continue;
            }

            const uint8 partyNo = PParty->GetMemberFlags(PLeader) & (PARTY_SECOND | PARTY_THIRD);

            for (auto* PTrust : PLeader->PTrusts)
            {
                if (row >= GROUP_TBL_MAX_ROWS)
                {
                    return;
                }

                const std::size_t base = GroupTblRow(row++);

                packet.ref<uint32>(base + 0x00) = PTrust->id;
                packet.ref<uint16>(base + 0x04) = PTrust->targid;
                packet.ref<uint8>(base + 0x06)  = partyNo; // PartyNo bits; no leader/recruiter flags
                packet.ref<uint8>(base + 0x07)  = 0;
                packet.ref<uint16>(base + 0x08) = static_cast<uint16>(PTrust->getZone());
                packet.ref<uint16>(base + 0x0A) = 0;
            }
        }
    }

    void OnCharZoneOut(CCharEntity* PChar) override
    {
        pendingRefresh_.erase(PChar->id);
    }

    void OnZoneTick(CZone* PZone) override
    {
        if (pendingRefresh_.empty())
        {
            return;
        }

        // Player counts are shared by everyone in the alliance; look each up once per tick.
        std::unordered_map<uint32, uint8> playerCounts;

        PZone->ForEachChar([&](CCharEntity* PChar)
        {
            if (pendingRefresh_.erase(PChar->id) == 0)
            {
                return;
            }

            if (PChar->PParty == nullptr || PChar->PParty->m_PAlliance == nullptr)
            {
                return;
            }

            for (auto* PParty : PChar->PParty->m_PAlliance->partyList)
            {
                if (PParty == nullptr || !PParty->HasTrusts())
                {
                    continue;
                }

                auto* PLeader = dynamic_cast<CCharEntity*>(PParty->GetLeader());
                if (PLeader == nullptr || PLeader->PTrusts.empty())
                {
                    continue;
                }

                const auto [it, inserted] = playerCounts.try_emplace(PParty->GetPartyID(), 0);
                if (inserted)
                {
                    it->second = CountPartyPlayers(PParty);
                }

                const uint8 partyNo = PParty->GetMemberFlags(PLeader) & (PARTY_SECOND | PARTY_THIRD);
                uint8       slot    = it->second;

                for (auto* PTrust : PLeader->PTrusts)
                {
                    auto listPacket = std::make_unique<GP_SERV_COMMAND_GROUP_LIST>(PTrust, slot++);

                    auto& gattr = listPacket->ref<uint8>(GROUP_LIST_GATTR_OFFSET);
                    gattr       = static_cast<uint8>((gattr & ~0x03) | partyNo);

                    PChar->pushPacket(std::move(listPacket));
                }
            }
        });
    }

    std::unordered_set<uint32> pendingRefresh_;
};

REGISTER_CPP_MODULE(AllianceTrustsModule);
