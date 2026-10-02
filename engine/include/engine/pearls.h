#pragma once

#include "event.h"
#include "types.h"

#include <random>

void InitPearlCountdowns(GameState& state, std::mt19937& rng, EventSink const& emit);
void PearlTick(GameState& state, std::mt19937& rng, EventSink const& emit);
