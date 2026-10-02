#include "core/state_view.h"

#include "engine/helpers.h"

#include <algorithm>
#include <cstdint>

namespace core {

namespace {

EdgeView ToView(Edge const& edge)
{
    switch (edge.mKind)
    {
    case ::EdgeKind::Kelp:
        return {kKelp, -1};
    case ::EdgeKind::Portal:
        return {kPortal, edge.mPortalId};
    case ::EdgeKind::Empty:
        break;
    }
    return {kOpen, -1};
}

uint8_t ToView(Direction dir)
{
    return static_cast<uint8_t>(DirFromChar(static_cast<char>(dir)));
}

/// The window offset of a board coordinate, or a value outside [-3, 3] if
/// it is out of sight (protocol.cc IsInVision, as a position).
int Offset(int from, int to, int size)
{
    int const offset = ((to - from) % size + size) % size;
    return offset <= kRadius ? offset : offset >= size - kRadius ? offset - size : kVision;
}

} // namespace

void ViewFromState(GameState const& state, Dragon const& dragon, View& view)
{
    view.mId = dragon.mId;
    view.mTeam = dragon.mTeam == Team::A ? 0 : 1;
    view.mWidth = state.mWidth;
    view.mHeight = state.mHeight;
    view.mUnitLimit = state.mUnitLimit;

    view.mRound = state.mRound;
    view.mFacing = ToView(dragon.mFacing);
    view.mLength = static_cast<int32_t>(dragon.mBody.size());
    view.mUnitCount = AliveUnitCount(state, dragon.mTeam);

    bool const readsEchoes = dragon.mProtocolMajor >= SONAR_ECHOES_PROTOCOL_MAJOR;
    view.mMessages.clear();
    for (uint64_t const message : dragon.mSonarInbox)
    {
        if (readsEchoes || message <= UINT32_MAX)
        {
            view.mMessages.push_back(message);
        }
    }
    view.mHasEchoes = readsEchoes;
    view.mEchoes = {};
    if (readsEchoes)
    {
        std::copy(dragon.mSonarEchoes.begin(), dragon.mSonarEchoes.end(), view.mEchoes.begin());
    }

    Point const head = dragon.mBody.front();
    // Up to 49 distinct dragons can be in sight; most windows hold one or two.
    std::array<DragonId, kTiles> seen{};
    int seenCount = 0;
    for (int row = 0; row < kVision; row++)
    {
        for (int col = 0; col < kVision; col++)
        {
            Point const at = WrapOntoBoard(state, {head.x + col - kRadius, head.y + row - kRadius});
            TileView& tile = view.mTiles[row * kVision + col];
            Tile const& contents = state.mTiles.At(at);
            tile.mX = static_cast<int16_t>(at.x);
            tile.mY = static_cast<int16_t>(at.y);
            tile.mPearl = contents.mHasPearl;
            tile.mPearlIn = contents.mSpawnsPearls ? contents.mNextPearl : -1;
            for (Direction const side : ALL_DIRECTIONS)
            {
                tile.mEdges[ToView(side)] = ToView(EdgeAt(state, EdgeOnTileSide(state, at, side)));
            }
            tile.mPart = {};
            DragonId const occupant = state.mOccupant.At(at);
            if (occupant != NO_DRAGON && std::find(seen.begin(), seen.begin() + seenCount, occupant) == seen.begin() + seenCount)
            {
                seen[seenCount++] = occupant;
            }
        }
    }

    // Each dragon in sight: walk its body once, placing every visible segment
    // with the facing BuildRoundBlock gives it.
    for (int i = 0; i < seenCount; i++)
    {
        Dragon const& other = state.mDragons[seen[i]];
        for (size_t segment = 0; segment < other.mBody.size(); segment++)
        {
            Point const at = other.mBody[segment];
            int const dx = Offset(head.x, at.x, state.mWidth);
            int const dy = Offset(head.y, at.y, state.mHeight);
            if (dx < -kRadius || dx > kRadius || dy < -kRadius || dy > kRadius)
            {
                continue;
            }
            PartView& part = view.mTiles[(kRadius + dy) * kVision + (kRadius + dx)].mPart;
            part.mId = other.mId;
            part.mTeam = other.mTeam == Team::A ? 0 : 1;
            part.mHead = segment == 0;
            if (segment == 0)
            {
                part.mDir = ToView(other.mFacing);
            }
            else
            {
                std::optional<Direction> const towardsHead =
                    DirectionOfStepBetween(state, other.mBody[segment], other.mBody[segment - 1]);
                RUNTIME_ASSERT(towardsHead, "dragon segments are not adjacent");
                part.mDir = ToView(*towardsHead);
            }
        }
    }
}

Standings StandingsFor(GameState const& state, Team team)
{
    Standings out;
    for (Dragon const& dragon : state.mDragons)
    {
        if (!dragon.mAlive)
        {
            continue;
        }
        int const side = dragon.mTeam == team ? 0 : 1;
        int const length = static_cast<int>(dragon.mBody.size());
        out.mUnits[side]++;
        out.mTotal[side] += length;
        out.mLongest[side] = std::max(out.mLongest[side], length);
        if (dragon.mId <= 1)
        {
            out.mQueen[side] = length;
        }
    }
    return out;
}

void PrivilegedFeatures(GameState const& state, Dragon const& dragon, Standings const& s, float* out)
{
    int const length = static_cast<int>(dragon.mBody.size());
    int longer = 0;
    for (Dragon const& other : state.mDragons)
    {
        if (other.mAlive && other.mTeam == dragon.mTeam && static_cast<int>(other.mBody.size()) > length)
        {
            longer++;
        }
    }
    out[0] = s.mUnits[0] / 16.0f;
    out[1] = s.mUnits[1] / 16.0f;
    out[2] = s.mTotal[0] / 64.0f;
    out[3] = s.mTotal[1] / 64.0f;
    out[4] = s.mLongest[0] / 32.0f;
    out[5] = s.mLongest[1] / 32.0f;
    out[6] = s.mQueen[0] / 32.0f;
    out[7] = s.mQueen[1] / 32.0f;
    out[8] = s.mQueen[0] > 0 ? 1.0f : 0.0f;
    out[9] = s.mQueen[1] > 0 ? 1.0f : 0.0f;
    out[10] = static_cast<float>(state.mRound) / MAX_ROUNDS;
    out[11] = length / 32.0f;
    out[12] = dragon.mId <= 1 ? 1.0f : 0.0f;
    out[13] = s.mUnits[0] > 1 ? static_cast<float>(longer) / static_cast<float>(s.mUnits[0] - 1) : 0.0f;
    out[14] = static_cast<float>(s.mTotal[0] - s.mTotal[1]) / static_cast<float>(s.mTotal[0] + s.mTotal[1] + 1);
    out[15] = static_cast<float>(s.mQueen[0] - s.mQueen[1]) / static_cast<float>(s.mQueen[0] + s.mQueen[1] + 1);
}

} // namespace core
