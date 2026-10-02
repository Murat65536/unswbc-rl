// Python bindings for the C++ core (module `bccore`).

#include "core/batch_env.h"
#include "core/careful.h"
#include "core/features.h"
#include "core/mapgen.h"
#include "core/state_view.h"
#include "core/view.h"

#include "engine/actions.h"
#include "engine/game.h"
#include "engine/helpers.h"
#include "engine/protocol.h"
#include "engine/scoring.h"

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/vector.h>

#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace nb = nanobind;
using namespace nb::literals;

namespace {

Symmetry SymmetryFrom(std::string const& name)
{
    if (name == "x")
    {
        return Symmetry::X;
    }
    if (name == "y")
    {
        return Symmetry::Y;
    }
    if (name == "xy")
    {
        return Symmetry::XY;
    }
    throw nb::value_error("symmetry must be 'x', 'y' or 'xy'");
}

template <typename T> nb::ndarray<nb::numpy, T> ToNumpy(std::vector<T> values)
{
    auto* owned = new std::vector<T>(std::move(values));
    nb::capsule owner(owned, [](void* p) noexcept { delete static_cast<std::vector<T>*>(p); });
    return nb::ndarray<nb::numpy, T>(owned->data(), {owned->size()}, owner);
}

template <typename T> nb::ndarray<nb::numpy, T> ToNumpy2(std::vector<T> values, size_t columns)
{
    size_t const rows = columns == 0 ? 0 : values.size() / columns;
    auto* owned = new std::vector<T>(std::move(values));
    nb::capsule owner(owned, [](void* p) noexcept { delete static_cast<std::vector<T>*>(p); });
    return nb::ndarray<nb::numpy, T>(owned->data(), {rows, columns}, owner);
}

core::View ViewFromBlocks(std::string const& init, std::string const& round)
{
    core::BlockReader reader;
    bool done = false;
    std::istringstream lines(init + round);
    std::string line;
    while (std::getline(lines, line))
    {
        done = reader.Feed(line);
    }
    if (!done)
    {
        throw nb::value_error("the round block is incomplete");
    }
    return reader.Current();
}

/// (observation, masks) for a View: masks[level] is the mask at that level.
nb::tuple Features(core::View const& view)
{
    std::vector<uint8_t> obs(core::kObsSize);
    core::Encode(view, obs.data());
    std::vector<uint8_t> masks(core::kMaskLevels * core::kNumActions);
    for (int level = 0; level < core::kMaskLevels; level++)
    {
        core::Mask(view, level, masks.data() + level * core::kNumActions);
    }
    return nb::make_tuple(ToNumpy(std::move(obs)), ToNumpy2(std::move(masks), core::kNumActions));
}

/// None if the two Views are equal, else where they first differ.
std::optional<std::string> ViewDifference(core::View const& a, core::View const& b)
{
    if (a == b)
    {
        return std::nullopt;
    }
    std::ostringstream out;
    auto const field = [&](char const* name, auto x, auto y) {
        if (x != y && out.tellp() == 0)
        {
            out << name << ": " << +x << " != " << +y;
        }
    };
    field("id", a.mId, b.mId);
    field("team", a.mTeam, b.mTeam);
    field("width", a.mWidth, b.mWidth);
    field("height", a.mHeight, b.mHeight);
    field("unit limit", a.mUnitLimit, b.mUnitLimit);
    field("round", a.mRound, b.mRound);
    field("facing", a.mFacing, b.mFacing);
    field("length", a.mLength, b.mLength);
    field("unit count", a.mUnitCount, b.mUnitCount);
    field("messages", a.mMessages.size(), b.mMessages.size());
    field("has echoes", a.mHasEchoes, b.mHasEchoes);
    for (int i = 0; i < core::kTiles && out.tellp() == 0; i++)
    {
        core::TileView const& x = a.mTiles[i];
        core::TileView const& y = b.mTiles[i];
        if (x == y)
        {
            continue;
        }
        out << "tile " << i << " (" << x.mX << "," << x.mY << ") vs (" << y.mX << "," << y.mY << "): pearl " << x.mPearl
            << "/" << y.mPearl << " in " << x.mPearlIn << "/" << y.mPearlIn << " part " << x.mPart.mId << "/"
            << y.mPart.mId << " dir " << +x.mPart.mDir << "/" << +y.mPart.mDir << " head " << x.mPart.mHead << "/"
            << y.mPart.mHead << " edges";
        for (int side = 0; side < 4; side++)
        {
            out << " " << +x.mEdges[side].mKind << ":" << x.mEdges[side].mPortal << "/" << +y.mEdges[side].mKind << ":"
                << y.mEdges[side].mPortal;
        }
    }
    if (out.tellp() == 0)
    {
        out << "messages or echoes differ";
    }
    return out.str();
}

nb::dict ResultDict(GameResult const& result, int rounds)
{
    nb::dict out;
    out["terminated"] = result.mTerminated;
    out["rounds"] = rounds;
    out["end_reason"] = result.mEndReason == GameEndReason::TeamEliminated ? 0 : 1;
    out["winner"] = result.mWinner ? nb::cast(std::string(1, static_cast<char>(*result.mWinner))) : nb::none();
    for (auto const& [name, standing] : {std::pair{"a", result.mTeamA}, std::pair{"b", result.mTeamB}})
    {
        out[(std::string(name) + "_dragons").c_str()] = standing.mDragonCount;
        out[(std::string(name) + "_length").c_str()] = standing.mTotalLength;
        out[(std::string(name) + "_longest").c_str()] = standing.mLongestDragon;
        out[(std::string(name) + "_queen").c_str()] = standing.mQueenLength;
    }
    return out;
}

/// One game on the vendored engine, played either whole with text
/// controllers (`run`, what the fidelity test compares with the real judge)
/// or a turn at a time (`begin`, `current`, then `reply`/`move`/`split`).
class Match
{
  public:
    Match(std::string const& mapText, uint64_t seed, bool record)
        : mGame(std::make_unique<Game>(LoadMap(mapText), seed, DebugOutput{.mRecord = record}))
    {
    }

