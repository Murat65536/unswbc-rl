#include "engine/protocol.h"
#include "engine/helpers.h"

#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <format>
#include <sstream>
#include <string_view>

// PLEASE REFER TO examples/input.txt BEFORE YOU READ THIS! IT WILL  MAKE MORE SENSE TRUST

// Also some esoteric optimisations have been benched: we use += and not << or std::format since its ~3x faster

std::string BuildInitBlock(GameState const& state, Dragon const& dragon)
{
    return std::format("ID {}\nTEAM {}\nMAP {} {}\nUNIT_LIMIT {}\n", dragon.mId, static_cast<char>(dragon.mTeam), state.mWidth,
                       state.mHeight, state.mUnitLimit);
}

static Point VisionTileAt(GameState const& state, Point head, int column, int row)
{
    return WrapOntoBoard(state, {head.x + column - VISION_RADIUS, head.y + row - VISION_RADIUS});
}

static bool IsInVision(GameState const& state, Point head, Point tile)
{
    auto const within = [](int from, int to, int size) {
        int const offset = ((to - from) % size + size) % size;
        return offset <= VISION_RADIUS || offset >= size - VISION_RADIUS;
    };
    return within(head.x, tile.x, state.mWidth) && within(head.y, tile.y, state.mHeight);
}

static Direction FacingOfSegment(GameState const& state, Dragon const& dragon, size_t segment)
{
    if (segment == 0)
    {
        return dragon.mFacing;
    }
    std::optional<Direction> const towardsHead =
        DirectionOfStepBetween(state, dragon.mBody[segment], dragon.mBody[segment - 1]);
    RUNTIME_ASSERT(towardsHead, "dragon segments are not adjacent");
    return *towardsHead;
}

