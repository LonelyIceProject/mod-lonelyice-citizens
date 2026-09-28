/*
 * Living capitals - "citizen" strategy / trigger / action and their
 * registration in the playerbots contexts. Runs on the bot's map thread; the registry is only read
 * through the thread-safe CityLife functions.
 */

#include "CitizenStrategy.h"

#include "Cell.h"
#include "CellImpl.h"
#include "EmoteAction.h"
#include "ExternalContexts.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Random.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <cmath>
#include <iterator>

namespace CityLife
{
    namespace
    {
        constexpr float ARRIVE_DISTANCE = 2.5f;
        constexpr float SOCIAL_RANGE = 30.0f;
        constexpr float SOCIAL_STAND = 2.5f;
        constexpr float CHAIR_RANGE = 15.0f;
        constexpr float GREET_RANGE = 8.0f;
        constexpr uint32 GREET_SAME_PLAYER_MS = 10 * MINUTE * IN_MILLISECONDS;
        constexpr uint32 STROLL_GIVE_UP_MS = 90 * IN_MILLISECONDS;

        bool Expired(uint32 until, uint32 now)
        {
            return int32(now - until) >= 0;
        }

        // "stroll:bank", "linger:mailbox", ...: string literals for the registry (list output).
        char const* StateName(CitizenAction::State state, uint8 kind)
        {
            static std::vector<std::string> const strolls = []
            {
                std::vector<std::string> v;
                for (uint8 k = 0; k < 16; ++k)
                    v.push_back(std::string("stroll:") + PoiKindName(k));
                return v;
            }();
            static std::vector<std::string> const lingers = []
            {
                std::vector<std::string> v;
                for (uint8 k = 0; k < 16; ++k)
                    v.push_back(std::string("linger:") + PoiKindName(k));
                return v;
            }();

            switch (state)
            {
                case CitizenAction::State::Wake:   return "wake";
                case CitizenAction::State::Stroll: return strolls[kind & 15].c_str();
                case CitizenAction::State::Linger: return lingers[kind & 15].c_str();
                case CitizenAction::State::Afk:    return "afk";
                case CitizenAction::State::Sit:    return "sit";
                case CitizenAction::State::Social: return "social";
                case CitizenAction::State::Fool:   return "fool";
            }
            return "?";
        }

        // Nearest spawned chair (tavern benches, stools) within range.
        struct NearestChairCheck
        {
            WorldObject const* source;
            float range;

            bool operator()(GameObject* go)
            {
                if (go->GetGoType() != GAMEOBJECT_TYPE_CHAIR || !go->isSpawned())
                    return false;

                if (std::fabs(go->GetPositionZ() - source->GetPositionZ()) > 3.0f || !source->IsWithinDistInMap(go, range))
                    return false;

                range = source->GetDistance(go);   // later matches must be closer
                return true;
            }
        };

        void SendTextEmote(Player* bot, uint32 textEmote, ObjectGuid target)
        {
            WorldPacket data(SMSG_TEXT_EMOTE);
            data << textEmote;
            data << EmoteActionBase::GetNumberOfEmoteVariants(TextEmotes(textEmote), bot->getRace(), bot->getGender());
            data << target;
            bot->GetSession()->HandleTextEmoteOpcode(data);
        }
    }

    // ================================================================== strategy / trigger
    void CitizenStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
    {
        triggers.push_back(new TriggerNode("citizen", { NextAction("citizen", 5.0f) }));
    }

    bool CitizenTrigger::IsActive()
    {
        CitizenView view;
        if (!bot->IsInWorld() || bot->IsBeingTeleported() || !GetCitizen(bot->GetGUID().GetCounter(), view))
            return false;

        if (!bot->IsAlive() || bot->IsInCombat() || bot->GetGroup() || bot->IsInFlight())
            return false;

        // not while something else moves the bot; its own walks must be tracked though
        if (bot->isMoving())
        {
            CitizenAction* action = dynamic_cast<CitizenAction*>(context->GetAction("citizen"));
            return action && action->OwnsMovement();
        }

        return true;
    }

