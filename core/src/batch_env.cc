#include "core/batch_env.h"

#include "engine/helpers.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace core {

namespace {

class LevelDraws
{
  public:
    explicit LevelDraws(uint64_t seed) : mRng(seed ^ 0x9E3779B97F4A7C15ull) {}

    int Int(int lo, int hi)
    {
        return hi <= lo ? lo : lo + static_cast<int>(mRng() % static_cast<uint64_t>(hi - lo + 1));
    }

    double Real(double lo, double hi)
    {
        return lo + (hi - lo) * (static_cast<double>(mRng() >> 11) * 0x1.0p-53);
    }

    uint64_t Bits()
    {
        return mRng();
    }

  private:
    std::mt19937_64 mRng;
};

uint64_t Mix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

} // namespace

Level MakeLevel(EnvConfig const& config, uint64_t levelSeed)
{
    LevelDraws draws(levelSeed);
    Level level;
    if (!config.mMaps.empty() && draws.Real(0.0, 1.0) < config.mMapProb)
    {
        level.mMapIndex = draws.Int(0, static_cast<int>(config.mMaps.size()) - 1);
        level.mMatchSeed = draws.Bits();
        return level;
    }

    std::vector<Symmetry> symmetries;
    for (auto [bit, symmetry] : {std::pair{1, Symmetry::X}, std::pair{2, Symmetry::Y}, std::pair{4, Symmetry::XY}})
    {
        if (config.mSymmetries & bit)
        {
            symmetries.push_back(symmetry);
        }
    }
    RUNTIME_ASSERT(!symmetries.empty(), "no symmetry allowed for generated levels");

    MapGenConfig& map = level.mMap;
    map.mWidth = draws.Int(config.mWidthLo, config.mWidthHi);
    map.mHeight = draws.Int(config.mHeightLo, config.mHeightHi);
    map.mSymmetry = symmetries[draws.Int(0, static_cast<int>(symmetries.size()) - 1)];
    map.mDragonsPerTeam = draws.Int(config.mDragonsLo, config.mDragonsHi);
    map.mStartLength = draws.Int(config.mStartLengthLo, config.mStartLengthHi);
    map.mKelp = draws.Real(config.mKelpLo, config.mKelpHi);
    map.mPortalPairs = draws.Int(config.mPortalsLo, config.mPortalsHi);
    map.mPearlBeds = draws.Real(config.mBedsLo, config.mBedsHi);
    map.mGapMinLo = 1;
    map.mGapMinHi = draws.Int(config.mGapMinHiLo, config.mGapMinHiHi);
    map.mGapSpread = draws.Int(config.mGapSpreadLo, config.mGapSpreadHi);
    map.mUnitLimit = config.mUnitLimit;
    level.mMapSeed = draws.Bits();
    level.mMatchSeed = draws.Bits();
    return level;
}

namespace {

/// The generated board, asking for fewer or shorter dragons when a crowded
/// small board has no room for them all.
template <typename Make> auto WithRoom(MapGenConfig map, uint64_t seed, Make make)
{
    for (;;)
    {
        try
        {
            return make(map, seed);
        }
        catch (std::runtime_error const&)
        {
            if (map.mDragonsPerTeam == 1 && map.mStartLength == MIN_DRAGON_LENGTH)
            {
                throw;
            }
            if (map.mDragonsPerTeam > 1)
            {
                map.mDragonsPerTeam--;
            }
            else
            {
                map.mStartLength--;
            }
        }
    }
}

} // namespace

std::string LevelText(EnvConfig const& config, Level const& level)
{
    if (level.mMapIndex >= 0)
    {
        return config.mMaps[level.mMapIndex];
    }
    return WithRoom(level.mMap, level.mMapSeed, GenerateMapText);
}

