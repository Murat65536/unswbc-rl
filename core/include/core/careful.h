#pragma once

// The careful scripted player, from a View alone: the training environment's
// stronger scripted opponent (core/batch_env.h, --scripted-careful), and a
// bot of its own (export/careful), so it can be played in the judge's
// sandbox too. It plays to the round limit far more often than the
// random-safe starter, so the round-limit tiebreaks (the queen first) decide
// its games.

#include "features.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace core {

/// Each careful dragon's noise: a 64-bit LCG from a fixed seed (each dragon
/// is a fresh process, so all start alike), nine bits a turn.
constexpr uint64_t kCarefulSeed = 0x853C49E6748FEA9Bull;

inline uint32_t CarefulNoise(uint64_t& rng)
{
    rng = rng * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<uint32_t>((rng >> 33) % (1u << 9));
}

/// A single step the level-2 mask allows that is not onto a head (so it never
/// trades heads, and never splits by choice), scored first by how long it can
/// be sure of surviving after it (Survival, with its Memory), then by the
/// room past it (a pocket smaller than the dragon scores low), a pearl there,
/// and how close enemy heads are (see EnemyReach), with three bits of `noise`
/// per step breaking ties. With no such step, the first action the mask
/// allows.
inline int CarefulAction(View const& view, uint32_t noise, Memory const* memory = nullptr)
{
    MoveSight const sight = LookAhead(view, memory);
    std::array<uint8_t, kNumActions> mask{};
    Mask(view, sight, 2, mask.data());
    std::array<uint8_t, kTiles> const reach = EnemyReach(view);
    int best = -1;
    int bestScore = 0;
    for (int a = 0; a < kSingleSteps; a++)
    {
        if (!mask[a] || sight.mHead[a] != kNoHead)
        {
            continue;
        }
        int const target = sight.mTarget[a];
        int score = 40; // out of sight: no better or worse than middling
        if (target >= 0)
        {
            Region const region = Explore(view, target);
            int const room = std::min(region.mTiles, 48);
            score = region.mOpen ? 60 + room : room >= view.mLength ? 30 + room : room;
            score += view.mTiles[target].mPearl ? 15 : 0;
            score -= 25 * reach[target];
        }
        score += sight.mSurvive[a] >= sight.mHorizon ? 200 : 5 * sight.mSurvive[a];
        score = score * 8 + static_cast<int>((noise >> (3 * a)) & 7);
        if (best < 0 || score > bestScore)
        {
            best = a;
            bestScore = score;
        }
    }
    if (best >= 0)
    {
        return best;
    }
    for (int a = 0; a < kNumActions; a++)
    {
        if (mask[a])
        {
            return a;
        }
    }
    return 0;
}

} // namespace core