    // ================================================================== action
    bool CitizenAction::Execute(Event /*event*/)
    {
        CitizenView view;
        if (!GetCitizen(bot->GetGUID().GetCounter(), view))
            return false;

        uint32 const now = getMSTime();
        if (view.serial != serial)
            Restart(view.serial, now);

        if (state == State::Wake)
        {
            if (!Expired(stateUntil, now))
                return false;

            PickNext(now, view);
            return true;
        }

        // outside the city (knocked back, fled a fight...): walk back; the manager teleports when far
        if (bot->GetZoneId() != view.zone && state != State::Stroll)
        {
            LeaveCurrent();
            if (!StartStroll(now, view, true))
                Enter(State::Wake, now, 10 * IN_MILLISECONDS);
            return true;
        }

        switch (state)
        {
            case State::Stroll: return DoStroll(now, view);
            case State::Linger: return DoLinger(now, view);
            case State::Sit:    return DoSit(now, view);
            case State::Social: return DoSocial(now, view);
            case State::Fool:   return DoFool(now, view);
            case State::Afk:
                if (Expired(stateUntil, now))
                    PickNext(now, view);
                return false;
            default:
                return false;
        }
    }

    void CitizenAction::Restart(uint32 newSerial, uint32 now)
    {
        LeaveCurrent();
        serial = newSerial;
        hasDest = false;
        lastPoi = -1;
        lastPlayerEmote = 0;
        lastGreeted.Clear();
        lastGreetedAt = 0;

        // AFK left over from the "minimal" activity mode before the pin
        if (bot->isAFK())
            bot->ToggleAFK();

        // spec 5: random delay of the first action after waking up
        Enter(State::Wake, now, urand(0, 20 * IN_MILLISECONDS));
    }

    void CitizenAction::Enter(State next, uint32 now, uint32 durationMs)
    {
        state = next;
        stateStart = now;
        stateUntil = now + durationMs;
        Report();
    }

    void CitizenAction::Report()
    {
        uint8 kind = 0;
        if (state == State::Stroll || state == State::Linger)
        {
            CitizenView view;
            if (GetCitizen(bot->GetGUID().GetCounter(), view) && lastPoi >= 0)
                if (std::shared_ptr<CityPois const> pois = GetPois(view.zone))
                    if (size_t(lastPoi) < pois->pois.size())
                        kind = pois->pois[lastPoi].kind;
        }

        ReportState(bot->GetGUID().GetCounter(), StateName(state, kind));
    }

    void CitizenAction::LeaveCurrent()
    {
        if (afkFlag)
            SetAfk(false);

        if (seated || chair)
        {
            if (bot->IsAlive() && bot->getStandState() != UNIT_STAND_STATE_STAND)
                bot->SetStandState(UNIT_STAND_STATE_STAND);
            seated = false;
            chair.Clear();
        }

        if (dancing)
        {
            bot->SetEmoteState(EMOTE_ONESHOT_NONE);
            dancing = false;
        }

        partner.Clear();
        partnerReached = false;
        socialEmotesLeft = 0;
        lingerEmoteAt = 0;
    }

    void CitizenAction::PickNext(uint32 now, CitizenView const& view)
    {
        LeaveCurrent();

        Config const& cfg = GetConfig();
        uint32 total = 0;
        for (uint32 w : cfg.weights)
            total += w;

        uint8 mood = MOOD_STROLL;
        if (total)
        {
            uint32 roll = urand(1, total);
            for (uint8 i = 0; i < MOOD_MAX; ++i)
            {
                if (roll <= cfg.weights[i])
                {
                    mood = i;
                    break;
                }
                roll -= cfg.weights[i];
            }
        }

        switch (mood)
        {
            case MOOD_AFK:
                StartAfk(now);
                return;
            case MOOD_SIT:
                StartSit(now);
                return;
            case MOOD_FOOL:
                StartFool(now);
                return;
            case MOOD_SOCIAL:
                if (StartSocial(now))
                    return;
                break;   // nobody around: stroll instead
            default:
                break;
        }

        if (!StartStroll(now, view, false))
            Enter(State::Wake, now, urand(10, 20) * IN_MILLISECONDS);
    }

