#include "engine/scoring.h"

#include <algorithm>
#include <tuple>
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
        // Dragons 0 and 1 are the queens: the map alternates teams line by line.
        if (dragon.mId <= 1)
        {
            standing.mQueenLength = length;
        }
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
    // 1.2.3 ranks the queen first, then the longest dragon, then total length.
    auto const rank = [](TeamStanding const& standing) {
        return std::tuple{standing.mQueenLength, standing.mLongestDragon, standing.mTotalLength};
    };
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
