#pragma once

// Everything the policy sees and does, computed from a View alone: the
// egocentric observation, the action set and its decoding into a reply, and
// the legality masks. Shared by the training environment and the bot (see
// view.h for why that makes them agree).
//
// Egocentric: the window is rotated so the dragon faces up, and every
// direction (edges, dragon headings, actions) is relative: forward, right,
// back, left. A policy then learns one situation instead of four.

#include "view.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace core {

// ---------------------------------------------------------------------------
// Directions relative to the facing.

constexpr int kForward = 0;
constexpr int kRight = 1;
constexpr int kBack = 2;
constexpr int kLeft = 3;

inline int ToWorld(int facing, int relative)
{
    return (facing + relative) & 3;
}

inline int ToRelative(int facing, int world)
{
    return (world - facing) & 3;
}

// ---------------------------------------------------------------------------
// Actions.
//
//   0..2   one step: forward, right, left (back is always into the neck)
//   3..11  two steps: 3 + 3 * first + second, each forward/right/left, the
//          second relative to the facing the first step leaves
//   12     split off the rear half: SPLIT floor(L / 2)
//   13     shed: split off the last two segments, SPLIT 2. A small child, and
//          the parent keeps the rest: how the queen grows a team while it
//          stays long (a team that starts as its queen alone has no other way)
//   14     dissolve: send no action at all, and die. Every second segment from
//          the head turns into a pearl: food for the queen, close by
//
// A two-step move is free from length 5 (ceil(L / 4) free steps), costs a
// segment at lengths 3 and 4, and kills a dragon of length 2 unless its first
// step eats a pearl.

constexpr int kTurns[3] = {kForward, kRight, kLeft};
constexpr int kSingleSteps = 3;
constexpr int kDoubleSteps = 9;
constexpr int kSplitAction = kSingleSteps + kDoubleSteps;
constexpr int kShedAction = kSplitAction + 1;
constexpr int kDissolveAction = kShedAction + 1;
constexpr int kNumActions = kDissolveAction + 1;

struct Command
{
    bool mSplit = false;
    int mSplitSize = 0;
    int mSteps = 0;
    std::array<uint8_t, 2> mDirs{};
    /// No action at all: the dragon dies (dissolve).
    bool mDissolve = false;
};

inline int FreeSteps(int length)
{
    return (length + 3) / 4;
}

inline Command Decode(View const& view, int action)
{
    Command command;
    if (action == kSplitAction || action == kShedAction)
    {
        command.mSplit = true;
        command.mSplitSize = action == kSplitAction ? view.mLength / 2 : 2;
        return command;
    }
    if (action == kDissolveAction)
    {
        command.mDissolve = true;
        return command;
    }
    if (action < kSingleSteps)
    {
        command.mSteps = 1;
        command.mDirs[0] = static_cast<uint8_t>(ToWorld(view.mFacing, kTurns[action]));
        return command;
    }
    int const first = (action - kSingleSteps) / 3;
    int const second = (action - kSingleSteps) % 3;
    command.mSteps = 2;
    command.mDirs[0] = static_cast<uint8_t>(ToWorld(view.mFacing, kTurns[first]));
    command.mDirs[1] = static_cast<uint8_t>(ToWorld(command.mDirs[0], kTurns[second]));
    return command;
}

/// The reply line for a command, e.g. "MOVE NE" or "SPLIT 3"; empty for
/// dissolve, which sends nothing before ENDTURN.
inline std::string FormatCommand(Command const& command)
{
    if (command.mDissolve)
    {
        return {};
    }
    if (command.mSplit)
    {
        char buffer[24];
        std::snprintf(buffer, sizeof buffer, "SPLIT %d", command.mSplitSize);
        return buffer;
    }
    std::string out = "MOVE ";
    for (int i = 0; i < command.mSteps; i++)
    {
        out += kDirChars[command.mDirs[i]];
    }
    return out;
}

// ---------------------------------------------------------------------------
// Reading the window.

constexpr int kUnknown = -2;
constexpr int kBlocked = -1;

/// The board edge on `side` of window tile `tile`, as (horizontal?, x, y) in
/// the engine's edge indexing: a tile's north and west sides are its own.
struct BoardEdge
{
    bool mHorizontal;
    int mX;
    int mY;

    bool operator==(BoardEdge const&) const = default;
};

inline BoardEdge EdgeOf(View const& view, int tile, int side)
{
    TileView const& t = view.mTiles[tile];
    switch (side)
    {
    case kNorth:
        return {true, t.mX, t.mY};
    case kSouth:
        return {true, t.mX, (t.mY + 1) % view.mHeight};
    case kWest:
        return {false, t.mX, t.mY};
    default:
        return {false, (t.mX + 1) % view.mWidth, t.mY};
    }
}

inline int WindowTileAt(View const& view, int x, int y)
{
    for (int i = 0; i < kTiles; i++)
    {
        if (view.mTiles[i].mX == x && view.mTiles[i].mY == y)
        {
            return i;
        }
    }
    return kUnknown;
}

/// Where a step from window tile `from` towards world direction `dir` lands,
/// as far as the dragon can tell: a window tile, kBlocked for kelp, or
/// kUnknown when the step leaves the window or crosses a portal whose far end
/// is out of sight. A portal leads out of its partner edge (the other edge
/// with its id), heading unchanged, exactly as engine TileAfterStep does.
inline int StepTarget(View const& view, int from, int dir)
{
    EdgeView const edge = view.mTiles[from].mEdges[dir];
    if (edge.mKind == kKelp)
    {
        return kBlocked;
    }
    if (edge.mKind == kOpen)
    {
        int const next = NeighbourInWindow(from, dir);
        return next < 0 ? kUnknown : next;
    }
    BoardEdge const near = EdgeOf(view, from, dir);
    for (int tile = 0; tile < kTiles; tile++)
    {
        for (int side = 0; side < 4; side++)
        {
            EdgeView const other = view.mTiles[tile].mEdges[side];
            if (other.mKind != kPortal || other.mPortal != edge.mPortal)
            {
                continue;
            }
            BoardEdge const far = EdgeOf(view, tile, side);
            if (far == near || far.mHorizontal != near.mHorizontal)
            {
                continue;
            }
            int x = far.mX;
            int y = far.mY;
            if (far.mHorizontal)
            {
                y = dir == kSouth ? far.mY : far.mY - 1;
            }
            else
            {
                x = dir == kEast ? far.mX : far.mX - 1;
            }
            x = (x % view.mWidth + view.mWidth) % view.mWidth;
            y = (y % view.mHeight + view.mHeight) % view.mHeight;
            return WindowTileAt(view, x, y);
        }
    }
    return kUnknown;
}

