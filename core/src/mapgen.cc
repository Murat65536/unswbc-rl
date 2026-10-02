#include "core/mapgen.h"

#include "engine/helpers.h"

#include <algorithm>
#include <array>
#include <random>
#include <sstream>
#include <vector>

namespace core {

namespace {

class Draws
{
  public:
    explicit Draws(uint64_t seed) : mRng(seed) {}

    /// Uniform in [lo, hi]. The modulo bias is irrelevant here, and unlike a
    /// std::uniform_int_distribution it is the same on every standard library.
    int Int(int lo, int hi)
    {
        return lo + static_cast<int>(mRng() % static_cast<uint64_t>(hi - lo + 1));
    }

    double Unit()
    {
        return static_cast<double>(mRng() >> 11) * 0x1.0p-53;
    }

    bool Chance(double p)
    {
        return Unit() < p;
    }

  private:
    std::mt19937_64 mRng;
};

bool FlipsX(Symmetry symmetry)
{
    return symmetry == Symmetry::Y || symmetry == Symmetry::XY;
}

bool FlipsY(Symmetry symmetry)
{
    return symmetry == Symmetry::X || symmetry == Symmetry::XY;
}

struct Board
{
    GameState mState;
    std::vector<char> mKelp[2];
    std::vector<int> mPortal[2];
    std::vector<char> mOccupied;
    std::vector<char> mNearDragon;

    explicit Board(MapGenConfig const& config)
    {
        mState.mWidth = config.mWidth;
        mState.mHeight = config.mHeight;
        mState.mSymmetry = config.mSymmetry;
        int const tiles = config.mWidth * config.mHeight;
        for (int axis = 0; axis < 2; axis++)
        {
            mKelp[axis].assign(tiles, 0);
            mPortal[axis].assign(tiles, -1);
        }
        mOccupied.assign(tiles, 0);
        mNearDragon.assign(tiles, 0);
    }

    int Index(Point p) const
    {
        return p.y * mState.mWidth + p.x;
    }

    int Axis(EdgeIndex e) const
    {
        return e.mOrientation == EdgeOrientation::Horizontal ? 0 : 1;
    }

    int Index(EdgeIndex e) const
    {
        return e.y * mState.mWidth + e.x;
    }

    Point Mirror(Point p) const
    {
        return MirrorTile(mState, p);
    }

    /// The edge a symmetric map must treat like `e`: the matching side of the
    /// mirrored tile, with north and south (east and west) swapped when the
    /// symmetry flips that axis.
    EdgeIndex Mirror(EdgeIndex e) const
    {
        Point const tile{e.x, e.y};
        Point const image = Mirror(tile);
        if (e.mOrientation == EdgeOrientation::Horizontal)
        {
            return EdgeOnTileSide(mState, image, FlipsY(mState.mSymmetry) ? Direction::South : Direction::North);
        }
        return EdgeOnTileSide(mState, image, FlipsX(mState.mSymmetry) ? Direction::East : Direction::West);
    }

    /// The two tiles an edge separates.
    std::array<Point, 2> Sides(EdgeIndex e) const
    {
        if (e.mOrientation == EdgeOrientation::Horizontal)
        {
            return {Point{e.x, e.y}, WrapOntoBoard(mState, {e.x, e.y - 1})};
        }
        return {Point{e.x, e.y}, WrapOntoBoard(mState, {e.x - 1, e.y})};
    }

