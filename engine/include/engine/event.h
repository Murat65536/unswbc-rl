#pragma once

#include "types.h"

#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

struct EventRoundStart
{
    int mRound;
};

struct EventTurnStart
{
    DragonId mId;
};

struct InstructionUsage
{
    uint64_t mCount;
    bool mExceeded;
};

struct EventPearlCountdown
{
    Point mTile;
    int mCountdown;
};

struct EventTileChange
{
    Point mTile;
    bool mHasPearl;
};

struct EventDragonAction
{
    DragonId mId;
    std::optional<PlayerAction> mAction;
    std::optional<InstructionUsage> mInstructions;
    bool mTle = false;
};

// sonar

struct EventSonarPing
{
    DragonId mSenderId;
    Direction mDirection;
    uint64_t mValue;
    Point mOrigin;
    Point mEnd;
    std::optional<DragonId> mHitId;
    SonarHitKind mHitKind;
};

// debug events

struct EventEngineLog
{
    DragonId mId;
    std::string mText;
};

struct EventDragonLog
{
    DragonId mId;
    std::string mText;
};

struct EventDragonIndicator
{
    DragonId mId;
    std::string mText;
};

struct EventDebugDraw
{
    DragonId mId;
    DebugDraw mDraw;
};

struct EventDragonUpdate
{
    DragonId mId;
    Direction mFacing;
    Point mHead;
    Point mTail;
};

struct EventDragonSplit
{
    DragonId mParentId;
    DragonId mChildId;
    Team mTeam;
    Direction mChildFacing;
    std::vector<Point> mParentBody;
    std::vector<Point> mChildBody;
};

enum class DragonDeathReason : char
{
    HitWall = 'W',
    HitSelf = 'S',
    HitOtherBody = 'O',
    HitHeadToHead = 'H',
    NoValidAction = 'A',
};

struct EventDragonDeath
{
    DragonId mId;
    DragonDeathReason mReason;
    Team mTeam;
};


using Event = std::variant<EventRoundStart, EventTurnStart, EventPearlCountdown, EventTileChange, EventDragonAction,
                           EventEngineLog, EventDragonLog, EventDragonIndicator, EventDebugDraw, EventDragonUpdate,
                           EventDragonSplit, EventDragonDeath, EventSonarPing>;

using EventSink = std::function<void(Event const&)>;
