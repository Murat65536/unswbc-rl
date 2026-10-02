#pragma once

// The training side of the shared core: a View built straight from the
// engine's state, with no text in between. It must equal what the bot's
// BlockReader makes of BuildInitBlock + BuildRoundBlock for the same dragon
// at the same moment; tests/test_core_contract.py checks that it does.

#include "core/view.h"

#include "engine/types.h"

namespace core {

/// The View `dragon` would be sent at the start of its turn now (before the
/// turn empties its sonar inbox).
void ViewFromState(GameState const& state, Dragon const& dragon, View& view);

/// Both teams' standings, from one team's side.
struct Standings
{
    int mUnits[2] = {0, 0};
    int mTotal[2] = {0, 0};
    int mLongest[2] = {0, 0};
    int mQueen[2] = {0, 0};
};

/// [0] is `team`, [1] its opponent.
Standings StandingsFor(GameState const& state, Team team);

/// Global facts no dragon can see, for the critic only (the actor never gets
/// them, and the bot never computes them): kPrivileged floats.
constexpr int kPrivileged = 16;
void PrivilegedFeatures(GameState const& state, Dragon const& dragon, Standings const& standings, float* out);

} // namespace core