inline bool Mine(View const& view, PartView const& part)
{
    return part.mId == view.mId;
}

/// Moving onto this part kills the mover alone. (Onto another dragon's head
/// is a trade instead: both die. See HeadKind.)
inline bool Fatal(View const& view, PartView const& part)
{
    return part.mId >= 0 && (!part.mHead || Mine(view, part));
}

constexpr uint8_t kNoHead = 0;
constexpr uint8_t kEnemyHead = 1;
constexpr uint8_t kAllyHead = 2;

/// Whether this part is another dragon's head, and whose: moving onto it
/// kills both dragons.
inline uint8_t HeadKind(View const& view, PartView const& part)
{
    if (part.mId < 0 || !part.mHead || Mine(view, part))
    {
        return kNoHead;
    }
    return part.mTeam == view.mTeam ? kAllyHead : kEnemyHead;
}

/// The queen is the lowest-id dragon of its team: 0 for A, 1 for B. A split
/// keeps the parent's id, so the queen stays the queen with what it keeps.
inline bool IsQueen(int id)
{
    return id == 0 || id == 1;
}

/// The window tile of this dragon's tail, when its whole body is in sight
/// (otherwise kUnknown): the one segment no other segment points at.
inline int OwnTail(View const& view)
{
    int seen = 0;
    std::array<bool, kTiles> pointedAt{};
    for (int tile = 0; tile < kTiles; tile++)
    {
        PartView const& part = view.mTiles[tile].mPart;
        if (!Mine(view, part))
        {
            continue;
        }
        seen++;
        if (!part.mHead)
        {
            int const next = StepTarget(view, tile, part.mDir);
            if (next < 0)
            {
                return kUnknown;
            }
            pointedAt[next] = true;
        }
    }
    if (seen != view.mLength)
    {
        return kUnknown;
    }
    for (int tile = 0; tile < kTiles; tile++)
    {
        if (Mine(view, view.mTiles[tile].mPart) && !pointedAt[tile])
        {
            return tile;
        }
    }
    return kUnknown;
}

/// The chain of this dragon's own segments in sight, head first, as window
/// tiles: each next one the segment whose direction steps onto the one
/// before, while the chain stays in sight. Returns how many there are.
inline int OwnChain(View const& view, std::array<int8_t, kTiles>& chain)
{
    std::array<int8_t, kTiles> behind;
    behind.fill(-1);
    for (int tile = 0; tile < kTiles; tile++)
    {
        PartView const& part = view.mTiles[tile].mPart;
        if (Mine(view, part) && !part.mHead)
        {
            int const next = StepTarget(view, tile, part.mDir);
            if (next >= 0)
            {
                behind[next] = static_cast<int8_t>(tile);
            }
        }
    }
    int n = 0;
    int at = kHeadTile;
    chain[n++] = static_cast<int8_t>(at);
    while (n < view.mLength && n < kTiles && behind[at] >= 0)
    {
        at = behind[at];
        chain[n++] = static_cast<int8_t>(at);
    }
    return n;
}

/// For each window tile holding one of this dragon's own segments, after
/// how many moves it is gone (the tail after 1, the segment before it after
/// 2, ...: L - its index from the head), along OwnChain; 0 elsewhere, and
/// where that cannot be told.
inline std::array<uint8_t, kTiles> OwnFreeTimes(View const& view)
{
    std::array<uint8_t, kTiles> frees{};
    std::array<int8_t, kTiles> chain{};
    int const n = OwnChain(view, chain);
    for (int index = 1; index < n; index++)
    {
        int const gone = view.mLength - index;
        frees[chain[index]] = static_cast<uint8_t>(gone < 255 ? gone : 255);
    }
    return frees;
}

/// What lies past window tile `start` (where a first step would land),
/// explored by plain steps through open edges within the window: how many
/// tiles the head could get to (start included), and whether the region goes
/// on out of sight (an open edge leaving the window, or a portal). Other
/// dragons' segments are walls; the dragon's own are walls only until they
/// are gone (see OwnFreeTimes), so following its own tail is room and a coil
/// closing in is not. A small count that goes nowhere is a pocket the dragon
/// would trap itself in.
struct Region
{
    int mTiles = 0;
    bool mOpen = false;
};

inline Region Explore(View const& view, int start, std::array<uint8_t, kTiles> const& frees)
{
    Region region;
    if (start < 0 || view.mTiles[start].mPart.mId >= 0)
    {
        return region;
    }
    // Tiles by the move the head could first stand on them (Dijkstra with
    // unit steps: a segment of its own can be entered once it is gone).
    constexpr int kHorizon = 48;
    std::array<bool, kTiles> seen{};
    std::array<std::array<uint8_t, kTiles>, kHorizon + 1> bucket;
    std::array<uint8_t, kHorizon + 1> count{};
    seen[start] = true;
    bucket[1][count[1]++] = static_cast<uint8_t>(start);
    int reached = 0;
    for (int t = 1; t <= kHorizon; t++)
    {
        for (int i = 0; i < count[t]; i++)
        {
            int const tile = bucket[t][i];
            reached++;
            for (int side = 0; side < 4; side++)
            {
                EdgeView const& edge = view.mTiles[tile].mEdges[side];
                if (edge.mKind == kKelp)
                {
                    continue;
                }
                int const next = edge.mKind == kOpen ? NeighbourInWindow(tile, side) : -1;
                if (next < 0)
                {
                    region.mOpen = true; // out of the window, or through a portal
                    continue;
                }
                if (seen[next])
                {
                    continue;
                }
                PartView const& part = view.mTiles[next].mPart;
                int arrive = t + 1;
                if (part.mId >= 0)
                {
                    if (!Mine(view, part) || frees[next] == 0)
                    {
                        continue;
                    }
                    // The head may not enter a tile its own segment still holds.
                    arrive = std::max(arrive, frees[next] + 1);
                }
                if (arrive <= kHorizon)
                {
                    seen[next] = true;
                    bucket[arrive][count[arrive]++] = static_cast<uint8_t>(next);
                }
            }
        }
    }
    region.mTiles = reached;
    return region;
}

inline Region Explore(View const& view, int start)
{
    return Explore(view, start, OwnFreeTimes(view));
}

