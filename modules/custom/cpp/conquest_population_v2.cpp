/*
===========================================================================

  Conquest Population Simulation
  LandSandBoat custom C++ module

  Reviewed against LandSandBoat base on 2026-10-19, including:
    #11413 - Conquest nation influence based on EXP instead of CP
    #11542 - Per-nation influence cap / overflow behavior
    #11685 - Expeditionary Force weighted mob-kill counter changes
    #11759 - Beastmen homepoint weights changed to 10 / 25 / 50 tiers
    #11819 - Updated Beastmen influence formula

  Purpose:
    Simulate a small background retail-like Conquest population on small
    private servers without replacing or modifying LSB's Conquest system.

  Behavior:
    - Periodically adds small amounts of STORED regional influence for:
        * San d'Oria
        * Bastok
        * Windurst
    - Also adds simulated mob_kills counter units and player_homepoints.
    - Does NOT calculate or store Beastmen influence itself.
    - LSB's normal Conquest code derives Beastmen influence from the current
      nation total, mob_kills, player_homepoints, and the region's native
      homepointKillWeight.
    - Regions can be inactive for an entire Conquest week so the whole map
      does not automatically become claimed.
    - Real player activity is additive and can override the simulation.
    - Uses server_variables as a cross-xi_map lease so only one map process
      performs each simulation tick.

  IMPORTANT ABOUT NATION INFLUENCE:
    These additions are already in STORED influence units. They intentionally
    do not pass through ConquestSystem::updateInfluencePoints(), which applies
    the native 1x/2x/3x regional ranking multiplier and then divides raw input
    by 10. This module is adding ambient final influence, not pretending that
    a specific amount of player EXP was earned.

  Retail/current-LSB Conquest week boundary:
    Monday 00:00 JST
    Sunday 15:00 UTC
    Sunday morning in the United States

===========================================================================
*/

#include "map/utils/moduleutils.h"

#include "common/database.h"
#include "common/logging.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <random>
#include <string>

namespace
{
    // =====================================================================
    // Module-local configuration
    // =====================================================================

    // One simulated population update every two Earth hours.
    constexpr int32 kTickIntervalSeconds = 2 * 60 * 60;

    // On first install (or after deleting the timer variable), perform the
    // first simulation tick after one minute for convenient testing.
    constexpr int32 kInitialDelaySeconds = 60;

    // If a map process claims a tick and crashes before scheduling the next
    // one, another map process may retry after this lease expires.
    constexpr int32 kLeaseSeconds = 5 * 60;

    // Do not write simulated activity immediately around the real weekly
    // tally/reset. LSB performs the tally at Monday 00:00 JST.
    constexpr int32 kTallyGuardSeconds = 5 * 60;

    // Avoid touching server_variables on every time-server tick.
    constexpr auto kLeaseCheckInterval = std::chrono::seconds(30);

    // Base STORED nation influence before regional activity/proximity
    // weighting. This is deliberately small so actual player activity can
    // dominate a region with a modest amount of play.
    constexpr int32 kInfluenceMin = 5;
    constexpr int32 kInfluenceMax = 12;

    // Base simulated mob_kills counter units before regional activity
    // weighting. Treat these as Conquest counter units, not a promise that
    // each unit represents exactly one literal retail mob kill.
    constexpr int32 kMobCounterMin = 25;
    constexpr int32 kMobCounterMax = 60;

    // Current native nation cap from ConquestSystem::updateInfluencePoints().
    // Packet construction multiplies influence by 100.
    constexpr int32 kInfluenceCap = INT32_MAX / 100;

    // Leave true while tuning. At a two-hour interval this is still modest.
    constexpr bool kVerboseLogging = true;

    // Existing server_variables is used only as a module-owned timer/lease.
    constexpr const char* kNextTickVariable = "[ConquestPopulation]NextTick";

    // =====================================================================
    // Region configuration
    // =====================================================================
    //
    // sandoria / bastok / windurst:
    //   Relative nation presence/proximity, 0-100. These are OUR population
    //   model weights, not LSB's Beastmen homepointKillWeight.
    //
    // activity:
    //   Overall simulated population density, 0-100.
    //
    // weeklyChance:
    //   Chance that the region receives ANY simulated activity during a
    //   Conquest week. If inactive, this module contributes nothing there
    //   for the entire week, allowing regions to remain Neutral unless real
    //   players visit.
    //
    // homepointChance:
    //   Chance per simulation tick to add one player_homepoints event.
    //
    //   Current LSB Beastmen model (PR #11819):
    //
    //     Beastmen% = Homepoints /
    //                 (1 + Homepoints + MobKills / Weight)
    //
    //     BeastmenInfluence =
    //         Weight * Homepoints * NationTotal / (Weight + MobKills)
    //
    //   Current native weights (PR #11759):
    //     General         = 10
    //     Semi-stronghold = 25
    //     Stronghold      = 50
    //
    //   The original Orc/Quadav/Yagudo stronghold regions get a higher
    //   simulated homepoint rate so they retain meaningful Beastmen pressure.
    //
    // Region IDs correspond to scripts/enum/region.lua IDs 0-18.
    // =====================================================================