    nb::dict Run(nb::callable reply, nb::object spawn, nb::object death)
    {
        // The game keeps its controllers as std::functions, which Python's
        // garbage collector cannot see into: they capture only `this`, and
        // the callables live here for the length of the run, so a callback
        // that refers back to this match is not a leaked cycle.
        mReply = reply;
        mSpawn = spawn;
        mDeath = death;
        struct Release
        {
            Match& mMatch;
            ~Release()
            {
                mMatch.mReply = mMatch.mSpawn = mMatch.mDeath = nb::none();
            }
        } release{*this};

        if (!death.is_none())
        {
            mGame->OnEvent([this](Event const& event) {
                if (auto const* dead = std::get_if<EventDragonDeath>(&event))
                {
                    mDeath(dead->mId, mGame->State().mRound, std::string(1, static_cast<char>(dead->mReason)));
                }
            });
        }
        mGame->SpawnControllersWith([this](Dragon const& dragon) -> DragonFn {
            if (!mSpawn.is_none())
            {
                mSpawn(dragon.mId, BuildInitBlock(mGame->State(), dragon));
            }
            DragonId const id = dragon.mId;
            return [this, id](std::string const& block) { return nb::cast<std::string>(mReply(id, block)); };
        });
        GameResult const result = mGame->Run();
        return ResultDict(result, mGame->State().mRound);
    }

    void Begin()
    {
        mGame->Begin();
    }

    std::optional<int> Current() const
    {
        return mGame->CurrentDragon();
    }

    std::string InitBlock(int id) const
    {
        return BuildInitBlock(mGame->State(), Find(id));
    }

    std::string RoundBlock(int id) const
    {
        return BuildRoundBlock(mGame->State(), Find(id));
    }

    void Reply(std::string const& text)
    {
        std::optional<DragonId> const id = mGame->CurrentDragon();
        if (!id)
        {
            throw nb::value_error("the game is over");
        }
        mGame->TakeTurnWith(ReadReply(Find(*id), text, DebugOutput{}, [](Event const&) {}));
    }

    void Act(PlayerAction action)
    {
        if (!mGame->CurrentDragon())
        {
            throw nb::value_error("the game is over");
        }
        ControllerReply reply;
        reply.mAction = std::move(action);
        mGame->TakeTurnWith(reply);
    }

    void MoveSteps(std::string const& steps)
    {
        ActionMove move;
        for (char const c : steps)
        {
            if (c != 'N' && c != 'E' && c != 'S' && c != 'W')
            {
                throw nb::value_error("steps are N, E, S and W");
            }
            move.mSteps.push_back(static_cast<Direction>(c));
        }
        Act(move);
    }

    nb::dict Result() const
    {
        GameState const& state = mGame->State();
        GameResult const result = mGame->Over() ? mGame->Result() : ResultAfterRound(state);
        return ResultDict(result, state.mRound);
    }

    /// (id, team, alive, facing, body head first) for every dragon ever spawned.
    std::vector<std::tuple<int, std::string, bool, std::string, std::vector<std::pair<int, int>>>> Dragons() const
    {
        std::vector<std::tuple<int, std::string, bool, std::string, std::vector<std::pair<int, int>>>> out;
        for (Dragon const& dragon : mGame->State().mDragons)
        {
            std::vector<std::pair<int, int>> body;
            for (Point const p : dragon.mBody)
            {
                body.emplace_back(p.x, p.y);
            }
            out.emplace_back(dragon.mId, std::string(1, static_cast<char>(dragon.mTeam)), dragon.mAlive,
                             std::string(1, static_cast<char>(dragon.mFacing)), std::move(body));
        }
        return out;
    }

