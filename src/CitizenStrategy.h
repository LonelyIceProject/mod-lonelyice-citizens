/*
 * Living capitals - the "citizen" strategy, trigger and action.
 *
 * The manager (CityLife.cpp) pins a bot and decorates its non-combat engine with
 * "-new rpg,-rpg,-grind,-move random,-lfg,-bg,-start duel,-pvp,-travel,+citizen"; the combat engine is
 * untouched (citizens defend themselves but never start a fight). The trigger fires every 2 s out of
 * combat; the action is a small state machine:
 *   WAKE (0-20 s) -> STROLL to a weighted POI -> LINGER 20-90 s facing it -> next mood
 *   AFK 2-8 min (flag, stand or sit) | SIT 1-4 min (tavern chair within 15 yd, else the floor)
 *   SOCIAL 30-60 s next to another bot, 2-4 talk emotes | FOOL 10-20 s dance or one silly emote
 * Replies to player emotes stay with the stock "emote" strategy.
 */

#ifndef MOD_LONELYICE_CITIZEN_STRATEGY_H
#define MOD_LONELYICE_CITIZEN_STRATEGY_H

#include "CityLife.h"
#include "NewRpgBaseAction.h"
#include "Strategy.h"
#include "Trigger.h"

class PlayerbotAI;
class Unit;

namespace CityLife
{
    class CitizenStrategy : public Strategy
    {
    public:
        CitizenStrategy(PlayerbotAI* botAI) : Strategy(botAI) { }

        std::string const getName() override { return "citizen"; }
        uint32 GetType() const override { return STRATEGY_TYPE_NONCOMBAT; }
        void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    };

    class CitizenTrigger : public Trigger
    {
    public:
        CitizenTrigger(PlayerbotAI* botAI) : Trigger(botAI, "citizen", 2000) { }

        bool IsActive() override;
    };

    // One instance per bot (owned by its AiObjectContext): the per-bot runtime lives here.
    class CitizenAction : public NewRpgBaseAction
    {
    public:
        enum class State : uint8
        {
            Wake,
            Stroll,
            Linger,
            Afk,
            Sit,
            Social,
            Fool
        };

        CitizenAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "citizen") { }

        bool Execute(Event event) override;
        bool isUseful() override { return true; }

        // The trigger lets a moving bot through only while the citizen itself walks.
        bool OwnsMovement() const { return state == State::Stroll || state == State::Social; }

    private:
        void Restart(uint32 serial, uint32 now);
        void Enter(State next, uint32 now, uint32 durationMs);
        void LeaveCurrent();
        void PickNext(uint32 now, CitizenView const& view);

        bool StartStroll(uint32 now, CitizenView const& view, bool nearest);
        bool StartSocial(uint32 now);
        void StartAfk(uint32 now);
        void StartSit(uint32 now);
        void StartFool(uint32 now);

        bool DoStroll(uint32 now, CitizenView const& view);
        bool DoLinger(uint32 now, CitizenView const& view);
        bool DoSit(uint32 now, CitizenView const& view);
        bool DoSocial(uint32 now, CitizenView const& view);
        bool DoFool(uint32 now, CitizenView const& view);

        // Greets a real player who came close (limits of spec section 5). True when it emoted.
        bool MaybeGreet(uint32 now, uint32 zone);
        void SetAfk(bool on);
        void Face(float x, float y);
        void Report();

        State state = State::Wake;
        uint32 serial = 0;
        uint32 stateStart = 0;
        uint32 stateUntil = 0;

        // stroll / linger
        bool hasDest = false;
        uint16 destMap = 0;
        float destX = 0.0f, destY = 0.0f, destZ = 0.0f;
        float faceX = 0.0f, faceY = 0.0f;
        int32 lastPoi = -1;
        uint32 lingerEmoteAt = 0;             // 0 = none planned

        // sit
        ObjectGuid chair;
        bool seated = false;
        bool afkFlag = false;                 // this action set the AFK flag

        // social
        ObjectGuid partner;
        bool partnerReached = false;
        uint32 nextSocialEmote = 0;
        uint8 socialEmotesLeft = 0;

        // fool
        bool dancing = false;

        // greeting limits
        uint32 lastPlayerEmote = 0;
        ObjectGuid lastGreeted;
        uint32 lastGreetedAt = 0;
    };
}

#endif