    struct RegionConfig
    {
        uint8       id;
        const char* name;

        uint8 sandoria;
        uint8 bastok;
        uint8 windurst;

        uint8 activity;
        uint8 weeklyChance;
        uint8 homepointChance;
    };

    constexpr std::array<RegionConfig, 19> kRegions =
    {{
        // id  Region                       San  Bas  Win  Act  Week  HP%
        // Native Beastmen tier: General (10)
        {  0, "Ronfaure",                   100,  10,  10, 100, 100,   4 },
        {  1, "Zulkheim",                    70,  70,  25,  90,  95,   7 },

        // Native Beastmen tier: Stronghold (50)
        // Original Orc stronghold pressure.
        {  2, "Norvallen",                   85,  15,  10,  70,  95,  50 },

        // Native Beastmen tier: General (10)
        {  3, "Gustaberg",                   10, 100,  10, 100, 100,   4 },

        // Native Beastmen tier: Stronghold (50)
        // Original Quadav stronghold pressure.
        {  4, "Derfland",                    15,  90,  15,  70,  95,  50 },

        // Native Beastmen tier: General (10)
        {  5, "Sarutabaruta",                10,  10, 100, 100, 100,   4 },
        {  6, "Kolshushu",                   20,  25,  80,  80,  85,   8 },

        // Native Beastmen tier: Stronghold (50)
        // Original Yagudo stronghold pressure.
        {  7, "Aragoneu",                    10,  15,  90,  70,  95,  50 },

        // Fauregandi is Semi-stronghold (25); Valdeaunia is Stronghold (50).
        {  8, "Fauregandi",                  55,  35,  25,  60,  70,  20 },
        {  9, "Valdeaunia",                  45,  35,  25,  55,  65,  22 },

        // Native Beastmen tier: Semi-stronghold (25)
        { 10, "Qufim Island",                45,  45,  45,  70,  85,  10 },
        { 11, "Li'Telor",                    20,  25,  60,  60,  70,  10 },
        { 12, "Kuzotz",                      20,  65,  25,  60,  70,  10 },
        { 13, "Vollbow",                     25,  60,  20,  50,  55,  10 },
        { 14, "Elshimo Lowlands",            20,  30,  55,  60,  70,  10 },
        { 15, "Elshimo Uplands",             15,  25,  50,  50,  60,  12 },
        { 16, "Tu'Lia",                      30,  30,  30,  30,  30,   8 },

        // Movalpolos is now a native Stronghold (50), not the older much
        // larger assumed value. Keep its simulated HP rate moderate because
        // the requested emphasis is the original three beastmen territories.
        { 17, "Movalpolos",                  20,  70,  20,  40,  45,  15 },

        // Native Beastmen tier: Semi-stronghold (25)
        { 18, "Tavnazian Archipelago",       55,  20,  20,  45,  50,  10 },
    }};

    // =====================================================================
    // Conquest week calculation
    // =====================================================================
    //
    // Retail / current LSB tally:
    //   Monday 00:00 JST
    // = Sunday 15:00 UTC
    //
    // Unix epoch:
    //   Thursday 1970-01-01 00:00 UTC
    //
    // Thursday 00:00 -> Sunday 15:00 = 87 hours = 313200 seconds.
    //
    // This is used only to produce a stable per-week activity decision and
    // to avoid racing the real tally. It does not trigger or replace LSB's
    // actual weekly tally.
    // =====================================================================

    constexpr int64 kWeekSeconds         = 7 * 24 * 60 * 60;
    constexpr int64 kConquestWeekAnchor = 313200;

    int64 positiveModulo(int64 value, int64 divisor)
    {
        const int64 result = value % divisor;
        return result >= 0 ? result : result + divisor;
    }

    int64 getConquestWeek(int64 epoch)
    {
        return (epoch - kConquestWeekAnchor) / kWeekSeconds;
    }