    bool Free(EdgeIndex e) const
    {
        int const axis = Axis(e);
        int const at = Index(e);
        if (mKelp[axis][at] || mPortal[axis][at] >= 0)
        {
            return false;
        }
        for (Point const side : Sides(e))
        {
            if (mNearDragon[Index(side)])
            {
                return false;
            }
        }
        return true;
    }
};

bool PlaceDragon(Board& board, Draws& draws, int length, std::vector<Point>& body)
{
    GameState const& state = board.mState;
    for (int attempt = 0; attempt < 200; attempt++)
    {
        body.clear();
        Point at{draws.Int(0, state.mWidth - 1), draws.Int(0, state.mHeight - 1)};
        std::vector<int> claimed;
        auto const usable = [&](Point p) {
            Point const image = board.Mirror(p);
            if (image == p || board.mOccupied[board.Index(p)] || board.mOccupied[board.Index(image)])
            {
                return false;
            }
            for (Point const segment : body)
            {
                if (segment == p || segment == image || board.Mirror(segment) == p)
                {
                    return false;
                }
            }
            return true;
        };
        if (!usable(at))
        {
            continue;
        }
        body.push_back(at);
        while (static_cast<int>(body.size()) < length)
        {
            std::array<Direction, 4> order = {Direction::North, Direction::East, Direction::South, Direction::West};
            for (int i = 3; i > 0; i--)
            {
                std::swap(order[i], order[draws.Int(0, i)]);
            }
            bool grown = false;
            for (Direction const dir : order)
            {
                Point const next = WrapOntoBoard(state, body.back() + dir);
                if (usable(next))
                {
                    body.push_back(next);
                    grown = true;
                    break;
                }
            }
            if (!grown)
            {
                break;
            }
        }
        if (static_cast<int>(body.size()) == length)
        {
            return true;
        }
    }
    return false;
}

void MarkDragon(Board& board, std::vector<Point> const& body)
{
    GameState const& state = board.mState;
    for (Point const segment : body)
    {
        board.mOccupied[board.Index(segment)] = 1;
        for (int dy = -1; dy <= 1; dy++)
        {
            for (int dx = -1; dx <= 1; dx++)
            {
                board.mNearDragon[board.Index(WrapOntoBoard(state, {segment.x + dx, segment.y + dy}))] = 1;
            }
        }
    }
}

EdgeIndex RandomEdge(Board const& board, Draws& draws, EdgeOrientation orientation)
{
    return {orientation, draws.Int(0, board.mState.mWidth - 1), draws.Int(0, board.mState.mHeight - 1)};
}

} // namespace

char const* SymmetryName(Symmetry symmetry)
{
    switch (symmetry)
    {
    case Symmetry::X:
        return "x";
    case Symmetry::Y:
        return "y";
    case Symmetry::XY:
        return "xy";
    case Symmetry::None:
        break;
    }
    return "";
}

namespace {

struct Generated
{
    MapGenConfig mConfig;
    Board mBoard;
    std::vector<std::array<int, 2>> mGaps;
    std::vector<std::vector<Point>> mTeamA;
    /// Each portal id's two ends, in id order.
    std::vector<std::array<EdgeIndex, 2>> mPortals;

    explicit Generated(MapGenConfig const& config) : mConfig(config), mBoard(config) {}
};

Generated Generate(MapGenConfig const& config, uint64_t seed)
{
    RUNTIME_ASSERT(config.mWidth >= VISION_SIZE && config.mHeight >= VISION_SIZE && config.mWidth <= MAX_MAP_SIDE &&
                       config.mHeight <= MAX_MAP_SIDE,
                   "map size out of range: " << config.mWidth << "x" << config.mHeight);
    RUNTIME_ASSERT(config.mStartLength >= MIN_DRAGON_LENGTH, "dragons start at length " << MIN_DRAGON_LENGTH << " or more");
    RUNTIME_ASSERT(config.mDragonsPerTeam >= 1, "each team needs a dragon");
    RUNTIME_ASSERT(config.mSymmetry != Symmetry::None, "contest maps are always symmetric");
    RUNTIME_ASSERT(config.mGapMinLo >= 1 && config.mGapMinLo <= config.mGapMinHi && config.mGapSpread >= 0,
                   "bad pearl gap range");

    Draws draws(seed);
    Generated generated(config);
    Board& board = generated.mBoard;
    int const width = config.mWidth;
    int const height = config.mHeight;

    // Pearl beds: a tile and its mirror share one spawn range.
    std::vector<std::array<int, 2>>& gaps = generated.mGaps;
    gaps.assign(width * height, {0, 0});
    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            Point const bed{x, y};
            Point const image = board.Mirror(bed);
            if (board.Index(image) < board.Index(bed) || !draws.Chance(config.mPearlBeds))
            {
                continue;
            }
            int const lo = draws.Int(config.mGapMinLo, config.mGapMinHi);
            int const hi = draws.Int(lo, lo + config.mGapSpread);
            gaps[board.Index(bed)] = gaps[board.Index(image)] = {lo, hi};
        }
    }

