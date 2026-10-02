#include "engine/helpers.h"

#include <algorithm>

namespace {


} // namespace

Point WrapOntoBoard(GameState const& state, Point p)
{
    p.x = ((p.x % state.mWidth) + state.mWidth) % state.mWidth;
    p.y = ((p.y % state.mHeight) + state.mHeight) % state.mHeight;
    return p;
}

Point MirrorTile(GameState const& state, Point p)
{
    switch (state.mSymmetry)
    {
    case Symmetry::X:
        return {p.x, state.mHeight - 1 - p.y};
    case Symmetry::Y:
        return {state.mWidth - 1 - p.x, p.y};
    case Symmetry::XY:
        return {state.mWidth - 1 - p.x, state.mHeight - 1 - p.y};
    case Symmetry::None:
        break;
    }
    return p;
}

EdgeIndex EdgeOnTileSide(GameState const& state, Point tile, Direction side)
{
    Point const from = WrapOntoBoard(state, tile);
    switch (side)
    {
    case Direction::North:
        return {EdgeOrientation::Horizontal, from.x, from.y};
    case Direction::South:
        return {EdgeOrientation::Horizontal, from.x, WrapOntoBoard(state, from + side).y};
    case Direction::West:
        return {EdgeOrientation::Vertical, from.x, from.y};
    default:
        return {EdgeOrientation::Vertical, WrapOntoBoard(state, from + side).x, from.y};
    }
}

Edge const& EdgeAt(GameState const& state, EdgeIndex edge)
{
    return edge.mOrientation == EdgeOrientation::Horizontal ? state.mHorizontalEdges.At(edge.x, edge.y)
                                                            : state.mVerticalEdges.At(edge.x, edge.y);
}

Edge& EdgeAt(GameState& state, EdgeIndex edge)
{
    return const_cast<Edge&>(EdgeAt(static_cast<GameState const&>(state), edge));
}

std::optional<EdgeIndex> EdgeIndexOf(GameState const& state, MapFileEdgeIndex index)
{
    int const stride = state.mWidth + 1;
    int const column = index % stride;
    int const row = index / stride;
    if (index < 0 || row > 2 * state.mHeight)
    {
        return std::nullopt;
    }
    if (row % 2 == 0)
    {
        // The last row wraps onto the first, and the last column only pads.
        if (column == state.mWidth || row == 2 * state.mHeight)
        {
            return std::nullopt;
        }
        return EdgeIndex{EdgeOrientation::Horizontal, column, row / 2};
    }
    // The last column wraps onto the first.
    if (column == state.mWidth)
    {
        return std::nullopt;
    }
    return EdgeIndex{EdgeOrientation::Vertical, column, (row - 1) / 2};
}

MapFileEdgeIndex MapFileEdgeIndexOf(GameState const& state, EdgeIndex edge)
{
    int const row = edge.mOrientation == EdgeOrientation::Horizontal ? 2 * edge.y : 2 * edge.y + 1;
    return row * (state.mWidth + 1) + edge.x;
}

Point TileAfterCrossing(GameState const& state, EdgeIndex edge, Direction heading)
{
    if (edge.mOrientation == EdgeOrientation::Horizontal)
    {
        return WrapOntoBoard(state, {edge.x, heading == Direction::South ? edge.y : edge.y - 1});
    }
    return WrapOntoBoard(state, {heading == Direction::East ? edge.x : edge.x - 1, edge.y});
}

std::optional<Point> TileAfterStep(GameState const& state, Point from, Direction dir)
{
    Edge const& boundary = EdgeAt(state, EdgeOnTileSide(state, from, dir));
    switch (boundary.mKind)
    {
    case EdgeKind::Kelp:
        return std::nullopt;
    case EdgeKind::Portal:
        return TileAfterCrossing(state, boundary.mPortalPartner, dir);
    default:
        return WrapOntoBoard(state, from + dir);
    }
}

std::optional<Direction> DirectionOfStepBetween(GameState const& state, Point from, Point to)
{
    for (Direction const dir : ALL_DIRECTIONS)
    {
        if (TileAfterStep(state, from, dir) == to)
        {
            return dir;
        }
    }
    return std::nullopt;
}

Dragon const* AliveDragonOccupying(GameState const& state, Point tile)
{
    for (Dragon const& dragon : state.mDragons)
    {
        if (dragon.mAlive && std::find(dragon.mBody.begin(), dragon.mBody.end(), tile) != dragon.mBody.end())
        {
            return &dragon;
        }
    }
    return nullptr;
}

Dragon* AliveDragonOccupying(GameState& state, Point tile)
{
    return const_cast<Dragon*>(AliveDragonOccupying(static_cast<GameState const&>(state), tile));
}

Dragon* DragonById(GameState& state, DragonId id)
{
    for (Dragon& dragon : state.mDragons)
    {
        if (dragon.mId == id)
        {
            return &dragon;
        }
    }
    return nullptr;
}

int AliveUnitCount(GameState const& state, Team team)
{
    return static_cast<int>(std::count_if(state.mDragons.begin(), state.mDragons.end(),
                                          [team](Dragon const& dragon) { return dragon.mAlive && dragon.mTeam == team; }));
}

bool IsHeadOf(Dragon const& dragon, Point tile)
{
    return !dragon.mBody.empty() && dragon.mBody.front() == tile;
}
