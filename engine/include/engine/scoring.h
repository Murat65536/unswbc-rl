#pragma once

#include "types.h"

/// `finalRound` scores the game as over after this round, whatever the round.
GameResult ResultAfterRound(GameState const& state, bool finalRound = false);
