#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#define RUNTIME_ASSERT(cond, msg)                                                                                              \
    {                                                                                                                          \
        if (!(cond))                                                                                                           \
        {                                                                                                                      \
            std::ostringstream _err;                                                                                           \
            _err << msg;                                                                                                       \
            throw std::runtime_error(_err.str());                                                                              \
        }                                                                                                                      \
    }

char const* const PROTOCOL_VERSION = "3.0.0";
int const LEGACY_PROTOCOL_MAJOR = 2;
int const SONAR_ECHOES_PROTOCOL_MAJOR = 3;

int const VISION_SIZE = 7;
int const VISION_RADIUS = VISION_SIZE / 2;

int const MIN_DRAGON_LENGTH = 2;
int const DEFAULT_UNIT_LIMIT = 64;
int const MAX_ROUNDS = 500;
/// Largest map side; the biggest maps in play are 64.
int const MAX_MAP_SIDE = 256;

/// What a limited game keeps of one team's bot output (logs, indicators,
/// drawings and the engine's notes about them): past either cap the rest is
/// dropped. One team's output is what a viewer of its replay decodes, so these
/// bound the replay and the judge's memory. Points already bound a turn.
int const MAX_TEAM_NOTES = 500'000;
int const MAX_TEAM_TEXT = 16 << 20;
/// Drawn as one line over the dragon; much longer runs off the board.
int const MAX_INDICATOR = 512;

uint32_t const PEARL_RNG_SEED = 0x5eed5eed;

enum class Team : char
{
    A = 'A',
    B = 'B',
};

enum class Direction : char
{
    North = 'N',
    East = 'E',
    South = 'S',
    West = 'W',
};

constexpr Direction ALL_DIRECTIONS[] = {Direction::North, Direction::East, Direction::South, Direction::West};

enum class SonarHitKind : char
{
    Empty = '.',
    Kelp = 'w',
    Ally = 'a',
    AllyHead = 'A',
    Enemy = 'e',
    EnemyHead = 'E',
};

constexpr std::string_view SONAR_ECHO_KIND_ORDER = "waAeE";

Direction Opposite(Direction);

struct Point
{
    int x;
    int y;

    bool operator==(Point const& other) const
    {
        return x == other.x && y == other.y;
    }

    Point operator+(Direction) const;
    Point operator-(Direction) const;
};

namespace std {
template <> struct hash<Point>
{
    size_t operator()(Point const&) const;
};
} // namespace std

template <typename T> class Array2d
{
  public:
    Array2d() = default;

    Array2d(int width, int height, T const& fill = T()) : mWidth(width), mData(width * height, fill) {}

    T& At(int x, int y)
    {
        return mData[y * mWidth + x];
    }

    T const& At(int x, int y) const
    {
        return mData[y * mWidth + x];
    }

    T& At(Point p)
    {
        return At(p.x, p.y);
    }

    T const& At(Point p) const
    {
        return At(p.x, p.y);
    }

    int GetWidth() const
    {
        return mWidth;
    }

    int GetHeight() const
    {
        return mWidth == 0 ? 0 : static_cast<int>(mData.size()) / mWidth;
    }

  private:
    int mWidth = 0;
    std::vector<T> mData;
};

struct Tile
{
    bool mHasPearl = false;
    bool mSpawnsPearls = false;
    int mMinRespawnGap = 0;
    int mMaxRespawnGap = 0;
    int mNextPearl = -1;
};

enum class EdgeKind : char
{
    Empty = ' ',
    Kelp = '#',
    Portal = 'P',
};

enum class EdgeOrientation
{
    Horizontal,
    Vertical,
};

struct EdgeIndex
{
    EdgeOrientation mOrientation;
    int x;
    int y;

    bool operator==(EdgeIndex const& other) const
    {
        return mOrientation == other.mOrientation && x == other.x && y == other.y;
    }
};

using MapFileEdgeIndex = int;

struct Edge
{
    EdgeKind mKind = EdgeKind::Empty;
    int mPortalId = -1;
    EdgeIndex mPortalPartner{EdgeOrientation::Horizontal, -1, -1};
};

using DragonId = int;
DragonId const NO_DRAGON = -1;

struct Dragon
{
    DragonId mId;
    Team mTeam;
    std::deque<Point> mBody;
    Direction mFacing;
    std::vector<uint64_t> mSonarInbox;
    std::array<int, SONAR_ECHO_KIND_ORDER.size()> mSonarEchoes{};
    int mProtocolMajor = LEGACY_PROTOCOL_MAJOR;
    bool mAlive = true;
};

enum class Symmetry : char
{
    None,
    X,
    Y,
    XY,
};

struct GameState
{
    int mWidth = 0;
    int mHeight = 0;
    int mUnitLimit = DEFAULT_UNIT_LIMIT;
    int mRound = 0;
    Symmetry mSymmetry = Symmetry::None;

    Array2d<Tile> mTiles;
    Array2d<Edge> mHorizontalEdges;
    Array2d<Edge> mVerticalEdges;

    std::vector<Dragon> mDragons;
    DragonId mNextDragonId = 0;
};

enum class GameEndReason
{
    TeamEliminated,
    RoundLimit,
};

struct TeamStanding
{
    int mDragonCount = 0;
    int mLongestDragon = 0;
    int mTotalLength = 0;
};

struct GameResult
{
    bool mTerminated = false;
    GameEndReason mEndReason = GameEndReason::RoundLimit;
    std::optional<Team> mWinner;
    TeamStanding mTeamA;
    TeamStanding mTeamB;
};

enum class DebugShape
{
    Line,
    Dot,
};

struct DebugDraw
{
    DebugShape mShape;
    Point mFrom;
    Point mTo;
    uint8_t mRed;
    uint8_t mGreen;
    uint8_t mBlue;
};

/// Bot output a game keeps. A team writes all of this for its own debugging,
/// so a game can be run without any of it; the judge drops the two text
/// channels, which a spamming bot inflates to tens of megabytes of replay.
struct DebugOutput
{
    bool mLogs = true;
    bool mIndicator = true;
    /// DOT and LINE.
    bool mDraw = true;
    /// The engine's own complaints about lines it cannot read.
    bool mParse = true;
    /// Hold each team to MAX_TEAM_NOTES, MAX_TEAM_TEXT and MAX_INDICATOR, as
    /// the judge does. A local run can go without.
    bool mLimits = true;
};

struct ActionMove
{
    std::vector<Direction> mSteps;
};

struct ActionSplit
{
    int mChildSegmentCount;
};

struct ActionSuicide
{
};

using PlayerAction = std::variant<ActionMove, ActionSplit, ActionSuicide>;

struct ControllerReply
{
    PlayerAction mAction = ActionSuicide{};
    std::optional<uint32_t> mSonar;
    std::map<Direction, uint64_t> mDirectedSonars;
    std::optional<int> mProtocolMajor;
    std::vector<std::string> mLogs;
    std::optional<std::string> mIndicator;
    std::vector<DebugDraw> mDraws;
    std::vector<std::string> mProtocolErrors;
};

using DragonFn = std::function<std::string(std::string const& roundBlock)>;
