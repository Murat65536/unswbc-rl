#include "engine/sonar.h"
#include "engine/helpers.h"

SonarHitKind CastSonar(GameState& state, Dragon& dragon, Direction direction, uint64_t value, EventSink const& emit)
{
    bool const fromTail = direction == Opposite(dragon.mFacing) && dragon.mBody.size() > 1;
    Point const origin = fromTail ? dragon.mBody.back() : dragon.mBody.front();
    if (fromTail)
    {
        direction = DirectionOfStepBetween(state, dragon.mBody[dragon.mBody.size() - 2], origin).value_or(direction);
    }
    Point at = origin;
    Dragon* hit = nullptr;
    SonarHitKind kind = SonarHitKind::Empty;

    int const maxSteps = state.mWidth + state.mHeight;
    for (int step = 0; step < maxSteps && hit == nullptr; step++)
    {
        std::optional<Point> const next = TileAfterStep(state, at, direction);
        if (!next)
        {
            kind = SonarHitKind::Kelp;
            break;
        }
        at = *next;
        hit = AliveDragonOccupying(state, at);
    }

    if (hit)
    {
        hit->mSonarInbox.push_back(value);
        bool const isHead = IsHeadOf(*hit, at);
        if (hit->mTeam == dragon.mTeam)
        {
            kind = isHead ? SonarHitKind::AllyHead : SonarHitKind::Ally;
        }
        else
        {
            kind = isHead ? SonarHitKind::EnemyHead : SonarHitKind::Enemy;
        }
    }
    emit(
        EventSonarPing{dragon.mId, direction, value, origin, at, hit ? std::optional<DragonId>(hit->mId) : std::nullopt, kind});
    return kind;
}
