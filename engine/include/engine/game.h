#pragma once

#include "event.h"
#include "types.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef UNSWBC_ENGINE_REPLAY
#include <capnp/message.h>
#endif

using ControllerFactory = std::function<DragonFn(Dragon const&)>;

GameState LoadMap(std::string const& mapText);

class Game
{
  public:
    /// `seed` is the match seed: it decides every pearl countdown.
    explicit Game(GameState state, uint64_t seed = 0, DebugOutput keep = {});

    void SpawnControllersWith(ControllerFactory factory);
    void OnEvent(EventSink sink);
    /// Called by the controller; attached to this turn's DragonAction event.
    void RecordInstructions(uint64_t count, bool exceeded, bool tle);

    /// `stop` is asked after every round; once it says yes the game ends
    /// there, scored as if that were the last round.
    GameResult Run(std::function<bool()> const& stop = {});

    /// The same game a turn at a time, with no controllers: Begin() sets up
    /// the board and the first round, CurrentDragon() names the dragon whose
    /// turn it is, and TakeTurnWith() plays that turn with the given reply and
    /// moves on to the next living dragon, starting new rounds as needed. The
    /// round block a controller would have been sent is BuildRoundBlock(State(),
    /// dragon), taken before TakeTurnWith() empties the sonar inbox.
    void Begin();
    std::optional<DragonId> CurrentDragon() const;
    void TakeTurnWith(ControllerReply const& reply);
    bool Over() const;
    GameResult const& Result() const;

    GameState const& State() const;
    std::vector<Event> const& Events() const;

#ifdef UNSWBC_ENGINE_REPLAY
    void WriteReplay(capnp::MessageBuilder& message, std::string const& mapText,
                     std::pair<std::string, std::string> const& teamNames) const;
#endif

  private:
    void TakeTurn(DragonId id);
    void StartTurn(Dragon& dragon);
    void FinishTurn(DragonId id, ControllerReply const& reply);
    void StartRound();
    /// Moves the turn cursor onto the next living dragon, ending rounds (and
    /// possibly the game) on the way.
    void SettleCursor();

    GameState mState;
    DebugOutput mKeep;
    std::vector<Event> mEvents;
    GameResult mResult;
    std::optional<InstructionUsage> mTurnInstructions;
    bool mTurnTle = false;
    std::unordered_map<DragonId, DragonFn> mControllers;
    ControllerFactory mSpawnController;
    EventSink mSink;
    EventSink mEmit;
    PearlRng mRng;
    std::function<bool()> const* mStop = nullptr;
    size_t mTurnIndex = 0;
    bool mBegun = false;
    bool mOver = false;
};
