#pragma once

#include "event.h"
#include "types.h"

void InitPearlCountdowns(GameState& state, PearlRng& rng, EventSink const& emit);
void PearlTick(GameState& state, PearlRng& rng, EventSink const& emit);