namespace detail
{
/// Marks where a head at `tile`, which cannot step towards `back`, could be
/// after its next move: 2 one step on, 1 two (a sprint, its first step onto
/// a tile with no dragon on it).
inline void MarkReach(View const& view, int tile, int back, std::array<uint8_t, kTiles>& reach)
{
    for (int dir = 0; dir < 4; dir++)
    {
        if (dir == back)
        {
            continue;
        }
        int const t1 = StepTarget(view, tile, dir);
        if (t1 < 0)
        {
            continue;
        }
        reach[t1] = 2;
        if (view.mTiles[t1].mPart.mId >= 0)
        {
            continue;
        }
        for (int dir2 = 0; dir2 < 4; dir2++)
        {
            if (dir2 == ((dir + 2) & 3))
            {
                continue;
            }
            int const t2 = StepTarget(view, t1, dir2);
            if (t2 >= 0 && reach[t2] < 1)
            {
                reach[t2] = 1;
            }
        }
    }
}
} // namespace detail

/// For each window tile, how soon a visible enemy head could move onto it on
/// that enemy's next turn (every other dragon moves before this one moves
/// again): 2 in one step, 1 in two (a sprint), 0 not as far as can be seen.
/// The enemy never steps back into its neck, and its first step must land on
/// a tile with no dragon on it to go on. Allies are left out: at mask level 1
/// and up they never move onto an ally's head.
inline std::array<uint8_t, kTiles> EnemyReach(View const& view)
{
    std::array<uint8_t, kTiles> reach{};
    for (int tile = 0; tile < kTiles; tile++)
    {
        PartView const& part = view.mTiles[tile].mPart;
        if (part.mId >= 0 && part.mHead && part.mTeam != view.mTeam)
        {
            detail::MarkReach(view, tile, (part.mDir + 2) & 3, reach);
        }
    }
    return reach;
}

/// How many steps a visible enemy needs to get its head onto each window
/// tile, through tiles with no dragon on them (it can step onto one to end
/// there), never back into its neck; 1 and 2 it can do on its next turn (a
/// sprint), and kFarFromEnemies stands for that many or more, or out of its
/// reach in sight. It counts enemy heads, and enemy tails too: a split turns
/// the parent's tail into the child's head, facing away from the parent, and
/// the child moves later in the same round, so every enemy tail can strike
/// like a head. A tail here is an enemy segment no segment in sight points at
/// (the rest of the body may be out of sight), unless it is within two
/// segments of its head (a dragon shorter than 4 cannot split).
constexpr int kFarFromEnemies = 5;

inline std::array<uint8_t, kTiles> EnemyDistance(View const& view)
{
    std::array<uint8_t, kTiles> dist;
    dist.fill(kFarFromEnemies);
    std::array<bool, kTiles> pointedAt{};
    for (int tile = 0; tile < kTiles; tile++)
    {
        PartView const& part = view.mTiles[tile].mPart;
        if (part.mId >= 0 && !part.mHead && part.mTeam != view.mTeam)
        {
            int const next = StepTarget(view, tile, part.mDir);
            if (next >= 0 && view.mTiles[next].mPart.mId == part.mId)
            {
                pointedAt[next] = true;
            }
        }
    }
    std::array<uint8_t, kTiles> here;
    std::array<int8_t, kTiles> queue;
    for (int source = 0; source < kTiles; source++)
    {
        PartView const& part = view.mTiles[source].mPart;
        if (part.mId < 0 || part.mTeam == view.mTeam)
        {
            continue;
        }
        int back = (part.mDir + 2) & 3;
        if (!part.mHead)
        {
            if (pointedAt[source])
            {
                continue;
            }
            bool tooShort = false;
            int at = source;
            for (int i = 0; i < 2 && !tooShort; i++)
            {
                int const next = StepTarget(view, at, view.mTiles[at].mPart.mDir);
                if (next < 0 || view.mTiles[next].mPart.mId != part.mId)
                {
                    break;
                }
                tooShort = view.mTiles[next].mPart.mHead;
                at = next;
            }
            if (tooShort)
            {
                continue;
            }
            back = part.mDir;
        }
        here.fill(kFarFromEnemies);
        int head = 0;
        int tail = 0;
        here[source] = 0;
        queue[tail++] = static_cast<int8_t>(source);
        while (head < tail)
        {
            int const at = queue[head++];
            if (here[at] + 1 >= kFarFromEnemies || (at != source && view.mTiles[at].mPart.mId >= 0))
            {
                continue;
            }
            for (int dir = 0; dir < 4; dir++)
            {
                if (at == source && dir == back)
                {
                    continue;
                }
                int const next = StepTarget(view, at, dir);
                if (next >= 0 && here[at] + 1 < here[next])
                {
                    here[next] = static_cast<uint8_t>(here[at] + 1);
                    queue[tail++] = static_cast<int8_t>(next);
                }
            }
        }
        for (int tile = 0; tile < kTiles; tile++)
        {
            if (tile != source)
            {
                dist[tile] = std::min(dist[tile], here[tile]);
            }
        }
    }
    return dist;
}

/// EnemyDistance as how soon an enemy could strike each tile: 2 in one
/// step, 1 in two (a sprint), 0 not on its next turn.
inline std::array<uint8_t, kTiles> EnemyThreat(View const& view)
{
    std::array<uint8_t, kTiles> const dist = EnemyDistance(view);
    std::array<uint8_t, kTiles> threat{};
    for (int tile = 0; tile < kTiles; tile++)
    {
        threat[tile] = dist[tile] == 1 ? 2 : dist[tile] == 2 ? 1 : 0;
    }
    return threat;
}

// ---------------------------------------------------------------------------
// Memory and survival.
//
// Each dragon is a process of its own for as long as it lives, so it can
// remember. What it keeps is its own body: the chain of segments it sees back
// from its head, then what it remembered last turn from where that chain
// ends. A body only ever trails its head's path, so after L moves the dragon
// knows all of it, including the parts far out of its window: a long dragon
// otherwise winds itself into a loop of its own body it cannot see. The bot
// keeps one Memory; the training environment keeps one per dragon and
// updates it from the same Views at the same turns, so the two remember the
// same things.

struct Cell
{
    int16_t mX = 0;
    int16_t mY = 0;

    bool operator==(Cell const&) const = default;
};

struct Memory
{
    /// The dragon's body as far as it knows, head first, in board coordinates.
    std::vector<Cell> mBody;