    // Dragons first, on an open board, so their bodies are connected; team B
    // is team A mirrored. Kelp and portals then keep off their neighbourhood.
    std::vector<std::vector<Point>>& teamA = generated.mTeamA;
    for (int i = 0; i < config.mDragonsPerTeam; i++)
    {
        std::vector<Point> body;
        RUNTIME_ASSERT(PlaceDragon(board, draws, config.mStartLength, body),
                       "no room for " << config.mDragonsPerTeam << " dragons of length " << config.mStartLength << " on a "
                                      << width << "x" << height << " map");
        std::vector<Point> image;
        for (Point const segment : body)
        {
            image.push_back(board.Mirror(segment));
        }
        MarkDragon(board, body);
        MarkDragon(board, image);
        teamA.push_back(std::move(body));
    }

    for (int axis = 0; axis < 2; axis++)
    {
        EdgeOrientation const orientation = axis == 0 ? EdgeOrientation::Horizontal : EdgeOrientation::Vertical;
        std::vector<char> seen(width * height, 0);
        for (int y = 0; y < height; y++)
        {
            for (int x = 0; x < width; x++)
            {
                EdgeIndex const edge{orientation, x, y};
                EdgeIndex const image = board.Mirror(edge);
                if (seen[board.Index(edge)])
                {
                    continue;
                }
                seen[board.Index(edge)] = seen[board.Index(image)] = 1;
                if (board.Free(edge) && board.Free(image) && draws.Chance(config.mKelp))
                {
                    board.mKelp[axis][board.Index(edge)] = board.mKelp[axis][board.Index(image)] = 1;
                }
            }
        }
    }

    // Portals. Crossing one leads out of its partner, so a symmetric map needs
    // the mirror of a pair to be a pair too: either the pair is its own
    // mirror, or the mirrored pair is placed alongside it with its own id.
    int nextPortal = 0;
    for (int placed = 0, attempt = 0; placed < config.mPortalPairs && attempt < 50 * (config.mPortalPairs + 1); attempt++)
    {
        EdgeOrientation const orientation = draws.Chance(0.5) ? EdgeOrientation::Horizontal : EdgeOrientation::Vertical;
        EdgeIndex const a = RandomEdge(board, draws, orientation);
        EdgeIndex const b = draws.Chance(0.5) ? board.Mirror(a) : RandomEdge(board, draws, orientation);
        if (a == b || !board.Free(a) || !board.Free(b))
        {
            continue;
        }
        EdgeIndex const ma = board.Mirror(a);
        EdgeIndex const mb = board.Mirror(b);
        bool const selfMirrored = (ma == a && mb == b) || (ma == b && mb == a);
        if (!selfMirrored)
        {
            if (ma == mb || ma == a || ma == b || mb == a || mb == b || !board.Free(ma) || !board.Free(mb))
            {
                continue;
            }
        }
        int const axis = board.Axis(a);
        board.mPortal[axis][board.Index(a)] = board.mPortal[axis][board.Index(b)] = nextPortal++;
        generated.mPortals.push_back({a, b});
        if (!selfMirrored)
        {
            board.mPortal[axis][board.Index(ma)] = board.mPortal[axis][board.Index(mb)] = nextPortal++;
            generated.mPortals.push_back({ma, mb});
        }
        placed++;
    }
    return generated;
}

} // namespace

