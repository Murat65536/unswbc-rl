#pragma once

// Everything the policy sees and does, computed from a View alone: the
// egocentric observation, the action set and its decoding into a reply, and
// the legality masks. Shared by the training environment and the bot (see
// view.h for why that makes them agree).
//
// Egocentric: the window is rotated so the dragon faces up, and every
// direction (edges, dragon headings, actions) is relative: forward, right,
// back, left. A policy then learns one situation instead of four.

#include "core/view.h"

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
//
// A two-step move is free from length 5 (ceil(L / 4) free steps), costs a
// segment at lengths 3 and 4, and kills a dragon of length 2 unless its first
// step eats a pearl.

constexpr int kTurns[3] = {kForward, kRight, kLeft};
constexpr int kSingleSteps = 3;
constexpr int kDoubleSteps = 9;
constexpr int kSplitAction = kSingleSteps + kDoubleSteps;
constexpr int kNumActions = kSplitAction + 1;

struct Command
{
    bool mSplit = false;
    int mSplitSize = 0;
    int mSteps = 0;
    std::array<uint8_t, 2> mDirs{};
};

inline int FreeSteps(int length)
{
    return (length + 3) / 4;
}

inline Command Decode(View const& view, int action)
{
    Command command;
    if (action == kSplitAction)
    {
        command.mSplit = true;
        command.mSplitSize = view.mLength / 2;
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

/// The reply line for a command, e.g. "MOVE NE" or "SPLIT 3".
inline std::string FormatCommand(Command const& command)
{
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

/// Moving onto this part kills the mover (a head of another dragon is a
/// trade instead: both die, and that is the policy's call).
inline bool Fatal(View const& view, PartView const& part)
{
    return part.mId >= 0 && (!part.mHead || Mine(view, part));
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

// ---------------------------------------------------------------------------
// Masks.
//
// Level 0 masks only what the rules make certain death whatever the board:
// an illegal split (child or parent under 2, or the unit limit reached).
//
// Level 1 also masks moves the dragon can see are certain death: a step into
// kelp, or onto a dragon segment that is not another dragon's head, at the
// first or the second step (for the second, its own tail has moved on unless
// the first step ate a pearl), and a two-step move at length 2 unless the
// first step eats a pearl (otherwise it cannot pay for the second). Steps
// whose landing tile is out of sight are never masked. If level 1 would mask
// every action, level 0 is used.

struct MoveSight
{
    std::array<int, kSingleSteps> mTarget{};
    std::array<bool, kNumActions> mVisiblyFatal{};
};

inline MoveSight LookAhead(View const& view)
{
    MoveSight sight;
    int const tail = OwnTail(view);
    for (int i = 0; i < kSingleSteps; i++)
    {
        int const first = ToWorld(view.mFacing, kTurns[i]);
        int const t1 = StepTarget(view, kHeadTile, first);
        sight.mTarget[i] = t1;
        bool const fatal1 = t1 == kBlocked || (t1 >= 0 && Fatal(view, view.mTiles[t1].mPart));
        sight.mVisiblyFatal[i] = fatal1;
        // Dying at the first step (or trading heads there) ends the move.
        bool const stops1 = fatal1 || (t1 >= 0 && view.mTiles[t1].mPart.mId >= 0);
        for (int j = 0; j < 3; j++)
        {
            int const action = kSingleSteps + 3 * i + j;
            if (stops1)
            {
                sight.mVisiblyFatal[action] = true;
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
        }
    }
    return sight;
}

inline bool SplitLegal(View const& view)
{
    return view.mLength >= 4 && view.mUnitCount < view.mUnitLimit;
}

/// 1 for every action allowed at `level` (0 or 1), given the view's LookAhead.
inline void Mask(View const& view, MoveSight const& sight, int level, uint8_t* out)
{
    for (int a = 0; a < kNumActions; a++)
    {
        out[a] = 1;
    }
    out[kSplitAction] = SplitLegal(view) ? 1 : 0;
    if (level < 1)
    {
        return;
    }
    std::array<uint8_t, kNumActions> strict{};
    bool any = false;
    for (int a = 0; a < kNumActions; a++)
    {
        strict[a] = out[a] && !(a < kSplitAction && sight.mVisiblyFatal[a]) ? 1 : 0;
        any = any || strict[a];
    }
    if (any)
    {
        for (int a = 0; a < kNumActions; a++)
        {
            out[a] = strict[a];
        }
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

constexpr int kPlanes = 22;
constexpr int kPlaneSize = kPlanes * kTiles;
constexpr int kScalars = 32;
constexpr int kObsSize = kPlaneSize + kScalars;

constexpr int kPlanePearl = 0;
constexpr int kPlaneNever = 1;
constexpr int kPlaneSoon = 2;
constexpr int kPlaneCountdown = 3;
constexpr int kPlaneKelp = 4;
constexpr int kPlanePortal = 8;
constexpr int kPlaneOwnHead = 12;
constexpr int kPlanePartDir = 18;

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
    case 1:
        return 1.0f / 4.0f;
    case 4:
        return 2.0f / kMaxRounds;
    default:
        return 1.0f;
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
    scalars[5] = view.mId >= 0 && view.mId <= 1;
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
}

inline void Encode(View const& view, uint8_t* out)
{
    Encode(view, LookAhead(view), out);
}

} // namespace core
