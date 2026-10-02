// Python bindings for the C++ core (module `bccore`).

#include "core/mapgen.h"

#include "engine/game.h"
#include "engine/helpers.h"
#include "engine/protocol.h"
#include "engine/scoring.h"

#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/vector.h>

#include <memory>
#include <optional>
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
        .def_prop_ro("round", [](Match const& self) { return self.State().mRound; })
        .def_prop_ro("width", [](Match const& self) { return self.State().mWidth; })
        .def_prop_ro("height", [](Match const& self) { return self.State().mHeight; })
        .def_prop_ro("unit_limit", [](Match const& self) { return self.State().mUnitLimit; });
}