    void Update(View const& view)
    {
        std::array<int8_t, kTiles> chain{};
        int const n = OwnChain(view, chain);
        size_t const length = view.mLength > 0 ? static_cast<size_t>(view.mLength) : 0;
        std::vector<Cell> body;
        body.reserve(std::max(length, static_cast<size_t>(n)));
        for (int i = 0; i < n; i++)
        {
            TileView const& tile = view.mTiles[chain[i]];
            body.push_back({tile.mX, tile.mY});
        }
        Cell const last = body.back();
        for (size_t j = 0; j < mBody.size(); j++)
        {
            if (mBody[j] == last)
            {
                for (size_t k = j + 1; k < mBody.size() && body.size() < length; k++)
                {
                    if (std::find(body.begin(), body.begin() + n, mBody[k]) != body.begin() + n)
                    {
                        break; // what it remembered runs into what it sees: stop there
                    }
                    body.push_back(mBody[k]);
                }
                break;
            }
        }
        if (body.size() > length)
        {
            body.resize(length);
        }
        mBody = std::move(body);
    }
};

/// How far ahead the survival search looks: the dragon's length plus 2 (by
/// then the whole of its present body has moved on), at least 8, at most 24.
inline int SurvivalHorizon(View const& view)
{
    return std::clamp(view.mLength + 2, 8, 24);
}

/// The survival search (over|yonder's "survives the horizon"): a depth-first
/// search over the dragon's own future single steps, from where a move ends,
/// for a path that lasts SurvivalHorizon moves. Other dragons' segments in
/// sight are walls, frozen where they are; the dragon's own segments (from its
/// Memory) are walls until they are gone, later by a move for each pearl eaten
/// on the way; the path it walks becomes body behind it. What is out of sight
/// is open water, a portal is a way out, and a search that runs out of
/// expansions counts as surviving: it says how long the dragon can be sure
/// of lasting as far as it knows, not that it will.
class Survival
{
  public:
    static constexpr int kSpan = 49; // a horizon of 24 either way
    static constexpr int kBudget = 1024;

    Survival(View const& view, Memory const* memory) : mView(view), mHorizon(SurvivalHorizon(view))
    {
        mWholeX = view.mWidth <= kSpan;
        mWholeY = view.mHeight <= kSpan;
        mW = mWholeX ? view.mWidth : kSpan;
        mH = mWholeY ? view.mHeight : kSpan;
        TileView const& head = view.mTiles[kHeadTile];
        mHeadX = head.mX;
        mHeadY = head.mY;
        mBlock.fill(0);
        mTile.fill(-1);
        mEntered.fill(0);

        // The dragon's own body, from memory (or from what it sees).
        std::vector<Cell> sighted;
        std::vector<Cell> const* body = memory != nullptr ? &memory->mBody : nullptr;
        if (body == nullptr || body->empty())
        {
            Memory fresh;
            fresh.Update(view);
            sighted = std::move(fresh.mBody);
            body = &sighted;
        }
        for (size_t i = 0; i < body->size(); i++)
        {
            int const at = Index((*body)[i].mX, (*body)[i].mY);
            if (at >= 0)
            {
                int const gone = view.mLength - static_cast<int>(i);
                mBlock[at] = static_cast<uint8_t>(2 + std::clamp(gone, 0, 250));
            }
        }
        // What it sees overrides what it remembers.
        std::array<uint8_t, kTiles> const frees = OwnFreeTimes(view);
        for (int tile = 0; tile < kTiles; tile++)
        {
            TileView const& t = view.mTiles[tile];
            int const at = Index(t.mX, t.mY);
            if (at < 0)
            {
                continue;
            }
            mTile[at] = static_cast<int8_t>(tile);
            PartView const& part = t.mPart;
            if (part.mId < 0)
            {
                mBlock[at] = 0;
            }
            else if (!Mine(view, part))
            {
                mBlock[at] = 1;
            }
            else if (mBlock[at] < 2)
            {
                mBlock[at] = static_cast<uint8_t>(frees[tile] > 0 ? 2 + std::min<int>(frees[tile], 250) : 1);
            }
        }
    }

    int Horizon() const
    {
        return mHorizon;
    }

    /// How many moves the dragon can be sure of after stepping onto window
    /// tiles `path` (one or two, the last where the move ends), as far as it
    /// can tell: Horizon() when some path lasts that long.
    int Depth(int const* path, int steps)
    {
        std::array<int, 2> cells{};
        int grow = 0;
        for (int i = 0; i < steps; i++)
        {
            TileView const& t = mView.mTiles[path[i]];
            cells[i] = Index(t.mX, t.mY);
            if (cells[i] < 0)
            {
                return mHorizon;
            }
            grow += t.mPearl ? 1 : 0;
        }
        for (int i = 0; i < steps; i++)
        {
            mEntered[cells[i]] = static_cast<uint8_t>(i + 1);
        }
        mExpansions = 0;
        int const depth = Search(cells[steps - 1], steps, grow);
        for (int i = 0; i < steps; i++)
        {
            mEntered[cells[i]] = 0;
        }
        return depth;
    }

  private:
    static constexpr int kEscape = -2;

    /// The grid cell of board cell (x, y), or -1 when it is out of reach.
    int Index(int x, int y) const
    {
        int lx = x;
        int ly = y;
        if (!mWholeX)
        {
            lx = ((x - mHeadX + kSpan / 2) % mView.mWidth + mView.mWidth) % mView.mWidth;
            if (lx >= kSpan)
            {
                return -1;
            }
        }
        if (!mWholeY)
        {
            ly = ((y - mHeadY + kSpan / 2) % mView.mHeight + mView.mHeight) % mView.mHeight;
            if (ly >= kSpan)
            {
                return -1;
            }
        }
        if (lx < 0 || ly < 0 || lx >= mW || ly >= mH)
        {
            return -1;
        }
        return ly * kSpan + lx;
    }

    /// Where a plain step from grid cell `at` towards `dir` goes: a grid cell,
    /// kBlocked for kelp, or kEscape (through a portal, or out of reach).
    int Step(int at, int dir) const
    {
        int const tile = mTile[at];
        if (tile >= 0)
        {
            uint8_t const kind = mView.mTiles[tile].mEdges[dir].mKind;
            if (kind == kKelp)
            {
                return kBlocked;
            }
            if (kind == kPortal)
            {
                return kEscape;
            }
        }
        int lx = at % kSpan + kStepX[dir];
        int ly = at / kSpan + kStepY[dir];
        if (mWholeX)
        {
            lx = (lx + mW) % mW;
        }
        if (mWholeY)
        {
            ly = (ly + mH) % mH;
        }
        if (lx < 0 || ly < 0 || lx >= mW || ly >= mH)
        {
            return kEscape;
        }
        int const next = ly * kSpan + lx;
        if (tile < 0 && mTile[next] >= 0)
        {
            uint8_t const kind = mView.mTiles[mTile[next]].mEdges[(dir + 2) & 3].mKind;
            if (kind == kKelp)
            {
                return kBlocked;
            }
            if (kind == kPortal)
            {
                return kEscape;
            }
        }
        return next;
    }

