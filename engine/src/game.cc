#include "engine/game.h"
#include "engine/actions.h"
#include "engine/helpers.h"
#include "engine/pearls.h"
#include "engine/protocol.h"
#include "engine/scoring.h"
#include "engine/sonar.h"

#include <array>
#include <format>
#include <utility>

namespace {

struct TeamOutput
{
    int mNotes = 0;
    int64_t mText = 0;
    bool mFull = false;
};

// The dragon behind an event that is bot output, and the length of its text.
std::optional<std::pair<DragonId, size_t>> OutputOf(Event const& event)
{
    if (auto const* e = std::get_if<EventDragonLog>(&event))
    {
        return std::pair{e->mId, e->mText.size()};
    }
    if (auto const* e = std::get_if<EventDragonIndicator>(&event))
    {
        return std::pair{e->mId, e->mText.size()};
    }
    if (auto const* e = std::get_if<EventEngineLog>(&event))
    {
        return std::pair{e->mId, e->mText.size()};
    }
    if (auto const* e = std::get_if<EventDebugDraw>(&event))
    {
        return std::pair{e->mId, size_t{0}};
    }
    return std::nullopt;
}

} // namespace

Game::Game(GameState state, DebugOutput keep) : mState(std::move(state)), mKeep(keep)
{
    mEmit = [this, spent = std::array<TeamOutput, 2>{}](Event const& event) mutable {
        auto const record = [this](Event const& e) {
            mEvents.push_back(e);
            if (mSink)
            {
                mSink(e);
            }
        };
        std::optional<std::pair<DragonId, size_t>> const output = mKeep.mLimits ? OutputOf(event) : std::nullopt;
        if (output)
        {
            TeamOutput& team = spent[DragonById(mState, output->first)->mTeam == Team::A ? 0 : 1];
            if (team.mFull)
            {
                return;
            }
            team.mText += static_cast<int64_t>(output->second);
            if (++team.mNotes > MAX_TEAM_NOTES || team.mText > MAX_TEAM_TEXT)
            {
                team.mFull = true;
                record(EventEngineLog{output->first,
                                      std::format("debug output limit reached ({} lines or {} MB per team), the rest of "
                                                  "this team's is dropped",
                                                  MAX_TEAM_NOTES, MAX_TEAM_TEXT >> 20)});
                return;
            }
        }
        record(event);
    };
}

void Game::SpawnControllersWith(ControllerFactory factory)
{
    mSpawnController = std::move(factory);
}

void Game::OnEvent(EventSink sink)
{
    mSink = std::move(sink);
}

void Game::RecordInstructions(uint64_t count, bool exceeded, bool tle)
{
    mTurnInstructions = InstructionUsage{count, exceeded};
    mTurnTle = tle;
}

GameState const& Game::State() const
{
    return mState;
}

std::vector<Event> const& Game::Events() const
{
    return mEvents;
}

GameResult Game::Run(std::function<bool()> const& stop)
{
    for (Dragon const& dragon : mState.mDragons)
    {
        mControllers[dragon.mId] = mSpawnController(dragon);
    }

    InitPearlCountdowns(mState, mRng, mEmit);
    for (Dragon const& dragon : mState.mDragons)
    {
        mEmit(EventDragonUpdate{dragon.mId, dragon.mFacing, dragon.mBody.front(), dragon.mBody.back()});
    }

    for (mState.mRound = 0; mState.mRound < MAX_ROUNDS; mState.mRound++)
    {
        mEmit(EventRoundStart{mState.mRound});
        PearlTick(mState, mRng, mEmit);

        for (size_t i = 0; i < mState.mDragons.size(); i++)
        {
            TakeTurn(mState.mDragons[i].mId);
        }

        mResult = ResultAfterRound(mState);
        if (!mResult.mTerminated && stop && stop())
        {
            mResult = ResultAfterRound(mState, true);
        }
        if (mResult.mTerminated)
        {
            break;
        }
    }
    return mResult;
}

void Game::TakeTurn(DragonId id)
{
    Dragon* dragon = DragonById(mState, id);
    if (!dragon->mAlive)
    {
        return;
    }

    mEmit(EventTurnStart{id});
    std::string const roundBlock = BuildRoundBlock(mState, *dragon);
    dragon->mSonarInbox.clear();
    dragon->mSonarEchoes = {};
    mTurnInstructions.reset();
    mTurnTle = false;
    std::string const replyText = mControllers.at(id)(roundBlock);
    ControllerReply const reply = ReadReply(*dragon, replyText, mKeep, mEmit);
    if (reply.mProtocolMajor)
    {
        dragon->mProtocolMajor = *reply.mProtocolMajor;
    }

    if (reply.mIndicator)
    {
        mEmit(EventDragonIndicator{id, *reply.mIndicator});
    }
    mEmit(EventDragonAction{id, mTurnTle ? std::nullopt : std::optional<PlayerAction>(reply.mAction), mTurnInstructions,
                            mTurnTle});

    if (ActionMove const* move = std::get_if<ActionMove>(&reply.mAction))
    {
        Move(mState, *dragon, move->mSteps, mEmit);
    }
    else if (ActionSplit const* split = std::get_if<ActionSplit>(&reply.mAction))
    {
        if (std::optional<DragonId> childId = Split(mState, *dragon, split->mChildSegmentCount, mEmit))
        {
            Dragon const& child = *DragonById(mState, *childId);
            mControllers[child.mId] = mSpawnController(child);
        }
    }
    else
    {
        Kill(mState, *dragon, DragonDeathReason::NoValidAction, mEmit);
    }

    dragon = DragonById(mState, id);
    if (!dragon->mAlive)
    {
        return;
    }
    std::map<Direction, uint64_t> sonars = reply.mDirectedSonars;
    if (reply.mSonar)
    {
        sonars.try_emplace(dragon->mFacing, *reply.mSonar);
    }
    for (Direction const direction : ALL_DIRECTIONS)
    {
        if (auto const sonar = sonars.find(direction); sonar != sonars.end())
        {
            SonarHitKind const kind = CastSonar(mState, *dragon, direction, sonar->second, mEmit);
            if (size_t const echo = SONAR_ECHO_KIND_ORDER.find(static_cast<char>(kind)); echo != std::string_view::npos)
            {
                dragon->mSonarEchoes[echo]++;
            }
        }
    }
}
