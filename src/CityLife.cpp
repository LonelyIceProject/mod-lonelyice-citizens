/*
 * Living capitals (city life): config, POI table, citizen registry, recruit / release manager,
 * playerbots hooks and the .citizen GM command.
 */

#include "CityLife.h"

#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "DBCStores.h"
#include "Engine.h"
#include "ExternalHooks.h"
#include "GameObjectData.h"
#include "Group.h"
#include "LFGMgr.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "Timer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <unordered_map>

using namespace Acore::ChatCommands;

void RegisterCitizenContexts();   // CitizenStrategy.cpp

namespace CityLife
{
    // ================================================================== cities (spec sections 0, 3, 4)
    namespace
    {
        struct CityDef
        {
            char const* key;              // config suffix and command name
            uint32 zone;
            uint16 map;
            float cx, cy, radius;         // coarse box for the POI scan (the zone check decides)
            TeamId team;                  // TEAM_NEUTRAL: both factions
            uint32 defaultCount;
            uint32 defaultMinLevel;       // 0 = Citizens.MinLevel
            std::vector<uint8> homeRaces;
        };

        std::vector<CityDef> const& Cities()
        {
            static std::vector<CityDef> const cities =
            {
                { "Stormwind",    1519, 0,   -8700.0f,   600.0f,  900.0f, TEAM_ALLIANCE, 8, 0,  { RACE_HUMAN } },
                { "Ironforge",    1537, 0,   -4840.0f, -1050.0f,  700.0f, TEAM_ALLIANCE, 6, 0,  { RACE_DWARF, RACE_GNOME } },
                { "Darnassus",    1657, 1,    9900.0f,  2400.0f,  800.0f, TEAM_ALLIANCE, 5, 0,  { RACE_NIGHTELF } },
                { "Exodar",       3557, 530, -3950.0f, -11650.0f, 700.0f, TEAM_ALLIANCE, 5, 0,  { RACE_DRAENEI } },
                { "Orgrimmar",    1637, 1,    1700.0f, -4450.0f,  800.0f, TEAM_HORDE,    8, 0,  { RACE_ORC, RACE_TROLL } },
                { "ThunderBluff", 1638, 1,   -1200.0f,    80.0f,  700.0f, TEAM_HORDE,    5, 0,  { RACE_TAUREN } },
                { "Undercity",    1497, 0,    1600.0f,   240.0f,  600.0f, TEAM_HORDE,    6, 0,  { RACE_UNDEAD_PLAYER } },
                { "Silvermoon",   3487, 530,  9650.0f, -7300.0f,  800.0f, TEAM_HORDE,    5, 0,  { RACE_BLOODELF } },
                { "Shattrath",    3703, 530, -1840.0f,  5360.0f,  700.0f, TEAM_NEUTRAL,  3, 58, { } },
                { "Dalaran",      4395, 571,  5800.0f,   630.0f,  600.0f, TEAM_NEUTRAL,  3, 68, { } },
            };
            return cities;
        }

        CityDef const* CityByZone(uint32 zone)
        {
            for (CityDef const& city : Cities())
                if (city.zone == zone)
                    return &city;
            return nullptr;
        }

        std::string Lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
            return text;
        }

        // Case-insensitive prefix of the key ("org", "thunder") or the zone id.
        CityDef const* CityByName(std::string const& name)
        {
            std::string const wanted = Lower(name);
            if (wanted.empty())
                return nullptr;

            for (CityDef const& city : Cities())
            {
                if (Lower(city.key).rfind(wanted, 0) == 0 || std::to_string(city.zone) == wanted)
                    return &city;
            }
            return nullptr;
        }

        // ------------------------------------------------------------------ config
        Config sConfig;
        std::map<uint32, uint32> sCount;      // zone -> wanted citizens
        std::map<uint32, uint32> sMinLevel;   // zone -> min level

        uint32 Opt(std::string const& key, uint32 def) { return sConfigMgr->GetOption<uint32>(key, def, false); }