BatchEnv::BatchEnv(int numGames, EnvConfig config, uint64_t seed)
    : mConfig(std::move(config)), mSlots(numGames), mSeeds(seed)
{
    RUNTIME_ASSERT(numGames > 0, "a batch needs at least one game");
    RUNTIME_ASSERT(mConfig.mMaxRounds >= 1 && mConfig.mMaxRounds <= MAX_ROUNDS, "max rounds out of range");
    for (std::string const& text : mConfig.mMaps)
    {
        mMapStates.push_back(LoadMap(text));
    }
    for (Slot& slot : mSlots)
    {
        slot.mNextSeed = mSeeds();
    }
    RUNTIME_ASSERT(mConfig.mScriptedFrac >= 0.0f && mConfig.mScriptedFrac <= 1.0f, "scripted share out of range");
    RUNTIME_ASSERT(mConfig.mQueenWeight >= 0.0f && mConfig.mLongestWeight >= 0.0f &&
                       mConfig.mQueenWeight + mConfig.mLongestWeight <= 1.0f + 1e-6f,
                   "the queen and longest weights must be non-negative and sum to at most 1");
    RUNTIME_ASSERT(mConfig.mMaskLevel >= 0 && mConfig.mMaskLevel < kMaskLevels, "no such mask level");
    mScriptedSlots = static_cast<int>(std::lround(mConfig.mScriptedFrac * numGames));
    RUNTIME_ASSERT(mConfig.mScriptedCareful >= 0.0f && mConfig.mScriptedCareful <= 1.0f, "careful share out of range");
    mCarefulSlots = static_cast<int>(std::lround(mConfig.mScriptedCareful * mScriptedSlots));

    int threads = mConfig.mThreads > 0 ? mConfig.mThreads : static_cast<int>(std::thread::hardware_concurrency());
    threads = std::clamp(threads, 1, numGames);
    for (int worker = 1; worker < threads; worker++)
    {
        mWorkers.emplace_back([this, worker, threads] {
            uint64_t seen = 0;
            for (;;)
            {
                std::function<void(int, int)> const* work = nullptr;
                {
                    std::unique_lock lock(mMutex);
                    mWake.wait(lock, [&] { return mStopping || mGeneration != seen; });
                    if (mStopping)
                    {
                        return;
                    }
                    seen = mGeneration;
                    work = mWork;
                }
                int const n = NumGames();
                std::exception_ptr failure;
                try
                {
                    (*work)(n * worker / threads, n * (worker + 1) / threads);
                }
                catch (...)
                {
                    failure = std::current_exception();
                }
                {
                    std::lock_guard lock(mMutex);
                    if (failure && !mFailure)
                    {
                        mFailure = failure;
                    }
                    if (--mPending == 0)
                    {
                        mDoneSignal.notify_one();
                    }
                }
            }
        });
    }
}

BatchEnv::~BatchEnv()
{
    {
        std::lock_guard lock(mMutex);
        mStopping = true;
    }
    mWake.notify_all();
    for (std::thread& worker : mWorkers)
    {
        worker.join();
    }
}

void BatchEnv::RunParallel(std::function<void(int, int)> const& work)
{
    int const threads = Threads();
    if (threads > 1)
    {
        std::lock_guard lock(mMutex);
        mWork = &work;
        mPending = threads - 1;
        mGeneration++;
    }
    mWake.notify_all();
    // The calling thread takes the first share. An exception in a share is
    // kept until every share is done, so no worker outlives `work`.
    std::exception_ptr failure;
    try
    {
        work(0, NumGames() / threads);
    }
    catch (...)
    {
        failure = std::current_exception();
    }
    if (threads > 1)
    {
        std::unique_lock lock(mMutex);
        mDoneSignal.wait(lock, [&] { return mPending == 0; });
        mWork = nullptr;
        if (!failure && mFailure)
        {
            failure = mFailure;
        }
        mFailure = nullptr;
    }
    if (failure)
    {
        std::rethrow_exception(failure);
    }
}

void BatchEnv::SetNextLevel(int slot, uint64_t levelSeed)
{
    RUNTIME_ASSERT(slot >= 0 && slot < NumGames(), "no such slot");
    mSlots[slot].mNextSeed = levelSeed;
}

