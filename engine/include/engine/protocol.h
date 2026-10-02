#pragma once

#include "event.h"
#include "types.h"

#include <string>

std::string BuildInitBlock(GameState const& state, Dragon const& dragon);
std::string BuildRoundBlock(GameState const& state, Dragon const& dragon);
ControllerReply ReadReply(Dragon const& dragon, std::string const& replyText, DebugOutput const& keep, EventSink const& emit);