    /// The head may stand on grid cell `at` after move `t`, `grow` pearls
    /// eaten on the way.
    bool Passable(int at, int t, int grow) const
    {
        if (mEntered[at] > 0)
        {
            return t - mEntered[at] > mView.mLength + grow;
        }
        uint8_t const block = mBlock[at];
        if (block == 1)
        {
            return false;
        }
        return block == 0 || t >= block - 2 + grow + 1;
    }

    int Search(int at, int t, int grow)
    {
        if (t >= mHorizon || ++mExpansions > kBudget)
        {
            return mHorizon;
        }
        int best = t;
        for (int dir = 0; dir < 4; dir++)
        {
            int const next = Step(at, dir);
            if (next == kEscape)
            {
                return mHorizon;
            }
            if (next < 0 || !Passable(next, t + 1, grow))
            {
                continue;
            }
            int const tile = mTile[next];
            int const ate = tile >= 0 && mView.mTiles[tile].mPearl ? 1 : 0;
            mEntered[next] = static_cast<uint8_t>(t + 1);
            int const depth = Search(next, t + 1, grow + ate);
            mEntered[next] = 0;
            best = std::max(best, depth);
            if (best >= mHorizon)
            {
                return mHorizon;
            }
        }
        return best;
    }

    View const& mView;
    int mHorizon;
    bool mWholeX = true;
    bool mWholeY = true;
    int mW = 0;
    int mH = 0;
    int mHeadX = 0;
    int mHeadY = 0;
    int mExpansions = 0;
    /// 0 open, 1 another dragon's segment, 2 + n the dragon's own, gone after n moves.
    std::array<uint8_t, kSpan * kSpan> mBlock;
    /// The window tile of each grid cell, or -1.
    std::array<int8_t, kSpan * kSpan> mTile;
    /// The move at which the searched path entered each cell, or 0.
    std::array<uint8_t, kSpan * kSpan> mEntered;
};

// ---------------------------------------------------------------------------
// Masks.
//
// Level 0 masks only what the rules make certain death whatever the board:
// an illegal split (child or parent under 2, or the unit limit reached).
//
// Dissolving is certain death: level 1 allows it only where the queen can
// eat what it leaves (MayDissolve).
//
// Level 1 also masks moves the dragon can see are certain death: a step into
// kelp, or onto a dragon segment that is not another dragon's head, at the
// first or the second step (for the second, its own tail has moved on unless
// the first step ate a pearl), and a two-step move at length 2 unless the
// first step eats a pearl (otherwise it cannot pay for the second). It also
// masks ending a move on an ally's head, which kills two of the team's
// dragons for nothing. (An enemy's head is a trade, and the policy's call.)
// Steps whose landing tile is out of sight are never masked.
//
// Level 2 also shields the queen, which decides the first round-limit
// tiebreak (a dead queen has length 0). Of its level-1 moves that do not end
// on another dragon's head, it keeps the first non-empty tier of: those it
// can be sure of surviving its horizon after (Survival, which knows where its
// whole body is from its Memory), else those that last longest, and of
// those, the ones that end out of every enemy's reach on its next turn, else
// out of one step's (EnemyThreat: heads, and the children enemies could split
// off at their tails, which move in the round they are born). Besides those
// it may shed two segments to grow its team (QueenMayShed). Only with none of
// those moves may it split (which halves it, but it survives), and only with
// no split either is it left level 1's choices. Every other dragon keeps out
// of its queen's way at level 2: it does not end a move beside the queen's
// head while it has another move (an ally there can be all that walls the
// queen in).
//
// When a level would mask every action, the level below it is used, but a
// dragon with nothing but fatal moves still avoids taking an ally with it.

constexpr int kMaskLevels = 3;

struct MoveSight
{
    std::array<int, kSingleSteps> mTarget{};
    std::array<bool, kNumActions> mVisiblyFatal{};
    /// The move ends on another dragon's head (kEnemyHead or kAllyHead).
    std::array<uint8_t, kNumActions> mHead{};
    /// The window tile the head ends on, when that is in sight and the move
    /// is neither visibly fatal nor a trade; -1 otherwise.
    std::array<int8_t, kNumActions> mLanding{};
    /// What lies past each first step F/R/L (Explore).
    std::array<Region, kSingleSteps> mRegion{};
    /// How soon an enemy head could reach where each move ends (EnemyReach at
    /// mLanding; 0 with no landing tile), and the head's tile now.
    std::array<uint8_t, kNumActions> mReach{};
    uint8_t mReachHere = 0;
    /// The same with enemy split children counted (EnemyThreat), and how many
    /// steps the nearest enemy (or child) needs to get there (EnemyDistance,
    /// kFarFromEnemies with no landing tile).
    std::array<uint8_t, kNumActions> mThreat{};
    uint8_t mThreatHere = 0;
    std::array<uint8_t, kNumActions> mEnemyDistance{};
    uint8_t mEnemyDistanceHere = 0;
    /// How many moves the dragon can be sure of after each move (Survival):
    /// mHorizon when some path lasts that long as far as it can tell; 0 for a
    /// move that is visibly fatal or a trade, 1 for one that ends out of sight
    /// (through a portal). Single steps always, two-step moves for the queen
    /// only.
    std::array<uint8_t, kNumActions> mSurvive{};
    uint8_t mHorizon = 0;
    /// The move ends next to this team's queen's head (in sight), where it
    /// would stand in the queen's way.
    std::array<bool, kNumActions> mBesideQueen{};
    /// This dragon's head is within two steps of its queen's head: pearls it
    /// left here (dissolving) would be the queen's to eat.
    bool mFeedSpot = false;
};

