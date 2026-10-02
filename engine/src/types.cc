#include "engine/types.h"

Direction Opposite(Direction dir)
{
    switch (dir)
    {
    case Direction::North:
        return Direction::South;
    case Direction::East:
        return Direction::West;
    case Direction::South:
        return Direction::North;
    default:
        return Direction::East;
    }
}

Point Point::operator+(Direction dir) const
{
    switch (dir)
    {
    case Direction::North:
        return {x, y - 1};
    case Direction::East:
        return {x + 1, y};
    case Direction::South:
        return {x, y + 1};
    default:
        return {x - 1, y};
    }
}

Point Point::operator-(Direction dir) const
{
    return *this + Opposite(dir);
}

namespace std {

size_t hash<Point>::operator()(Point const& p) const
{
    size_t const h1 = std::hash<int>{}(p.x);
    size_t const h2 = std::hash<int>{}(p.y);
    return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
}

} // namespace std
