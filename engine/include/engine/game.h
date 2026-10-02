#pragma once

#include "event.h"
#include "types.h"

#include <capnp/message.h>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using ControllerFactory = std::function<DragonFn(Dragon const&)>;

GameState LoadMap(std::string const& mapText);

class Game
{
  public:
    explicit Game(GameState state, DebugOutput keep = {});

    void SpawnControllersWith(ControllerFactory factory);
    void OnEvent(EventSink sink);
    /// Called by the controller; attached to this turn's DragonAction event.
    void RecordInstructions(uint64_t count, bool exceeded, bool tle);

    /// `stop` is asked after every round; once it says yes the game ends
    /// there, scored as if that were the last round.
    GameResult Run(std::function<bool()> const& stop = {});

    GameState const& State() const;
    std::vector<Event> const& Events() const;

    void WriteReplay(capnp::MessageBuilder& message, std::string const& mapText,
                     std::pair<std::string, std::string> const& teamNames) const;

  private:
    void TakeTurn(DragonId id);

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
    std::mt19937 mRng{PEARL_RNG_SEED};
};