    /// Row-major [y][x] pearls and countdowns (-1 where a tile never spawns).
    std::pair<std::vector<std::vector<bool>>, std::vector<std::vector<int>>> Tiles() const
    {
        GameState const& state = mGame->State();
        std::vector<std::vector<bool>> pearls(state.mHeight, std::vector<bool>(state.mWidth));
        std::vector<std::vector<int>> countdowns(state.mHeight, std::vector<int>(state.mWidth));
        for (int y = 0; y < state.mHeight; y++)
        {
            for (int x = 0; x < state.mWidth; x++)
            {
                Tile const& tile = state.mTiles.At(x, y);
                pearls[y][x] = tile.mHasPearl;
                countdowns[y][x] = tile.mSpawnsPearls ? tile.mNextPearl : -1;
            }
        }
        return {pearls, countdowns};
    }

    void SetPearl(int x, int y, bool present)
    {
        GameState& state = const_cast<GameState&>(mGame->State());
        if (x < 0 || y < 0 || x >= state.mWidth || y >= state.mHeight)
        {
            throw nb::index_error("off the board");
        }
        state.mTiles.At(x, y).mHasPearl = present;
    }

    GameState const& State() const
    {
        return mGame->State();
    }

    core::View ViewOf(int id) const
    {
        core::View view;
        core::ViewFromState(mGame->State(), Find(id), view);
        return view;
    }

    /// Whether the current dragon would die this turn taking `action`: the
    /// action is decoded from its View and played on a copy of the board.
    bool Probe(int action) const
    {
        std::optional<DragonId> const id = mGame->CurrentDragon();
        if (!id || action < 0 || action >= core::kNumActions)
        {
            throw nb::value_error("no current dragon, or no such action");
        }
        GameState copy = mGame->State();
        Dragon& dragon = copy.mDragons[*id];
        core::View view;
        core::ViewFromState(copy, dragon, view);
        core::Command const command = core::Decode(view, action);
        if (command.mDissolve)
        {
            return true;
        }
        if (command.mSplit)
        {
            Split(copy, dragon, command.mSplitSize, {});
        }
        else
        {
            std::vector<Direction> steps;
            for (int i = 0; i < command.mSteps; i++)
            {
                steps.push_back(static_cast<Direction>(core::kDirChars[command.mDirs[i]]));
            }
            Move(copy, dragon, steps, {});
        }
        return !copy.mDragons[*id].mAlive;
    }

    /// Raises unless the occupancy grid holds exactly the living bodies.
    void CheckInvariants() const
    {
        GameState const& state = mGame->State();
        Array2d<DragonId> expected(state.mWidth, state.mHeight, NO_DRAGON);
        for (Dragon const& dragon : state.mDragons)
        {
            if (dragon.mId != static_cast<int>(&dragon - state.mDragons.data()))
            {
                throw std::runtime_error("dragon ids are not their indices");
            }
            if (!dragon.mAlive)
            {
                continue;
            }
            for (Point const segment : dragon.mBody)
            {
                if (expected.At(segment) != NO_DRAGON)
                {
                    throw std::runtime_error("two living segments share a tile");
                }
                expected.At(segment) = dragon.mId;
            }
        }
        for (int y = 0; y < state.mHeight; y++)
        {
            for (int x = 0; x < state.mWidth; x++)
            {
                if (expected.At(x, y) != state.mOccupant.At(x, y))
                {
                    throw std::runtime_error("occupancy grid is wrong at (" + std::to_string(x) + ", " +
                                             std::to_string(y) + ")");
                }
            }
        }
    }

  private:
    Dragon const& Find(int id) const
    {
        GameState const& state = mGame->State();
        if (id < 0 || id >= static_cast<int>(state.mDragons.size()))
        {
            throw nb::index_error("no such dragon");
        }
        return state.mDragons[id];
    }