/// What the dragon can work out about its moves, from its View and, if it has
/// one, its Memory (without, it knows only the body it sees).
inline MoveSight LookAhead(View const& view, Memory const* memory = nullptr)
{
    MoveSight sight;
    sight.mLanding.fill(-1);
    int const tail = OwnTail(view);
    for (int i = 0; i < kSingleSteps; i++)
    {
        int const first = ToWorld(view.mFacing, kTurns[i]);
        int const t1 = StepTarget(view, kHeadTile, first);
        sight.mTarget[i] = t1;
        bool const fatal1 = t1 == kBlocked || (t1 >= 0 && Fatal(view, view.mTiles[t1].mPart));
        sight.mVisiblyFatal[i] = fatal1;
        sight.mHead[i] = t1 >= 0 ? HeadKind(view, view.mTiles[t1].mPart) : kNoHead;
        if (t1 >= 0 && !fatal1 && sight.mHead[i] == kNoHead)
        {
            sight.mLanding[i] = static_cast<int8_t>(t1);
        }
        // Dying at the first step (or trading heads there) ends the move.
        bool const stops1 = fatal1 || (t1 >= 0 && view.mTiles[t1].mPart.mId >= 0);
        for (int j = 0; j < 3; j++)
        {
            int const action = kSingleSteps + 3 * i + j;
            if (stops1)
            {
                sight.mVisiblyFatal[action] = true;
                sight.mHead[action] = sight.mHead[i];
                continue;
            }
            if (t1 < 0)
            {
                continue;
            }
            if (view.mLength < 3 && !view.mTiles[t1].mPearl)
            {
                // Length 2 pays for the second step with its life.
                sight.mVisiblyFatal[action] = true;
                continue;
            }
            int const second = ToWorld(first, kTurns[j]);
            int const t2 = StepTarget(view, t1, second);
            if (t2 == kBlocked)
            {
                sight.mVisiblyFatal[action] = true;
                continue;
            }
            if (t2 < 0)
            {
                continue;
            }
            PartView const& part = view.mTiles[t2].mPart;
            bool const tailMoved = t2 == tail && !view.mTiles[t1].mPearl;
            sight.mVisiblyFatal[action] = Fatal(view, part) && !tailMoved;
            sight.mHead[action] = HeadKind(view, part);
            if (!sight.mVisiblyFatal[action] && sight.mHead[action] == kNoHead)
            {
                sight.mLanding[action] = static_cast<int8_t>(t2);
            }
        }
    }
    std::array<uint8_t, kTiles> const frees = OwnFreeTimes(view);
    for (int i = 0; i < kSingleSteps; i++)
    {
        sight.mRegion[i] = Explore(view, sight.mTarget[i], frees);
    }
    std::array<uint8_t, kTiles> const reach = EnemyReach(view);
    for (int a = 0; a < kSplitAction; a++)
    {
        sight.mReach[a] = sight.mLanding[a] >= 0 ? reach[sight.mLanding[a]] : 0;
    }
    sight.mReachHere = reach[kHeadTile];
    std::array<uint8_t, kTiles> const distance = EnemyDistance(view);
    auto const threatAt = [](int d) { return static_cast<uint8_t>(d == 1 ? 2 : d == 2 ? 1 : 0); };
    for (int a = 0; a < kSplitAction; a++)
    {
        sight.mEnemyDistance[a] = sight.mLanding[a] >= 0 ? distance[sight.mLanding[a]] : kFarFromEnemies;
        sight.mThreat[a] = threatAt(sight.mEnemyDistance[a]);
    }
    sight.mEnemyDistanceHere = distance[kHeadTile];
    sight.mThreatHere = threatAt(sight.mEnemyDistanceHere);

    if (!IsQueen(view.mId))
    {
        std::array<bool, kTiles> beside{};
        for (int tile = 0; tile < kTiles; tile++)
        {
            PartView const& part = view.mTiles[tile].mPart;
            if (part.mId >= 0 && part.mHead && IsQueen(part.mId) && part.mTeam == view.mTeam)
            {
                for (int dir = 0; dir < 4; dir++)
                {
                    int const next = StepTarget(view, tile, dir);
                    if (next >= 0)
                    {
                        beside[next] = true;
                    }
                }
            }
        }
        for (int a = 0; a < kSplitAction; a++)
        {
            sight.mBesideQueen[a] = sight.mLanding[a] >= 0 && beside[sight.mLanding[a]];
        }
        bool near = beside[kHeadTile];
        for (int dir = 0; dir < 4 && !near; dir++)
        {
            int const next = StepTarget(view, kHeadTile, dir);
            near = next >= 0 && beside[next];
        }
        sight.mFeedSpot = near;
    }

    Survival survival(view, memory);
    sight.mHorizon = static_cast<uint8_t>(survival.Horizon());
    int const moves = IsQueen(view.mId) ? kSplitAction : kSingleSteps;
    for (int a = 0; a < moves; a++)
    {
        if (sight.mVisiblyFatal[a] || sight.mHead[a] != kNoHead)
        {
            continue;
        }
        if (sight.mLanding[a] < 0)
        {
            // Through a portal whose far end is out of sight: it lasts the
            // one move, as far as the dragon can tell, so a known way is
            // preferred (a portal can land a long dragon on its own body).
            sight.mSurvive[a] = 1;
            continue;
        }
        std::array<int, 2> path{};
        int steps = 1;
        if (a < kSingleSteps)
        {
            path[0] = sight.mLanding[a];
        }
        else
        {
            path[0] = sight.mTarget[(a - kSingleSteps) / 3];
            path[1] = sight.mLanding[a];
            steps = 2;
        }
        sight.mSurvive[a] = static_cast<uint8_t>(survival.Depth(path.data(), steps));
    }
    return sight;
}

inline bool SplitLegal(View const& view)
{
    return view.mLength >= 4 && view.mUnitCount < view.mUnitLimit;
}

/// The queen may shed two segments to grow its team while it stays long:
/// from length 6, before round 350 (late on, length is what counts), and not
/// with an enemy (or a child an enemy could split off) one step from where it
/// stays.
inline bool QueenMayShed(View const& view, MoveSight const& sight)
{
    return view.mLength >= 6 && view.mRound < 350 && sight.mThreatHere < 2;
}

/// A dragon may dissolve where its queen can eat it: beside or a step from
/// the queen's head, at length 3 or more (it leaves ceil(L / 2) pearls). The
/// queen never does.
inline bool MayDissolve(View const& view, MoveSight const& sight)
{
    return !IsQueen(view.mId) && sight.mFeedSpot && view.mLength >= 3;
}

