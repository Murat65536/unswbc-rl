#include "engine/game.h"
#include "replay.capnp.h"

#include <memory>
#include <string_view>

namespace {

template <typename CapnpEnum, typename CharEnum> CapnpEnum OrdinalOf(std::string_view order, CharEnum value)
{
    return static_cast<CapnpEnum>(order.find(static_cast<char>(value)));
}

replay::Team ToCapnp(Team team)
{
    return OrdinalOf<replay::Team>("AB", team);
}

replay::Direction ToCapnp(Direction direction)
{
    return OrdinalOf<replay::Direction>("NESW", direction);
}

replay::SonarHitKind ToCapnp(SonarHitKind kind)
{
    return OrdinalOf<replay::SonarHitKind>("?.waAeE", kind);
}

replay::DragonDeathReason ToCapnp(DragonDeathReason reason)
{
    return OrdinalOf<replay::DragonDeathReason>("WSOHA", reason);
}

void FillCapnp(replay::Point::Builder out, Point point)
{
    out.setX(point.x);
    out.setY(point.y);
}

void FillCapnp(capnp::List<replay::Point>::Builder out, std::vector<Point> const& points)
{
    for (size_t i = 0; i < points.size(); i++)
    {
        FillCapnp(out[i], points[i]);
    }
}

void FillCapnp(replay::PlayerAction::Builder out, ActionMove const& move)
{
    capnp::List<replay::Direction>::Builder steps = out.initMove(move.mSteps.size());
    for (size_t i = 0; i < move.mSteps.size(); i++)
    {
        steps.set(i, ToCapnp(move.mSteps[i]));
    }
}

void FillCapnp(replay::PlayerAction::Builder out, ActionSplit const& split)
{
    out.setSplit(split.mChildSegmentCount);
}

void FillCapnp(replay::PlayerAction::Builder out, ActionSuicide const&)
{
    out.setSuicide();
}

void FillCapnp(replay::DebugDraw::Builder out, DebugDraw const& draw)
{
    out.setShape(static_cast<replay::DebugShape>(draw.mShape));
    FillCapnp(out.initFrom(), draw.mFrom);
    FillCapnp(out.initTo(), draw.mTo);
    out.setRed(draw.mRed);
    out.setGreen(draw.mGreen);
    out.setBlue(draw.mBlue);
}

void FillCapnp(replay::Event::Builder out, EventRoundStart const& e)
{
    out.initRoundStart().setRound(e.mRound);
}

void FillCapnp(replay::Event::Builder out, EventTurnStart const& e)
{
    out.initTurnStart().setId(e.mId);
}

void FillCapnp(replay::Event::Builder out, EventPearlCountdown const& e)
{
    replay::EventPearlCountdown::Builder b = out.initPearlCountdown();
    FillCapnp(b.initTile(), e.mTile);
    b.setCountdown(e.mCountdown);
}

void FillCapnp(replay::Event::Builder out, EventTileChange const& e)
{
    replay::EventTileChange::Builder b = out.initTileChange();
    FillCapnp(b.initTile(), e.mTile);
    b.setHasPearl(e.mHasPearl);
}

void FillCapnp(replay::Event::Builder out, EventDragonAction const& e)
{
    replay::EventDragonAction::Builder b = out.initDragonAction();
    b.setId(e.mId);
    if (e.mAction)
    {
        std::visit([&](auto const& action) { FillCapnp(b.initAction(), action); }, *e.mAction);
    }
    if (e.mInstructions)
    {
        auto usage = b.initInstructions();
        usage.setCount(e.mInstructions->mCount);
        usage.setExceeded(e.mInstructions->mExceeded);
    }
    b.setTle(e.mTle);
}

void FillCapnp(replay::Event::Builder out, EventEngineLog const& e)
{
    replay::EventEngineLog::Builder b = out.initEngineLog();
    b.setId(e.mId);
    b.setText(e.mText.c_str());
}

void FillCapnp(replay::Event::Builder out, EventDragonLog const& e)
{
    replay::EventDragonLog::Builder b = out.initDragonLog();
    b.setId(e.mId);
    b.setText(e.mText.c_str());
}

void FillCapnp(replay::Event::Builder out, EventDragonIndicator const& e)
{
    replay::EventDragonIndicator::Builder b = out.initDragonIndicator();
    b.setId(e.mId);
    b.setText(e.mText.c_str());
}

void FillCapnp(replay::Event::Builder out, EventDebugDraw const& e)
{
    replay::EventDebugDraw::Builder b = out.initDebugDraw();
    b.setId(e.mId);
    FillCapnp(b.initDraw(), e.mDraw);
}

void FillCapnp(replay::Event::Builder out, EventDragonUpdate const& e)
{
    replay::EventDragonUpdate::Builder b = out.initDragonUpdate();
    b.setId(e.mId);
    b.setFacing(ToCapnp(e.mFacing));
    FillCapnp(b.initHead(), e.mHead);
    FillCapnp(b.initTail(), e.mTail);
}

void FillCapnp(replay::Event::Builder out, EventDragonSplit const& e)
{
    replay::EventDragonSplit::Builder b = out.initDragonSplit();
    b.setParentId(e.mParentId);
    b.setChildId(e.mChildId);
    b.setTeam(ToCapnp(e.mTeam));
    b.setChildFacing(ToCapnp(e.mChildFacing));
    FillCapnp(b.initParentBody(e.mParentBody.size()), e.mParentBody);
    FillCapnp(b.initChildBody(e.mChildBody.size()), e.mChildBody);
}

void FillCapnp(replay::Event::Builder out, EventDragonDeath const& e)
{
    replay::EventDragonDeath::Builder b = out.initDragonDeath();
    b.setId(e.mId);
    b.setReason(ToCapnp(e.mReason));
}

void FillCapnp(replay::Event::Builder out, EventSonarPing const& e)
{
    replay::EventSonarPing::Builder b = out.initSonarPing();
    b.setSenderId(e.mSenderId);
    b.setDirection(ToCapnp(e.mDirection));
    b.setValue64(e.mValue);
    b.setHitKind(ToCapnp(e.mHitKind));
    FillCapnp(b.initOrigin(), e.mOrigin);
    FillCapnp(b.initEnd(), e.mEnd);
    if (e.mHitId)
    {
        b.setHitId(*e.mHitId);
    }
    else
    {
        b.setNoHit();
    }
}

void FillCapnp(replay::TeamStanding::Builder out, TeamStanding const& standing)
{
    out.setDragonCount(standing.mDragonCount);
    out.setLongestDragon(standing.mLongestDragon);
    out.setTotalLength(standing.mTotalLength);
}

void FillCapnp(replay::GameResult::Builder out, GameResult const& result)
{
    out.setTerminated(result.mTerminated);
    out.setEndReason(static_cast<replay::GameEndReason>(result.mEndReason));
    if (result.mWinner)
    {
        out.setWinner(ToCapnp(*result.mWinner));
    }
    else
    {
        out.setNoWinner();
    }
    FillCapnp(out.initTeamA(), result.mTeamA);
    FillCapnp(out.initTeamB(), result.mTeamB);
}

} // namespace

void Game::WriteReplay(capnp::MessageBuilder& message, std::string const& mapText,
                       std::pair<std::string, std::string> const& teamNames) const
{
    replay::Replay::Builder replay = message.initRoot<replay::Replay>();
    replay.setFormatVersion(replay::REPLAY_FORMAT_VERSION);
    replay.setMap(mapText.c_str());
    replay.setBotA(teamNames.first.c_str());
    replay.setBotB(teamNames.second.c_str());

    capnp::List<replay::Event>::Builder out = replay.initEvents(mEvents.size());
    for (size_t i = 0; i < mEvents.size(); i++)
    {
        std::visit([&](auto const& event) { FillCapnp(out[i], event); }, mEvents[i]);
    }

    FillCapnp(replay.initResult(), mResult);
}