std::string GenerateMapText(MapGenConfig const& config, uint64_t seed)
{
    Generated const generated = Generate(config, seed);
    Board const& board = generated.mBoard;
    GameState const& state = board.mState;
    std::vector<std::array<int, 2>> const& gaps = generated.mGaps;
    std::vector<std::vector<Point>> const& teamA = generated.mTeamA;
    int const width = config.mWidth;
    int const height = config.mHeight;

    std::ostringstream out;
    out << "MAP " << width << " " << height << "\n";
    if (config.mSymmetry != Symmetry::None)
    {
        out << "SYMMETRY " << SymmetryName(config.mSymmetry) << "\n";
    }
    if (config.mUnitLimit != DEFAULT_UNIT_LIMIT)
    {
        out << "UNIT_LIMIT " << config.mUnitLimit << "\n";
    }

    std::ostringstream tiles;
    int tileCount = 0;
    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            auto const [lo, hi] = gaps[y * width + x];
            if (hi > 0)
            {
                tiles << "TILE " << x << " " << y << " " << lo << " " << hi << "\n";
                tileCount++;
            }
        }
    }
    out << "TILE_COUNT " << tileCount << "\n" << tiles.str();

    std::ostringstream edges;
    int edgeCount = 0;
    for (int y = 0; y < height; y++)
    {
        for (int axis = 0; axis < 2; axis++)
        {
            for (int x = 0; x < width; x++)
            {
                EdgeIndex const edge{axis == 0 ? EdgeOrientation::Horizontal : EdgeOrientation::Vertical, x, y};
                int const at = board.Index(edge);
                if (board.mKelp[axis][at])
                {
                    edges << "EDGE " << MapFileEdgeIndexOf(state, edge) << " 1 -1\n";
                    edgeCount++;
                }
                else if (board.mPortal[axis][at] >= 0)
                {
                    edges << "EDGE " << MapFileEdgeIndexOf(state, edge) << " 2 " << board.mPortal[axis][at] << "\n";
                    edgeCount++;
                }
            }
        }
    }
    out << "EDGE_COUNT " << edgeCount << "\n" << edges.str();

    // Teams alternate line by line, so dragons 0 and 1 are the two queens.
    out << "DRAGON_COUNT " << 2 * teamA.size() << "\n";
    for (std::vector<Point> const& body : teamA)
    {
        for (int team = 0; team < 2; team++)
        {
            out << "DRAGON " << team << " " << body.size();
            for (Point const segment : body)
            {
                Point const p = team == 0 ? segment : board.Mirror(segment);
                out << " " << p.x << " " << p.y;
            }
            out << "\n";
        }
    }
    return out.str();
}

GameState GenerateMapState(MapGenConfig const& config, uint64_t seed)
{
    // What LoadMap would make of GenerateMapText's text, built directly:
    // tests/test_rules.py checks the two agree field by field.
    Generated generated = Generate(config, seed);
    Board& board = generated.mBoard;
    GameState state = std::move(board.mState);
    int const width = config.mWidth;
    int const height = config.mHeight;
    state.mUnitLimit = config.mUnitLimit;
    state.mTiles = Array2d<Tile>(width, height);
    state.mHorizontalEdges = Array2d<Edge>(width, height);
    state.mVerticalEdges = Array2d<Edge>(width, height);
    state.mOccupant = Array2d<DragonId>(width, height, NO_DRAGON);

    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            auto const [lo, hi] = generated.mGaps[y * width + x];
            if (hi > 0)
            {
                Tile& tile = state.mTiles.At(x, y);
                tile.mSpawnsPearls = true;
                tile.mMinRespawnGap = lo;
                tile.mMaxRespawnGap = hi;
            }
            for (int axis = 0; axis < 2; axis++)
            {
                if (board.mKelp[axis][y * width + x])
                {
                    (axis == 0 ? state.mHorizontalEdges : state.mVerticalEdges).At(x, y).mKind = EdgeKind::Kelp;
                }
            }
        }
    }
    for (size_t id = 0; id < generated.mPortals.size(); id++)
    {
        auto const [a, b] = generated.mPortals[id];
        Edge& from = EdgeAt(state, a);
        Edge& to = EdgeAt(state, b);
        from.mKind = to.mKind = EdgeKind::Portal;
        from.mPortalId = to.mPortalId = static_cast<int>(id);
        from.mPortalPartner = b;
        to.mPortalPartner = a;
    }

    for (std::vector<Point> const& body : generated.mTeamA)
    {
        for (int team = 0; team < 2; team++)
        {
            Dragon dragon;
            dragon.mId = state.mNextDragonId++;
            dragon.mTeam = team == 0 ? Team::A : Team::B;
            for (Point const segment : body)
            {
                Point const p = team == 0 ? segment : MirrorTile(state, segment);
                dragon.mBody.push_back(p);
                state.mOccupant.At(p) = dragon.mId;
            }
            dragon.mFacing = *DirectionOfStepBetween(state, dragon.mBody[1], dragon.mBody[0]);
            state.mDragons.push_back(std::move(dragon));
        }
    }
    return state;
}

} // namespace core