/// 1 for every action allowed at `level` (0, 1 or 2), given the view's LookAhead.
inline void Mask(View const& view, MoveSight const& sight, int level, uint8_t* out)
{
    for (int a = 0; a < kNumActions; a++)
    {
        out[a] = 1;
    }
    out[kSplitAction] = SplitLegal(view) ? 1 : 0;
    out[kShedAction] = SplitLegal(view) ? 1 : 0;
    if (level < 1)
    {
        return;
    }
    std::array<uint8_t, kNumActions> strict{};
    bool any = false;
    for (int a = 0; a < kNumActions; a++)
    {
        // Dissolving is certain death, worth it only as the queen's food.
        bool const fatal = (a < kSplitAction && (sight.mVisiblyFatal[a] || sight.mHead[a] == kAllyHead)) ||
                           (a == kDissolveAction && !MayDissolve(view, sight));
        strict[a] = out[a] && !fatal ? 1 : 0;
        any = any || strict[a];
    }
    if (!any)
    {
        // Every action is fatal: at least die alone.
        for (int a = 0; a < kSplitAction; a++)
        {
            strict[a] = out[a] && sight.mHead[a] != kAllyHead ? 1 : 0;
            any = any || strict[a];
        }
        if (any)
        {
            for (int a = 0; a < kNumActions; a++)
            {
                out[a] = strict[a];
            }
        }
        return;
    }
    if (level >= 2 && !IsQueen(view.mId))
    {
        // Keep out of the queen's way: an ally standing beside its head can
        // be all that walls it in.
        std::array<uint8_t, kNumActions> clear = strict;
        bool anyClear = false;
        for (int a = 0; a < kSplitAction; a++)
        {
            clear[a] = strict[a] && !sight.mBesideQueen[a] ? 1 : 0;
            anyClear = anyClear || clear[a];
        }
        if (anyClear)
        {
            strict = clear;
        }
    }
    if (level >= 2 && IsQueen(view.mId))
    {
        std::array<uint8_t, kNumActions> tier{};
        bool chosen = false;
        int longest = 0;
        for (int a = 0; a < kSplitAction; a++)
        {
            if (strict[a] && sight.mHead[a] == kNoHead)
            {
                longest = std::max<int>(longest, sight.mSurvive[a]);
            }
        }
        // Those that survive the horizon, else those that last longest; of
        // them, those that end out of every enemy's reach (and the reach of
        // the children enemies could split off), else out of one step's.
        for (int t = 0; t < 2 && !chosen; t++)
        {
            int safest = 2;
            for (int a = 0; a < kSplitAction; a++)
            {
                bool ok = strict[a] && sight.mHead[a] == kNoHead;
                ok = ok && (t == 1 ? sight.mSurvive[a] == longest : sight.mSurvive[a] >= sight.mHorizon);
                tier[a] = ok ? 1 : 0;
                if (ok)
                {
                    safest = std::min<int>(safest, sight.mThreat[a]);
                    chosen = true;
                }
            }
            for (int a = 0; a < kSplitAction; a++)
            {
                tier[a] = tier[a] && sight.mThreat[a] == safest ? 1 : 0;
            }
        }
        if (!chosen && (strict[kSplitAction] || strict[kShedAction]))
        {
            tier[kSplitAction] = strict[kSplitAction];
            tier[kShedAction] = strict[kShedAction];
            chosen = true;
        }
        else if (chosen && strict[kShedAction] && QueenMayShed(view, sight))
        {
            tier[kShedAction] = 1;
        }
        if (chosen)
        {
            strict = tier;
        }
    }
    for (int a = 0; a < kNumActions; a++)
    {
        out[a] = strict[a];
    }
}

// ---------------------------------------------------------------------------
// The observation: kObsSize uint8 codes. Feature i means code * FeatureScale(i),
// a fixed scale per feature, so the trainer feeds code * scale to the network
// and the bot folds the scales into its first layer. Most codes are 0/1.
//
// Planes, each 7x7 in the egocentric window (row 0 ahead, column 0 left):
//   0  pearl                       1  never spawns
//   2  spawns at the next tick     3  countdown, min(pearlIn, 32) / 32
//   4..7   kelp on the forward / right / back / left side
//   8..11  portal on the forward / right / back / left side
//   12 own head   13 own body   14 ally head   15 ally body
//   16 enemy head 17 enemy body
//   18..21 a dragon part's direction (heading, or towards its head):
//          forward / right / back / left
//   22 a segment of this team's queen (this dragon's own, if it is the queen)
//   23 a segment of the enemy queen
// Scalars, after the planes:
//   0  length, min(L, 64) / 64     1  free steps, min(ceil(L / 4), 16) / 4
//   2  team units / 64             3  units below the limit, min(64) / 64
//   4  round / 500 (code round / 2)
//   5  is the queen                6  a split is legal
//   7..9    one step F/R/L is visibly fatal
//   10..12  one step F/R/L lands on a pearl
//   13..15  one step F/R/L lands on an enemy head
//   16..18  one step F/R/L lands on an ally head
//   19..21  one step F/R/L lands out of sight
//   22..30  two-step move 3..11 is visibly fatal
//   31  two-step moves are free (L >= 5)
//   32..34  tiles reachable past one step F/R/L within the window, min(48) / 48
//           (its own segments counting once they are gone)
//   35..37  and whether that region goes on out of sight (see Explore)
//   38..49  where move 0..11 ends, an enemy head could reach next: 1 in one
//           step, 0.5 in two (EnemyReach / 2; 0 if the move is visibly
//           fatal, a trade, or ends out of sight)
//   50      the same for the tile the head is on now (where a split leaves it)
//   51..53  moves the dragon can be sure of after one step F/R/L (Survival),
//           min(24) / 24
//   54      its survival horizon, / 24
//   55      its queen's head is within two steps (where dissolving feeds it)
//   56..67  how close to an enemy move 0..11 ends, counting the children
//           enemies could split off at their tails: 5 - EnemyDistance, / 4
//           (1 one step from an enemy, 0.25 four, 0 five or more, and 0 if the
//           move is visibly fatal, a trade, or ends out of sight)
//   68      the same for the tile the head is on now
//
// New features are appended (planes after the planes, scalars after the
// scalars), so a checkpoint for an older layout can be widened with zero
// weights (rl/warm_start.py).

constexpr int kPlanes = 24;
constexpr int kPlaneSize = kPlanes * kTiles;
constexpr int kScalars = 69;
constexpr int kObsSize = kPlaneSize + kScalars;

constexpr int kPlanePearl = 0;
constexpr int kPlaneNever = 1;
constexpr int kPlaneSoon = 2;
constexpr int kPlaneCountdown = 3;
constexpr int kPlaneKelp = 4;
constexpr int kPlanePortal = 8;
constexpr int kPlaneOwnHead = 12;
constexpr int kPlanePartDir = 18;
constexpr int kPlaneOwnQueen = 22;
constexpr int kPlaneEnemyQueen = 23;

