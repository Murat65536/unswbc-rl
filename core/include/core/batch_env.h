#pragma once

// Thousands of games stepped together across threads, for training.
//
// Each game slot always has exactly one decision outstanding: the dragon
// whose turn it is, seeing the board exactly as the dragons before it in the
// round left it (the engine's own turn order, so a split child is asked later
// in its birth round). step() plays every slot's decision, advances each game
// to its next decision (ending rounds, ticking pearls, and starting a new
// level when a game ends), and writes the new decisions' observations,
// masks and critic features straight into the caller's arrays.
//
// Rows. Decision n of step t is row t * num_games + n (t counts reset() as
// step 0). A dragon's transition from one of its decisions to its next
// completes when it decides again: that row reports `prev_row` (the earlier
// decision) and `prev_reward`. Transitions that end otherwise (the game ends,
// or the training horizon cuts it) arrive in the step's completions.
//
// Reward, per dragon, for its team: the result (+1 / -1 / 0) when the game
// ends, plus potential-based shaping gamma * phi(next) - phi(now), with phi a
// bounded function of the queens' and the teams' lengths, evaluated when the
// dragon decides. Over a dragon's whole trajectory the shaping telescopes to
// -phi(first decision), so it changes no optimal policy. A dragon that dies
// waits for the result: its last transition completes when the game ends,
// with the result discounted by the rounds in between.

#include "core/features.h"
#include "core/mapgen.h"
#include "core/state_view.h"

#include "engine/game.h"

#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace core {

struct EnvConfig
{
    int mThreads = 0; // 0: one per core
    int mMaskLevel = 1;
    /// Rounds after which a training game is cut (truncated, not scored):
    /// MAX_ROUNDS plays every game to its real end.
    int mMaxRounds = MAX_ROUNDS;

    float mGamma = 0.99f;
    float mWin = 1.0f;
    float mLoss = -1.0f;
    float mDraw = 0.0f;
    /// phi = scale * (queenWeight * (Qa - Qb) / (Qa + Qb + 2) + (1 - queenWeight) * (Ta - Tb) / (Ta + Tb + 2)),
    /// with Q the queens' lengths (0 once dead) and T the teams' total lengths.
    float mShaping = 0.5f;
    float mQueenWeight = 0.5f;

    // Generated levels: each value drawn uniformly per level from [lo, hi].
    int mWidthLo = 16, mWidthHi = 40;
    int mHeightLo = 16, mHeightHi = 40;
    /// Bit 0: x, bit 1: y, bit 2: xy.
    int mSymmetries = 7;
    int mDragonsLo = 1, mDragonsHi = 4;
    int mStartLengthLo = 3, mStartLengthHi = 6;
    float mKelpLo = 0.0f, mKelpHi = 0.12f;
    int mPortalsLo = 0, mPortalsHi = 4;
    float mBedsLo = 0.15f, mBedsHi = 0.6f;
    int mGapMinHiLo = 2, mGapMinHiHi = 30;
    int mGapSpreadLo = 10, mGapSpreadHi = 400;
    int mUnitLimit = DEFAULT_UNIT_LIMIT;

    /// Fixed maps (e.g. the official ones) mixed in with this probability.
    std::vector<std::string> mMaps;
    float mMapProb = 0.0f;
};

/// The level a seed names: the same map and match seed every time, in any
/// process, so a level sampler (PLR) can replay it.
struct Level
{
    int mMapIndex = -1; // into EnvConfig::mMaps, or -1 when generated
    MapGenConfig mMap;  // when generated
    uint64_t mMapSeed = 0;
    uint64_t mMatchSeed = 0;
};

Level MakeLevel(EnvConfig const& config, uint64_t levelSeed);
/// The level's board as .map text (what the real judge would load).
std::string LevelText(EnvConfig const& config, Level const& level);

/// A finished game, for logging and level scoring.
struct EpisodeInfo
{
    int32_t mSlot = 0;
    uint64_t mLevelSeed = 0;
    int32_t mMapIndex = -1;
    int32_t mRounds = 0;
    /// 0: A won, 1: B won, 2: draw, 3: cut at the training horizon.
    int32_t mOutcome = 0;
    int32_t mDragons = 0;
    std::array<int32_t, 2> mTotal{};
    std::array<int32_t, 2> mQueen{};
};

