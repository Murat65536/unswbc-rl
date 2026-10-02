#pragma once

// What one dragon knows on its turn: the init block and the round block of
// the wire protocol (docs/rules/protocol.md), as a plain struct.
//
// This header is shared, byte for byte, by the training environment and the
// submitted bot. The bot fills a View by parsing its stdin (BlockReader
// below); training fills it straight from the engine's state
// (core/state_view.h). tests/test_core_contract.py checks that the two give
// the same View for the same position, so everything computed from a View
// (core/features.h: the observation, the mask, the action decoding) is the
// same in training and in the judge by construction.
//
// C++20, standard library only: the judge compiles the bot with clang 20 and
// libc++ (-std=c++20 -O2 -msimd128).

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace core {

constexpr int kVision = 7;
constexpr int kRadius = 3;
constexpr int kTiles = kVision * kVision;
constexpr int kHeadTile = kRadius * kVision + kRadius;
constexpr int kMaxRounds = 500;

/// Directions in protocol order, which is also clockwise: turning right adds 1.
enum Dir : uint8_t
{
    kNorth = 0,
    kEast = 1,
    kSouth = 2,
    kWest = 3,
};

constexpr char kDirChars[4] = {'N', 'E', 'S', 'W'};
constexpr int kStepX[4] = {0, 1, 0, -1};
constexpr int kStepY[4] = {-1, 0, 1, 0};

inline int DirFromChar(char c)
{
    switch (c)
    {
    case 'N':
        return kNorth;
    case 'E':
        return kEast;
    case 'S':
        return kSouth;
    case 'W':
        return kWest;
    default:
        return -1;
    }
}

enum EdgeType : uint8_t
{
    kOpen = 0,
    kKelp = 1,
    kPortal = 2,
};

struct EdgeView
{
    uint8_t mKind = kOpen;
    int32_t mPortal = -1;

    bool operator==(EdgeView const&) const = default;
};

struct PartView
{
    /// -1 when no dragon is on the tile.
    int32_t mId = -1;
    /// 0 for team A, 1 for team B.
    uint8_t mTeam = 0;
    /// A head's heading; a body segment's direction towards the head.
    uint8_t mDir = 0;
    bool mHead = false;

    bool operator==(PartView const&) const = default;
};

struct TileView
{
    int16_t mX = 0;
    int16_t mY = 0;
    bool mPearl = false;
    /// Rounds until the tile next tries to spawn; -1 if it never does.
    int32_t mPearlIn = -1;
    /// The edges on its N, E, S and W sides.
    std::array<EdgeView, 4> mEdges{};
    PartView mPart{};

    bool operator==(TileView const&) const = default;
};

struct View
{
    // Init block.
    int32_t mId = -1;
    uint8_t mTeam = 0;
    int32_t mWidth = 0;
    int32_t mHeight = 0;
    int32_t mUnitLimit = 64;

    // Round block.
    int32_t mRound = 0;
    uint8_t mFacing = kNorth;
    int32_t mLength = 0;
    int32_t mUnitCount = 0;
    std::vector<uint64_t> mMessages;
    bool mHasEchoes = false;
    std::array<int32_t, 5> mEchoes{};
    /// Row by row from three tiles north of the head, each row from three
    /// tiles west: the head is kHeadTile.
    std::array<TileView, kTiles> mTiles{};

    bool operator==(View const&) const = default;
};

/// The tile one step from window tile `tile` towards `dir` within the window,
/// or -1 off its edge. A plain step: what lies past a portal is elsewhere.
inline int NeighbourInWindow(int tile, int dir)
{
    int const col = tile % kVision + kStepX[dir];
    int const row = tile / kVision + kStepY[dir];
    if (col < 0 || col >= kVision || row < 0 || row >= kVision)
    {
        return -1;
    }
    return row * kVision + col;
}

/// Reads the engine's stdin a line at a time into a View: the init block
/// (sent when the process starts), then a round block per turn. Feed()
/// returns true when a line completes a round block. Blank lines and
/// anything after '#' are ignored, as the official helpers do.
class BlockReader
{
  public:
    bool Feed(std::string_view line)
    {
        if (size_t const hash = line.find('#'); hash != std::string_view::npos)
        {
            line = line.substr(0, hash);
        }
        Fields f(line);
        if (f.Empty())
        {
            return false;
        }

        switch (mStage)
        {
        case Stage::Header:
            return Header(f);
        case Stage::Messages:
            mView.mMessages.push_back(f.U64());
            if (static_cast<int>(mView.mMessages.size()) == mMessages)
            {
                mStage = Stage::Tiles;
            }
            return false;
        case Stage::Tiles:
            if (mTile == 0 && f.Starts("ECHOES"))
            {
                f.Word();
                mView.mHasEchoes = true;
                for (int32_t& count : mView.mEchoes)
                {
                    count = static_cast<int32_t>(f.Int());
                }
                return false;
            }
            {
                TileView& tile = mView.mTiles[mTile++];
                tile.mX = static_cast<int16_t>(f.Int());
                tile.mY = static_cast<int16_t>(f.Int());
                tile.mPearl = f.Int() != 0;
                tile.mPearlIn = static_cast<int32_t>(f.Int());
                tile.mEdges = {};
                tile.mPart = {};
            }
            if (mTile == kTiles)
            {
                mStage = Stage::BodyCount;
            }
            return false;
        case Stage::BodyCount:
            f.Word();
            mBodies = static_cast<int>(f.Int());
            mStage = mBodies > 0 ? Stage::Bodies : Stage::HorizontalEdges;
            return false;
        case Stage::Bodies:
            Body(f);
            if (--mBodies == 0)
            {
                mStage = Stage::HorizontalEdges;
            }
            return false;
        case Stage::HorizontalEdges:
            // Row r is the north side of tile row r; row 7 the south side of row 6.
            for (int col = 0; col < kVision; col++)
            {
                EdgeView const edge = Edge(f.Word());
                if (mEdgeRow < kVision)
                {
                    mView.mTiles[mEdgeRow * kVision + col].mEdges[kNorth] = edge;
                }
                if (mEdgeRow > 0)
                {
                    mView.mTiles[(mEdgeRow - 1) * kVision + col].mEdges[kSouth] = edge;
                }
            }
            if (++mEdgeRow == kVision + 1)
            {
                mEdgeRow = 0;
                mStage = Stage::VerticalEdges;
            }
            return false;
        case Stage::VerticalEdges:
            // Column c is the west side of tile column c; column 7 the east side of column 6.
            for (int col = 0; col <= kVision; col++)
            {
                EdgeView const edge = Edge(f.Word());
                if (col < kVision)
                {
                    mView.mTiles[mEdgeRow * kVision + col].mEdges[kWest] = edge;
                }
                if (col > 0)
                {
                    mView.mTiles[mEdgeRow * kVision + col - 1].mEdges[kEast] = edge;
                }
            }
            if (++mEdgeRow == kVision)
            {
                mEdgeRow = 0;
                mStage = Stage::Header;
                return true;
            }
            return false;
        }
        return false;
    }

    View const& Current() const
    {
        return mView;
    }

  private:
    enum class Stage
    {
        Header,
        Messages,
        Tiles,
        BodyCount,
        Bodies,
        HorizontalEdges,
        VerticalEdges,
    };

    /// Whitespace-separated fields of one line.
    class Fields
    {
      public:
        explicit Fields(std::string_view line) : mRest(line)
        {
            Skip();
        }

        bool Empty() const
        {
            return mRest.empty();
        }

        bool Starts(std::string_view word) const
        {
            return mRest.substr(0, word.size()) == word;
        }

        std::string_view Word()
        {
            size_t end = 0;
            while (end < mRest.size() && !Space(mRest[end]))
            {
                end++;
            }
            std::string_view const word = mRest.substr(0, end);
            mRest.remove_prefix(end);
            Skip();
            return word;
        }

        int64_t Int()
        {
            std::string_view const word = Word();
            bool const negative = !word.empty() && word[0] == '-';
            int64_t value = 0;
            for (size_t i = negative ? 1 : 0; i < word.size(); i++)
            {
                value = value * 10 + (word[i] - '0');
            }
            return negative ? -value : value;
        }

        uint64_t U64()
        {
            uint64_t value = 0;
            for (char const c : Word())
            {
                value = value * 10 + static_cast<uint64_t>(c - '0');
            }
            return value;
        }

      private:
        static bool Space(char c)
        {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n';
        }

        void Skip()
        {
            while (!mRest.empty() && Space(mRest.front()))
            {
                mRest.remove_prefix(1);
            }
        }

        std::string_view mRest;
    };

    static EdgeView Edge(std::string_view symbol)
    {
        if (symbol == "w")
        {
            return {kKelp, -1};
        }
        if (symbol.empty() || symbol == ".")
        {
            return {kOpen, -1};
        }
        int32_t id = 0;
        for (char const c : symbol)
        {
            id = id * 10 + (c - '0');
        }
        return {kPortal, id};
    }

    bool Header(Fields& f)
    {
        std::string_view const key = f.Word();
        if (key == "ID")
        {
            mView.mId = static_cast<int32_t>(f.Int());
        }
        else if (key == "TEAM")
        {
            mView.mTeam = f.Word() == "B" ? 1 : 0;
        }
        else if (key == "MAP")
        {
            mView.mWidth = static_cast<int32_t>(f.Int());
            mView.mHeight = static_cast<int32_t>(f.Int());
        }
        else if (key == "UNIT_LIMIT")
        {
            mView.mUnitLimit = static_cast<int32_t>(f.Int());
        }
        else if (key == "ROUND")
        {
            mView.mRound = static_cast<int32_t>(f.Int());
        }
        else if (key == "DIR")
        {
            std::string_view const dir = f.Word();
            mView.mFacing = static_cast<uint8_t>(dir.empty() ? 0 : DirFromChar(dir[0]));
        }
        else if (key == "LENGTH")
        {
            mView.mLength = static_cast<int32_t>(f.Int());
        }
        else if (key == "UNIT_COUNT")
        {
            mView.mUnitCount = static_cast<int32_t>(f.Int());
        }
        else if (key == "NUM_MSGS")
        {
            mMessages = static_cast<int>(f.Int());
            mView.mMessages.clear();
            mView.mHasEchoes = false;
            mView.mEchoes = {};
            mTile = 0;
            mStage = mMessages > 0 ? Stage::Messages : Stage::Tiles;
        }
        return false;
    }

    void Body(Fields& f)
    {
        PartView part;
        part.mTeam = f.Word() == "B" ? 1 : 0;
        part.mId = static_cast<int32_t>(f.Int());
        int const x = static_cast<int>(f.Int());
        int const y = static_cast<int>(f.Int());
        std::string_view const dir = f.Word();
        part.mDir = static_cast<uint8_t>(dir.empty() ? 0 : DirFromChar(dir[0]));
        part.mHead = f.Int() != 0;
        for (TileView& tile : mView.mTiles)
        {
            if (tile.mX == x && tile.mY == y)
            {
                tile.mPart = part;
                break;
            }
        }
    }

    View mView;
    Stage mStage = Stage::Header;
    int mMessages = 0;
    int mTile = 0;
    int mBodies = 0;
    int mEdgeRow = 0;
};

} // namespace core
