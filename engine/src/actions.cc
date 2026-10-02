#include "engine/actions.h"
#include "engine/helpers.h"

#include <algorithm>
#include <string>

void Move(GameState& state, Dragon& dragon, std::vector<Direction> const& steps, EventSink const& emit)
{
    for (size_t stepIndex = 0; stepIndex < steps.size(); stepIndex++)
    {
        bool const mustPayForStep = stepIndex > 0;
        if (mustPayForStep)
        {
            if (dragon.mBody.size() <= static_cast<size_t>(MIN_DRAGON_LENGTH))
            {
                emit(EventEngineLog{dragon.mId, "can't pay for step " + std::to_string(stepIndex + 1)});
                Kill(state, dragon, DragonDeathReason::NoValidAction, emit);
                return;
            }
        }

        Step(state, dragon, steps[stepIndex], mustPayForStep, emit);
        if (!dragon.mAlive)
        {
            return;
        }
    }
}

void Step(GameState& state, Dragon& dragon, Direction direction, bool mustPayForStep, EventSink const& emit)
{
    dragon.mFacing = direction;

    std::optional<Point> const destination = TileAfterStep(state, dragon.mBody.front(), direction);
    if (!destination)
    {
        Kill(state, dragon, DragonDeathReason::HitWall, emit);
        return;
    }

    if (std::find(dragon.mBody.begin(), dragon.mBody.end(), *destination) != dragon.mBody.end())
    {
        Kill(state, dragon, DragonDeathReason::HitSelf, emit);
        return;
    }

    if (Dragon* other = AliveDragonOccupying(state, *destination))
    {
        if (IsHeadOf(*other, *destination))
        {
            Kill(state, *other, DragonDeathReason::HitHeadToHead, emit);
            Kill(state, dragon, DragonDeathReason::HitHeadToHead, emit);
            return;
        }
        Kill(state, dragon, DragonDeathReason::HitOtherBody, emit);
        return;
    }

    dragon.mBody.push_front(*destination);
    Tile& tile = state.mTiles.At(*destination);
    if (tile.mHasPearl)
    {
        tile.mHasPearl = false;
        emit(EventTileChange{*destination, false});
    }
    else
    {
        dragon.mBody.pop_back();
    }

    if (mustPayForStep)
    {
        dragon.mBody.pop_back();
    }

    emit(EventDragonUpdate{dragon.mId, dragon.mFacing, dragon.mBody.front(), dragon.mBody.back()});
}

std::optional<DragonId> Split(GameState& state, Dragon& dragon, int childSegmentCount, EventSink const& emit)
{
    int const parentLength = static_cast<int>(dragon.mBody.size());
    // Checked before subtracting: a bot's INT_MIN would overflow it.
    if (childSegmentCount < MIN_DRAGON_LENGTH || childSegmentCount > parentLength - MIN_DRAGON_LENGTH)
    {
        emit(EventEngineLog{dragon.mId, "can't split " + std::to_string(childSegmentCount) + " segments off a length of " +
                                            std::to_string(parentLength)});
        Kill(state, dragon, DragonDeathReason::NoValidAction, emit);
        return std::nullopt;
    }

    int const unitCount = AliveUnitCount(state, dragon.mTeam);
    if (unitCount >= state.mUnitLimit)
    {
        emit(EventEngineLog{dragon.mId, "can't split: unit limit of " + std::to_string(state.mUnitLimit) + " reached"});
        Kill(state, dragon, DragonDeathReason::NoValidAction, emit);
        return std::nullopt;
    }

    Dragon child;
    child.mId = state.mNextDragonId++;
    child.mTeam = dragon.mTeam;
    child.mProtocolMajor = dragon.mProtocolMajor;
    child.mBody.assign(dragon.mBody.rbegin(), dragon.mBody.rbegin() + childSegmentCount);
    dragon.mBody.erase(dragon.mBody.begin() + (parentLength - childSegmentCount), dragon.mBody.end());

    std::optional<Direction> const childFacing = DirectionOfStepBetween(state, child.mBody[1], child.mBody[0]);
    RUNTIME_ASSERT(childFacing, "child segments are not adjacent");
    child.mFacing = *childFacing;

    emit(EventDragonSplit{dragon.mId, child.mId, child.mTeam, child.mFacing,
                          std::vector<Point>(dragon.mBody.begin(), dragon.mBody.end()),
                          std::vector<Point>(child.mBody.begin(), child.mBody.end())});

    state.mDragons.push_back(std::move(child));
    return state.mDragons.back().mId;
}

void Kill(GameState& state, Dragon& dragon, DragonDeathReason reason, EventSink const& emit)
{
    emit(EventDragonDeath{dragon.mId, reason, dragon.mTeam});

    size_t const length = dragon.mBody.size();
    for (size_t segment = 0; segment < length; segment += 2)
    {
        Point const where = dragon.mBody[segment];
        state.mTiles.At(where).mHasPearl = true;
        emit(EventTileChange{where, true});
    }

    dragon.mAlive = false;
}