int64_t BatchEnv::Turns() const
{
    int64_t turns = 0;
    for (Slot const& slot : mSlots)
    {
        turns += slot.mTurns;
    }
    return turns;
}

void BatchEnv::StartLevel(Slot& slot, int index)
{
    uint64_t const seed = slot.mNextSeed;
    // Without a level set from outside, the next one is drawn from this
    // slot's own sequence, so slots never share a generator across threads.
    slot.mNextSeed = Mix(seed ^ (static_cast<uint64_t>(index) << 32) ^ static_cast<uint64_t>(slot.mEpisode));

    Level const level = MakeLevel(mConfig, seed);
    GameState state = level.mMapIndex >= 0 ? mMapStates[level.mMapIndex]
                                           : WithRoom(level.mMap, level.mMapSeed, GenerateMapState);
    slot.mDragons = static_cast<int>(state.mDragons.size());
    slot.mGame = std::make_unique<Game>(std::move(state), level.mMatchSeed, DebugOutput{.mRecord = false});
    slot.mGame->Begin();
    slot.mLevelSeed = seed;
    slot.mMapIndex = level.mMapIndex;
    slot.mEpisode++;
    slot.mAgents.assign(slot.mDragons, Agent{});
    slot.mScriptedTeam = index >= NumGames() - mScriptedSlots ? 1 - (index + slot.mEpisode) % 2 : -1;
    slot.mCareful = slot.mScriptedTeam >= 0 && index < NumGames() - mScriptedSlots + mCarefulSlots;
    slot.mScriptRng.clear();
    slot.mQueenDied = {-1, -1};
    slot.mQueenDeath = {0, 0};
    slot.mQueenSplits = {0, 0};
}

namespace {

/// No move the dragon can see survives (each is visibly fatal or onto a
/// head), and it cannot split.
bool Cornered(View const& view)
{
    if (SplitLegal(view))
    {
        return false;
    }
    MoveSight const sight = LookAhead(view);
    for (int a = 0; a < kSplitAction; a++)
    {
        if (!sight.mVisiblyFatal[a] && sight.mHead[a] == kNoHead)
        {
            return false;
        }
    }
    return true;
}

} // namespace

void BatchEnv::TakeTurn(Slot& slot, ControllerReply const& reply)
{
    GameState const& state = slot.mGame->State();
    DragonId const mover = *slot.mGame->CurrentDragon();
    Team const moverTeam = state.mDragons[mover].mTeam;
    bool const cornered = IsQueen(mover) && Cornered(slot.mView);
    slot.mGame->TakeTurnWith(reply);
    slot.mTurns++;
    for (DragonId id = 0; id < 2 && id < static_cast<int>(state.mDragons.size()); id++)
    {
        Dragon const& queen = state.mDragons[id];
        int const team = queen.mTeam == Team::A ? 0 : 1;
        if (!queen.mAlive && slot.mQueenDied[team] < 0)
        {
            slot.mQueenDied[team] = state.mRound;
            slot.mQueenDeath[team] = id == mover ? (cornered ? 1 : 2) : moverTeam == queen.mTeam ? 4 : 3;
        }
    }
}

namespace {

ControllerReply ReplyFor(Command const& command)
{
    ControllerReply reply;
    if (command.mSplit)
    {
        reply.mAction = ActionSplit{command.mSplitSize};
        return reply;
    }
    ActionMove move;
    for (int i = 0; i < command.mSteps; i++)
    {
        move.mSteps.push_back(static_cast<Direction>(kDirChars[command.mDirs[i]]));
    }
    reply.mAction = std::move(move);
    return reply;
}

} // namespace

