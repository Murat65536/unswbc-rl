#include "engine/scoring.h"

#include <algorithm>
#include <utility>

// NOTE: i know that this is inefficient since we will recompute the results even in rounds that haven't ended yet
// but might be nice to emit in vis
GameResult ResultAfterRound(GameState const& state, bool finalRound)
{
    GameResult result;
    for (Dragon const& dragon : state.mDragons)
    {
        if (!dragon.mAlive)
        {
            continue;
        }
        TeamStanding& standing = dragon.mTeam == Team::A ? result.mTeamA : result.mTeamB;
        int const length = static_cast<int>(dragon.mBody.size());
        standing.mDragonCount += 1;
        standing.mLongestDragon = std::max(standing.mLongestDragon, length);
        standing.mTotalLength += length;
    }

    bool const teamAEliminated = result.mTeamA.mDragonCount == 0;
    bool const teamBEliminated = result.mTeamB.mDragonCount == 0;
    if (teamAEliminated || teamBEliminated)
    {
        result.mTerminated = true;
        result.mEndReason = GameEndReason::TeamEliminated;
        if (teamAEliminated != teamBEliminated)
        {
            result.mWinner = teamAEliminated ? Team::B : Team::A;
        }
        return result;
    }

    bool const lastRound = finalRound || state.mRound + 1 >= MAX_ROUNDS;
    if (!lastRound)
    {
        return result;
    }

    result.mTerminated = true;
    result.mEndReason = GameEndReason::RoundLimit;
    auto const rank = [](TeamStanding const& standing) { return std::pair{standing.mLongestDragon, standing.mTotalLength}; };
    if (rank(result.mTeamA) > rank(result.mTeamB))
    {
        result.mWinner = Team::A;
    }
    else if (rank(result.mTeamB) > rank(result.mTeamA))
    {
        result.mWinner = Team::B;
    }
    return result;
}
