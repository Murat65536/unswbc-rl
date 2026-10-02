#pragma once

#include "event.h"
#include "types.h"

#include <optional>
#include <vector>

void Move(GameState& state, Dragon& dragon, std::vector<Direction> const& steps, EventSink const& emit);
void Step(GameState& state, Dragon& dragon, Direction direction, bool mustPayForStep, EventSink const& emit);
std::optional<DragonId> Split(GameState& state, Dragon& dragon, int childSegmentCount, EventSink const& emit);
void Kill(GameState& state, Dragon& dragon, DragonDeathReason reason, EventSink const& emit);