void BatchEnv::PlayScripted(Slot& slot)
{
    GameState const& state = slot.mGame->State();
    DragonId const id = *slot.mGame->CurrentDragon();
    ViewFromState(state, state.mDragons[id], slot.mView);
    if (id >= static_cast<int>(slot.mScriptRng.size()))
    {
        slot.mScriptRng.resize(id + 1, kCarefulSeed);
    }
    uint64_t& rng = slot.mScriptRng[id];
    auto const next = [&rng](int bound) {
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<int>((rng >> 33) % static_cast<uint64_t>(bound));
    };
    if (slot.mCareful)
    {
        if (id >= static_cast<int>(slot.mAgents.size()))
        {
            slot.mAgents.resize(id + 1);
        }
        Memory& memory = slot.mAgents[id].mMemory;
        memory.Update(slot.mView);
        int const action = CarefulAction(slot.mView, CarefulNoise(slot.mScriptRng[id]), &memory);
        TakeTurn(slot, ReplyFor(Decode(slot.mView, action)));
        return;
    }
    std::array<int, 4> order = {kNorth, kEast, kSouth, kWest};
    for (int i = 3; i > 0; i--)
    {
        std::swap(order[i], order[next(i + 1)]);
    }
    // The starter's rule: the first direction, in random order, that is not
    // into kelp and not onto a visible dragon on the plain neighbouring tile.
    int choice = kNorth;
    for (int const dir : order)
    {
        TileView const& here = slot.mView.mTiles[kHeadTile];
        int const ahead = NeighbourInWindow(kHeadTile, dir);
        if (here.mEdges[dir].mKind == kKelp || (ahead >= 0 && slot.mView.mTiles[ahead].mPart.mId >= 0))
        {
            continue;
        }
        choice = dir;
        break;
    }
    ControllerReply reply;
    reply.mAction = ActionMove{{static_cast<Direction>(kDirChars[choice])}};
    TakeTurn(slot, reply);
}

void BatchEnv::Advance(Slot& slot, int index)
{
    for (;;)
    {
        if (slot.mGame->Over())
        {
            FinishGame(slot, index, false);
            continue;
        }
        if (slot.mGame->State().mRound >= mConfig.mMaxRounds)
        {
            FinishGame(slot, index, true);
            continue;
        }
        if (slot.mScriptedTeam >= 0)
        {
            DragonId const id = *slot.mGame->CurrentDragon();
            Team const team = slot.mGame->State().mDragons[id].mTeam;
            if ((team == Team::A ? 0 : 1) == slot.mScriptedTeam)
            {
                PlayScripted(slot);
                continue;
            }
        }
        return;
    }
}

float BatchEnv::Phi(GameState const& state, Team team) const
{
    return Phi(StandingsFor(state, team));
}

float BatchEnv::Phi(Standings const& s) const
{
    auto const lead = [](int ours, int theirs) {
        return static_cast<float>(ours - theirs) / static_cast<float>(ours + theirs + 2);
    };
    float const wq = mConfig.mQueenWeight;
    float const wl = mConfig.mLongestWeight;
    return mConfig.mShaping * (wq * lead(s.mQueen[0], s.mQueen[1]) + wl * lead(s.mLongest[0], s.mLongest[1]) +
                               (1.0f - wq - wl) * lead(s.mTotal[0], s.mTotal[1]));
}

