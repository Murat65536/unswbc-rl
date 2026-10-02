#include "engine/game.h"
#include "engine/helpers.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <sstream>

namespace {

struct MapReader
{
    GameState mState;
    int mDeclaredTiles = -1;
    int mDeclaredEdges = -1;
    int mDeclaredDragons = -1;
    int mTilesRead = 0;
    int mEdgesRead = 0;
    std::map<int, std::vector<EdgeIndex>> mPortalEnds;

    void ReadLine(std::string const& line)
    {
        std::istringstream fields(line);
        std::string keyword;
        fields >> keyword;

        if (keyword == "MAP")
        {
            // A second MAP would resize the grid under what was already read.
            RUNTIME_ASSERT(mState.mWidth == 0, "MAP appears twice: " << line);
            RUNTIME_ASSERT(fields >> mState.mWidth >> mState.mHeight, "MAP needs a width and a height: " << line);
            RUNTIME_ASSERT(mState.mWidth >= VISION_SIZE && mState.mHeight >= VISION_SIZE,
                           "map must be at least " << VISION_SIZE << "x" << VISION_SIZE << ", got " << mState.mWidth << "x"
                                                   << mState.mHeight);
            RUNTIME_ASSERT(mState.mWidth <= MAX_MAP_SIDE && mState.mHeight <= MAX_MAP_SIDE,
                           "map must be at most " << MAX_MAP_SIDE << "x" << MAX_MAP_SIDE << ", got " << mState.mWidth << "x"
                                                  << mState.mHeight);
            mState.mTiles = Array2d<Tile>(mState.mWidth, mState.mHeight);
            mState.mHorizontalEdges = Array2d<Edge>(mState.mWidth, mState.mHeight);
            mState.mVerticalEdges = Array2d<Edge>(mState.mWidth, mState.mHeight);
            return;
        }

        RUNTIME_ASSERT(mState.mWidth > 0, "MAP must come before " << keyword);

        if (keyword == "UNIT_LIMIT")
        {
            RUNTIME_ASSERT((fields >> mState.mUnitLimit) && mState.mUnitLimit > 0,
                           "UNIT_LIMIT needs a positive count: " << line);
        }
        else if (keyword == "SYMMETRY")
        {
            ReadSymmetry(fields, line);
        }
        else if (keyword == "TILE_COUNT")
        {
            RUNTIME_ASSERT(fields >> mDeclaredTiles, "TILE_COUNT needs a count: " << line);
        }
        else if (keyword == "EDGE_COUNT")
        {
            RUNTIME_ASSERT(fields >> mDeclaredEdges, "EDGE_COUNT needs a count: " << line);
        }
        else if (keyword == "DRAGON_COUNT")
        {
            RUNTIME_ASSERT(fields >> mDeclaredDragons, "DRAGON_COUNT needs a count: " << line);
        }
        else if (keyword == "TILE")
        {
            ReadTile(fields, line);
        }
        else if (keyword == "EDGE")
        {
            ReadEdge(fields, line);
        }
        else if (keyword == "DRAGON")
        {
            ReadDragon(fields, line);
        }
        else if (keyword == "MAP_NAME")
        {
        }
        else
        {
            RUNTIME_ASSERT(false, "unknown map line: " << line);
        }
    }

    bool InBounds(Point p) const
    {
        return p.x >= 0 && p.x < mState.mWidth && p.y >= 0 && p.y < mState.mHeight;
    }

    void ReadSymmetry(std::istringstream& fields, std::string const& line)
    {
        std::string value;
        std::string extra;
        RUNTIME_ASSERT((fields >> value) && !(fields >> extra), "SYMMETRY needs one of x, y, xy: " << line);
        if (value == "x")
        {
            mState.mSymmetry = Symmetry::X;
        }
        else if (value == "y")
        {
            mState.mSymmetry = Symmetry::Y;
        }
        else if (value == "xy")
        {
            mState.mSymmetry = Symmetry::XY;
        }
        else
        {
            RUNTIME_ASSERT(false, "SYMMETRY must be x, y or xy: " << line);
        }
    }