        void LoadConfig()
        {
            Config cfg;
            cfg.enable = sConfigMgr->GetOption<bool>("Citizens.Enable", true, false);
            cfg.homeRaceShare = std::clamp(sConfigMgr->GetOption<float>("Citizens.HomeRaceShare", 0.7f, false), 0.0f, 1.0f);
            cfg.releaseAfterSec = Opt("Citizens.ReleaseAfterSec", 300);
            cfg.managerIntervalMs = std::max<uint32>(1000, Opt("Citizens.ManagerIntervalMs", 10000));
            cfg.recruitPerTick = std::max<uint32>(1, Opt("Citizens.RecruitPerTick", 2));
            cfg.emoteToPlayerCooldownSec = Opt("Citizens.EmoteToPlayerCooldownSec", 180);
            cfg.cityEmoteCooldownSec = Opt("Citizens.CityEmoteCooldownSec", 45);
            cfg.debug = sConfigMgr->GetOption<bool>("Citizens.Debug", false, false);

            // "stroll:45,afk:20,sit:10,social:15,fool:5"; unknown names are ignored, missing ones keep 0
            std::string const weights = sConfigMgr->GetOption<std::string>("Citizens.Weights",
                "stroll:45,afk:20,sit:10,social:15,fool:5", false);
            static char const* const names[MOOD_MAX] = { "stroll", "afk", "sit", "social", "fool" };
            uint32 parsed[MOOD_MAX] = { };
            bool any = false;
            size_t start = 0;
            while (start <= weights.size())
            {
                size_t end = weights.find(',', start);
                std::string item = weights.substr(start, end == std::string::npos ? std::string::npos : end - start);
                size_t colon = item.find(':');
                if (colon != std::string::npos)
                {
                    std::string name = Lower(item.substr(0, colon));
                    name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char c) { return std::isspace(c); }), name.end());
                    uint32 value = uint32(std::strtoul(item.c_str() + colon + 1, nullptr, 10));
                    for (uint8 i = 0; i < MOOD_MAX; ++i)
                    {
                        if (name == names[i])
                        {
                            parsed[i] = value;
                            any = any || value > 0;
                        }
                    }
                }

                if (end == std::string::npos)
                    break;
                start = end + 1;
            }

            if (any)
                std::copy(std::begin(parsed), std::end(parsed), std::begin(cfg.weights));

            uint32 const minLevel = Opt("Citizens.MinLevel", 10);
            sCount.clear();
            sMinLevel.clear();
            for (CityDef const& city : Cities())
            {
                sCount[city.zone] = Opt(std::string("Citizens.Count.") + city.key, city.defaultCount);
                sMinLevel[city.zone] = Opt(std::string("Citizens.MinLevel.") + city.key,
                                           std::max(minLevel, city.defaultMinLevel));
            }

            sConfig = cfg;
        }

        uint32 WantedCount(uint32 zone)
        {
            auto itr = sCount.find(zone);
            return itr != sCount.end() ? itr->second : 0;
        }

        uint32 MinLevel(uint32 zone)
        {
            auto itr = sMinLevel.find(zone);
            return itr != sMinLevel.end() ? itr->second : 10;
        }
    }

    Config const& GetConfig()
    {
        return sConfig;
    }

    // ================================================================== POI table (section 3)
    namespace
    {
        enum PoiKind : uint8
        {
            POI_BANK = 0,
            POI_AUCTION,
            POI_MAILBOX,
            POI_INN,
            POI_GUILD,
            POI_FLIGHT,
            POI_BATTLEMASTER,
            POI_STABLE,
            POI_TRAINER,
            POI_VENDOR,
            POI_KIND_MAX
        };

        char const* const sKindNames[POI_KIND_MAX] = { "bank", "auction", "mailbox", "inn", "guild", "flight",
                                                       "battlemaster", "stable", "trainer", "vendor" };
        float const sKindWeights[POI_KIND_MAX] = { 5.0f, 5.0f, 3.0f, 3.0f, 2.0f, 2.0f, 1.0f, 1.0f, 1.0f, 1.0f };

        int32 KindOfNpc(uint32 npcflag)
        {
            if (npcflag & UNIT_NPC_FLAG_BANKER)
                return POI_BANK;
            if (npcflag & UNIT_NPC_FLAG_AUCTIONEER)
                return POI_AUCTION;
            if (npcflag & UNIT_NPC_FLAG_INNKEEPER)
                return POI_INN;
            if (npcflag & (UNIT_NPC_FLAG_PETITIONER | UNIT_NPC_FLAG_TABARDDESIGNER))
                return POI_GUILD;
            if (npcflag & UNIT_NPC_FLAG_FLIGHTMASTER)
                return POI_FLIGHT;
            if (npcflag & UNIT_NPC_FLAG_BATTLEMASTER)
                return POI_BATTLEMASTER;
            if (npcflag & UNIT_NPC_FLAG_STABLEMASTER)
                return POI_STABLE;
            if (npcflag & UNIT_NPC_FLAG_TRAINER)
                return POI_TRAINER;
            if (npcflag & UNIT_NPC_FLAG_VENDOR)
                return POI_VENDOR;
            return -1;
        }

        CityDef const* CityAround(uint16 map, float x, float y)
        {
            for (CityDef const& city : Cities())
            {
                if (city.map != map)
                    continue;

                float const dx = x - city.cx;
                float const dy = y - city.cy;
                if (dx * dx + dy * dy <= city.radius * city.radius)
                    return &city;
            }
            return nullptr;
        }

        // Stand point 3-6 yd in front of the spawn (along its orientation), on the ground and in line of
        // sight of it; falls back to shorter offsets and finally the spawn point itself.
        void PlaceStandPoint(Map* map, float nx, float ny, float nz, float o, float minDist, float maxDist, Poi& poi)
        {
            poi.x = nx;
            poi.y = ny;
            poi.z = nz;
            poi.faceX = nx;
            poi.faceY = ny;

            float const dists[] = { minDist + (maxDist - minDist) * float(rand_norm()), minDist, 1.5f };
            for (float d : dists)
            {
                float const x = nx + std::cos(o) * d;
                float const y = ny + std::sin(o) * d;
                float const gz = map->GetHeight(PHASEMASK_NORMAL, x, y, nz + 2.0f, true, 6.0f);
                if (gz <= INVALID_HEIGHT || std::fabs(gz - nz) > 2.5f)
                    continue;

                if (!map->isInLineOfSight(nx, ny, nz + 1.5f, x, y, gz + 1.5f, PHASEMASK_NORMAL, LINEOFSIGHT_ALL_CHECKS,
                                          VMAP::ModelIgnoreFlags::Nothing))
                    continue;

                poi.x = x;
                poi.y = y;
                poi.z = gz + 0.05f;
                return;
            }
        }

        std::shared_mutex sPoiLock;
        std::map<uint32, std::shared_ptr<CityPois const>> sPois;

        void BuildPois()
        {
            uint32 const startMs = getMSTime();
            std::map<uint32, std::shared_ptr<CityPois>> built;
            std::map<uint32, std::array<uint32, POI_KIND_MAX>> kindCounts;
            std::vector<uint8> kinds;   // parallel to each city's pois

            FactionTemplateEntry const* alliance = sFactionTemplateStore.LookupEntry(1);   // Human PC
            FactionTemplateEntry const* horde = sFactionTemplateStore.LookupEntry(2);      // Orc PC

            auto add = [&](CityDef const& city, Poi poi)
            {
                std::shared_ptr<CityPois>& entry = built[city.zone];
                if (!entry)
                {
                    entry = std::make_shared<CityPois>();
                    entry->zone = city.zone;
                    kindCounts[city.zone].fill(0);
                }
                ++kindCounts[city.zone][poi.kind];
                entry->pois.push_back(poi);
            };

            for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
            {
                if (!(data.phaseMask & PHASEMASK_NORMAL))
                    continue;

                CityDef const* city = CityAround(data.mapid, data.posX, data.posY);
                if (!city)
                    continue;

                CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(data.id);
                if (!info)
                    continue;

                int32 const kind = KindOfNpc(info->npcflag | data.npcflag);
                if (kind < 0)
                    continue;

                if (sMapMgr->GetZoneId(PHASEMASK_NORMAL, data.mapid, data.posX, data.posY, data.posZ) != city->zone)
                    continue;

                Poi poi;
                poi.map = data.mapid;
                poi.kind = uint8(kind);
                if (FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(info->faction))
                {
                    poi.hostileToAlliance = alliance && faction->IsHostileTo(*alliance);
                    poi.hostileToHorde = horde && faction->IsHostileTo(*horde);
                }

                Map* map = sMapMgr->CreateBaseMap(data.mapid);
                PlaceStandPoint(map, data.posX, data.posY, data.posZ, data.orientation, 3.0f, 6.0f, poi);
                add(*city, poi);
            }

            for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
            {
                if (!(data.phaseMask & PHASEMASK_NORMAL))
                    continue;

                CityDef const* city = CityAround(data.mapid, data.posX, data.posY);
                if (!city)
                    continue;

                GameObjectTemplate const* info = sObjectMgr->GetGameObjectTemplate(data.id);
                if (!info || info->type != GAMEOBJECT_TYPE_MAILBOX)
                    continue;

                if (sMapMgr->GetZoneId(PHASEMASK_NORMAL, data.mapid, data.posX, data.posY, data.posZ) != city->zone)
                    continue;

                Poi poi;
                poi.map = data.mapid;
                poi.kind = POI_MAILBOX;
                Map* map = sMapMgr->CreateBaseMap(data.mapid);
                PlaceStandPoint(map, data.posX, data.posY, data.posZ, data.orientation, 2.0f, 3.0f, poi);
                add(*city, poi);
            }

            // Kind weight is shared by all spawns of that kind: ten vendors do not outweigh one bank.
            uint32 total = 0;
            std::map<uint32, std::shared_ptr<CityPois const>> result;
            for (auto& [zone, city] : built)
            {
                for (Poi& poi : city->pois)
                    poi.weight = sKindWeights[poi.kind] / float(std::max<uint32>(1, kindCounts[zone][poi.kind]));

                total += uint32(city->pois.size());
                result[zone] = city;
            }

            {
                std::unique_lock<std::shared_mutex> guard(sPoiLock);
                sPois = std::move(result);
            }

            LOG_INFO("server.loading", "CityLife: {} POIs in {} cities ({} ms)", total, built.size(),
                     GetMSTimeDiffToNow(startMs));
            for (CityDef const& city : Cities())
            {
                auto itr = built.find(city.zone);
                std::string kindsText;
                if (itr != built.end())
                {
                    for (uint8 k = 0; k < POI_KIND_MAX; ++k)
                        if (kindCounts[city.zone][k])
                            kindsText += Acore::StringFormat(" {}={}", sKindNames[k], kindCounts[city.zone][k]);
                }
                LOG_INFO("server.loading", "CityLife:   {} (zone {}): {} POIs{}", city.key, city.zone,
                         itr != built.end() ? itr->second->pois.size() : 0, kindsText);
            }
        }
    }

    std::shared_ptr<CityPois const> GetPois(uint32 zone)
    {
        std::shared_lock<std::shared_mutex> guard(sPoiLock);
        auto itr = sPois.find(zone);
        return itr != sPois.end() ? itr->second : nullptr;
    }

    char const* PoiKindName(uint8 kind)
    {
        return kind < POI_KIND_MAX ? sKindNames[kind] : "?";
    }

    bool PoiAllowed(Poi const& poi, Player const* bot)
    {
        return bot->GetTeamId() == TEAM_ALLIANCE ? !poi.hostileToAlliance : !poi.hostileToHorde;
    }

    int32 PickPoi(CityPois const& city, Player const* bot, int32 exclude)
    {
        float total = 0.0f;
        std::vector<float> weights(city.pois.size(), 0.0f);
        for (size_t i = 0; i < city.pois.size(); ++i)
        {
            Poi const& poi = city.pois[i];
            if (int32(i) == exclude || !PoiAllowed(poi, bot))
                continue;

            // spec 5: prefer points within 250 yd
            bool const close = poi.map == bot->GetMapId() && bot->GetExactDist2dSq(poi.x, poi.y) < 250.0f * 250.0f;
            weights[i] = poi.weight * (close ? 1.0f : 0.25f);
            total += weights[i];
        }

        if (total <= 0.0f)
            return -1;

        float roll = float(rand_norm()) * total;
        for (size_t i = 0; i < weights.size(); ++i)
        {
            if (weights[i] <= 0.0f)
                continue;

            roll -= weights[i];
            if (roll <= 0.0f)
                return int32(i);
        }

        for (size_t i = weights.size(); i-- > 0;)
            if (weights[i] > 0.0f)
                return int32(i);

        return -1;
    }

    int32 NearestPoi(CityPois const& city, Player const* bot, float* distance)
    {
        int32 best = -1;
        float bestSq = 0.0f;
        for (size_t i = 0; i < city.pois.size(); ++i)
        {
            Poi const& poi = city.pois[i];
            if (poi.map != bot->GetMapId() || !PoiAllowed(poi, bot))
                continue;

            float const sq = bot->GetExactDist2dSq(poi.x, poi.y);
            if (best < 0 || sq < bestSq)
            {
                best = int32(i);
                bestSq = sq;
            }
        }

        if (distance)
            *distance = best < 0 ? -1.0f : std::sqrt(bestSq);
        return best;
    }

    // ================================================================== registry
    namespace
    {
        struct Citizen
        {
            uint32 zone = 0;
            uint32 serial = 0;
            std::string name;
            char const* state = "wake";   // string literal from the action
            uint32 stateSince = 0;
            uint32 pinnedAt = 0;
            uint32 outsideSince = 0;      // manager: first tick seen outside the city zone (0 = inside)
        };

        std::shared_mutex sRegistryLock;
        std::unordered_map<ObjectGuid::LowType, Citizen> sCitizens;
        std::atomic<uint32> sCitizenCount{ 0 };   // lock-free fast path of IsPinned with nobody pinned
        uint32 sSerial = 0;

        std::mutex sEmoteLock;
        std::unordered_map<uint32, uint32> sCityEmoteAt;   // zone -> getMSTime() of the last player emote
    }

    bool GetCitizen(ObjectGuid::LowType guid, CitizenView& out)
    {
        if (!sCitizenCount.load(std::memory_order_relaxed))
            return false;

        std::shared_lock<std::shared_mutex> guard(sRegistryLock);
        auto itr = sCitizens.find(guid);
        if (itr == sCitizens.end())
            return false;

        out.zone = itr->second.zone;
        out.serial = itr->second.serial;
        return true;
    }

    bool IsCitizen(ObjectGuid::LowType guid)
    {
        if (!sCitizenCount.load(std::memory_order_relaxed))
            return false;

        std::shared_lock<std::shared_mutex> guard(sRegistryLock);
        return sCitizens.find(guid) != sCitizens.end();
    }

    void ReportState(ObjectGuid::LowType guid, char const* state)
    {
        std::unique_lock<std::shared_mutex> guard(sRegistryLock);
        auto itr = sCitizens.find(guid);
        if (itr == sCitizens.end() || itr->second.state == state)
            return;

        itr->second.state = state;
        itr->second.stateSince = getMSTime();
    }

    bool TryCityEmote(uint32 zone)
    {
        uint32 const now = getMSTime();
        std::lock_guard<std::mutex> guard(sEmoteLock);
        auto itr = sCityEmoteAt.find(zone);
        if (itr != sCityEmoteAt.end() && getMSTimeDiff(itr->second, now) < GetConfig().cityEmoteCooldownSec * IN_MILLISECONDS)
            return false;

        sCityEmoteAt[zone] = now;
        return true;
    }

    // ================================================================== playerbots hooks (spec section 2)
    namespace
    {
        void DecorateEngine(Player* bot, Engine* engine, uint8 botState)
        {
            if (botState != BOT_STATE_NON_COMBAT || !IsCitizen(bot->GetGUID().GetCounter()))
                return;

            // Combat engine untouched: citizens defend themselves but never go looking for a fight.
            static char const* const removed[] = { "new rpg", "rpg", "grind", "move random", "lfg", "bg",
                                                   "start duel", "pvp", "travel" };
            for (char const* name : removed)
                engine->removeStrategy(name, false);

            engine->addStrategy("citizen", false);
        }
    }

    // ================================================================== manager (section 4)
    namespace
    {
        enum class ReleaseReason : uint8 { CityEmpty, Command, Died, Grouped, Busy, Gone, Disabled };

        char const* ReasonName(ReleaseReason reason)
        {
            switch (reason)
            {
                case ReleaseReason::CityEmpty: return "city empty";
                case ReleaseReason::Command:   return "command";
                case ReleaseReason::Died:      return "died";
                case ReleaseReason::Grouped:   return "grouped";
                case ReleaseReason::Busy:      return "bg/lfg";
                case ReleaseReason::Gone:      return "logged out";
                case ReleaseReason::Disabled:  return "disabled";
            }
            return "?";
        }

        struct CityRuntime
        {
            bool present = false;         // real player in the zone (or GM test) on the last tick
            bool test = false;            // .citizen test: recruit as if a player were there, until release
            uint32 lastPresent = 0;       // getMSTime() of the last tick with a player
            uint32 realPlayers = 0;
        };

        // World thread only.
        std::map<uint32, CityRuntime> sCityState;
        std::unordered_map<ObjectGuid::LowType, uint32> sCooldown;   // released guid -> not before (ms)
        uint32 sTimer = 0;
        constexpr uint32 RECRUIT_COOLDOWN_MS = 10 * MINUTE * IN_MILLISECONDS;
        constexpr float WITNESS_RANGE = 150.0f;   // RandomTeleport rule: no blink in sight of a player
        constexpr float LEASH_WALK_RANGE = 300.0f;
        constexpr uint32 LEASH_WALK_MS = 120 * IN_MILLISECONDS;

        std::vector<Player*> RealPlayers()
        {
            std::vector<Player*> out;
            for (Player* player : sRandomPlayerbotMgr.GetPlayers())
                if (player && player->IsInWorld() && IsRealPlayer(player))
                    out.push_back(player);
            return out;
        }

        // Any non-random-bot player (real, altbot, selfbot) near the point: over-strict on purpose.
        bool Witnessed(uint32 map, float x, float y)
        {
            for (Player* player : sRandomPlayerbotMgr.GetPlayers())
            {
                if (!player || !player->IsInWorld() || player->GetMapId() != map)
                    continue;

                if (player->GetExactDist2dSq(x, y) < WITNESS_RANGE * WITNESS_RANGE)
                    return true;
            }
            return false;
        }

        std::vector<std::pair<ObjectGuid::LowType, Citizen>> Snapshot()
        {
            std::shared_lock<std::shared_mutex> guard(sRegistryLock);
            return { sCitizens.begin(), sCitizens.end() };
        }

        uint32 CountIn(uint32 zone)
        {
            std::shared_lock<std::shared_mutex> guard(sRegistryLock);
            uint32 n = 0;
            for (auto const& [guid, citizen] : sCitizens)
                n += citizen.zone == zone ? 1 : 0;
            return n;
        }

        void SetOutsideSince(ObjectGuid::LowType guid, uint32 value)
        {
            std::unique_lock<std::shared_mutex> guard(sRegistryLock);
            auto itr = sCitizens.find(guid);
            if (itr != sCitizens.end())
                itr->second.outsideSince = value;
        }

        // Teleport to a random allowed POI of the city unless a player could see the blink.
        bool TeleportToPoi(Player* bot, PlayerbotAI* botAI, CityPois const& city)
        {
            if (!bot->IsInWorld() || bot->IsBeingTeleported() || bot->IsInFlight() || bot->IsRooted() || !bot->IsAlive())
                return false;

            if (botAI->HasPlayerNearby(WITNESS_RANGE))
                return false;

            for (uint8 attempt = 0; attempt < 6; ++attempt)
            {
                int32 const index = PickPoi(city, bot, -1);
                if (index < 0)
                    return false;

                Poi const& poi = city.pois[index];
                if (Witnessed(poi.map, poi.x, poi.y))
                    continue;

                bot->GetMotionMaster()->Clear();
                botAI->Reset(true);
                bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
                float const o = std::atan2(poi.faceY - poi.y, poi.faceX - poi.x);
                return bot->TeleportTo(poi.map, poi.x, poi.y, poi.z, Position::NormalizeOrientation(o));
            }

            return false;
        }

        void Pin(Player* bot, PlayerbotAI* botAI, CityDef const& city, bool teleported)
        {
            ObjectGuid::LowType const guid = bot->GetGUID().GetCounter();
            uint32 const now = getMSTime();
            {
                std::unique_lock<std::shared_mutex> guard(sRegistryLock);
                Citizen& citizen = sCitizens[guid];
                citizen.zone = city.zone;
                citizen.serial = ++sSerial;
                citizen.name = bot->GetName();
                citizen.state = "wake";
                citizen.stateSince = now;
                citizen.pinnedAt = now;
                citizen.outsideSince = 0;
                sCitizenCount.store(uint32(sCitizens.size()));
            }

            // Registered first: ResetStrategies runs the decorate hook, which must see the pin.
            if (!teleported)
                botAI->Reset(true);
            botAI->ResetStrategies();

            if (GetConfig().debug)
                LOG_INFO("module", "CityLife: {} (L{} race {}) joins {} ({})", bot->GetName(), bot->GetLevel(),
                         bot->getRace(), city.key, teleported ? "teleported" : "already there");
        }

        // Unpins and gives the bot back to random-bot life. bot may be nullptr (logged out).
        void Release(ObjectGuid::LowType guid, Player* bot, ReleaseReason reason)
        {
            std::string name;
            uint32 zone = 0;
            {
                std::unique_lock<std::shared_mutex> guard(sRegistryLock);
                auto itr = sCitizens.find(guid);
                if (itr == sCitizens.end())
                    return;

                name = itr->second.name;
                zone = itr->second.zone;
                sCitizens.erase(itr);
                sCitizenCount.store(uint32(sCitizens.size()));
            }

            sCooldown[guid] = getMSTime() + RECRUIT_COOLDOWN_MS;

            if (GetConfig().debug)
                LOG_INFO("module", "CityLife: {} leaves zone {} ({})", name, zone, ReasonName(reason));

            if (!bot || !bot->IsInWorld())
                return;

            if (bot->isAFK())
                bot->ToggleAFK();

            if (bot->IsAlive())
            {
                if (bot->getStandState() != UNIT_STAND_STATE_STAND)
                    bot->SetStandState(UNIT_STAND_STATE_STAND);
                bot->SetEmoteState(EMOTE_ONESHOT_NONE);
            }

            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (!botAI)
                return;

            bool const free = bot->IsAlive() && !bot->IsInCombat() && !bot->GetGroup() && !bot->IsBeingTeleported();
            if (free)
                botAI->Reset(true);
            botAI->ResetStrategies();

            // spec 0: back to normal life right away, unless a player could see the blink
            // (RandomTeleport checks that itself too).
            bool const teleport = reason == ReleaseReason::CityEmpty || reason == ReleaseReason::Command ||
                                  reason == ReleaseReason::Disabled;
            if (teleport && free && !botAI->HasPlayerNearby(WITNESS_RANGE))
                sRandomPlayerbotMgr.RandomTeleportForLevel(bot);
        }

        void ReleaseCity(uint32 zone, ReleaseReason reason)
        {
            for (auto const& [guid, citizen] : Snapshot())
            {
                if (zone && citizen.zone != zone)
                    continue;

                Release(guid, ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(guid)), reason);
            }
        }

        struct Candidate
        {
            Player* bot = nullptr;
            PlayerbotAI* botAI = nullptr;
            uint32 zone = 0;
            bool taken = false;
        };

        bool Eligible(Player* bot, PlayerbotAI* botAI, uint32 now)
        {
            if (!bot || !botAI || !bot->IsInWorld() || bot->IsBeingTeleported() || !bot->IsAlive())
                return false;

            if (!bot->GetSession() || bot->GetSession()->IsLoggingOut() || bot->IsDuringRemoveFromWorld())
                return false;

            if (bot->GetGroup() || bot->InBattleground() || bot->InBattlegroundQueue() || bot->InArena())
                return false;

            if (bot->IsInCombat() || bot->IsInFlight() || bot->GetTransport() || bot->GetVehicle())
                return false;

            if (!bot->GetMap() || bot->GetMap()->Instanceable())
                return false;

            if (sLFGMgr->GetState(bot->GetGUID()) != lfg::LFG_STATE_NONE)
                return false;

            ObjectGuid::LowType const guid = bot->GetGUID().GetCounter();
            auto cd = sCooldown.find(guid);
            if (cd != sCooldown.end())
            {
                if (int32(cd->second - now) > 0)
                    return false;
                sCooldown.erase(cd);
            }

            return !IsCitizen(guid);
        }

        bool IsHomeRace(CityDef const& city, uint8 race)
        {
            return std::find(city.homeRaces.begin(), city.homeRaces.end(), race) != city.homeRaces.end();
        }

        Candidate* PickRandom(std::vector<Candidate*> const& pool)
        {
            return pool.empty() ? nullptr : pool[urand(0, uint32(pool.size()) - 1)];
        }

        // Up to RecruitPerTick new citizens for one city: bots already in the zone first, then the others
        // (HomeRaceShare of them of the city's home races; neutral cities ~50/50 by faction), teleported.
        void Recruit(CityDef const& city, uint32 missing, std::vector<Candidate>& candidates)
        {
            std::shared_ptr<CityPois const> pois = GetPois(city.zone);
            if (!pois || pois->pois.empty())
                return;

            uint32 const minLevel = MinLevel(city.zone);
            uint32 budget = std::min(missing, GetConfig().recruitPerTick);
            uint32 attempts = 0;

            while (budget && attempts++ < 12)
            {
                TeamId team = city.team;
                if (team == TEAM_NEUTRAL)
                    team = urand(0, 1) ? TEAM_ALLIANCE : TEAM_HORDE;

                std::vector<Candidate*> inZone, home, other, otherTeam;
                for (Candidate& c : candidates)
                {
                    if (c.taken || c.bot->GetLevel() < minLevel)
                        continue;

                    if (city.team != TEAM_NEUTRAL && c.bot->GetTeamId() != city.team)
                        continue;

                    if (c.zone == city.zone)
                        inZone.push_back(&c);
                    else if (c.bot->GetTeamId() != team)
                        otherTeam.push_back(&c);
                    else if (IsHomeRace(city, c.bot->getRace()))
                        home.push_back(&c);
                    else
                        other.push_back(&c);
                }

                if (Candidate* local = PickRandom(inZone))
                {
                    local->taken = true;
                    Pin(local->bot, local->botAI, city, false);
                    --budget;
                    continue;
                }

                if (home.empty() && other.empty())
                    std::swap(other, otherTeam);   // neutral city: the other faction has nobody eligible

                std::vector<Candidate*>* pool = &other;
                if (!home.empty() && (other.empty() || rand_norm() < GetConfig().homeRaceShare))
                    pool = &home;

                Candidate* picked = PickRandom(*pool);
                if (!picked)
                    return;

                picked->taken = true;   // tried once per tick, even when the teleport is not possible now
                if (!TeleportToPoi(picked->bot, picked->botAI, *pois))
                    continue;

                Pin(picked->bot, picked->botAI, city, true);
                --budget;
            }
        }

        // A citizen outside its city walks back (the action does it) when a POI is within 300 yd; otherwise,
        // or after 2 minutes outside, it is teleported back unseen.
        void Leash(ObjectGuid::LowType guid, Citizen const& citizen, Player* bot, uint32 now)
        {
            if (bot->GetZoneId() == citizen.zone)
            {
                if (citizen.outsideSince)
                    SetOutsideSince(guid, 0);
                return;
            }

            if (!citizen.outsideSince)
            {
                SetOutsideSince(guid, now);
                return;
            }

            std::shared_ptr<CityPois const> pois = GetPois(citizen.zone);
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (!pois || !botAI || bot->IsInCombat())
                return;

            float distance = -1.0f;
            NearestPoi(*pois, bot, &distance);
            if (distance >= 0.0f && distance <= LEASH_WALK_RANGE && getMSTimeDiff(citizen.outsideSince, now) < LEASH_WALK_MS)
                return;

            if (TeleportToPoi(bot, botAI, *pois))
                SetOutsideSince(guid, 0);
        }

        void Tick()
        {
            Config const& cfg = GetConfig();
            uint32 const now = getMSTime();

            // 1. where are the real players
            std::map<uint32, uint32> playersByZone;
            for (Player* player : RealPlayers())
                ++playersByZone[player->GetZoneId()];

            // 2. validate every citizen (death, group, BG/LFG, logout)
            for (auto const& [guid, citizen] : Snapshot())
            {
                Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(guid));
                if (!bot)
                {
                    Release(guid, nullptr, ReleaseReason::Gone);
                    continue;
                }

                if (!bot->IsInWorld() || bot->IsBeingTeleported())
                    continue;

                if (bot->isDead())
                    Release(guid, bot, ReleaseReason::Died);
                else if (bot->GetGroup())
                    Release(guid, bot, ReleaseReason::Grouped);
                else if (bot->InBattleground() || bot->InBattlegroundQueue() || bot->InArena() ||
                         sLFGMgr->GetState(bot->GetGUID()) != lfg::LFG_STATE_NONE)
                    Release(guid, bot, ReleaseReason::Busy);
                else
                    Leash(guid, citizen, bot, now);
            }

            // 3. presence, release after ReleaseAfterSec, recruiting
            std::vector<Candidate> candidates;
            bool candidatesBuilt = false;

            for (CityDef const& city : Cities())
            {
                CityRuntime& state = sCityState[city.zone];
                auto players = playersByZone.find(city.zone);
                state.realPlayers = players != playersByZone.end() ? players->second : 0;
                state.present = cfg.enable && (state.realPlayers > 0 || state.test);
                if (state.present)
                    state.lastPresent = now;

                uint32 const have = CountIn(city.zone);
                if (!state.present)
                {
                    if (have && (!cfg.enable || getMSTimeDiff(state.lastPresent, now) >= cfg.releaseAfterSec * IN_MILLISECONDS))
                        ReleaseCity(city.zone, cfg.enable ? ReleaseReason::CityEmpty : ReleaseReason::Disabled);
                    continue;
                }

                uint32 const wanted = WantedCount(city.zone);
                if (have >= wanted)
                    continue;

                if (!candidatesBuilt)
                {
                    candidatesBuilt = true;
                    for (auto const& [botGuid, bot] : sRandomPlayerbotMgr.GetAllBots())
                    {
                        PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
                        if (!Eligible(bot, botAI, now))
                            continue;

                        Candidate c;
                        c.bot = bot;
                        c.botAI = botAI;
                        c.zone = bot->GetZoneId();
                        candidates.push_back(c);
                    }
                }

                Recruit(city, wanted - have, candidates);
            }
        }

        // ------------------------------------------------------------------ scripts
        class CityLifeWorldScript : public WorldScript
        {
        public:
            CityLifeWorldScript() : WorldScript("CityLifeWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD,
                                                                         WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE }) { }

            void OnAfterConfigLoad(bool /*reload*/) override
            {
                LoadConfig();
            }

            void OnStartup() override
            {
                BuildPois();
            }

            void OnUpdate(uint32 diff) override
            {
                sTimer += diff;
                if (sTimer < GetConfig().managerIntervalMs)
                    return;

                sTimer = 0;
                Tick();
            }
        };

        class CityLifePlayerScript : public PlayerScript
        {
        public:
            CityLifePlayerScript() : PlayerScript("CityLifePlayerScript", { PLAYERHOOK_ON_LOGOUT }) { }

            void OnPlayerLogout(Player* player) override
            {
                ObjectGuid::LowType const guid = player->GetGUID().GetCounter();
                if (!IsCitizen(guid))
                    return;

                std::unique_lock<std::shared_mutex> guard(sRegistryLock);
                sCitizens.erase(guid);
                sCitizenCount.store(uint32(sCitizens.size()));
            }
        };

        // ------------------------------------------------------------------ .citizen
        std::string FirstWord(std::string_view args)
        {
            std::string const text(args);
            size_t const b = text.find_first_not_of(" \t");
            if (b == std::string::npos)
                return "";

            size_t const e = text.find_first_of(" \t", b);
            return text.substr(b, e == std::string::npos ? std::string::npos : e - b);
        }

        class CityLifeCommandScript : public CommandScript
        {
        public:
            CityLifeCommandScript() : CommandScript("CityLifeCommandScript") { }

            ChatCommandTable GetCommands() const override
            {
                static ChatCommandTable citizenCommandTable =
                {
                    { "status",  HandleStatus,  SEC_ADMINISTRATOR, Console::Yes },
                    { "list",    HandleList,    SEC_ADMINISTRATOR, Console::Yes },
                    { "test",    HandleTest,    SEC_ADMINISTRATOR, Console::Yes },
                    { "release", HandleRelease, SEC_ADMINISTRATOR, Console::Yes },
                    { "reload",  HandleReload,  SEC_ADMINISTRATOR, Console::Yes },
                };
                static ChatCommandTable commandTable =
                {
                    { "citizen", citizenCommandTable },
                };
                return commandTable;
            }

            static bool HandleStatus(ChatHandler* handler)
            {
                Config const& cfg = GetConfig();
                uint32 const now = getMSTime();
                handler->PSendSysMessage("citizens: enable {}, total {}, release after {} s, interval {} ms",
                                         cfg.enable ? 1 : 0, sCitizenCount.load(), cfg.releaseAfterSec, cfg.managerIntervalMs);
                for (CityDef const& city : Cities())
                {
                    CityRuntime const& state = sCityState[city.zone];
                    std::shared_ptr<CityPois const> pois = GetPois(city.zone);
                    uint32 const have = CountIn(city.zone);
                    std::string grace;
                    if (!state.present && have)
                    {
                        uint32 const left = cfg.releaseAfterSec * IN_MILLISECONDS -
                            std::min(cfg.releaseAfterSec * IN_MILLISECONDS, getMSTimeDiff(state.lastPresent, now));
                        grace = Acore::StringFormat(", release in {} s", left / IN_MILLISECONDS);
                    }
                    handler->PSendSysMessage("  {} (zone {}): citizens {}/{}, real players {}{}, POIs {}{}", city.key,
                                             city.zone, have, WantedCount(city.zone), state.realPlayers,
                                             state.test ? ", TEST" : "", pois ? pois->pois.size() : 0, grace);
                }
                return true;
            }

            static bool HandleList(ChatHandler* handler, Tail args)
            {
                std::string const word = FirstWord(args);
                CityDef const* only = nullptr;
                if (!word.empty() && !(only = CityByName(word)))
                {
                    handler->PSendSysMessage("citizen: unknown city '{}'", word);
                    return true;
                }

                uint32 const now = getMSTime();
                uint32 shown = 0;
                for (auto const& [guid, citizen] : Snapshot())
                {
                    if (only && citizen.zone != only->zone)
                        continue;

                    CityDef const* city = CityByZone(citizen.zone);
                    Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(guid));
                    ++shown;
                    if (!bot || !bot->IsInWorld())
                    {
                        handler->PSendSysMessage("  {} [{}] offline", citizen.name, city ? city->key : "?");
                        continue;
                    }

                    handler->PSendSysMessage("  {} L{} race {} [{}] state={} {}s{} | map {} zone {} area {} pos {:.1f} {:.1f} {:.1f}{}",
                                             citizen.name, bot->GetLevel(), bot->getRace(), city ? city->key : "?",
                                             citizen.state, getMSTimeDiff(citizen.stateSince, now) / IN_MILLISECONDS,
                                             bot->isAFK() ? " AFK" : "", bot->GetMapId(), bot->GetZoneId(), bot->GetAreaId(),
                                             bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                                             bot->isMoving() ? " moving" : "");
                }

                handler->PSendSysMessage("citizen: {} listed", shown);
                return true;
            }

            static bool HandleTest(ChatHandler* handler, Tail args)
            {
                std::string const word = FirstWord(args);
                CityDef const* city = CityByName(word);
                if (!city)
                {
                    handler->PSendSysMessage("citizen: usage .citizen test <city> (Stormwind, Ironforge, Darnassus, Exodar, "
                                             "Orgrimmar, ThunderBluff, Undercity, Silvermoon, Shattrath, Dalaran)");
                    return true;
                }

                CityRuntime& state = sCityState[city->zone];
                state.test = true;
                state.lastPresent = getMSTime();
                sTimer = GetConfig().managerIntervalMs;   // recruit on the next world tick
                handler->PSendSysMessage("citizen: test mode for {} (zone {}), up to {} citizens; '.citizen release all' ends it",
                                         city->key, city->zone, WantedCount(city->zone));
                return true;
            }

            static bool HandleRelease(ChatHandler* handler, Tail args)
            {
                std::string const word = FirstWord(args);
                if (word.empty())
                {
                    handler->SendSysMessage("citizen: usage .citizen release <name|city|all>");
                    return true;
                }

                if (Lower(word) == "all")
                {
                    uint32 const count = sCitizenCount.load();
                    for (auto& [zone, state] : sCityState)
                        state.test = false;
                    ReleaseCity(0, ReleaseReason::Command);
                    handler->PSendSysMessage("citizen: released {} citizens, test mode off", count);
                    return true;
                }

                std::string const lowered = Lower(word);
                for (auto const& [guid, citizen] : Snapshot())
                {
                    if (Lower(citizen.name) != lowered)
                        continue;

                    Release(guid, ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(guid)),
                            ReleaseReason::Command);
                    handler->PSendSysMessage("citizen: released {} (not recruited again for 10 min)", citizen.name);
                    return true;
                }

                if (CityDef const* city = CityByName(word))
                {
                    uint32 const count = CountIn(city->zone);
                    sCityState[city->zone].test = false;
                    ReleaseCity(city->zone, ReleaseReason::Command);
                    handler->PSendSysMessage("citizen: released {} citizens of {}, test mode off", count, city->key);
                    return true;
                }

                handler->PSendSysMessage("citizen: '{}' is neither a citizen nor a city", word);
                return true;
            }

            static bool HandleReload(ChatHandler* handler)
            {
                // re-reads the config files (like .reload config does before its hooks), then our keys
                bool const loaded = sConfigMgr->Reload();
                LoadConfig();
                BuildPois();
                handler->PSendSysMessage("citizen: config {}, POIs rebuilt; enable {}", loaded ? "reloaded" : "reload FAILED",
                                         GetConfig().enable ? 1 : 0);
                return true;
            }
        };
    }

    void AddScripts()
    {
        LoadConfig();

        RegisterCitizenContexts();
        PlayerbotExternalHooks::Register(
            [](Player* bot) { return IsCitizen(bot->GetGUID().GetCounter()); },
            [](Player* bot, Engine* engine, uint8 botState) { DecorateEngine(bot, engine, botState); });

        new CityLifeWorldScript();
        new CityLifePlayerScript();
        new CityLifeCommandScript();
    }
}

void AddCityLifeScripts()
{
    CityLife::AddScripts();
}