/// A transition that ended without a next decision: at the game's end
/// (mBoot < 0, nothing to bootstrap) or cut at the horizon (mBoot indexes the
/// bootstrap observations, whose value stands in for what would have come).
struct Completion
{
    int64_t mRow = -1;
    float mReward = 0.0f;
    int32_t mBoot = -1;
};

/// Where step() and reset() write the new decisions (row n per slot n).
struct DecisionOut
{
    uint8_t* mObs = nullptr;      // [n, kObsSize]
    uint8_t* mMask = nullptr;     // [n, kNumActions]
    float* mPrivileged = nullptr; // [n, kPrivileged]
    int64_t* mPrevRow = nullptr;  // [n]
    float* mPrevReward = nullptr; // [n]
    int32_t* mInfo = nullptr;     // [n, kInfo]: dragon id, team, round, episode (per slot)
};

constexpr int kInfo = 4;

class BatchEnv
{
  public:
    BatchEnv(int numGames, EnvConfig config, uint64_t seed);
    ~BatchEnv();

    int NumGames() const
    {
        return static_cast<int>(mSlots.size());
    }
    int Threads() const
    {
        return static_cast<int>(mWorkers.size()) + 1;
    }

    /// The level each slot plays next time a game there ends (and, at
    /// reset(), now). Unset slots draw a fresh seed.
    void SetNextLevel(int slot, uint64_t levelSeed);

    /// Starts a new level in every slot and writes the first decisions.
    void Reset(DecisionOut const& out);

    /// Plays actions[n] for slot n's outstanding decision, then writes the
    /// next decisions.
    void Step(int32_t const* actions, DecisionOut const& out);

    std::vector<Completion> const& Completions() const
    {
        return mCompletions;
    }
    std::vector<uint8_t> const& BootObs() const
    {
        return mBootObs;
    }
    std::vector<uint8_t> const& BootMask() const
    {
        return mBootMask;
    }
    std::vector<float> const& BootPrivileged() const
    {
        return mBootPrivileged;
    }
    std::vector<EpisodeInfo> const& Episodes() const
    {
        return mEpisodes;
    }
    int64_t StepIndex() const
    {
        return mStep;
    }
    /// Turns played since construction.
    int64_t Turns() const;

  private:
    struct Agent
    {
        int64_t mLastRow = -1;
        float mLastPhi = 0.0f;
        int mLastRound = 0;
    };

    struct Slot
    {
        std::unique_ptr<Game> mGame;
        uint64_t mLevelSeed = 0;
        int mMapIndex = -1;
        uint64_t mNextSeed = 0;
        int32_t mEpisode = 0;
        int mDragons = 0;
        std::vector<Agent> mAgents;
        View mView;
        int64_t mTurns = 0;

        // Filled by a worker during a step, gathered afterwards.
        std::vector<Completion> mDone;
        std::vector<uint8_t> mBootObs;
        std::vector<uint8_t> mBootMask;
        std::vector<float> mBootPrivileged;
        std::vector<EpisodeInfo> mEpisodes;
    };

    void StartLevel(Slot& slot, int index);
    void Decide(Slot& slot, int index, DecisionOut const& out);
    void FinishGame(Slot& slot, int index, bool truncated);
    float Phi(GameState const& state, Team team) const;
    float Phi(Standings const& standings) const;
    void Play(Slot& slot, int index, int action);
    void RunParallel(std::function<void(int, int)> const& work);
    void Gather();

    EnvConfig mConfig;
    std::vector<Slot> mSlots;
    std::vector<GameState> mMapStates;
    std::mt19937_64 mSeeds;
    int64_t mStep = 0;

    std::vector<Completion> mCompletions;
    std::vector<uint8_t> mBootObs;
    std::vector<uint8_t> mBootMask;
    std::vector<float> mBootPrivileged;
    std::vector<EpisodeInfo> mEpisodes;

    // A fixed pool: workers wait for a generation bump, run their share of
    // `mWork`, and report back.
    std::vector<std::thread> mWorkers;
    std::mutex mMutex;
    std::condition_variable mWake;
    std::condition_variable mDoneSignal;
    std::function<void(int, int)> const* mWork = nullptr;
    uint64_t mGeneration = 0;
    int mPending = 0;
    bool mStopping = false;
    std::exception_ptr mFailure;
};

} // namespace core