static void AppendInt(std::string& out, int value)
{
    if (value < 0)
    {
        out += '-';
    }
    auto magnitude = value < 0 ? 0u - static_cast<unsigned>(value) : static_cast<unsigned>(value);
    char digits[12];
    int at = 0;
    do
    {
        digits[at++] = static_cast<char>('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    while (at > 0)
    {
        out += digits[--at];
    }
}

static void AppendEdge(std::string& out, Edge const& edge)
{
    switch (edge.mKind)
    {
    case EdgeKind::Kelp:
        out += 'w';
        return;
    case EdgeKind::Portal:
        AppendInt(out, edge.mPortalId);
        return;
    case EdgeKind::Empty:
        break;
    }
    out += '.';
}

std::string BuildRoundBlock(GameState const& state, Dragon const& dragon)
{
    Point const head = dragon.mBody.front();

    std::string out;
    out.reserve(1536);
    bool const readsSonarEchoes = dragon.mProtocolMajor >= SONAR_ECHOES_PROTOCOL_MAJOR;
    std::vector<uint64_t> readableMessages;
    for (uint64_t const message : dragon.mSonarInbox)
    {
        if (readsSonarEchoes || message <= UINT32_MAX)
        {
            readableMessages.push_back(message);
        }
    }
    out = std::format("ROUND {}\nDIR {}\nLENGTH {}\nUNIT_COUNT {}\nNUM_MSGS {}\n", state.mRound,
                      static_cast<char>(dragon.mFacing), dragon.mBody.size(), AliveUnitCount(state, dragon.mTeam),
                      readableMessages.size());
    for (uint64_t const message : readableMessages)
    {
        out += std::format("{}\n", message);
    }
    if (readsSonarEchoes)
    {
        out += "ECHOES";
        for (int const count : dragon.mSonarEchoes)
        {
            out += std::format(" {}", count);
        }
        out += '\n';
    }

    for (int row = 0; row < VISION_SIZE; row++)
    {
        for (int column = 0; column < VISION_SIZE; column++)
        {
            Point const tile = VisionTileAt(state, head, column, row);
            Tile const& contents = state.mTiles.At(tile);
            int const pearlIn = contents.mSpawnsPearls ? contents.mNextPearl : -1;
            AppendInt(out, tile.x);
            out += ' ';
            AppendInt(out, tile.y);
            out += contents.mHasPearl ? " 1 " : " 0 ";
            AppendInt(out, pearlIn);
            out += '\n';
        }
    }

    std::string bodies;
    int bodyCount = 0;
    for (Dragon const& other : state.mDragons)
    {
        if (!other.mAlive)
        {
            continue;
        }
        for (size_t segment = 0; segment < other.mBody.size(); segment++)
        {
            Point const tile = other.mBody[segment];
            if (!IsInVision(state, head, tile))
            {
                continue;
            }
            bodies += static_cast<char>(other.mTeam);
            bodies += ' ';
            AppendInt(bodies, other.mId);
            bodies += ' ';
            AppendInt(bodies, tile.x);
            bodies += ' ';
            AppendInt(bodies, tile.y);
            bodies += ' ';
            bodies += static_cast<char>(FacingOfSegment(state, other, segment));
            bodies += segment == 0 ? " 1\n" : " 0\n";
            bodyCount++;
        }
    }
    out += "DRAGON_BODIES ";
    AppendInt(out, bodyCount);
    out += '\n';
    out += bodies;

    for (int row = 0; row <= VISION_SIZE; row++)
    {
        for (int column = 0; column < VISION_SIZE; column++)
        {
            EdgeIndex const edge = row < VISION_SIZE
                                       ? EdgeOnTileSide(state, VisionTileAt(state, head, column, row), Direction::North)
                                       : EdgeOnTileSide(state, VisionTileAt(state, head, column, row - 1), Direction::South);
            if (column > 0)
            {
                out += ' ';
            }
            AppendEdge(out, EdgeAt(state, edge));
        }
        out += "\n";
    }

    for (int row = 0; row < VISION_SIZE; row++)
    {
        for (int column = 0; column <= VISION_SIZE; column++)
        {
            EdgeIndex const edge = column < VISION_SIZE
                                       ? EdgeOnTileSide(state, VisionTileAt(state, head, column, row), Direction::West)
                                       : EdgeOnTileSide(state, VisionTileAt(state, head, column - 1, row), Direction::East);
            if (column > 0)
            {
                out += ' ';
            }
            AppendEdge(out, EdgeAt(state, edge));
        }
        out += "\n";
    }

    return out;
}

static char const* AfterSpaces(char const* text)
{
    // just in case some guy wants to do goofy utf8 trickery
    while (isspace(static_cast<unsigned char>(*text)))
    {
        text++;
    }
    return text;
}
// check that we actually read up to end of line and theres no extra stuff being sent
static bool NothingAfter(char const* args, int consumed)
{
    return args[consumed] == '\0';
}

// read the actual steps of a multi-move MOVE command
static std::optional<std::vector<Direction>> StepsFrom(char const* letters)
{
    std::vector<Direction> steps;
    char const* at = AfterSpaces(letters);
    for (; *at != '\0' && !isspace(static_cast<unsigned char>(*at)); at++)
    {
        switch (*at)
        {
        case 'N':
        case 'E':
        case 'S':
        case 'W':
            steps.push_back(static_cast<Direction>(*at));
            break;
        default:
            return std::nullopt;
        }
    }
    if (steps.empty() || *AfterSpaces(at) != '\0')
    {
        return std::nullopt;
    }
    return steps;
}

static std::optional<std::pair<Direction, uint64_t>> DirectedSonarFrom(char const* args)
{
    char direction = 0;
    char digits[21] = "";
    int consumed = 0;
    if (sscanf(args, " %c %20[0-9] %n", &direction, digits, &consumed) != 2 || !NothingAfter(args, consumed) ||
        std::string_view("NESW").find(direction) == std::string_view::npos)
    {
        return std::nullopt;
    }
    uint64_t value = 0;
    char const* const digitsEnd = digits + strlen(digits);
    auto const [end, error] = std::from_chars(digits, digitsEnd, value);
    if (error != std::errc{} || end != digitsEnd)
    {
        return std::nullopt;
    }
    return std::pair{static_cast<Direction>(direction), value};
}

ControllerReply ReadReply(Dragon const& dragon, std::string const& replyText, DebugOutput const& keep, EventSink const& emit)
{
    ControllerReply reply;
    std::istringstream lines(replyText.substr(0, replyText.rfind('\n') + 1));
    std::string line;

    while (std::getline(lines, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }

        char keyword[16] = "";
        int argsStart = 0;
        sscanf(line.c_str(), "%15s%n", keyword, &argsStart);
        std::string const command = keyword;
        char const* args = line.c_str() + argsStart;
        int consumed = 0;

        if (command.empty())
        {
            continue;
        }
        // Sandbox/host framing. Not a game command; ignore it if it is still in the reply.
        if (command == "ENDTURN" && *AfterSpaces(args) == '\0')
        {
            break;
        }
        if (command == "MOVE")
        {
            if (std::optional<std::vector<Direction>> const steps = StepsFrom(args))
            {
                reply.mAction = ActionMove{*steps};
                continue;
            }
        }
        else if (command == "SPLIT")
        {
            int childSegmentCount = 0;
            if (sscanf(args, "%d %n", &childSegmentCount, &consumed) == 1 && NothingAfter(args, consumed))
            {
                reply.mAction = ActionSplit{childSegmentCount};
                continue;
            }
        }
        else if (command == "SONAR")
        {
            uint32_t value = 0;
            if (sscanf(args, "%u %n", &value, &consumed) == 1 && NothingAfter(args, consumed))
            {
                reply.mSonar = value;
                continue;
            }
            if (std::optional<std::pair<Direction, uint64_t>> const sonar = DirectedSonarFrom(args))
            {
                reply.mDirectedSonars[sonar->first] = sonar->second;
                continue;
            }
        }
        else if (command == "PROTOCOL")
        {
            int major = 0;
            if (sscanf(args, "%d %n", &major, &consumed) == 1 && NothingAfter(args, consumed))
            {
                reply.mProtocolMajor = major;
                continue;
            }
        }
        else if (command == "INDICATOR")
        {
            if (keep.mIndicator)
            {
                std::string_view const text = AfterSpaces(args);
                reply.mIndicator = std::string(keep.mLimits ? text.substr(0, MAX_INDICATOR) : text);
            }
            continue;
        }
        else if (command == "LOG")
        {
            if (keep.mLogs)
            {
                emit(EventDragonLog{dragon.mId, AfterSpaces(args)});
            }
            continue;
        }
        else if (command == "DOT")
        {
            DebugDraw dot{DebugShape::Dot, {}, {}, 0, 0, 0};
            if (sscanf(args, "%d %d %hhu %hhu %hhu %n", &dot.mFrom.x, &dot.mFrom.y, &dot.mRed, &dot.mGreen, &dot.mBlue,
                       &consumed) == 5 &&
                NothingAfter(args, consumed))
            {
                dot.mTo = dot.mFrom;
                if (keep.mDraw)
                {
                    emit(EventDebugDraw{dragon.mId, dot});
                }
                continue;
            }
        }
        else if (command == "LINE")
        {
            DebugDraw segment{DebugShape::Line, {}, {}, 0, 0, 0};
            if (sscanf(args, "%d %d %d %d %hhu %hhu %hhu %n", &segment.mFrom.x, &segment.mFrom.y, &segment.mTo.x,
                       &segment.mTo.y, &segment.mRed, &segment.mGreen, &segment.mBlue, &consumed) == 7 &&
                NothingAfter(args, consumed))
            {
                if (keep.mDraw)
                {
                    emit(EventDebugDraw{dragon.mId, segment});
                }
                continue;
            }
        }

        if (keep.mParse)
        {
            emit(EventEngineLog{dragon.mId, "can't read line: " + line});
        }
    }
    return reply;
}