void BatchEnv::Decide(Slot& slot, int index, DecisionOut const& out)
{
    GameState const& state = slot.mGame->State();
    DragonId const id = *slot.mGame->CurrentDragon();
    Dragon const& dragon = state.mDragons[id];

    if (id >= static_cast<int>(slot.mAgents.size()))
    {
        slot.mAgents.resize(id + 1);
    }
    Agent& agent = slot.mAgents[id];
    ViewFromState(state, dragon, slot.mView);
    agent.mMemory.Update(slot.mView);
    MoveSight const sight = LookAhead(slot.mView, &agent.mMemory);
    Encode(slot.mView, sight, out.mObs + static_cast<size_t>(index) * kObsSize);
    Mask(slot.mView, sight, mConfig.mMaskLevel, out.mMask + static_cast<size_t>(index) * kNumActions);
    Standings const standings = StandingsFor(state, dragon.mTeam);
    PrivilegedFeatures(state, dragon, standings, out.mPrivileged + static_cast<size_t>(index) * kPrivileged);

    float const phi = Phi(standings);
    int64_t const row = mStep * NumGames() + index;
    if (agent.mLastRow >= 0)
    {
        out.mPrevRow[index] = agent.mLastRow;
        out.mPrevReward[index] = mConfig.mGamma * phi - agent.mLastPhi;
    }
    else
    {
        out.mPrevRow[index] = -1;
        out.mPrevReward[index] = 0.0f;
    }
    agent.mLastRow = row;
    agent.mLastPhi = phi;
    agent.mLastRound = state.mRound;

    int32_t* info = out.mInfo + static_cast<size_t>(index) * kInfo;
    info[0] = id;
    info[1] = dragon.mTeam == Team::A ? 0 : 1;
    info[2] = state.mRound;
    info[3] = slot.mEpisode;
}

void BatchEnv::FinishGame(Slot& slot, int index, bool truncated)
{
    GameState const& state = slot.mGame->State();
    GameResult const& result = slot.mGame->Result();
    float const gamma = mConfig.mGamma;
    for (DragonId id = 0; id < static_cast<int>(slot.mAgents.size()); id++)
    {
        Agent const& agent = slot.mAgents[id];
        if (agent.mLastRow < 0)
        {
            continue;
        }
        Dragon const& dragon = state.mDragons[id];
        Completion done;
        done.mRow = agent.mLastRow;
        if (!truncated)
        {
            // Rounds from the dragon's last decision to the one it would
            // have made after the final round.
            int const rounds = state.mRound + 1 - agent.mLastRound;
            float outcome = mConfig.mDraw;
            if (result.mWinner)
            {
                outcome = *result.mWinner == dragon.mTeam ? mConfig.mWin : mConfig.mLoss;
            }
            done.mReward = std::pow(gamma, static_cast<float>(rounds)) * outcome - agent.mLastPhi;
        }
        else
        {
            // Cut at the horizon: a living dragon bootstraps from what it
            // would see now; a dead one has no view, and its team's
            // potential stands in for the result it is still waiting for.
            int const rounds = state.mRound - agent.mLastRound;
            done.mReward = std::pow(gamma, static_cast<float>(rounds)) * Phi(state, dragon.mTeam) - agent.mLastPhi;
            if (dragon.mAlive)
            {
                done.mBoot = static_cast<int32_t>(slot.mBootObs.size() / kObsSize);
                ViewFromState(state, dragon, slot.mView);
                Memory memory = agent.mMemory;
                memory.Update(slot.mView);
                MoveSight const sight = LookAhead(slot.mView, &memory);
                size_t const at = slot.mBootObs.size();
                slot.mBootObs.resize(at + kObsSize);
                Encode(slot.mView, sight, slot.mBootObs.data() + at);
                size_t const maskAt = slot.mBootMask.size();
                slot.mBootMask.resize(maskAt + kNumActions);
                Mask(slot.mView, sight, mConfig.mMaskLevel, slot.mBootMask.data() + maskAt);
                size_t const privAt = slot.mBootPrivileged.size();
                slot.mBootPrivileged.resize(privAt + kPrivileged);
                PrivilegedFeatures(state, dragon, StandingsFor(state, dragon.mTeam),
                                   slot.mBootPrivileged.data() + privAt);
            }
        }
        slot.mDone.push_back(done);
    }

    EpisodeInfo info;
    info.mSlot = index;
    info.mLevelSeed = slot.mLevelSeed;
    info.mMapIndex = slot.mMapIndex;
    info.mRounds = state.mRound + (truncated ? 0 : 1);
    info.mOutcome = truncated ? 3 : !result.mWinner ? 2 : *result.mWinner == Team::A ? 0 : 1;
    info.mDragons = static_cast<int32_t>(state.mDragons.size());
    Standings const s = StandingsFor(state, Team::A);
    info.mTotal = {s.mTotal[0], s.mTotal[1]};
    info.mQueen = {s.mQueen[0], s.mQueen[1]};
    info.mLongest = {s.mLongest[0], s.mLongest[1]};
    info.mQueenDied = slot.mQueenDied;
    info.mQueenDeath = slot.mQueenDeath;
    info.mQueenSplits = slot.mQueenSplits;
    if (!truncated)
    {
        TeamStanding const& a = result.mTeamA;
        TeamStanding const& b = result.mTeamB;
        info.mDecider = result.mEndReason == GameEndReason::TeamEliminated ? 0
                        : a.mQueenLength != b.mQueenLength                 ? 1
                        : a.mLongestDragon != b.mLongestDragon             ? 2
                        : a.mTotalLength != b.mTotalLength                 ? 3
                                                                           : 4;
    }
    info.mScriptedTeam = slot.mScriptedTeam;
    info.mScriptedCareful = slot.mCareful ? 1 : 0;
    slot.mEpisodes.push_back(info);

    StartLevel(slot, index);
}