    void ReadTile(std::istringstream& fields, std::string const& line)
    {
        Point at;
        int minGap = 0;
        int maxGap = 0;
        RUNTIME_ASSERT(fields >> at.x >> at.y >> minGap >> maxGap, "TILE needs x y minGap maxGap: " << line);
        RUNTIME_ASSERT(InBounds(at), "TILE out of bounds: " << line);
        RUNTIME_ASSERT(maxGap >= 0 && minGap >= 0 && (maxGap == 0 || minGap <= maxGap), "TILE has bad gaps: " << line);

        Tile& tile = mState.mTiles.At(at);
        tile.mSpawnsPearls = maxGap > 0;
        tile.mMinRespawnGap = minGap;
        tile.mMaxRespawnGap = maxGap;
        mTilesRead++;
    }

    void ReadEdge(std::istringstream& fields, std::string const& line)
    {
        MapFileEdgeIndex fileIndex = 0;
        int kind = 0;
        int portalId = -1;
        RUNTIME_ASSERT(fields >> fileIndex >> kind >> portalId, "EDGE needs index kind portalId: " << line);

        RUNTIME_ASSERT(fileIndex >= 0 && fileIndex < (2 * mState.mHeight + 1) * (mState.mWidth + 1),
                       "EDGE index is off the map: " << line);
        std::optional<EdgeIndex> const index = EdgeIndexOf(mState, fileIndex);
        // TODO: drop this once we no longer need right / bottom edge support.
        // The map editor writes them, so they are skipped without a word: a
        // warning per edge was thousands of log lines per match.
        if (!index)
        {
            mEdgesRead++;
            return;
        }

        Edge& edge = EdgeAt(mState, *index);

        switch (kind)
        {
        case 0:
            edge.mKind = EdgeKind::Empty;
            break;
        case 1:
            edge.mKind = EdgeKind::Kelp;
            break;
        case 2:
            RUNTIME_ASSERT(portalId >= 0, "portal EDGE needs a portal id: " << line);
            edge.mPortalId = edge.mKind == EdgeKind::Portal ? std::min(edge.mPortalId, portalId) : portalId;
            edge.mKind = EdgeKind::Portal;
            mPortalEnds[portalId].push_back(*index);
            break;
        default:
            RUNTIME_ASSERT(false, "EDGE has unknown kind: " << line);
        }
        mEdgesRead++;
    }

    void ReadDragon(std::istringstream& fields, std::string const& line)
    {
        int teamNumber = 0;
        int segmentCount = 0;
        RUNTIME_ASSERT(fields >> teamNumber >> segmentCount, "DRAGON needs team and segment count: " << line);
        RUNTIME_ASSERT(teamNumber == 0 || teamNumber == 1, "DRAGON team must be 0 or 1: " << line);
        RUNTIME_ASSERT(segmentCount >= MIN_DRAGON_LENGTH, "DRAGON is too short: " << line);

        Dragon dragon;
        dragon.mId = mState.mNextDragonId++;
        dragon.mTeam = teamNumber == 0 ? Team::A : Team::B;
        for (int i = 0; i < segmentCount; i++)
        {
            Point segment;
            RUNTIME_ASSERT(fields >> segment.x >> segment.y, "DRAGON ran out of segments: " << line);
            RUNTIME_ASSERT(InBounds(segment), "DRAGON segment out of bounds: " << line);
            RUNTIME_ASSERT(AliveDragonOccupying(mState, segment) == nullptr, "DRAGON overlaps another dragon: " << line);
            dragon.mBody.push_back(segment);
        }
        mState.mDragons.push_back(std::move(dragon));
    }