    std::unique_ptr<Game> mGame;
    nb::object mReply = nb::none();
    nb::object mSpawn = nb::none();
    nb::object mDeath = nb::none();
};

/// None if two boards are the same in every field the engine reads.
std::optional<std::string> BoardDifference(GameState const& a, GameState const& b)
{
    auto const differ = [](std::string what) { return std::optional<std::string>(std::move(what)); };
    if (a.mWidth != b.mWidth || a.mHeight != b.mHeight || a.mUnitLimit != b.mUnitLimit || a.mRound != b.mRound ||
        a.mSymmetry != b.mSymmetry || a.mNextDragonId != b.mNextDragonId)
    {
        return differ("header");
    }
    for (int y = 0; y < a.mHeight; y++)
    {
        for (int x = 0; x < a.mWidth; x++)
        {
            Tile const& s = a.mTiles.At(x, y);
            Tile const& t = b.mTiles.At(x, y);
            if (s.mHasPearl != t.mHasPearl || s.mSpawnsPearls != t.mSpawnsPearls || s.mMinRespawnGap != t.mMinRespawnGap ||
                s.mMaxRespawnGap != t.mMaxRespawnGap || s.mNextPearl != t.mNextPearl)
            {
                return differ("tile " + std::to_string(x) + "," + std::to_string(y));
            }
            for (int axis = 0; axis < 2; axis++)
            {
                Edge const& e = (axis == 0 ? a.mHorizontalEdges : a.mVerticalEdges).At(x, y);
                Edge const& f = (axis == 0 ? b.mHorizontalEdges : b.mVerticalEdges).At(x, y);
                if (e.mKind != f.mKind || e.mPortalId != f.mPortalId || !(e.mPortalPartner == f.mPortalPartner))
                {
                    return differ("edge " + std::to_string(axis) + " " + std::to_string(x) + "," + std::to_string(y));
                }
            }
            if (a.mOccupant.At(x, y) != b.mOccupant.At(x, y))
            {
                return differ("occupant " + std::to_string(x) + "," + std::to_string(y));
            }
        }
    }
    if (a.mDragons.size() != b.mDragons.size())
    {
        return differ("dragon count");
    }
    for (size_t i = 0; i < a.mDragons.size(); i++)
    {
        Dragon const& d = a.mDragons[i];
        Dragon const& e = b.mDragons[i];
        if (d.mId != e.mId || d.mTeam != e.mTeam || d.mBody != e.mBody || d.mFacing != e.mFacing ||
            d.mProtocolMajor != e.mProtocolMajor || d.mAlive != e.mAlive)
        {
            return differ("dragon " + std::to_string(i));
        }
    }
    return std::nullopt;
}

template <typename T> using Array2 = nb::ndarray<T, nb::shape<-1, -1>, nb::c_contig, nb::device::cpu>;
template <typename T> using Array1 = nb::ndarray<T, nb::shape<-1>, nb::c_contig, nb::device::cpu>;

core::EnvConfig ConfigFrom(nb::dict const& options)
{
    core::EnvConfig c;
    for (auto [key, value] : options)
    {
        std::string const name = nb::cast<std::string>(key);
        auto const pair = [&](int& lo, int& hi) {
            auto const [a, b] = nb::cast<std::pair<int, int>>(value);
            lo = a;
            hi = b;
        };
        auto const pairf = [&](float& lo, float& hi) {
            auto const [a, b] = nb::cast<std::pair<float, float>>(value);
            lo = a;
            hi = b;
        };
        if (name == "threads") c.mThreads = nb::cast<int>(value);
        else if (name == "mask_level") c.mMaskLevel = nb::cast<int>(value);
        else if (name == "max_rounds") c.mMaxRounds = nb::cast<int>(value);
        else if (name == "gamma") c.mGamma = nb::cast<float>(value);
        else if (name == "win") c.mWin = nb::cast<float>(value);
        else if (name == "loss") c.mLoss = nb::cast<float>(value);
        else if (name == "draw") c.mDraw = nb::cast<float>(value);
        else if (name == "shaping") c.mShaping = nb::cast<float>(value);
        else if (name == "queen_weight") c.mQueenWeight = nb::cast<float>(value);
        else if (name == "longest_weight") c.mLongestWeight = nb::cast<float>(value);
        else if (name == "width") pair(c.mWidthLo, c.mWidthHi);
        else if (name == "height") pair(c.mHeightLo, c.mHeightHi);
        else if (name == "symmetries")
        {
            c.mSymmetries = 0;
            for (std::string const& s : nb::cast<std::vector<std::string>>(value))
            {
                c.mSymmetries |= s == "x" ? 1 : s == "y" ? 2 : s == "xy" ? 4 : throw nb::value_error("symmetries are x, y, xy");
            }
        }
        else if (name == "dragons_per_team") pair(c.mDragonsLo, c.mDragonsHi);
        else if (name == "start_length") pair(c.mStartLengthLo, c.mStartLengthHi);
        else if (name == "kelp") pairf(c.mKelpLo, c.mKelpHi);
        else if (name == "portal_pairs") pair(c.mPortalsLo, c.mPortalsHi);
        else if (name == "pearl_beds") pairf(c.mBedsLo, c.mBedsHi);
        else if (name == "gap_min_hi") pair(c.mGapMinHiLo, c.mGapMinHiHi);
        else if (name == "gap_spread") pair(c.mGapSpreadLo, c.mGapSpreadHi);
        else if (name == "unit_limit") c.mUnitLimit = nb::cast<int>(value);
        else if (name == "maps") c.mMaps = nb::cast<std::vector<std::string>>(value);
        else if (name == "map_prob") c.mMapProb = nb::cast<float>(value);
        else if (name == "scripted_frac") c.mScriptedFrac = nb::cast<float>(value);
        else if (name == "scripted_careful") c.mScriptedCareful = nb::cast<float>(value);
        else throw nb::value_error(("unknown environment option: " + name).c_str());
    }
    return c;
}

/// The environment, writing into arrays the caller owns (numpy or torch).
/// One dragon's process as far as features go: its Memory across its turns,
/// and what it works out each turn, exactly as the bot (and the training
/// environment) does.
class Brain
{
  public:
    nb::tuple Observe(core::View view)
    {
        mView = std::move(view);
        mMemory.Update(mView);
        mSight = core::LookAhead(mView, &mMemory);
        mObserved = true;
        std::vector<uint8_t> obs(core::kObsSize);
        core::Encode(mView, mSight, obs.data());
        std::vector<uint8_t> masks(core::kMaskLevels * core::kNumActions);
        for (int level = 0; level < core::kMaskLevels; level++)
        {
            core::Mask(mView, mSight, level, masks.data() + level * core::kNumActions);
        }
        return nb::make_tuple(ToNumpy(std::move(obs)), ToNumpy2(std::move(masks), core::kNumActions));
    }