void BatchEnv::Play(Slot& slot, int index, int action)
{
    ControllerReply reply;
    if (action < 0 || action >= kNumActions)
    {
        reply.mAction = ActionSuicide{};
    }
    else
    {
        Command const command = Decode(slot.mView, action);
        reply = ReplyFor(command);
        if (command.mSplit && IsQueen(slot.mView.mId))
        {
            slot.mQueenSplits[slot.mView.mTeam]++;
        }
    }
    TakeTurn(slot, reply);
    // Start the next game here as soon as this one ends (or is cut).
    Advance(slot, index);
}

void BatchEnv::Gather()
{
    mCompletions.clear();
    mBootObs.clear();
    mBootMask.clear();
    mBootPrivileged.clear();
    mEpisodes.clear();
    for (Slot& slot : mSlots)
    {
        int32_t const offset = static_cast<int32_t>(mBootObs.size() / kObsSize);
        for (Completion done : slot.mDone)
        {
            if (done.mBoot >= 0)
            {
                done.mBoot += offset;
            }
            mCompletions.push_back(done);
        }
        mBootObs.insert(mBootObs.end(), slot.mBootObs.begin(), slot.mBootObs.end());
        mBootMask.insert(mBootMask.end(), slot.mBootMask.begin(), slot.mBootMask.end());
        mBootPrivileged.insert(mBootPrivileged.end(), slot.mBootPrivileged.begin(), slot.mBootPrivileged.end());
        mEpisodes.insert(mEpisodes.end(), slot.mEpisodes.begin(), slot.mEpisodes.end());
        slot.mDone.clear();
        slot.mBootObs.clear();
        slot.mBootMask.clear();
        slot.mBootPrivileged.clear();
        slot.mEpisodes.clear();
    }
}

void BatchEnv::Reset(DecisionOut const& out)
{
    mStep = 0;
    std::function<void(int, int)> const work = [&](int begin, int end) {
        for (int i = begin; i < end; i++)
        {
            Slot& slot = mSlots[i];
            slot.mDone.clear();
            slot.mBootObs.clear();
            slot.mBootMask.clear();
            slot.mBootPrivileged.clear();
            slot.mEpisodes.clear();
            StartLevel(slot, i);
            Advance(slot, i);
            Decide(slot, i, out);
        }
    };
    RunParallel(work);
    Gather();
}

void BatchEnv::Step(int32_t const* actions, DecisionOut const& out)
{
    RUNTIME_ASSERT(mSlots[0].mGame, "reset() the environment before stepping it");
    mStep++;
    std::function<void(int, int)> const work = [&](int begin, int end) {
        for (int i = begin; i < end; i++)
        {
            Play(mSlots[i], i, actions[i]);
            Decide(mSlots[i], i, out);
        }
    };
    RunParallel(work);
    Gather();
}

} // namespace core