    // ------------------------------------------------------------------ STROLL / LINGER
    bool CitizenAction::StartStroll(uint32 now, CitizenView const& view, bool nearest)
    {
        std::shared_ptr<CityPois const> pois = GetPois(view.zone);
        if (!pois || pois->pois.empty())
            return false;

        int32 const index = nearest ? NearestPoi(*pois, bot, nullptr) : PickPoi(*pois, bot, lastPoi);
        if (index < 0)
            return false;

        Poi const& poi = pois->pois[index];
        hasDest = true;
        destMap = poi.map;
        destX = poi.x;
        destY = poi.y;
        destZ = poi.z;
        faceX = poi.faceX;
        faceY = poi.faceY;
        lastPoi = index;
        Enter(State::Stroll, now, STROLL_GIVE_UP_MS);
        return true;
    }

    bool CitizenAction::DoStroll(uint32 now, CitizenView const& view)
    {
        if (!hasDest || bot->GetMapId() != destMap)
        {
            // another continent: the manager's leash teleports it back
            Enter(State::Wake, now, 5 * IN_MILLISECONDS);
            return false;
        }

        if (bot->GetExactDist2d(destX, destY) <= ARRIVE_DISTANCE && std::fabs(bot->GetPositionZ() - destZ) < 5.0f)
        {
            if (bot->isMoving())
                bot->StopMoving();

            Face(faceX, faceY);
            uint32 const duration = urand(20, 90) * IN_MILLISECONDS;
            Enter(State::Linger, now, duration);
            // spec 5: 30% one "talking" emote while standing there
            lingerEmoteAt = urand(0, 99) < 30 ? now + urand(2 * IN_MILLISECONDS, duration - IN_MILLISECONDS) : 0;
            return true;
        }

        if (Expired(stateUntil, now))
        {
            PickNext(now, view);   // unreachable in 90 s: give up
            return false;
        }

        return MoveFarTo(WorldPosition(destMap, destX, destY, destZ));
    }

    bool CitizenAction::DoLinger(uint32 now, CitizenView const& view)
    {
        if (Expired(stateUntil, now))
        {
            PickNext(now, view);
            return false;
        }

        if (MaybeGreet(now, view.zone))
            return true;

        if (lingerEmoteAt && Expired(lingerEmoteAt, now))
        {
            lingerEmoteAt = 0;
            Face(faceX, faceY);
            bot->HandleEmoteCommand(TalkAction::GetRandomEmote(nullptr, false));
            return true;
        }

        return false;
    }

    // ------------------------------------------------------------------ AFK / SIT
    void CitizenAction::StartAfk(uint32 now)
    {
        Enter(State::Afk, now, urand(2 * MINUTE, 8 * MINUTE) * IN_MILLISECONDS);
        SetAfk(true);
        if (urand(0, 99) < 40)
        {
            bot->SetStandState(UNIT_STAND_STATE_SIT);
            seated = true;
        }
    }

    void CitizenAction::StartSit(uint32 now)
    {
        Enter(State::Sit, now, urand(1 * MINUTE, 4 * MINUTE) * IN_MILLISECONDS);

        GameObject* found = nullptr;
        NearestChairCheck check{ bot, CHAIR_RANGE };
        Acore::GameObjectLastSearcher<NearestChairCheck> searcher(bot, found, check);
        Cell::VisitObjects(bot, searcher, CHAIR_RANGE);

        if (found)
        {
            chair = found->GetGUID();
            return;
        }

        bot->SetStandState(UNIT_STAND_STATE_SIT);
        seated = true;
    }

    bool CitizenAction::DoSit(uint32 now, CitizenView const& view)
    {
        if (Expired(stateUntil, now))
        {
            PickNext(now, view);
            return false;
        }

        if (!chair || seated)
            return false;

        GameObject* go = ObjectAccessor::GetGameObject(*bot, chair);
        if (!go || !go->isSpawned() || getMSTimeDiff(stateStart, now) > 30 * IN_MILLISECONDS)
        {
            // chair gone or not reachable: the floor will do
            chair.Clear();
            bot->SetStandState(UNIT_STAND_STATE_SIT);
            seated = true;
            return true;
        }

        if (bot->GetExactDist2d(go) > 2.0f)
            return MoveTo(go->GetMapId(), go->GetPositionX(), go->GetPositionY(), go->GetPositionZ(), false, false,
                          false, true);

        if (bot->isMoving())
            bot->StopMoving();

        go->Use(bot);   // picks a free slot, places the bot on it and sets the sitting stand state
        seated = true;
        return true;
    }

