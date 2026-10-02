#pragma once

// Procedural maps for training and for the fidelity tests. A map is generated
// as .map text and read back with the engine's own LoadMap, so the real judge
// (which only takes text) and this engine always load the same board.

#include "engine/types.h"

#include <cstdint>
#include <string>

namespace core {

struct MapGenConfig
{
    int mWidth = 24;
    int mHeight = 24;
    /// X mirrors y, Y mirrors x, XY rotates 180 degrees (engine MirrorTile).
    Symmetry mSymmetry = Symmetry::XY;
    int mDragonsPerTeam = 2;
    int mStartLength = 3;
    /// Chance that a free edge is kelp (mirrored edges follow).
    double mKelp = 0.05;
    /// Portal pairs to try to place; a pair off the line of symmetry brings
    /// its mirrored pair along, so the map can hold up to twice this many.
    int mPortalPairs = 2;
    /// Chance that a tile spawns pearls (mirrored tiles share it).
    double mPearlBeds = 0.4;
    /// A bed's gap range is [lo, hi]: lo drawn from [mGapMinLo, mGapMinHi],
    /// hi from [lo, lo + mGapSpread].
    int mGapMinLo = 1;
    int mGapMinHi = 10;
    int mGapSpread = 60;
    int mUnitLimit = DEFAULT_UNIT_LIMIT;
};

/// The same seed and config give the same text on every platform: the draws
/// use std::mt19937_64 (fully specified by the standard) and no distribution
/// objects (whose algorithms differ between standard libraries).
std::string GenerateMapText(MapGenConfig const& config, uint64_t seed);

char const* SymmetryName(Symmetry symmetry);

} // namespace core