/// The window tile shown at egocentric (row, col) for this facing.
constexpr int WorldTile(int facing, int row, int col)
{
    int const forward = kRadius - row;
    int const right = col - kRadius;
    int const rightDir = (facing + 1) & 3;
    int const dx = forward * kStepX[facing] + right * kStepX[rightDir];
    int const dy = forward * kStepY[facing] + right * kStepY[rightDir];
    return (kRadius + dy) * kVision + (kRadius + dx);
}

/// WorldTile for every facing and egocentric cell.
struct EgoTable
{
    std::array<std::array<uint8_t, kTiles>, 4> mTile{};

    constexpr EgoTable()
    {
        for (int facing = 0; facing < 4; facing++)
        {
            for (int cell = 0; cell < kTiles; cell++)
            {
                mTile[facing][cell] = static_cast<uint8_t>(WorldTile(facing, cell / kVision, cell % kVision));
            }
        }
    }
};

inline constexpr EgoTable kEgo{};

inline float FeatureScale(int feature)
{
    if (feature < kPlaneSize)
    {
        return feature / kTiles == kPlaneCountdown ? 1.0f / 32.0f : 1.0f;
    }
    switch (feature - kPlaneSize)
    {
    case 0:
    case 2:
    case 3:
        return 1.0f / 64.0f;
    case 32:
    case 33:
    case 34:
        return 1.0f / 48.0f;
    case 1:
        return 1.0f / 4.0f;
    case 4:
        return 2.0f / kMaxRounds;
    default:
        return feature - kPlaneSize >= 56   ? 0.25f
               : feature - kPlaneSize >= 55 ? 1.0f
               : feature - kPlaneSize >= 51 ? 1.0f / 24.0f
               : feature - kPlaneSize >= 38 ? 0.5f
                                            : 1.0f;
    }
}

inline void Mask(View const& view, int level, uint8_t* out)
{
    Mask(view, LookAhead(view), level, out);
}

inline void Encode(View const& view, MoveSight const& sight, uint8_t* out)
{
    std::memset(out, 0, kObsSize);
    int const facing = view.mFacing;
    for (int cell = 0; cell < kTiles; cell++)
    {
        {
            TileView const& tile = view.mTiles[kEgo.mTile[facing][cell]];
            auto const set = [&](int plane, int code) { out[plane * kTiles + cell] = static_cast<uint8_t>(code); };
            set(kPlanePearl, tile.mPearl);
            if (tile.mPearlIn < 0)
            {
                set(kPlaneNever, 1);
            }
            else
            {
                set(kPlaneSoon, tile.mPearlIn == 1);
                set(kPlaneCountdown, tile.mPearlIn < 32 ? tile.mPearlIn : 32);
            }
            for (int side = 0; side < 4; side++)
            {
                EdgeView const& edge = tile.mEdges[side];
                int const relative = ToRelative(facing, side);
                if (edge.mKind == kKelp)
                {
                    set(kPlaneKelp + relative, 1);
                }
                else if (edge.mKind == kPortal)
                {
                    set(kPlanePortal + relative, 1);
                }
            }
            PartView const& part = tile.mPart;
            if (part.mId >= 0)
            {
                int const owner = Mine(view, part) ? 0 : part.mTeam == view.mTeam ? 1 : 2;
                set(kPlaneOwnHead + 2 * owner + (part.mHead ? 0 : 1), 1);
                set(kPlanePartDir + ToRelative(facing, part.mDir), 1);
                if (IsQueen(part.mId))
                {
                    set(part.mTeam == view.mTeam ? kPlaneOwnQueen : kPlaneEnemyQueen, 1);
                }
            }
        }
    }

    uint8_t* scalars = out + kPlaneSize;
    int const length = view.mLength;
    scalars[0] = static_cast<uint8_t>(length < 64 ? length : 64);
    int const free = FreeSteps(length);
    scalars[1] = static_cast<uint8_t>(free < 16 ? free : 16);
    scalars[2] = static_cast<uint8_t>(view.mUnitCount < 64 ? view.mUnitCount : 64);
    int const room = view.mUnitLimit - view.mUnitCount;
    scalars[3] = static_cast<uint8_t>(room < 0 ? 0 : room < 64 ? room : 64);
    scalars[4] = static_cast<uint8_t>((view.mRound < kMaxRounds ? view.mRound : kMaxRounds - 1) / 2);
    scalars[5] = IsQueen(view.mId);
    scalars[6] = SplitLegal(view);
    for (int i = 0; i < kSingleSteps; i++)
    {
        int const target = sight.mTarget[i];
        scalars[7 + i] = sight.mVisiblyFatal[i];
        if (target >= 0)
        {
            TileView const& tile = view.mTiles[target];
            scalars[10 + i] = tile.mPearl;
            bool const head = tile.mPart.mId >= 0 && tile.mPart.mHead && !Mine(view, tile.mPart);
            scalars[13 + i] = head && tile.mPart.mTeam != view.mTeam;
            scalars[16 + i] = head && tile.mPart.mTeam == view.mTeam;
        }
        else if (target == kUnknown)
        {
            scalars[19 + i] = 1;
        }
    }
    for (int a = kSingleSteps; a < kSplitAction; a++)
    {
        scalars[22 + a - kSingleSteps] = sight.mVisiblyFatal[a];
    }
    scalars[31] = free >= 2;
    for (int i = 0; i < kSingleSteps; i++)
    {
        Region const& region = sight.mRegion[i];
        scalars[32 + i] = static_cast<uint8_t>(region.mTiles < 48 ? region.mTiles : 48);
        scalars[35 + i] = region.mOpen;
    }
    for (int a = 0; a < kSplitAction; a++)
    {
        scalars[38 + a] = sight.mReach[a];
    }
    scalars[50] = sight.mReachHere;
    for (int i = 0; i < kSingleSteps; i++)
    {
        scalars[51 + i] = sight.mSurvive[i];
    }
    scalars[54] = sight.mHorizon;
    scalars[55] = sight.mFeedSpot;
    for (int a = 0; a < kSplitAction; a++)
    {
        scalars[56 + a] = static_cast<uint8_t>(kFarFromEnemies - sight.mEnemyDistance[a]);
    }
    scalars[68] = static_cast<uint8_t>(kFarFromEnemies - sight.mEnemyDistanceHere);
}

inline void Encode(View const& view, uint8_t* out)
{
    Encode(view, LookAhead(view), out);
}

} // namespace core
