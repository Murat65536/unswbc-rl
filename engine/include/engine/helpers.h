#pragma once

#include "types.h"

#include <optional>

Point WrapOntoBoard(GameState const& state, Point p);

Point MirrorTile(GameState const& state, Point p);

EdgeIndex EdgeOnTileSide(GameState const& state, Point tile, Direction side);
Edge& EdgeAt(GameState& state, EdgeIndex edge);
Edge const& EdgeAt(GameState const& state, EdgeIndex edge);

// TODO: once the map editor stops including the bottom / top edges we will
std::optional<EdgeIndex> EdgeIndexOf(GameState const& state, MapFileEdgeIndex index);
MapFileEdgeIndex MapFileEdgeIndexOf(GameState const& state, EdgeIndex edge);

Point TileAfterCrossing(GameState const& state, EdgeIndex edge, Direction heading);
std::optional<Point> TileAfterStep(GameState const& state, Point from, Direction dir);
std::optional<Direction> DirectionOfStepBetween(GameState const& state, Point from, Point to);

Dragon* AliveDragonOccupying(GameState& state, Point tile);
Dragon const* AliveDragonOccupying(GameState const& state, Point tile);
Dragon* DragonById(GameState& state, DragonId id);
int AliveUnitCount(GameState const& state, Team team);
bool IsHeadOf(Dragon const& dragon, Point tile);