    core::View const& Current() const
    {
        if (!mObserved)
        {
            throw nb::value_error("observe a turn first");
        }
        return mView;
    }

    int Careful(uint32_t noise) const
    {
        return core::CarefulAction(Current(), noise, &mMemory);
    }

    std::string DecodeAction(int action) const
    {
        if (action < 0 || action >= core::kNumActions)
        {
            throw nb::value_error("no such action");
        }
        return core::FormatCommand(core::Decode(Current(), action));
    }

    std::vector<std::pair<int, int>> Body() const
    {
        std::vector<std::pair<int, int>> out;
        for (core::Cell const& cell : mMemory.mBody)
        {
            out.emplace_back(cell.mX, cell.mY);
        }
        return out;
    }

    nb::tuple SurvivalOf() const
    {
        Current();
        return nb::make_tuple(std::vector<int>(mSight.mSurvive.begin(), mSight.mSurvive.end()), int(mSight.mHorizon));
    }

  private:
    core::Memory mMemory;
    core::View mView;
    core::MoveSight mSight;
    bool mObserved = false;
};

class PyBatchEnv
{
  public:
    PyBatchEnv(int numGames, uint64_t seed, nb::dict const& options) : mEnv(numGames, ConfigFrom(options), seed) {}

    nb::dict Reset(Array2<uint8_t> obs, Array2<uint8_t> mask, Array2<float> privileged, Array1<int64_t> prevRow,
                   Array1<float> prevReward, Array2<int32_t> info)
    {
        core::DecisionOut const out = Out(obs, mask, privileged, prevRow, prevReward, info);
        {
            nb::gil_scoped_release release;
            mEnv.Reset(out);
        }
        return Results();
    }

    nb::dict Step(Array1<int32_t> actions, Array2<uint8_t> obs, Array2<uint8_t> mask, Array2<float> privileged,
                  Array1<int64_t> prevRow, Array1<float> prevReward, Array2<int32_t> info)
    {
        Check(actions.shape(0) == static_cast<size_t>(mEnv.NumGames()), "actions");
        core::DecisionOut const out = Out(obs, mask, privileged, prevRow, prevReward, info);
        int32_t const* data = actions.data();
        {
            nb::gil_scoped_release release;
            mEnv.Step(data, out);
        }
        return Results();
    }

    core::BatchEnv& Env()
    {
        return mEnv;
    }

  private:
    static void Check(bool ok, char const* what)
    {
        if (!ok)
        {
            throw nb::value_error((std::string("wrong shape: ") + what).c_str());
        }
    }

    core::DecisionOut Out(Array2<uint8_t>& obs, Array2<uint8_t>& mask, Array2<float>& privileged, Array1<int64_t>& prevRow,
                          Array1<float>& prevReward, Array2<int32_t>& info)
    {
        size_t const n = mEnv.NumGames();
        Check(obs.shape(0) == n && obs.shape(1) == core::kObsSize, "obs");
        Check(mask.shape(0) == n && mask.shape(1) == core::kNumActions, "mask");
        Check(privileged.shape(0) == n && privileged.shape(1) == core::kPrivileged, "privileged");
        Check(prevRow.shape(0) == n && prevReward.shape(0) == n, "prev_row / prev_reward");
        Check(info.shape(0) == n && info.shape(1) == core::kInfo, "info");
        return {obs.data(), mask.data(), privileged.data(), prevRow.data(), prevReward.data(), info.data()};
    }