    // ------------------------------------------------------------------ SOCIAL
    bool CitizenAction::StartSocial(uint32 now)
    {
        Player* best = nullptr;
        bool bestCitizen = false;
        float bestDist = 0.0f;

        for (ObjectGuid const& guid : AI_VALUE(GuidVector, "nearest friendly players"))
        {
            Unit* unit = botAI->GetUnit(guid);
            Player* other = unit ? unit->ToPlayer() : nullptr;
            if (!other || other == bot || !other->IsAlive() || other->IsInCombat() || other->IsInFlight())
                continue;

            // talk to bots only (a real player gets at most a greeting)
            if (!GET_PLAYERBOT_AI(other) || other->GetMapId() != bot->GetMapId())
                continue;

            float const dist = bot->GetExactDist2d(other);
            if (dist > SOCIAL_RANGE)
                continue;

            bool const citizen = IsCitizen(other->GetGUID().GetCounter());
            if (!best || (citizen && !bestCitizen) || (citizen == bestCitizen && dist < bestDist))
            {
                best = other;
                bestCitizen = citizen;
                bestDist = dist;
            }
        }

        if (!best)
            return false;

        partner = best->GetGUID();
        partnerReached = false;
        socialEmotesLeft = uint8(urand(2, 4));
        nextSocialEmote = 0;
        Enter(State::Social, now, urand(30, 60) * IN_MILLISECONDS);
        return true;
    }

    bool CitizenAction::DoSocial(uint32 now, CitizenView const& view)
    {
        Player* other = ObjectAccessor::GetPlayer(*bot, partner);
        if (Expired(stateUntil, now) || !other || !other->IsAlive() || other->GetMapId() != bot->GetMapId() ||
            bot->GetExactDist2d(other) > SOCIAL_RANGE + 10.0f)
        {
            PickNext(now, view);
            return false;
        }

        if (!partnerReached)
        {
            if (bot->GetExactDist2d(other) <= SOCIAL_STAND + 0.8f)
            {
                partnerReached = true;
                if (bot->isMoving())
                    bot->StopMoving();
                Face(other->GetPositionX(), other->GetPositionY());
                nextSocialEmote = now + urand(1, 4) * IN_MILLISECONDS;
                return true;
            }

            if (getMSTimeDiff(stateStart, now) > 20 * IN_MILLISECONDS)
            {
                PickNext(now, view);   // could not get there
                return false;
            }

            float const angle = other->GetAngle(bot);
            return MoveTo(other->GetMapId(), other->GetPositionX() + std::cos(angle) * SOCIAL_STAND,
                          other->GetPositionY() + std::sin(angle) * SOCIAL_STAND, other->GetPositionZ(), false, false,
                          false, true);
        }

        if (!socialEmotesLeft || !Expired(nextSocialEmote, now))
            return false;

        static uint32 const talk[] = { EMOTE_ONESHOT_TALK, EMOTE_ONESHOT_TALK, EMOTE_ONESHOT_LAUGH, EMOTE_ONESHOT_POINT,
                                       EMOTE_ONESHOT_BOW, EMOTE_ONESHOT_EXCLAMATION, EMOTE_ONESHOT_QUESTION };
        Face(other->GetPositionX(), other->GetPositionY());
        if (!other->isMoving() && !other->HasInArc(float(M_PI) / 2.0f, bot))
            other->SetFacingToObject(bot);   // same map thread: the partner is a bot here
        bot->HandleEmoteCommand(talk[urand(0, std::size(talk) - 1)]);
        --socialEmotesLeft;
        nextSocialEmote = now + urand(6, 14) * IN_MILLISECONDS;
        return true;
    }

