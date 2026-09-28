/*
 * Living capitals (city life): config, points of interest, citizen registry and the recruit / release
 * manager.
 *
 * While a real player is in a capital zone, a few random bots of that city's faction are "pinned" there
 * as citizens (first bots already in the zone, the rest teleported to a point of interest out of sight);
 * ReleaseAfterSec after the last real player left they are released back to normal random-bot life.
 * Nothing is stored in the DB.
 *
 * Threads: the manager, the GM command and the POI build run on the world thread (maps idle); the
 * citizen trigger / action run on map threads and only read the registry through the functions below.
 */

#ifndef MOD_LONELYICE_CITY_LIFE_H
#define MOD_LONELYICE_CITY_LIFE_H

#include "Define.h"
#include "ObjectGuid.h"

#include <memory>
#include <string>
#include <vector>

class Player;

namespace CityLife
{
    // ------------------------------------------------------------------ config (spec section 6)
    enum CitizenMood : uint8
    {
        MOOD_STROLL = 0,
        MOOD_AFK,
        MOOD_SIT,
        MOOD_SOCIAL,
        MOOD_FOOL,
        MOOD_MAX
    };

    struct Config
    {
        bool enable = true;
        float homeRaceShare = 0.7f;
        uint32 releaseAfterSec = 300;
        uint32 managerIntervalMs = 10000;
        uint32 recruitPerTick = 2;
        uint32 emoteToPlayerCooldownSec = 180;
        uint32 cityEmoteCooldownSec = 45;
        uint32 weights[MOOD_MAX] = { 45, 20, 10, 15, 5 };
        bool debug = false;
    };

    // Written only on the world thread (startup, .citizen reload) while maps are not updating.
    Config const& GetConfig();

    // ------------------------------------------------------------------ points of interest (section 3)
    struct Poi
    {
        uint16 map = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f;             // stand point, 3-6 yd in front of the NPC / mailbox
        float faceX = 0.0f, faceY = 0.0f;               // what the citizen looks at
        uint8 kind = 0;                                 // PoiKind (CityLife.cpp)
        float weight = 0.0f;                            // kind weight / spawns of that kind in the city
        bool hostileToAlliance = false;                 // neutral cities: NPC attacks that faction
        bool hostileToHorde = false;
    };

    struct CityPois
    {
        uint32 zone = 0;
        std::vector<Poi> pois;
    };

    // Immutable snapshot for the city of this zone (nullptr: not a capital / no POI). Safe on any thread.
    std::shared_ptr<CityPois const> GetPois(uint32 zone);

    char const* PoiKindName(uint8 kind);

    // Neutral cities: false when the POI's NPC is hostile to the bot's faction.
    bool PoiAllowed(Poi const& poi, Player const* bot);

    // Weighted random POI index allowed for the bot (points within 250 yd preferred), -1 if none.
    int32 PickPoi(CityPois const& city, Player const* bot, int32 exclude);

    // Nearest allowed POI on the bot's map, -1 if none; *distance = 2D distance.
    int32 NearestPoi(CityPois const& city, Player const* bot, float* distance);

    // ------------------------------------------------------------------ registry
    struct CitizenView
    {
        uint32 zone = 0;                                // capital zone the citizen belongs to
        uint32 serial = 0;                              // changes on every new pin (resets the action)
    };

    // Thread-safe. False when the bot is not a citizen.
    bool GetCitizen(ObjectGuid::LowType guid, CitizenView& out);
    bool IsCitizen(ObjectGuid::LowType guid);

    // The citizen action reports its state for .citizen list (thread-safe, cheap).
    void ReportState(ObjectGuid::LowType guid, char const* state);

    // Emote towards a real player: at most one per CityEmoteCooldownSec per city. True = go ahead.
    bool TryCityEmote(uint32 zone);

    void AddScripts();
}

#endif