    nb::dict Results() const
    {
        nb::dict out;
        auto const& done = mEnv.Completions();
        std::vector<int64_t> rows;
        std::vector<float> rewards;
        std::vector<int32_t> boots;
        for (core::Completion const& c : done)
        {
            rows.push_back(c.mRow);
            rewards.push_back(c.mReward);
            boots.push_back(c.mBoot);
        }
        out["done_row"] = ToNumpy(std::move(rows));
        out["done_reward"] = ToNumpy(std::move(rewards));
        out["done_boot"] = ToNumpy(std::move(boots));
        out["boot_obs"] = ToNumpy2(mEnv.BootObs(), core::kObsSize);
        out["boot_mask"] = ToNumpy2(mEnv.BootMask(), core::kNumActions);
        out["boot_priv"] = ToNumpy2(mEnv.BootPrivileged(), core::kPrivileged);

        std::vector<int32_t> slot, mapIndex, rounds, outcome, dragons, totalA, totalB, queenA, queenB, longestA, longestB,
            queenDiedA, queenDiedB, queenDeathA, queenDeathB, queenSplitsA, queenSplitsB, decider, scripted, careful;
        std::vector<uint64_t> level;
        for (core::EpisodeInfo const& e : mEnv.Episodes())
        {
            slot.push_back(e.mSlot);
            level.push_back(e.mLevelSeed);
            mapIndex.push_back(e.mMapIndex);
            rounds.push_back(e.mRounds);
            outcome.push_back(e.mOutcome);
            dragons.push_back(e.mDragons);
            totalA.push_back(e.mTotal[0]);
            totalB.push_back(e.mTotal[1]);
            queenA.push_back(e.mQueen[0]);
            queenB.push_back(e.mQueen[1]);
            longestA.push_back(e.mLongest[0]);
            longestB.push_back(e.mLongest[1]);
            queenDiedA.push_back(e.mQueenDied[0]);
            queenDiedB.push_back(e.mQueenDied[1]);
            queenDeathA.push_back(e.mQueenDeath[0]);
            queenDeathB.push_back(e.mQueenDeath[1]);
            queenSplitsA.push_back(e.mQueenSplits[0]);
            queenSplitsB.push_back(e.mQueenSplits[1]);
            decider.push_back(e.mDecider);
            scripted.push_back(e.mScriptedTeam);
            careful.push_back(e.mScriptedCareful);
        }
        nb::dict episodes;
        episodes["slot"] = ToNumpy(std::move(slot));
        episodes["level_seed"] = ToNumpy(std::move(level));
        episodes["map_index"] = ToNumpy(std::move(mapIndex));
        episodes["rounds"] = ToNumpy(std::move(rounds));
        episodes["outcome"] = ToNumpy(std::move(outcome));
        episodes["dragons"] = ToNumpy(std::move(dragons));
        episodes["total_a"] = ToNumpy(std::move(totalA));
        episodes["total_b"] = ToNumpy(std::move(totalB));
        episodes["queen_a"] = ToNumpy(std::move(queenA));
        episodes["queen_b"] = ToNumpy(std::move(queenB));
        episodes["longest_a"] = ToNumpy(std::move(longestA));
        episodes["longest_b"] = ToNumpy(std::move(longestB));
        episodes["queen_died_a"] = ToNumpy(std::move(queenDiedA));
        episodes["queen_died_b"] = ToNumpy(std::move(queenDiedB));
        episodes["queen_death_a"] = ToNumpy(std::move(queenDeathA));
        episodes["queen_death_b"] = ToNumpy(std::move(queenDeathB));
        episodes["queen_splits_a"] = ToNumpy(std::move(queenSplitsA));
        episodes["queen_splits_b"] = ToNumpy(std::move(queenSplitsB));
        episodes["decider"] = ToNumpy(std::move(decider));
        episodes["scripted_team"] = ToNumpy(std::move(scripted));
        episodes["scripted_careful"] = ToNumpy(std::move(careful));
        out["episodes"] = episodes;
        return out;
    }

    core::BatchEnv mEnv;
};

} // namespace