    // ------------------------------------------------------------------ FOOL
    void CitizenAction::StartFool(uint32 now)
    {
        if (urand(0, 1))
        {
            Enter(State::Fool, now, urand(10, 20) * IN_MILLISECONDS);
            bot->SetEmoteState(EMOTE_STATE_DANCE);
            dancing = true;
            return;
        }

        static uint32 const silly[] = { EMOTE_ONESHOT_FLEX, EMOTE_ONESHOT_CHEER, EMOTE_ONESHOT_CHICKEN, EMOTE_ONESHOT_ROAR };
        Enter(State::Fool, now, 6 * IN_MILLISECONDS);
        bot->HandleEmoteCommand(silly[urand(0, std::size(silly) - 1)]);
    }

    bool CitizenAction::DoFool(uint32 now, CitizenView const& view)
    {
        if (Expired(stateUntil, now))
            PickNext(now, view);
        return false;
    }

    // ------------------------------------------------------------------ helpers
    bool CitizenAction::MaybeGreet(uint32 now, uint32 zone)
    {
        Config const& cfg = GetConfig();
        if (lastPlayerEmote && getMSTimeDiff(lastPlayerEmote, now) < cfg.emoteToPlayerCooldownSec * IN_MILLISECONDS)
            return false;

        Player* target = nullptr;
        for (ObjectGuid const& guid : AI_VALUE(GuidVector, "nearest friendly players"))
        {
            Unit* unit = botAI->GetUnit(guid);
            Player* other = unit ? unit->ToPlayer() : nullptr;
            if (!other || !IsRealPlayer(other) || !other->IsAlive() || (other->IsGameMaster() && !other->isGMVisible()))
                continue;

            if (bot->GetExactDist2d(other) > GREET_RANGE)
                continue;

            if (other->GetGUID() == lastGreeted && getMSTimeDiff(lastGreetedAt, now) < GREET_SAME_PLAYER_MS)
                continue;

            target = other;
            break;
        }

        if (!target || !TryCityEmote(zone))
            return false;

        Face(target->GetPositionX(), target->GetPositionY());
        SendTextEmote(bot, urand(0, 1) ? TEXT_EMOTE_HELLO : TEXT_EMOTE_WAVE, target->GetGUID());
        lastPlayerEmote = now;
        lastGreeted = target->GetGUID();
        lastGreetedAt = now;
        return true;
    }

    void CitizenAction::SetAfk(bool on)
    {
        if (bot->isAFK() != on)
            bot->ToggleAFK();
        afkFlag = on;
    }

    void CitizenAction::Face(float x, float y)
    {
        if (!bot->isMoving())
            bot->SetFacingTo(bot->GetAngle(x, y));
    }

    // ================================================================== contexts
    namespace
    {
        class CitizenStrategyContext : public NamedObjectContext<Strategy>
        {
        public:
            CitizenStrategyContext() : NamedObjectContext<Strategy>(false, false)
            {
                creators["citizen"] = [](PlayerbotAI* botAI) -> Strategy* { return new CitizenStrategy(botAI); };
            }
        };

        class CitizenActionContext : public NamedObjectContext<Action>
        {
        public:
            CitizenActionContext() : NamedObjectContext<Action>(false, false)
            {
                creators["citizen"] = [](PlayerbotAI* botAI) -> Action* { return new CitizenAction(botAI); };
            }
        };

        class CitizenTriggerContext : public NamedObjectContext<Trigger>
        {
        public:
            CitizenTriggerContext() : NamedObjectContext<Trigger>(false, false)
            {
                creators["citizen"] = [](PlayerbotAI* botAI) -> Trigger* { return new CitizenTrigger(botAI); };
            }
        };
    }
}

void RegisterCitizenContexts()
{
    using namespace CityLife;
    PlayerbotExternalContexts::Register<Strategy>([]() -> NamedObjectContext<Strategy>* { return new CitizenStrategyContext(); });
    PlayerbotExternalContexts::Register<Action>([]() -> NamedObjectContext<Action>* { return new CitizenActionContext(); });
    PlayerbotExternalContexts::Register<Trigger>([]() -> NamedObjectContext<Trigger>* { return new CitizenTriggerContext(); });
}