    bool isNearWeeklyTally(int64 epoch)
    {
        const int64 secondsSinceBoundary =
            positiveModulo(epoch - kConquestWeekAnchor, kWeekSeconds);

        return secondsSinceBoundary < kTallyGuardSeconds ||
               secondsSinceBoundary > (kWeekSeconds - kTallyGuardSeconds);
    }

    // Deterministic SplitMix64-style mixer. Weekly region activation remains
    // stable across xi_map restarts without a second persistence table.
    uint64 mix64(uint64 value)
    {
        value += 0x9E3779B97F4A7C15ULL;
        value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
        return value ^ (value >> 31);
    }

    bool regionActiveThisWeek(const RegionConfig& region, int64 conquestWeek)
    {
        const uint64 seed =
            static_cast<uint64>(conquestWeek) ^
            (static_cast<uint64>(region.id) * 0x9E3779B97F4A7C15ULL);

        const uint8 roll = static_cast<uint8>((mix64(seed) % 100) + 1);
        return roll <= region.weeklyChance;
    }
}

// =========================================================================
// Module
// =========================================================================

class ConquestPopulationModule : public CPPModule
{
public:
    void OnInit() override
    {
        const int32 now = static_cast<int32>(std::time(nullptr));

        // INSERT IGNORE preserves the existing schedule across restarts.
        const auto result = db::preparedStmt(
            "INSERT IGNORE INTO server_variables (name, value, expiry) "
            "VALUES (?, ?, 0)",
            std::string(kNextTickVariable),
            now + kInitialDelaySeconds);

        if (!result)
        {
            ShowError("[ConquestPopulation] Failed to initialize module timer.");
            return;
        }

        ShowInfo("[ConquestPopulation] Conquest population simulation loaded (2026-10-19 rules).");
    }

    void OnTimeServerTick() override
    {
        const auto steadyNow = std::chrono::steady_clock::now();

        if (steadyNow < nextLeaseCheck_)
        {
            return;
        }

        nextLeaseCheck_ = steadyNow + kLeaseCheckInterval;

        const int32 now = static_cast<int32>(std::time(nullptr));

        // Every xi_map process loads CPP modules. The atomic database claim
        // ensures only one process performs a particular simulation tick.
        if (!claimTick(now))
        {
            return;
        }

        // Avoid putting a simulated tick on the wrong side of the weekly
        // tally/reset if map/world execute at nearly the same time.
        if (isNearWeeklyTally(now))
        {
            if (kVerboseLogging)
            {
                ShowInfo("[ConquestPopulation] Weekly tally guard active; delaying simulation tick.");
            }

            scheduleTickAt(now + (kTallyGuardSeconds * 2));
            return;
        }

        simulateTick(now);
        scheduleTickAt(now + kTickIntervalSeconds);
    }

private:
    std::mt19937 rng_{ std::random_device{}() };
    std::chrono::steady_clock::time_point nextLeaseCheck_{};

    // ---------------------------------------------------------------------
    // Cross-xi_map timer/lease
    // ---------------------------------------------------------------------

    bool claimTick(int32 now)
    {
        const int32 leaseUntil = now + kLeaseSeconds;

        // Only one map process can change a due timestamp to the lease value.
        const auto result = db::preparedStmt(
            "UPDATE server_variables "
            "SET value = ? "
            "WHERE name = ? AND value <= ?",
            leaseUntil,
            std::string(kNextTickVariable),
            now);

        if (!result)
        {
            ShowError("[ConquestPopulation] Failed to claim simulation tick.");
            return false;
        }

        return result->rowsAffected() == 1;
    }

    void scheduleTickAt(int32 nextTick)
    {
        const auto result = db::preparedStmt(
            "UPDATE server_variables "
            "SET value = ? "
            "WHERE name = ?",
            nextTick,
            std::string(kNextTickVariable));

        if (!result)
        {
            ShowError("[ConquestPopulation] Failed to schedule next simulation tick.");
        }
    }

    // ---------------------------------------------------------------------
    // Random generation
    // ---------------------------------------------------------------------

    int32 randomBetween(int32 minimum, int32 maximum)
    {
        std::uniform_int_distribution<int32> distribution(minimum, maximum);
        return distribution(rng_);
    }

    bool percentChance(uint8 chance)
    {
        if (chance == 0)
        {
            return false;
        }

        if (chance >= 100)
        {
            return true;
        }

        std::uniform_int_distribution<int32> distribution(1, 100);
        return distribution(rng_) <= chance;
    }

    int32 generateInfluence(uint8 proximityWeight, uint8 activity)
    {
        if (proximityWeight == 0 || activity == 0)
        {
            return 0;
        }

        const int32 base = randomBetween(kInfluenceMin, kInfluenceMax);

        return base *
               static_cast<int32>(proximityWeight) *
               static_cast<int32>(activity) /
               10000;
    }

