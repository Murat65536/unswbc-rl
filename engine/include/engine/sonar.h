#pragma once

#include "event.h"
#include "types.h"

SonarHitKind CastSonar(GameState& state, Dragon& dragon, Direction direction, uint64_t value, EventSink const& emit);