NB_MODULE(bccore, m)
{
    m.doc() = "The C++ core: the organisers' engine at the 1.2.3 rules, the map generator, and (later) the shared "
              "observation core and the batched training environment.";

    m.attr("MAX_ROUNDS") = MAX_ROUNDS;
    m.attr("VISION_SIZE") = VISION_SIZE;

    m.def(
        "generate_map",
        [](int width, int height, std::string const& symmetry, uint64_t seed, int dragons_per_team, int start_length,
           double kelp, int portal_pairs, double pearl_beds, int gap_min_lo, int gap_min_hi, int gap_spread,
           int unit_limit) {
            core::MapGenConfig config;
            config.mWidth = width;
            config.mHeight = height;
            config.mSymmetry = SymmetryFrom(symmetry);
            config.mDragonsPerTeam = dragons_per_team;
            config.mStartLength = start_length;
            config.mKelp = kelp;
            config.mPortalPairs = portal_pairs;
            config.mPearlBeds = pearl_beds;
            config.mGapMinLo = gap_min_lo;
            config.mGapMinHi = gap_min_hi;
            config.mGapSpread = gap_spread;
            config.mUnitLimit = unit_limit;
            return core::GenerateMapText(config, seed);
        },
        "width"_a, "height"_a, "symmetry"_a = "xy", "seed"_a = 0, "dragons_per_team"_a = 2, "start_length"_a = 3,
        "kelp"_a = 0.05, "portal_pairs"_a = 2, "pearl_beds"_a = 0.4, "gap_min_lo"_a = 1, "gap_min_hi"_a = 10,
        "gap_spread"_a = 60, "unit_limit"_a = DEFAULT_UNIT_LIMIT,
        "A symmetric .map text, the same for the same arguments on every platform.");
    m.def(
        "generated_state_difference",
        [](int width, int height, std::string const& symmetry, uint64_t seed, int dragons_per_team, int portal_pairs,
           double kelp, int unit_limit) {
            core::MapGenConfig config;
            config.mWidth = width;
            config.mHeight = height;
            config.mSymmetry = SymmetryFrom(symmetry);
            config.mDragonsPerTeam = dragons_per_team;
            config.mPortalPairs = portal_pairs;
            config.mKelp = kelp;
            config.mUnitLimit = unit_limit;
            return BoardDifference(core::GenerateMapState(config, seed), LoadMap(core::GenerateMapText(config, seed)));
        },
        "width"_a, "height"_a, "symmetry"_a, "seed"_a, "dragons_per_team"_a = 2, "portal_pairs"_a = 2, "kelp"_a = 0.05,
        "unit_limit"_a = DEFAULT_UNIT_LIMIT,
        "None if the board built directly equals LoadMap of the generated text.");

    m.attr("OBS_SIZE") = core::kObsSize;
    m.attr("NUM_PLANES") = core::kPlanes;
    m.attr("NUM_SCALARS") = core::kScalars;
    m.attr("NUM_ACTIONS") = core::kNumActions;
    m.attr("NUM_PRIVILEGED") = core::kPrivileged;
    m.attr("NUM_MASK_LEVELS") = core::kMaskLevels;
    m.def(
        "feature_scales",
        [] {
            std::vector<float> scales(core::kObsSize);
            for (int i = 0; i < core::kObsSize; i++)
            {
                scales[i] = core::FeatureScale(i);
            }
            return ToNumpy(std::move(scales));
        },
        "Feature i of an observation means code * feature_scales()[i].");
    m.def(
        "features_from_blocks",
        [](std::string const& init, std::string const& round) { return Features(ViewFromBlocks(init, round)); },
        "init"_a, "round_block"_a,
        "(observation, masks) from the protocol text, as a dragon's first turn would compute them (no memory "
        "yet); masks[level] is that level's. Brain keeps the memory across turns.");
    m.def(
        "careful_noise",
        [](uint64_t state) {
            uint32_t const noise = core::CarefulNoise(state);
            return nb::make_tuple(noise, state);
        },
        "state"_a, "(noise, next state) of a careful dragon's generator.");
    m.attr("CAREFUL_SEED") = core::kCarefulSeed;
    m.def(
        "decode_action",
        [](std::string const& init, std::string const& round, int action) {
            if (action < 0 || action >= core::kNumActions)
            {
                throw nb::value_error("no such action");
            }
            return core::FormatCommand(core::Decode(ViewFromBlocks(init, round), action));
        },
        "init"_a, "round_block"_a, "action"_a, "The reply line an action makes, e.g. 'MOVE NE'.");

    m.attr("NUM_INFO") = core::kInfo;
    m.def(
        "level_map",
        [](uint64_t level_seed, nb::dict const& options) {
            core::EnvConfig const config = ConfigFrom(options);
            core::Level const level = core::MakeLevel(config, level_seed);
            return nb::make_tuple(core::LevelText(config, level), level.mMatchSeed, level.mMapIndex);
        },
        "level_seed"_a, "options"_a = nb::dict(),
        "(map text, match seed, fixed-map index or -1) of the level a seed names under these options.");

    nb::class_<PyBatchEnv>(m, "BatchEnv")
        .def(nb::init<int, uint64_t, nb::dict const&>(), "num_games"_a, "seed"_a = 0, "options"_a = nb::dict())
        .def_prop_ro("num_games", [](PyBatchEnv& self) { return self.Env().NumGames(); })
        .def_prop_ro("threads", [](PyBatchEnv& self) { return self.Env().Threads(); })
        .def_prop_ro("step_index", [](PyBatchEnv& self) { return self.Env().StepIndex(); })
        .def_prop_ro("turns", [](PyBatchEnv& self) { return self.Env().Turns(); })
        .def("set_next_level", [](PyBatchEnv& self, int slot, uint64_t seed) { self.Env().SetNextLevel(slot, seed); },
             "slot"_a, "level_seed"_a)
        // The outputs are written in place, so they must be the caller's own
        // arrays: noconvert() refuses a wrong dtype or layout instead of
        // silently writing into a temporary copy.
        .def("reset", &PyBatchEnv::Reset, nb::arg("obs").noconvert(), nb::arg("mask").noconvert(),
             nb::arg("priv").noconvert(), nb::arg("prev_row").noconvert(), nb::arg("prev_reward").noconvert(),
             nb::arg("info").noconvert())
        .def("step", &PyBatchEnv::Step, "actions"_a, nb::arg("obs").noconvert(), nb::arg("mask").noconvert(),
             nb::arg("priv").noconvert(), nb::arg("prev_row").noconvert(), nb::arg("prev_reward").noconvert(),
             nb::arg("info").noconvert());

    nb::class_<Match>(m, "Match")
        .def(nb::init<std::string const&, uint64_t, bool>(), "map_text"_a, "seed"_a = 0, "record"_a = false)
        .def("run", &Match::Run, "reply"_a, "spawn"_a = nb::none(), "death"_a = nb::none(),
             "Plays the whole game. reply(id, round_block) -> reply text; spawn(id, init_block) when a dragon's "
             "process would start; death(id, round, reason). Returns the result.")
        .def("begin", &Match::Begin)
        .def("current", &Match::Current, "The dragon whose turn it is, or None once the game is over.")
        .def("init_block", &Match::InitBlock, "id"_a)
        .def("round_block", &Match::RoundBlock, "id"_a)
        .def("reply", &Match::Reply, "text"_a, "Plays the current dragon's turn with this reply text.")
        .def("move", &Match::MoveSteps, "steps"_a)
        .def("split", [](Match& self, int n) { self.Act(ActionSplit{n}); }, "n"_a)
        .def("suicide", [](Match& self) { self.Act(ActionSuicide{}); })
        .def("result", &Match::Result)
        .def("dragons", &Match::Dragons)
        .def("tiles", &Match::Tiles)
        .def("set_pearl", &Match::SetPearl, "x"_a, "y"_a, "present"_a = true)
        .def("check_invariants", &Match::CheckInvariants)
        .def("probe", &Match::Probe, "action"_a,
             "Whether the current dragon would die this turn taking this action (played on a copy).")
        .def("features", [](Match const& self, int id) { return Features(self.ViewOf(id)); }, "id"_a,
             "(observation, masks) from the engine's state, as on a dragon's first turn (no memory yet).")
        .def(
            "view_difference",
            [](Match const& self, int id) {
                return ViewDifference(self.ViewOf(id), ViewFromBlocks(self.InitBlock(id), self.RoundBlock(id)));
            },
            "id"_a, "None if the View built from the state equals the one parsed from the init and round blocks.")
        .def_prop_ro("round", [](Match const& self) { return self.State().mRound; })
        .def_prop_ro("width", [](Match const& self) { return self.State().mWidth; })
        .def_prop_ro("height", [](Match const& self) { return self.State().mHeight; })
        .def_prop_ro("unit_limit", [](Match const& self) { return self.State().mUnitLimit; });

    nb::class_<Brain>(m, "Brain",
                      "One dragon's process: its memory across its turns, and the observation and masks it works "
                      "out each turn, as the bot and the training environment do. Feed it every turn of one dragon.")
        .def(nb::init<>())
        .def(
            "observe",
            [](Brain& self, std::string const& init, std::string const& round) {
                return self.Observe(ViewFromBlocks(init, round));
            },
            "init"_a, "round_block"_a, "This turn's (observation, masks) from the protocol text.")
        .def(
            "observe_match", [](Brain& self, Match const& match, int id) { return self.Observe(match.ViewOf(id)); },
            "match"_a, "id"_a, "This turn's (observation, masks) from the engine's state, as training computes them.")
        .def("careful", &Brain::Careful, "noise"_a, "The careful player's action this turn (core/careful.h).")
        .def("decode", &Brain::DecodeAction, "action"_a, "The reply line an action makes this turn.")
        .def_prop_ro("body", &Brain::Body, "The dragon's body as it remembers it, head first.")
        .def_prop_ro("survival", &Brain::SurvivalOf, "(moves it can be sure of after each action, its horizon).")
        .def("copy", [](Brain const& self) { return Brain(self); });
}