    void Finish()
    {
        RUNTIME_ASSERT(mState.mWidth > 0, "map has no MAP line");
        RUNTIME_ASSERT(mDeclaredTiles == mTilesRead, "TILE_COUNT is " << mDeclaredTiles << " but read " << mTilesRead);
        RUNTIME_ASSERT(mDeclaredEdges == mEdgesRead, "EDGE_COUNT is " << mDeclaredEdges << " but read " << mEdgesRead);
        RUNTIME_ASSERT(mDeclaredDragons == static_cast<int>(mState.mDragons.size()),
                       "DRAGON_COUNT is " << mDeclaredDragons << " but read " << mState.mDragons.size());

        for (int y = 0; y < mState.mHeight; y++)
        {
            for (int x = 0; x < mState.mWidth; x++)
            {
                Tile const& bed = mState.mTiles.At(x, y);
                Point const mirror = MirrorTile(mState, {x, y});
                Tile const& other = mState.mTiles.At(mirror);
                bool const same = bed.mSpawnsPearls == other.mSpawnsPearls &&
                                  (!bed.mSpawnsPearls ||
                                   (bed.mMinRespawnGap == other.mMinRespawnGap && bed.mMaxRespawnGap == other.mMaxRespawnGap));
                RUNTIME_ASSERT(same, "pearl bed at (" << x << ", " << y << ") doesn't match its mirror at (" << mirror.x << ", "
                                                      << mirror.y << ")");
            }
        }

        // Crossing a portal must lead back the way it came: the pair are both
        // still portals, lie the same way, and belong to no other portal.
        std::vector<EdgeIndex> seen;
        for (auto const& [portalId, ends] : mPortalEnds)
        {
            for (EdgeIndex const& end : ends)
            {
                RUNTIME_ASSERT(std::find(seen.begin(), seen.end(), end) == seen.end(),
                               "portal " << portalId << " reuses an edge of another portal at (" << end.x << ", " << end.y
                                         << ")");
                seen.push_back(end);
                RUNTIME_ASSERT(EdgeAt(mState, end).mKind == EdgeKind::Portal,
                               "portal " << portalId << " end at (" << end.x << ", " << end.y << ") was overwritten");
            }
            if (ends.size() == 2)
            {
                RUNTIME_ASSERT(ends[0].mOrientation == ends[1].mOrientation,
                               "portal " << portalId << " joins a horizontal edge to a vertical one");
            }
        }

        for (auto const& [portalId, ends] : mPortalEnds)
        {
            if (ends.size() != 2)
            {
                continue;
            }
            Edge& from = EdgeAt(mState, ends[0]);
            Edge& to = EdgeAt(mState, ends[1]);
            from.mPortalPartner = ends[1];
            to.mPortalPartner = ends[0];
            from.mPortalId = to.mPortalId = std::min(from.mPortalId, to.mPortalId);
        }

        for (auto const& [portalId, ends] : mPortalEnds)
        {
            if (ends.size() == 2)
            {
                continue;
            }
            for (EdgeIndex const& end : ends)
            {
                RUNTIME_ASSERT(EdgeAt(mState, end).mPortalPartner.x >= 0,
                               "portal " << portalId << " has " << ends.size() << " end(s) and joins nothing at "
                                         << (end.mOrientation == EdgeOrientation::Horizontal ? "top of " : "left of ") << "("
                                         << end.x << ", " << end.y << ")");
            }
        }

        for (Dragon& dragon : mState.mDragons)
        {
            for (size_t i = 1; i < dragon.mBody.size(); i++)
            {
                RUNTIME_ASSERT(DirectionOfStepBetween(mState, dragon.mBody[i], dragon.mBody[i - 1]),
                               "dragon " << dragon.mId << " segment " << i << " is not adjacent to the one before it");
            }
            dragon.mFacing = *DirectionOfStepBetween(mState, dragon.mBody[1], dragon.mBody[0]);
        }
    }
};

} // namespace

GameState LoadMap(std::string const& mapText)
{
    MapReader reader;
    std::istringstream lines(mapText);
    std::string line;
    while (std::getline(lines, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (line.empty())
        {
            continue;
        }
        if (line == "END")
        {
            break;
        }
        reader.ReadLine(line);
    }
    reader.Finish();
    return std::move(reader.mState);
}