    int32 generateMobCounter(uint8 activity)
    {
        const int32 base = randomBetween(kMobCounterMin, kMobCounterMax);

        // A simulated active region always contributes at least one counter
        // unit. Current LSB still treats a homepoint with zero mob_kills as
        // maximum Beastmen influence.
        return std::max<int32>(
            1,
            base * static_cast<int32>(activity) / 100);
    }

    void ensureNationPresence(
        const RegionConfig& region,
        int32& sandoria,
        int32& bastok,
        int32& windurst)
    {
        if (sandoria + bastok + windurst > 0)
        {
            return;
        }

        // Low-activity regions can round all three generated values to zero.
        // If a homepoint happened while NationTotal == 0, native LSB would
        // calculate MAX Beastmen influence. Give one point to a strongest
        // locally weighted nation. Ties are randomized to avoid nation bias.
        const uint8 strongestWeight =
            std::max({ region.sandoria, region.bastok, region.windurst });

        std::array<uint8, 3> candidates{};
        uint8                candidateCount = 0;

        if (region.sandoria == strongestWeight)
        {
            candidates[candidateCount++] = 0;
        }

        if (region.bastok == strongestWeight)
        {
            candidates[candidateCount++] = 1;
        }

        if (region.windurst == strongestWeight)
        {
            candidates[candidateCount++] = 2;
        }

        const uint8 selected =
            candidates[randomBetween(0, candidateCount - 1)];

        if (selected == 0)
        {
            sandoria = 1;
        }
        else if (selected == 1)
        {
            bastok = 1;
        }
        else
        {
            windurst = 1;
        }
    }

    // ---------------------------------------------------------------------
    // Simulation
    // ---------------------------------------------------------------------

    void simulateTick(int32 now)
    {
        const int64 conquestWeek = getConquestWeek(now);

        int32 activeRegions  = 0;
        int32 updatedRegions = 0;

        for (const auto& region : kRegions)
        {
            if (!regionActiveThisWeek(region, conquestWeek))
            {
                if (kVerboseLogging)
                {
                    ShowInfoFmt(
                        "[ConquestPopulation] {}: inactive this Conquest week",
                        region.name);
                }

                continue;
            }

            ++activeRegions;

            // Roll each nation independently so weekly results are not a
            // mathematically fixed ratio of the proximity table.
            int32 sandoria = generateInfluence(region.sandoria, region.activity);
            int32 bastok   = generateInfluence(region.bastok, region.activity);
            int32 windurst = generateInfluence(region.windurst, region.activity);

            ensureNationPresence(region, sandoria, bastok, windurst);

            const int32 mobCounter = generateMobCounter(region.activity);
            const int32 homepoints = percentChance(region.homepointChance) ? 1 : 0;

            // Additive only: real player contributions are preserved.
            //
            // Cap each nation at the same native cap used by the current
            // world Conquest system. We intentionally do NOT perform native
            // overflow-drain behavior here: this simulator contributes tiny
            // ambient values and should never use a synthetic tick to reduce
            // another nation's real-player score.
            const auto result = db::preparedStmt(
                "UPDATE conquest_system SET "
                "sandoria_influence = LEAST(sandoria_influence + ?, ?), "
                "bastok_influence   = LEAST(bastok_influence + ?, ?), "
                "windurst_influence = LEAST(windurst_influence + ?, ?), "
                "mob_kills          = mob_kills + ?, "
                "player_homepoints  = player_homepoints + ? "
                "WHERE region_id = ?",
                sandoria,
                kInfluenceCap,
                bastok,
                kInfluenceCap,
                windurst,
                kInfluenceCap,
                mobCounter,
                homepoints,
                region.id);

            if (!result)
            {
                ShowErrorFmt(
                    "[ConquestPopulation] Failed updating region {} ({})",
                    region.id,
                    region.name);
                continue;
            }

            ++updatedRegions;

            if (kVerboseLogging)
            {
                ShowInfoFmt(
                    "[ConquestPopulation] {}: "
                    "S+{} B+{} W+{} mobCounter+{} HP+{}",
                    region.name,
                    sandoria,
                    bastok,
                    windurst,
                    mobCounter,
                    homepoints);
            }
        }

        ShowInfoFmt(
            "[ConquestPopulation] Tick complete: {} active, {} updated, "
            "Conquest week {}.",
            activeRegions,
            updatedRegions,
            conquestWeek);
    }
};

REGISTER_CPP_MODULE(ConquestPopulationModule);
